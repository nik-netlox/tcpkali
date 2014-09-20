#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stddef.h> /* offsetof(3) */
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#ifdef  HAVE_SCHED_H
#include <sched.h>
#endif
#include <assert.h>

#include <ev.h>

#include "tcpkali.h"
#include "tcpkali_atomic.h"

struct loop_arguments {
    struct addresses addresses;
    const void *sample_data;
    size_t      sample_data_size;
    struct remote_stats {
        long connection_attempts;
        long connection_failures;
    } *remote_stats;    /* Per-thread remote server stats */
    unsigned int address_offset;
    int control_pipe;
    int thread_no;
    size_t total_data_transmitted;
    enum {
        THREAD_TERMINATING = 1
    } thread_flags;
    atomic_t open_connections;
};

struct connection {
    off_t write_offset;
    size_t data_transmitted;
    struct sockaddr *remote_address;
    struct remote_stats *remote_stats;
    struct ev_loop *loop;
    ev_io watcher;
};

/*
 * Helper functions defined at the end of the file.
 */
static long number_of_cpus();
static int max_open_files();
static void *single_engine_loop_thread(void *argp);
static void start_new_connection(EV_P);
static void close_connection(struct connection *conn);
static void connection_cb(EV_P_ ev_io *w, int revents);
static void control_cb(EV_P_ ev_io *w, int revents);
static struct sockaddr *pick_remote_address(struct loop_arguments *largs, struct remote_stats **remote_stats);
static void largest_contiguous_chunk(struct loop_arguments *largs, off_t *current_offset, const void **position, size_t *available_length);

int start_engine(struct addresses addresses) {
    int fildes[2];

    int rc = pipe(fildes);
    assert(rc == 0);

    int rd_pipe = fildes[0];
    int wr_pipe = fildes[1];

    pthread_t thread;

    long ncpus = number_of_cpus();
    assert(ncpus >= 1);
    fprintf(stderr, "Using %ld available CPUs\n", ncpus);

    for(int n = 0; n < ncpus; n++) {
        struct loop_arguments *loop_args = calloc(1, sizeof(*loop_args));
        loop_args->sample_data = "abc";
        loop_args->sample_data_size = 3;
        loop_args->addresses = addresses;
        loop_args->remote_stats = calloc(addresses.n_addrs, sizeof(loop_args->remote_stats[0]));
        loop_args->control_pipe = rd_pipe;
        loop_args->address_offset = n;
        loop_args->thread_no = n;

        int rc = pthread_create(&thread, 0,
                                single_engine_loop_thread, loop_args);
        assert(rc == 0);
    }

    return wr_pipe;
}

static void *single_engine_loop_thread(void *argp) {
    struct loop_arguments *largs = (struct loop_arguments *)argp;
    struct ev_loop *loop = ev_loop_new(
        EVFLAG_SIGNALFD | ev_recommended_backends() | EVBACKEND_KQUEUE);
    ev_set_userdata(loop, largs);

    ev_io control_watcher;
    signal(SIGPIPE, SIG_IGN);

    ev_io_init(&control_watcher, control_cb, largs->control_pipe, EV_READ);
    ev_io_start(loop, &control_watcher);

    ev_run(loop, 0);

    fprintf(stderr, "Exiting cpu thread %d\n"
            "  %d open_connections\n"
            "  %ld total_data_transmitted\n",
        largs->thread_no,
        largs->open_connections,
        largs->total_data_transmitted);

    return 0;
}

static void control_cb(EV_P_ ev_io *w, int __attribute__((unused)) revents) {
    struct loop_arguments *largs = ev_userdata(EV_A);

    char c;
    if(read(w->fd, &c, 1) != 1) {
        fprintf(stderr, "%d Reading from control channel %d returned: %s\n",
            largs->thread_no, w->fd, strerror(errno));
        return;
    }
    switch(c) {
    case 'c':
        start_new_connection(EV_A);
        break;
    case 'b':
        largs->thread_flags |= THREAD_TERMINATING;
        ev_io_stop(EV_A_ w);
        break;
    default:
        fprintf(stderr, "Unknown operation '%c' from a control channel %d\n",
            c, w->fd);
    }
}

