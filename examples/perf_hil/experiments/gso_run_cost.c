// gso_run_cost.c - what one UDP_SEGMENT send of a run of n datagrams costs the sender, against the same n datagrams as
// n messages of one sendmmsg() (hal_linux.c send_batch_flags() without send offload), in instructions (user + kernel)
// counted by perf_event_open() around the send loop only. gso_run_cost.sh runs it in a private namespace whose veth
// cannot cut a run itself (tx-udp-segmentation off), so the kernel segments each run in software before the device -
// what the Pi 5's macb does (tx-udp-segmentation: off [fixed], ~/rig_results_safe/gro_probe_20261010-173941.txt).
//
// The model it feeds (hal_linux.c, TT_GSO_MIN_SEGMENTS): cost(plain, n) = n x p and cost(gso, n) = F + n x s, so a
// run pays only once n > F / (p - s). The fixed part F (building the large skb, the segmentation entry, the segment
// list) and the per-datagram parts p and s are read off the straight lines through the n sweep, not tuned to a rig.
//
// Usage: gso_run_cost <dst ip> <port> <datagram bytes> <runs per n> n1 [n2 ...]
// Prints one line per n: "n=<n> mode=<plain|gso> runs=<r> insns_per_datagram=<x> insns_per_run=<y>".
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
#include <sys/uio.h>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif

#define MAX_RUN 64

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

static uint64_t counter_read(int fd) {
    uint64_t value = 0;
    if (read(fd, &value, sizeof(value)) != (ssize_t)sizeof(value)) {
        return 0;
    }
    return value;
}

// One run of n datagrams: n messages (plain) or one message with the UDP_SEGMENT cmsg (gso). -1 on a send error.
static int send_run(int sock, const struct sockaddr_in* dst, uint8_t (*payload)[1472], size_t size, int n, int gso) {
    struct mmsghdr msgs[MAX_RUN];
    struct iovec iov[MAX_RUN];
    union {
        char bytes[CMSG_SPACE(sizeof(uint16_t))];
        struct cmsghdr align;
    } control;
    memset(msgs, 0, sizeof(msgs));
    for (int k = 0; k < n; k++) {
        iov[k].iov_base = payload[k];
        iov[k].iov_len = size;
    }
    if (!gso) {
        for (int k = 0; k < n; k++) {
            msgs[k].msg_hdr.msg_name = (void*)dst;
            msgs[k].msg_hdr.msg_namelen = sizeof(*dst);
            msgs[k].msg_hdr.msg_iov = &iov[k];
            msgs[k].msg_hdr.msg_iovlen = 1;
        }
        int sent = 0;
        while (sent < n) {
            int r = sendmmsg(sock, msgs + sent, (unsigned)(n - sent), 0);
            if (r <= 0) {
                return -1;
            }
            sent += r;
        }
        return 0;
    }
    msgs[0].msg_hdr.msg_name = (void*)dst;
    msgs[0].msg_hdr.msg_namelen = sizeof(*dst);
    msgs[0].msg_hdr.msg_iov = iov;
    msgs[0].msg_hdr.msg_iovlen = (size_t)n;
    if (n > 1) {
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

int main(int argc, char** argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s <dst ip> <port> <datagram bytes> <runs per n> n1 [n2 ...]\n", argv[0]);
        return 2;
    }
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &dst.sin_addr) != 1) {
        fprintf(stderr, "bad address %s\n", argv[1]);
        return 2;
    }
    size_t size = (size_t)atoi(argv[3]);
    long runs = atol(argv[4]);
    if (size == 0 || size > 1472 || runs <= 0) {
        fprintf(stderr, "datagram bytes 1..1472 (never more on the wire), runs > 0\n");
        return 2;
    }
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int sndbuf = 4 << 20;
    (void)setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    static uint8_t payload[MAX_RUN][1472];
    memset(payload, 0xa5, sizeof(payload));
    int fd = counter_open();
    if (fd < 0) {
        perror("perf_event_open");
        return 1;
    }
    for (int a = 5; a < argc; a++) {
        int n = atoi(argv[a]);
        if (n < 1 || n > MAX_RUN || (size_t)n * size > 65507) {
            fprintf(stderr, "skipping n=%d\n", n);
            continue;
        }
        for (int gso = 0; gso <= 1; gso++) {
            // A short warm-up (route, neighbour, socket memory) outside the count.
            for (long r = 0; r < 200; r++) {
                if (send_run(sock, &dst, payload, size, n, gso) != 0) {
                    perror("send");
                    return 1;
                }
            }
            ioctl(fd, PERF_EVENT_IOC_RESET, 0);
            ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
            for (long r = 0; r < runs; r++) {
                if (send_run(sock, &dst, payload, size, n, gso) != 0) {
                    perror("send");
                    return 1;
                }
            }
            ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
            uint64_t insns = counter_read(fd);
            printf("n=%d mode=%s runs=%ld insns_per_datagram=%.1f insns_per_run=%.1f\n", n, gso ? "gso" : "plain", runs,
                   (double)insns / (double)(runs * n), (double)insns / (double)runs);
            fflush(stdout);
        }
    }
    close(fd);
    close(sock);
    return 0;
}
