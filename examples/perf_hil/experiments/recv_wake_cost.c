// What waking up for a datagram costs, by how the receiver waits (2026-09-26, RMW_PERF_PLAN §8.1). On the rig
// the pong's kernel receive - tap to the receive call returning - was 18.2 us for rmw_tickle against 12.6 for
// CycloneDDS: 13.8 us to ppoll() returning, then 4.4 for the recvfrom(). CycloneDDS's receive thread blocks
// in recvmsg() on its data socket instead. This measures, on one host, a datagram's send-to-return time for
// a receiver thread waiting three ways:
//   ppoll3    ppoll() on three descriptors (a second socket and an eventfd, as tt_receive() watches), then
//             recvfrom() - rmw_tickle's poll thread today
//   recvfrom  a blocking recvfrom() on the socket alone
//   recvmsg   a blocking recvmsg() on the socket alone - CycloneDDS's recvUC thread
// The sender stamps CLOCK_MONOTONIC into the datagram just before sendto(); the receiver takes the time when
// its receive call returns. The two threads are pinned to different CPUs, and the sender waits between
// datagrams so the receiver is asleep each time, as a pong is between pings.
//
// HOW TO READ IT, written before running: ppoll3 against recvfrom is what replacing the wait would buy,
// as a relative figure (loopback and x86 are not the rig's NIC and Cortex-A76). recvfrom and recvmsg are
// expected to agree - the control. If ppoll3 is within ~1 us of recvfrom here, the wait style is not what
// costs the rig's 5.6 us, and bpftrace has to find it; if it is several us slower, the design is worth
// prototyping for the rig.
//
// Seen on x86 (2026-09-26, rx CPU 2, tx CPU 4, loopback, medians of 20000): recvfrom 11.02-11.04 us, recvmsg
// 11.24, ppoll3 11.83-11.86 - ppoll3 +0.8 us (+7%), the control pair agreeing within 0.2.
//
// epoll3 (added 2026-09-26): the same three descriptors registered once in an epoll set, epoll_wait() then
// recvfrom() - whether keeping the registration across waits, instead of ppoll()'s per-call setup, recovers
// the wait's cost. Seen on x86 (same CPUs, 20000 each, twice interleaved): recvfrom 11.04 / 11.14 us,
// ppoll3 11.92 / 12.06, epoll3 11.80 / 12.03 - no gain over ppoll3 here; the Pi, where ppoll3 cost +2.8 us,
// decides.
//
// uring and uring_dt (added 2026-10-20, ROADMAP "rmw kernel wake"): the wait and the read in one system call. Per
// datagram one IORING_OP_RECV is submitted and waited for in the same io_uring_enter(to_submit 1, min_complete 1,
// GETEVENTS); the idle socket and the eventfd each hold an IORING_OP_POLL_ADD that never completes, as the
// well-known socket and the wake fd would. uring_dt sets the ring up with IORING_SETUP_DEFER_TASKRUN (and
// SINGLE_ISSUER, which it needs), so the copy runs in the waiting thread's own io_uring_enter(). Each style also
// reports the receiving thread's CPU time per datagram (CLOCK_THREAD_CPUTIME_ID over the run): a wake that costs
// less latency but more CPU is not a gain for every rmw cell.
//
// HOW TO READ uring, written before its first run (2026-10-20): against epoll3 (TickLE's wait today: a kept set,
// then recvfrom), a median at least 0.3 us lower on the PC, beyond the spread of each style's two interleaved passes,
// and CPU per datagram not higher, makes "wait and read in one io_uring_enter()" worth a HAL prototype; within the
// spread, the second system call is not where the PC's time goes and the rig (Pi, where recvfrom after a wait cost
// +2.35 us, 8.5) has to decide. Against recvfrom (Fast DDS's wait), it says how much of the gap one call closes.
//
// Seen on x86 (2026-10-20, in a private netns's loopback, rx CPU 2, tx CPU 4, 20000 each, two passes of two
// interleaved runs; the medians moved by up to 1.3 us between runs of one style, so the p10s are read): p10 recvfrom
// 10.20-10.31 us, recvmsg 10.34-10.35, epoll3 10.94-11.03, ppoll3 11.03-11.20, uring 10.78-10.87, uring_dt
// 10.52-10.66. Receiving CPU per datagram: recvfrom 3.82-4.82 us, epoll3 4.46-4.96, uring_dt 4.08-4.38. So the
// wait-then-read costs ~0.7 us over a blocking read on this PC; one io_uring_enter() with DEFER_TASKRUN takes back
// ~0.4 of it, the plain ring ~0.15. The bar above (0.3 us) is met by uring_dt only, and only as a p10.
//
// Build: cc -O2 -pthread -o /tmp/recv_wake_cost recv_wake_cost.c   Run: /tmp/recv_wake_cost [rx_cpu tx_cpu]
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming)
#define _GNU_SOURCE
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/io_uring.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
// __NR_io_uring_*: <sys/syscall.h> is the portable spelling (hal_linux.c); the numbers live in an architecture header.
#include <sys/syscall.h> // NOLINT(misc-include-cleaner)

