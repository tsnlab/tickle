// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 TSN Lab, Inc.
//
// wake_cost.c - what does it cost to wake a blocked reader, by four mechanisms?
//
// WHY THIS EXISTS. COMPARISON 2.2c measured our shared-memory path at 0.058 ms against our own kernel path at
// 0.050 on a p2 round trip, pure arm against pure arm, ranges separable. Both vendors GAIN from their segments
// (CycloneDDS -41%, FastDDS -29%) because they signal a waiting reader inside shared memory - an interprocess
// condition variable, which is a futex. We send a zero-length UDP datagram (segment_doorbells_sent), so a
// segment-carried sample still costs a trip through the IP stack to wake the reader asleep on its socket.
//
// That was a hypothesis about where 8 us per round trip went. This measures it.
//
// THE ARITHMETIC, written before the first run. A round trip carries two wakes, one each way, so for the
// doorbell to account for the whole 8 us gap an alternative must save about 4 us PER WAKE - about 8 us on this
// benchmark's own round trip. The reading rule lives in wake_cost.sh, which owns the verdict.
//
// HOW THE CANDIDATES WERE CHOSEN, after getting it wrong once. The first version had three arms picked by
// argument: UDP as the control, a unix-domain datagram because it "skips the IP stack and keeps the single ppoll
// wait point", and a futex as the floor. On the rig the unix arm came out SLOWER than UDP, and the mechanism that
// won - a FIFO - was not in that run at all. The candidates are now enumerated from the constraints a replacement
// has to satisfy, and only then argued about:
//
//   nameable   the writer is a different process, so an unnamed fd would have to be passed over SCM_RIGHTS
//   pollable   the reader waits in one ppoll, and anything that cannot join that set rewrites the wait loop
//
//   udp     nameable, pollable - what we send today, the control
//   unix    nameable, pollable - the argued favourite, and slower than the control
//   fifo    nameable, pollable - and never enters the socket layer
//   futex   neither - the floor, whose price is a rewritten wait loop
//
// Usage: wake_cost <udp|unix|fifo|futex> <server|client> <iterations>
//   Run the server first; the client prints the RESULT line. Build with -D_GNU_SOURCE, for syscall().
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/futex.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>

#define UDP_SERVER_PORT 19411
#define UDP_CLIENT_PORT 19412
#define UNIX_SERVER_PATH "/tmp/wake_cost_server.sock"
#define UNIX_CLIENT_PATH "/tmp/wake_cost_client.sock"
#define FUTEX_PATH "/dev/shm/wake_cost_futex"
// A FIFO is nameable like the segment file and pollable like the socket, so it fits the reader's existing single
// ppoll wait point, and unlike a unix datagram it does not enter the socket layer at all.
#define FIFO_TO_SERVER "/tmp/wake_cost_fifo_s"
#define FIFO_TO_CLIENT "/tmp/wake_cost_fifo_c"

#define OWNER_RW_MODE 0600
#define NS_PER_SEC 1000000000ULL
#define NS_PER_US 1000.0
#define TAIL_PERCENTILE 99
#define PERCENT 100
#define EXPECTED_ARGS 4

static uint64_t now_ns(void) {
    struct timespec spec;
    clock_gettime(CLOCK_MONOTONIC, &spec); // NOLINT(misc-include-cleaner) - <time.h>, via its own internal header
    return ((uint64_t)spec.tv_sec * NS_PER_SEC) + (uint64_t)spec.tv_nsec;
}

static int cmp_u64(const void* lhs, const void* rhs) {
    uint64_t left = *(const uint64_t*)lhs;
    uint64_t right = *(const uint64_t*)rhs;
    return (left > right) - (left < right);
}

// Every client arm ends here, so the four print one shape and the harness has one line to parse.
static void report(const char* mech, uint64_t* samples, long iters) {
    qsort(samples, (size_t)iters, sizeof(uint64_t), cmp_u64);
    double sum = 0;
    for (long idx = 0; idx < iters; idx++) {
        sum += (double)samples[idx];
    }
    long mid = iters / 2;
    long tail = (iters * TAIL_PERCENTILE) / PERCENT;
    printf("RESULT: role=client mechanism=%s n=%ld rtt_mean_us=%.3f rtt_p50_us=%.3f rtt_min_us=%.3f "
           "rtt_p99_us=%.3f\n",
           mech, iters, sum / (double)iters / NS_PER_US, (double)samples[mid] / NS_PER_US,
           (double)samples[0] / NS_PER_US, (double)samples[tail] / NS_PER_US);
}

