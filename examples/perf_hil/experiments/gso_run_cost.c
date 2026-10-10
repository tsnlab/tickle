// gso_run_cost.c - what one UDP_SEGMENT send of a run of n datagrams costs the sender, against the same n datagrams as
// n messages of one sendmmsg() (hal_linux.c send_batch_flags() without send offload), in instructions (user + kernel)
// counted by perf_event_open() around the send loop only. gso_run_cost.sh runs it in a private namespace whose veth
// cannot cut a run itself (tx-udp-segmentation off), so the kernel segments each run in software before the device -
// what the Pi 5's macb does (tx-udp-segmentation: off [fixed], ~/rig_results_safe/gro_probe_20261010-173941.txt).
//
// The model it feeds (hal_linux.c, "Which runs go as one"): cost(plain, n) = n x p + c and cost(gso, n) = n x s + F,
// so a run pays once n > (F - c) / (p - s). The fixed parts and the per-datagram parts are read off the straight
// lines through the n sweep, not tuned to a rig.
//
// Usage: gso_run_cost <dst ip> <port> <datagram bytes> <runs per n> n1 [n2 ...]
// Prints two lines per n: "n=<n> mode=<plain|gso> runs=<r> insns_per_datagram=<x> insns_per_run=<y>".
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming)
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <linux/perf_event.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif

#define MAX_RUN 64
#define WIRE_MAX 1472          // never more on the wire (DESIGN.md section 8)
#define GSO_MAX_BYTES 65507    // one IPv4 UDP datagram's payload, which a run is cut from
#define WARMUP_RUNS 200        // route, neighbour, socket memory: outside the count
#define SNDBUF_BYTES (4 << 20) // so a sweep never waits on the send buffer
#define FILL_BYTE 0xa5
#define FIRST_N_ARG 5
#define ARG_PORT 2
#define ARG_SIZE 3
#define ARG_RUNS 4

static int counter_open(void) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof(attr);
    attr.config = PERF_COUNT_HW_INSTRUCTIONS;
    attr.disabled = 1;
    attr.exclude_kernel = 0;
    attr.exclude_hv = 1;
    return (int)syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0);
}

static uint64_t counter_read(int counter) {
    uint64_t value = 0;
    if (read(counter, &value, sizeof(value)) != (ssize_t)sizeof(value)) {
        return 0;
    }
    return value;
}

static uint8_t g_payload[MAX_RUN][WIRE_MAX];

// One run of `count` datagrams: `count` messages (plain) or one message with the UDP_SEGMENT cmsg (gso). -1 on error.
static int send_run(int sock, const struct sockaddr_in* dst, size_t size, int count, int gso) {
    struct mmsghdr msgs[MAX_RUN];
    struct iovec iov[MAX_RUN]; // NOLINT(misc-include-cleaner) - struct iovec: <sys/socket.h> brings <sys/uio.h>'s
    union {
        char bytes[CMSG_SPACE(sizeof(uint16_t))];
        struct cmsghdr align;
    } control;
    memset(msgs, 0, sizeof(msgs));
    for (int k = 0; k < count; k++) {
        iov[k].iov_base = g_payload[k];
        iov[k].iov_len = size;
    }
    if (!gso) {
        for (int k = 0; k < count; k++) {
            msgs[k].msg_hdr.msg_name = (void*)dst;
            msgs[k].msg_hdr.msg_namelen = sizeof(*dst);
            msgs[k].msg_hdr.msg_iov = &iov[k];
            msgs[k].msg_hdr.msg_iovlen = 1;
        }
        int sent = 0;
        while (sent < count) {
            int result = sendmmsg(sock, msgs + sent, (unsigned)(count - sent), 0);
            if (result <= 0) {
                return -1;
            }
            sent += result;
        }
        return 0;
    }
    msgs[0].msg_hdr.msg_name = (void*)dst;
    msgs[0].msg_hdr.msg_namelen = sizeof(*dst);
    msgs[0].msg_hdr.msg_iov = iov;
    msgs[0].msg_hdr.msg_iovlen = (size_t)count;
    if (count > 1) {
        uint16_t segment = (uint16_t)size;
        msgs[0].msg_hdr.msg_control = control.bytes;
        msgs[0].msg_hdr.msg_controllen = sizeof(control.bytes);
        struct cmsghdr* header = CMSG_FIRSTHDR(&msgs[0].msg_hdr);
        header->cmsg_level = SOL_UDP;
        header->cmsg_type = UDP_SEGMENT;
        header->cmsg_len = CMSG_LEN(sizeof(segment));
        memcpy(CMSG_DATA(header), &segment, sizeof(segment));
    }
    return sendmmsg(sock, msgs, 1, 0) == 1 ? 0 : -1;
}

// Both modes for one run length: a warm-up, then `runs` runs inside the counter. 0, or 1 on a send error.
static int measure(int sock, int counter, const struct sockaddr_in* dst, size_t size, long runs, int count) {
    for (int gso = 0; gso <= 1; gso++) {
        for (long run = 0; run < WARMUP_RUNS + runs; run++) {
            if (run == WARMUP_RUNS) {
                ioctl(counter, PERF_EVENT_IOC_RESET, 0);
                ioctl(counter, PERF_EVENT_IOC_ENABLE, 0);
            }
            if (send_run(sock, dst, size, count, gso) != 0) {
                perror("send");
                return 1;
            }
        }
        ioctl(counter, PERF_EVENT_IOC_DISABLE, 0);
        uint64_t insns = counter_read(counter);
        printf("n=%d mode=%s runs=%ld insns_per_datagram=%.1f insns_per_run=%.1f\n", count, gso ? "gso" : "plain", runs,
               (double)insns / (double)(runs * count), (double)insns / (double)runs);
        fflush(stdout);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc <= FIRST_N_ARG) {
        fprintf(stderr, "usage: %s <dst ip> <port> <datagram bytes> <runs per n> n1 [n2 ...]\n", argv[0]);
        return 2;
    }
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)atoi(argv[ARG_PORT]));
    if (inet_pton(AF_INET, argv[1], &dst.sin_addr) != 1) {
        fprintf(stderr, "bad address %s\n", argv[1]);
        return 2;
    }
    size_t size = (size_t)atoi(argv[ARG_SIZE]);
    long runs = atol(argv[ARG_RUNS]);
    if (size == 0 || size > WIRE_MAX || runs <= 0) {
        fprintf(stderr, "datagram bytes 1..%d (never more on the wire), runs > 0\n", WIRE_MAX);
        return 2;
    }
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int sndbuf = SNDBUF_BYTES;
    // NOLINTNEXTLINE(misc-include-cleaner) - SOL_SOCKET, SO_SNDBUF: <sys/socket.h>'s glibc-private headers
    (void)setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    memset(g_payload, FILL_BYTE, sizeof(g_payload));
    int counter = counter_open();
    if (counter < 0) {
        perror("perf_event_open");
        return 1;
    }
    int failed = 0;
    for (int arg = FIRST_N_ARG; arg < argc && !failed; arg++) {
        int count = atoi(argv[arg]);
        if (count < 1 || count > MAX_RUN || (size_t)count * size > GSO_MAX_BYTES) {
            fprintf(stderr, "skipping n=%d\n", count);
            continue;
        }
        failed = measure(sock, counter, &dst, size, runs, count);
    }
    close(counter);
    close(sock);
    return failed;
}