#define ROUNDS 20000
#define NS_PER_SEC 1000000000ULL
#define GAP_NS 200000L // between datagrams: long enough that the receiver has gone back to sleep
#define PCT_10 10
#define PCT_90 90
#define PCT_DIV 100
#define NS_PER_US 1e3
#define P10_INDEX (ROUNDS * PCT_10 / PCT_DIV)
#define P50_INDEX (ROUNDS / 2)
#define P90_INDEX (ROUNDS * PCT_90 / PCT_DIV)
#define DEFAULT_RX_CPU 2
#define DEFAULT_TX_CPU 4
#define DATAGRAM_BYTES 76

enum wait_style { WAIT_PPOLL3, WAIT_RECVFROM, WAIT_RECVMSG, WAIT_EPOLL3, WAIT_URING, WAIT_URING_DT };

// The smallest io_uring there is, without liburing: one ring, mapped, used by the receiving thread alone.
struct uring {
    int fd;
    uint32_t* sq_tail;
    uint32_t* sq_mask;
    uint32_t* sq_array;
    uint32_t* cq_head;
    uint32_t* cq_tail;
    uint32_t* cq_mask;
    struct io_uring_cqe* cqes;
    struct io_uring_sqe* sqes;
};

#define URING_ENTRIES 8U
#define URING_DATA_RECV 1U

static int uring_setup(struct uring* ring, unsigned flags) {
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags = flags;
    ring->fd = (int)syscall(__NR_io_uring_setup, URING_ENTRIES, &params); // NOLINT(misc-include-cleaner) - above
    if (ring->fd < 0) {
        return -1;
    }
    size_t sq_len = params.sq_off.array + (params.sq_entries * sizeof(uint32_t));
    size_t cq_len = params.cq_off.cqes + (params.cq_entries * sizeof(struct io_uring_cqe));
    uint8_t* sq_map =
        mmap(NULL, sq_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring->fd, IORING_OFF_SQ_RING);
    uint8_t* cq_map =
        mmap(NULL, cq_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring->fd, IORING_OFF_CQ_RING);
    ring->sqes = mmap(NULL, params.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE, ring->fd, IORING_OFF_SQES);
    if (sq_map == MAP_FAILED || cq_map == MAP_FAILED || ring->sqes == MAP_FAILED) {
        return -1;
    }
    ring->sq_tail = (uint32_t*)(sq_map + params.sq_off.tail);
    ring->sq_mask = (uint32_t*)(sq_map + params.sq_off.ring_mask);
    ring->sq_array = (uint32_t*)(sq_map + params.sq_off.array);
    ring->cq_head = (uint32_t*)(cq_map + params.cq_off.head);
    ring->cq_tail = (uint32_t*)(cq_map + params.cq_off.tail);
    ring->cq_mask = (uint32_t*)(cq_map + params.cq_off.ring_mask);
    ring->cqes = (struct io_uring_cqe*)(cq_map + params.cq_off.cqes);
    return 0;
}

static struct io_uring_sqe* uring_sqe(struct uring* ring) {
    uint32_t tail = *ring->sq_tail;
    uint32_t index = tail & *ring->sq_mask;
    struct io_uring_sqe* sqe = &ring->sqes[index];
    memset(sqe, 0, sizeof(*sqe));
    ring->sq_array[index] = index;
    __atomic_store_n(ring->sq_tail, tail + 1, __ATOMIC_RELEASE);
    return sqe;
}

static long uring_enter(const struct uring* ring, unsigned submit, unsigned wait) {
    // NOLINTNEXTLINE(misc-include-cleaner) - __NR_io_uring_enter: <sys/syscall.h>, above
    return syscall(__NR_io_uring_enter, ring->fd, submit, wait, wait > 0 ? IORING_ENTER_GETEVENTS : 0U, NULL, 0);
}

struct shared {
    int socket_fd;
    int other_fd;      // a second, idle socket, as the well-known socket is to the data socket
    int event_fd;      // an idle eventfd, as the wake fd is
    int epoll_fd;      // the three above registered once (epoll3)
    struct uring ring; // uring, uring_dt: set up by the receiving thread itself
    enum wait_style style;
    uint64_t cpu_ns; // the receiving thread's CPU time over the run
    int cpu;
    uint64_t samples[ROUNDS];
};

