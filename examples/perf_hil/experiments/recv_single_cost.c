// What one receive call costs when exactly one datagram is waiting - recvfrom() against recvmmsg() at
// several vlen (2026-09-26, the c10 latency question on ef0c7ea0). recvmmsg() with MSG_DONTWAIT does not
// return after the first datagram: it tries the next slot too and only then sees EAGAIN, and that try
// happens before the caller can process what it got - on a ping-pong responder, on the critical path.
//
// HOW TO READ IT: recvfrom is the control. If recvmmsg's median per call exceeds it by a few hundred ns
// on x86 (more on the rig's Cortex-A76), the extra probe is a candidate for c10's +6 us round trip
// (four receives per round trip: client and server, each direction). If they match within noise, it
// is not, and the latency difference is elsewhere.
//
// Build: cc -O2 -o /tmp/recv_single_cost recv_single_cost.c   Run: taskset -c 2 /tmp/recv_single_cost
// Seen on x86 (2026-09-26): recvfrom 0.69-0.71 us, recvmmsg vlen 1 0.84, vlen 2/8/32 0.98-0.99.
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming)
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <netinet/in.h>
#include <sys/socket.h>

#define ROUNDS 200000
#define MAX_VLEN 32
#define SLOT_BYTES 1472
#define PAYLOAD_BYTES 76 // p1's sample
#define NS_PER_SEC 1000000000ULL
#define SETTLE_NS 2000L // lets the datagram land before the timed call
#define PCT_10 10
#define PCT_90 90
#define PCT_DIV 100

static uint64_t now_ns(void) {
    struct timespec now;
    // NOLINTNEXTLINE(misc-include-cleaner) - CLOCK_MONOTONIC comes from <time.h> above
    clock_gettime(CLOCK_MONOTONIC, &now);
    return ((uint64_t)now.tv_sec * NS_PER_SEC) + (uint64_t)now.tv_nsec;
}

static int compare_u64(const void* left, const void* right) {
    uint64_t lhs = *(const uint64_t*)left;
    uint64_t rhs = *(const uint64_t*)right;
    return (lhs > rhs) - (lhs < rhs);
}

static uint64_t samples[ROUNDS];
static uint8_t bufs[MAX_VLEN][SLOT_BYTES];

// One timed receive with exactly one datagram waiting: recvfrom() when vlen is 0, recvmmsg() otherwise.
static int timed_receive(int receiver, struct mmsghdr* msgs, int vlen, uint64_t* took) {
    uint64_t start = now_ns();
    int got = 0;
    if (vlen == 0) {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        got = recvfrom(receiver, bufs[0], sizeof(bufs[0]), MSG_DONTWAIT, (struct sockaddr*)&addr, &len) > 0 ? 1 : 0;
    } else {
        msgs[0].msg_hdr.msg_namelen = sizeof(struct sockaddr_in);
        got = recvmmsg(receiver, msgs, (unsigned int)vlen, MSG_DONTWAIT, NULL);
    }
    *took = now_ns() - start;
    return got;
}

static int run(int receiver, int sender, const struct sockaddr_in* dest, int vlen) {
    struct mmsghdr msgs[MAX_VLEN];
    struct iovec iov[MAX_VLEN]; // NOLINT(misc-include-cleaner) - <sys/socket.h> provides it
    struct sockaddr_in from[MAX_VLEN];
    memset(msgs, 0, sizeof(msgs));
    for (int i = 0; i < MAX_VLEN; i++) {
        iov[i].iov_base = bufs[i];
        iov[i].iov_len = sizeof(bufs[i]);
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_name = &from[i];
    }
    uint8_t payload[PAYLOAD_BYTES] = {0};
    const struct timespec settle = {0, SETTLE_NS};
    for (int round = 0; round < ROUNDS; round++) {
        sendto(sender, payload, sizeof(payload), 0, (const struct sockaddr*)dest, sizeof(*dest));
        nanosleep(&settle, NULL);
        if (timed_receive(receiver, msgs, vlen, &samples[round]) != 1) {
            fprintf(stderr, "round %d: nothing received\n", round);
            return 1;
        }
    }
    qsort(samples, ROUNDS, sizeof(samples[0]), compare_u64);
    printf("%-9s vlen %2d  median %5llu ns  p10 %5llu  p90 %5llu\n", vlen == 0 ? "recvfrom" : "recvmmsg", vlen,
           (unsigned long long)samples[ROUNDS / 2], (unsigned long long)samples[ROUNDS * PCT_10 / PCT_DIV],
           (unsigned long long)samples[ROUNDS * PCT_90 / PCT_DIV]);
    return 0;
}

int main(void) {
    int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = __builtin_bswap32(INADDR_LOOPBACK); // htonl() on a little-endian host
    if (receiver < 0 || sender < 0 || bind(receiver, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        return 1;
    }
    socklen_t len = sizeof(addr);
    getsockname(receiver, (struct sockaddr*)&addr, &len);
    static const int arms[] = {0, 1, 2, 8, 32, 0}; // recvfrom first and last: the drift control
    for (size_t i = 0; i < sizeof(arms) / sizeof(arms[0]); i++) {
        if (run(receiver, sender, &addr, arms[i]) != 0) {
            return 1;
        }
    }
    return 0;
}