// Two words, one per direction, so neither side can consume the wake it just sent.
struct futex_pair {
    uint32_t to_server;
    uint32_t to_client;
};

static int futex_wait(uint32_t* addr, uint32_t expected) {
    return (int)syscall(SYS_futex, addr, FUTEX_WAIT, expected, NULL, NULL, 0);
}

static int futex_wake(uint32_t* addr) {
    return (int)syscall(SYS_futex, addr, FUTEX_WAKE, 1, NULL, NULL, 0);
}

static struct futex_pair* futex_map(bool create) {
    int fdesc = open(FUTEX_PATH, create ? (O_CREAT | O_RDWR | O_TRUNC) : O_RDWR, OWNER_RW_MODE);
    if (fdesc < 0) {
        perror("futex open");
        return NULL;
    }
    if (create && ftruncate(fdesc, sizeof(struct futex_pair)) != 0) {
        perror("ftruncate");
        close(fdesc);
        return NULL;
    }
    void* mapping = mmap(NULL, sizeof(struct futex_pair), PROT_READ | PROT_WRITE, MAP_SHARED, fdesc, 0);
    close(fdesc);
    if (mapping == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    return (struct futex_pair*)mapping;
}

// FUTEX_WAIT returns at once when the value already moved, which is what makes the handshake safe with no lock -
// and also why this arm's minimum is far below the others'. An iteration whose peer is already running costs no
// syscall at all, which a reader parked in ppoll could never reproduce, so the futex figure flatters itself by an
// amount this benchmark does not separate.
static void futex_spin(uint32_t* word, uint32_t already) {
    uint32_t seen = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    while (seen == already) {
        futex_wait(word, seen);
        seen = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    }
}

static int run_futex(bool server, long iters) {
    struct futex_pair* pair = futex_map(server);
    if (pair == NULL) {
        return 1;
    }
    if (server) {
        for (long idx = 0; idx < iters; idx++) {
            futex_spin(&pair->to_server, (uint32_t)idx);
            __atomic_store_n(&pair->to_client, (uint32_t)(idx + 1), __ATOMIC_RELEASE);
            futex_wake(&pair->to_client);
        }
        printf("RESULT: role=server mechanism=futex n=%ld\n", iters);
        return 0;
    }
    uint64_t* samples = calloc((size_t)iters, sizeof(uint64_t));
    if (samples == NULL) {
        return 1;
    }
    for (long idx = 0; idx < iters; idx++) {
        uint64_t start = now_ns();
        __atomic_store_n(&pair->to_server, (uint32_t)(idx + 1), __ATOMIC_RELEASE);
        futex_wake(&pair->to_server);
        futex_spin(&pair->to_client, (uint32_t)idx);
        samples[idx] = now_ns() - start;
    }
    report("futex", samples, iters);
    free(samples);
    return 0;
}

static int run_fifo(bool server, long iters) {
    // Created by whichever side gets there first; open() then blocks until the peer opens the other end, which is
    // the rendezvous, so neither side has to sleep and hope.
    mkfifo(FIFO_TO_SERVER, OWNER_RW_MODE);
    mkfifo(FIFO_TO_CLIENT, OWNER_RW_MODE);
    int rfd = -1;
    int wfd = -1;
    if (server) {
        rfd = open(FIFO_TO_SERVER, O_RDONLY);
        wfd = open(FIFO_TO_CLIENT, O_WRONLY);
    } else {
        wfd = open(FIFO_TO_SERVER, O_WRONLY);
        rfd = open(FIFO_TO_CLIENT, O_RDONLY);
    }
    if (rfd < 0 || wfd < 0) {
        perror("fifo open");
        return 1;
    }
    char one = 'x';
    if (server) {
        for (long idx = 0; idx < iters; idx++) {
            if (read(rfd, &one, 1) != 1 || write(wfd, &one, 1) != 1) {
                perror("fifo server");
                return 1;
            }
        }
        printf("RESULT: role=server mechanism=fifo n=%ld\n", iters);
        return 0;
    }
    uint64_t* samples = calloc((size_t)iters, sizeof(uint64_t));
    if (samples == NULL) {
        return 1;
    }
    for (long idx = 0; idx < iters; idx++) {
        uint64_t start = now_ns();
        if (write(wfd, &one, 1) != 1 || read(rfd, &one, 1) != 1) {
            perror("fifo client");
            return 1;
        }
        samples[idx] = now_ns() - start;
    }
    report("fifo", samples, iters);
    free(samples);
    return 0;
}

// Bound to `self`. The CLIENT also connects to `peer`, so its send needs no address; the SERVER does not, because
// when it starts the client has not bound yet and connect() would fail with ENOENT - which is what the first
// version of this did. The server learns its peer from the first datagram instead.
static int dgram_socket(int family, bool do_connect, const char* self_path, const char* peer_path, uint16_t self_port,
                        uint16_t peer_port) {
    int sock = socket(family, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    if (family == AF_UNIX) {
        struct sockaddr_un own = {.sun_family = AF_UNIX};
        unlink(self_path);
        strncpy(own.sun_path, self_path, sizeof(own.sun_path) - 1);
        if (bind(sock, (struct sockaddr*)&own, sizeof(own)) != 0) {
            perror("bind unix");
            close(sock);
            return -1;
        }
        if (do_connect) {
            struct sockaddr_un dst = {.sun_family = AF_UNIX};
            strncpy(dst.sun_path, peer_path, sizeof(dst.sun_path) - 1);
            // connect() on a datagram socket only sets the default peer; it does not make it a stream.
            if (connect(sock, (struct sockaddr*)&dst, sizeof(dst)) != 0) {
                perror("connect unix");
                close(sock);
                return -1;
            }
        }
        return sock;
    }
    struct sockaddr_in own = {.sin_family = AF_INET, .sin_port = htons(self_port)};
    own.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(sock, (struct sockaddr*)&own, sizeof(own)) != 0) {
        perror("bind udp");
        close(sock);
        return -1;
    }
    if (do_connect) {
        struct sockaddr_in dst = {.sin_family = AF_INET, .sin_port = htons(peer_port)};
        dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(sock, (struct sockaddr*)&dst, sizeof(dst)) != 0) {
            perror("connect udp");
            close(sock);
            return -1;
        }
    }
    return sock;
}