static uint64_t now_ns(void) {
    struct timespec now;
    // NOLINTNEXTLINE(misc-include-cleaner) - CLOCK_MONOTONIC comes from <time.h> above
    clock_gettime(CLOCK_MONOTONIC, &now);
    return ((uint64_t)now.tv_sec * NS_PER_SEC) + (uint64_t)now.tv_nsec;
}

static void pin(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

static int compare_u64(const void* left, const void* right) {
    uint64_t lhs = *(const uint64_t*)left;
    uint64_t rhs = *(const uint64_t*)right;
    return (lhs > rhs) - (lhs < rhs);
}

// One blocking receive in the given style; the datagram lands in buf.
static ssize_t receive_one(struct shared* ctx, uint8_t* buf, size_t len) {
    if (ctx->style == WAIT_URING || ctx->style == WAIT_URING_DT) {
        struct io_uring_sqe* sqe = uring_sqe(&ctx->ring);
        sqe->opcode = IORING_OP_RECV;
        sqe->fd = ctx->socket_fd;
        sqe->addr = (uint64_t)(uintptr_t)buf;
        sqe->len = (uint32_t)len;
        sqe->user_data = URING_DATA_RECV;
        if (uring_enter(&ctx->ring, 1, 1) < 0) {
            return -1;
        }
        uint32_t head = *ctx->ring.cq_head;
        if (head == __atomic_load_n(ctx->ring.cq_tail, __ATOMIC_ACQUIRE)) {
            return -1;
        }
        const struct io_uring_cqe* cqe = &ctx->ring.cqes[head & *ctx->ring.cq_mask];
        ssize_t got = cqe->user_data == URING_DATA_RECV ? cqe->res : -1;
        __atomic_store_n(ctx->ring.cq_head, head + 1, __ATOMIC_RELEASE);
        return got;
    }
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    if (ctx->style == WAIT_PPOLL3) {
        // NOLINTNEXTLINE(misc-include-cleaner) - struct pollfd/POLLIN/ppoll come from <poll.h> above
        struct pollfd fds[3] = {{.fd = ctx->other_fd, .events = POLLIN, .revents = 0},
                                {.fd = ctx->event_fd, .events = POLLIN, .revents = 0},
                                {.fd = ctx->socket_fd, .events = POLLIN, .revents = 0}};
        if (ppoll(fds, 3, NULL, NULL) <= 0) { // NOLINT(misc-include-cleaner) - <poll.h>
            return -1;
        }
        return recvfrom(ctx->socket_fd, buf, len, MSG_DONTWAIT, (struct sockaddr*)&from, &from_len);
    }
    if (ctx->style == WAIT_EPOLL3) {
        struct epoll_event ready;
        if (epoll_wait(ctx->epoll_fd, &ready, 1, -1) <= 0) {
            return -1;
        }
        return recvfrom(ctx->socket_fd, buf, len, MSG_DONTWAIT, (struct sockaddr*)&from, &from_len);
    }
    if (ctx->style == WAIT_RECVFROM) {
        return recvfrom(ctx->socket_fd, buf, len, 0, (struct sockaddr*)&from, &from_len);
    }
    struct iovec iov = {.iov_base = buf, .iov_len = len}; // NOLINT(misc-include-cleaner) - <sys/socket.h>
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &from;
    msg.msg_namelen = sizeof(from);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    return recvmsg(ctx->socket_fd, &msg, 0);
}

static void* receiver(void* arg) {
    struct shared* ctx = arg;
    pin(ctx->cpu);
    if (ctx->style == WAIT_URING || ctx->style == WAIT_URING_DT) {
        unsigned flags = ctx->style == WAIT_URING_DT ? (IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN) : 0U;
        if (uring_setup(&ctx->ring, flags) != 0) {
            fprintf(stderr, "io_uring_setup refused\n");
            exit(1);
        }
        const int idle[2] = {ctx->other_fd, ctx->event_fd}; // armed once, never complete
        for (int i = 0; i < 2; i++) {
            struct io_uring_sqe* sqe = uring_sqe(&ctx->ring);
            sqe->opcode = IORING_OP_POLL_ADD;
            sqe->fd = idle[i];
            sqe->poll32_events = POLLIN;
            sqe->user_data = 2U + (unsigned)i;
        }
        if (uring_enter(&ctx->ring, 2, 0) != 2) {
            fprintf(stderr, "io_uring poll submission refused\n");
            exit(1);
        }
    }
    uint8_t buf[DATAGRAM_BYTES];
    struct timespec cpu_start;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_start); // NOLINT(misc-include-cleaner) - <time.h>
    for (int round = 0; round < ROUNDS; round++) {
        if (receive_one(ctx, buf, sizeof(buf)) < (ssize_t)sizeof(uint64_t)) {
            fprintf(stderr, "receive failed at round %d\n", round);
            exit(1);
        }
        uint64_t returned = now_ns();
        uint64_t sent = 0;
        memcpy(&sent, buf, sizeof(sent));
        ctx->samples[round] = returned - sent;
    }
    struct timespec cpu_end;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_end); // NOLINT(misc-include-cleaner) - <time.h>
    ctx->cpu_ns = (((uint64_t)cpu_end.tv_sec - (uint64_t)cpu_start.tv_sec) * NS_PER_SEC) + (uint64_t)cpu_end.tv_nsec -
                  (uint64_t)cpu_start.tv_nsec;
    if (ctx->style == WAIT_URING || ctx->style == WAIT_URING_DT) {
        close(ctx->ring.fd); // the polls and the mappings go with it at exit
    }
    return NULL;
}

