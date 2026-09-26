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

#include <netinet/in.h>
#include <sys/eventfd.h>
#include <sys/socket.h>

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

enum wait_style { WAIT_PPOLL3, WAIT_RECVFROM, WAIT_RECVMSG };

struct shared {
    int socket_fd;
    int other_fd; // a second, idle socket, as the well-known socket is to the data socket
    int event_fd; // an idle eventfd, as the wake fd is
    enum wait_style style;
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
static ssize_t receive_one(const struct shared* ctx, uint8_t* buf, size_t len) {
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
    uint8_t buf[DATAGRAM_BYTES];
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
    printf("%-9s send -> receive returned: median %6.2f us  p10 %6.2f  p90 %6.2f\n", name, median_us, p10_us, p90_us);
    close(ctx.socket_fd);
    close(ctx.other_fd);
    close(ctx.event_fd);
    close(sender);
}

int main(int argc, char** argv) {
    int rx_cpu = argc > 1 ? atoi(argv[1]) : DEFAULT_RX_CPU;
    int tx_cpu = argc > 2 ? atoi(argv[2]) : DEFAULT_TX_CPU;
    run(WAIT_RECVFROM, "recvfrom", rx_cpu, tx_cpu);
    run(WAIT_PPOLL3, "ppoll3", rx_cpu, tx_cpu);
    run(WAIT_RECVMSG, "recvmsg", rx_cpu, tx_cpu);
    run(WAIT_PPOLL3, "ppoll3", rx_cpu, tx_cpu); // twice, interleaved: drift control
    run(WAIT_RECVFROM, "recvfrom", rx_cpu, tx_cpu);
    return 0;
}