static int run_dgram(const char* mech, bool is_unix, bool server, long iters) {
    int sock = dgram_socket(is_unix ? AF_UNIX : AF_INET, !server, server ? UNIX_SERVER_PATH : UNIX_CLIENT_PATH,
                            server ? UNIX_CLIENT_PATH : UNIX_SERVER_PATH, server ? UDP_SERVER_PORT : UDP_CLIENT_PORT,
                            server ? UDP_CLIENT_PORT : UDP_SERVER_PORT);
    if (sock < 0) {
        return 1;
    }
    char buf[8];
    if (server) {
        struct sockaddr_storage peer;
        for (long idx = 0; idx < iters; idx++) {
            socklen_t len = sizeof(peer);
            if (recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&peer, &len) < 0 && errno != EINTR) {
                perror("recvfrom");
                return 1;
            }
            if (sendto(sock, buf, 0, 0, (struct sockaddr*)&peer, len) < 0) {
                perror("sendto");
                return 1;
            }
        }
        printf("RESULT: role=server mechanism=%s n=%ld\n", mech, iters);
        return 0;
    }
    uint64_t* samples = calloc((size_t)iters, sizeof(uint64_t));
    if (samples == NULL) {
        return 1;
    }
    for (long idx = 0; idx < iters; idx++) {
        uint64_t start = now_ns();
        if (send(sock, buf, 0, 0) < 0) {
            perror("send");
            return 1;
        }
        if (recv(sock, buf, sizeof(buf), 0) < 0 && errno != EINTR) {
            perror("recv");
            return 1;
        }
        samples[idx] = now_ns() - start;
    }
    report(mech, samples, iters);
    free(samples);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != EXPECTED_ARGS) {
        fprintf(stderr, "usage: %s <udp|unix|fifo|futex> <server|client> <iterations>\n", argv[0]);
        return 2;
    }
    const char* mech = argv[1];
    bool server = strcmp(argv[2], "server") == 0;
    long iters = atol(argv[3]);
    if (iters < 1) {
        fprintf(stderr, "iterations must be >= 1\n");
        return 2;
    }
    if (strcmp(mech, "futex") == 0) {
        return run_futex(server, iters);
    }
    if (strcmp(mech, "fifo") == 0) {
        return run_fifo(server, iters);
    }
    bool is_unix = strcmp(mech, "unix") == 0;
    if (!is_unix && strcmp(mech, "udp") != 0) {
        fprintf(stderr, "mechanism must be udp, unix, fifo or futex\n");
        return 2;
    }
    return run_dgram(mech, is_unix, server, iters);
}