static void run(enum wait_style style, const char* name, int rx_cpu, int tx_cpu) {
    static struct shared ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    ctx.other_fd = socket(AF_INET, SOCK_DGRAM, 0);
    ctx.event_fd = eventfd(0, EFD_NONBLOCK);
    ctx.style = style;
    ctx.cpu = rx_cpu;
    ctx.epoll_fd = epoll_create1(0);
    const int watched[3] = {ctx.other_fd, ctx.event_fd, ctx.socket_fd};
    for (int i = 0; i < 3; i++) {
        struct epoll_event interest = {.events = EPOLLIN, .data = {.fd = watched[i]}};
        epoll_ctl(ctx.epoll_fd, EPOLL_CTL_ADD, watched[i], &interest);
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = __builtin_bswap32(INADDR_LOOPBACK); // htonl() on a little-endian host
    if (bind(ctx.socket_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        exit(1);
    }
    socklen_t len = sizeof(addr);
    getsockname(ctx.socket_fd, (struct sockaddr*)&addr, &len);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);

    pthread_t thread; // NOLINT(misc-include-cleaner) - <pthread.h> above
    pthread_create(&thread, NULL, receiver, &ctx);
    pin(tx_cpu);
    const struct timespec gap = {0, GAP_NS};
    nanosleep(&gap, NULL);
    uint8_t payload[DATAGRAM_BYTES] = {0};
    for (int round = 0; round < ROUNDS; round++) {
        nanosleep(&gap, NULL);
        uint64_t sent = now_ns();
        memcpy(payload, &sent, sizeof(sent));
        sendto(sender, payload, sizeof(payload), 0, (const struct sockaddr*)&addr, sizeof(addr));
    }
    pthread_join(thread, NULL);
    qsort(ctx.samples, ROUNDS, sizeof(ctx.samples[0]), compare_u64);
    const size_t p50 = P50_INDEX;
    const size_t p10 = P10_INDEX;
    const size_t p90 = P90_INDEX;
    const double median_us = (double)ctx.samples[p50] / NS_PER_US;
    const double p10_us = (double)ctx.samples[p10] / NS_PER_US;
    const double p90_us = (double)ctx.samples[p90] / NS_PER_US;
    printf("%-9s send -> receive returned: median %6.2f us  p10 %6.2f  p90 %6.2f  rx cpu %5.2f us/datagram\n", name,
           median_us, p10_us, p90_us, (double)ctx.cpu_ns / ROUNDS / NS_PER_US);
    close(ctx.socket_fd);
    close(ctx.other_fd);
    close(ctx.event_fd);
    close(ctx.epoll_fd);
    close(sender);
}

int main(int argc, char** argv) {
    int rx_cpu = argc > 1 ? atoi(argv[1]) : DEFAULT_RX_CPU;
    int tx_cpu = argc > 2 ? atoi(argv[2]) : DEFAULT_TX_CPU;
    run(WAIT_RECVFROM, "recvfrom", rx_cpu, tx_cpu);
    run(WAIT_PPOLL3, "ppoll3", rx_cpu, tx_cpu);
    run(WAIT_EPOLL3, "epoll3", rx_cpu, tx_cpu);
    run(WAIT_URING, "uring", rx_cpu, tx_cpu);
    run(WAIT_URING_DT, "uring_dt", rx_cpu, tx_cpu);
    run(WAIT_RECVMSG, "recvmsg", rx_cpu, tx_cpu);
    run(WAIT_URING_DT, "uring_dt", rx_cpu, tx_cpu);
    run(WAIT_URING, "uring", rx_cpu, tx_cpu);
    run(WAIT_EPOLL3, "epoll3", rx_cpu, tx_cpu);
    run(WAIT_PPOLL3, "ppoll3", rx_cpu, tx_cpu); // twice each, interleaved: drift control
    run(WAIT_RECVFROM, "recvfrom", rx_cpu, tx_cpu);
    return 0;
}