static void start_new_connection(EV_P) {
    struct loop_arguments *largs = ev_userdata(EV_A);
    struct remote_stats *remote_stats;

    struct sockaddr *sa = pick_remote_address(largs, &remote_stats);

    remote_stats->connection_attempts++;

    int sockfd = socket(sa->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if(sockfd == -1) {
        switch(errno) {
        case ENFILE:
            fprintf(stderr, "System socket table is full (%d), consider changing ulimit -n", max_open_files());
            exit(1);
        }
        return; /* Come back later */
    }
    int rc = fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL) | O_NONBLOCK);
    assert(rc != -1);

    rc = connect(sockfd, sa, sizeof(*sa));
    if(rc == -1) {
        char buf[INET6_ADDRSTRLEN+64];
        switch(errno) {
        case EINPROGRESS:
            break;
        default:
            remote_stats->connection_failures++;
            if(remote_stats->connection_failures == 1) {
                fprintf(stderr, "Connection to %s is not done: %s\n",
                        format_sockaddr(sa, buf, sizeof(buf)), strerror(errno));
            }
            close(sockfd);
            return;
        }
    }

    struct connection *conn = calloc(1, sizeof(*conn));
    conn->remote_address = sa;
    conn->loop = EV_A;
    conn->remote_stats = remote_stats;
    largs->open_connections++;

    ev_io_init(&conn->watcher, connection_cb, sockfd, EV_WRITE);
    ev_io_start(EV_A_ &conn->watcher);
}

static struct sockaddr *pick_remote_address(struct loop_arguments *largs, struct remote_stats **remote_stats) {

    /*
     * If it is known that a particular destination is broken, choose
     * the working one right away.
     */
    int off = 0;
    for(int attempts = 0; attempts < largs->addresses.n_addrs; attempts++) {
        off = largs->address_offset++ % largs->addresses.n_addrs;
        struct remote_stats *rs = &largs->remote_stats[off];
        if(rs->connection_attempts > 10
            && rs->connection_failures == rs->connection_attempts) {
            continue;
        } else {
            break;
        }
    }

    *remote_stats = &largs->remote_stats[off];
    return &largs->addresses.addrs[off];
}

static void connection_cb(EV_P_ ev_io *w, int revents) {
    struct loop_arguments *largs = ev_userdata(EV_A);
    struct connection *conn = (struct connection *)((char *)w - offsetof(struct connection, watcher));

    if(largs->thread_flags & THREAD_TERMINATING) {
        largs->total_data_transmitted += conn->data_transmitted;
        largs->open_connections--;
        close_connection(conn);
        return;
    }

    if(revents & EV_WRITE) {
        const void *position;
        size_t available_length;
        largest_contiguous_chunk(largs, &conn->write_offset, &position, &available_length);
        ssize_t wrote = write(w->fd, position, available_length);
        if(wrote == -1) {
            char buf[INET6_ADDRSTRLEN+64];
            switch(errno) {
            case EPIPE:
                fprintf(stderr, "Connection closed by %s\n",
                    format_sockaddr(conn->remote_address, buf, sizeof(buf)));
                conn->remote_stats->connection_failures++;
                largs->total_data_transmitted += conn->data_transmitted;
                largs->open_connections--;
                close_connection(conn);
                break;
            }
        } else {
            conn->write_offset += wrote;
            conn->data_transmitted += wrote;
        }
    }
}

/*
 * Compute the largest amount of data we can send to the channel
 * using a single write() call.
 */
static void largest_contiguous_chunk(struct loop_arguments *largs, off_t *current_offset, const void **position, size_t *available_length) {

    size_t size = largs->sample_data_size;
    size_t available = size - *current_offset;
    if(available) {
        *position = largs->sample_data + *current_offset;
        *available_length = available;
    } else {
        *position = largs->sample_data;
        *available_length = size;
        *current_offset = 0;
    }
}

static void close_connection(struct connection *conn) {
    ev_io_stop(conn->loop, &conn->watcher);
    free(conn);
}

/*
 * Determine the limit on open files.
 */
static int max_open_files() {
    return sysconf(_SC_OPEN_MAX);
}


/*
 * Determine the amount of parallelism available in this system.
 */
static long number_of_cpus() {
    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);

#ifdef   HAVE_SCHED_GETAFFINITY
    cpu_set_t cs;
    CPU_ZERO(&cs);
    sched_getaffinity(0, sizeof(cs), &cs);
    ncpus = CPU_COUNT(&cs);
#endif

    return ncpus;
}
