/* frag_reasm_probe.c - paced UDP sender and per-sequence receiver for frag_reasm_model.sh.
 *
 * No TickLE code. A datagram carries a 16-byte header (magic, flags, seq) and n units of
 * 1446 bytes, so its UDP payload is 16 + 1446*n bytes; above 1472 the kernel IP-fragments it
 * (IP_PMTUDISC_DONT, so DF is clear and the stack fragments instead of refusing).
 *
 *   send <dst_ip> <port> <n> <payload_bytes_per_s> <warm_s> <measure_s>
 *       Paces datagrams at payload_bytes_per_s counting only the n*1446 unit bytes. Datagrams sent
 *       after warm_s carry FLAG_MEASURE. Prints one summary line on stdout.
 *   recv <port> <deadline_s>
 *       Receives until deadline_s after start, marks each seq in a bitmap, writes the bitmap
 *       (bit i = seq i received at least once) to stdout and one summary line to stderr.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define UNIT 1446
#define HDR 16
#define MAGIC 0x46524151u /* "FRAQ" */
#define FLAG_MEASURE 1u
#define MAXDG 65507

struct hdr {
    uint32_t magic;
    uint32_t flags;
    uint64_t seq;
};

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int do_send(int argc, char** argv) {
    if (argc != 8)
        return 2;
    const char* dst = argv[2];
    int port = atoi(argv[3]);
    int n = atoi(argv[4]);
    double rate = atof(argv[5]);
    double warm = atof(argv[6]);
    double meas = atof(argv[7]);
    size_t len = HDR + (size_t)UNIT * (size_t)n;
    if (n < 1 || len > MAXDG) {
        fprintf(stderr, "size %zu exceeds %d\n", len, MAXDG);
        return 2;
    }
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    int pmtu = IP_PMTUDISC_DONT;
    if (s < 0 || setsockopt(s, IPPROTO_IP, IP_MTU_DISCOVER, &pmtu, sizeof pmtu) != 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    inet_pton(AF_INET, dst, &sa.sin_addr);
    char* buf = calloc(1, len);
    for (size_t i = HDR; i < len; i++)
        buf[i] = (char)(i * 31u);
    struct hdr* h = (struct hdr*)buf;
    h->magic = MAGIC;
    double interval = 1e9 * (double)UNIT * n / rate;
    uint64_t t0 = now_ns();
    uint64_t t_warm = t0 + (uint64_t)(warm * 1e9);
    uint64_t t_end = t_warm + (uint64_t)(meas * 1e9);
    uint64_t seq = 0, errs = 0, first_meas = UINT64_MAX, meas_sent = 0;
    int last_errno = 0;
    for (;;) {
        uint64_t due = t0 + (uint64_t)(interval * (double)seq);
        if (due >= t_end)
            break;
        struct timespec ts = {.tv_sec = (time_t)(due / 1000000000ull), .tv_nsec = (long)(due % 1000000000ull)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
        }
        h->seq = seq;
        h->flags = (due >= t_warm) ? FLAG_MEASURE : 0u;
        if (h->flags && first_meas == UINT64_MAX)
            first_meas = seq;
        if (sendto(s, buf, len, 0, (struct sockaddr*)&sa, sizeof sa) != (ssize_t)len) {
            errs++;
            last_errno = errno;
        }
        if (h->flags)
            meas_sent++;
        seq++;
    }
    uint64_t t1 = now_ns();
    printf("SEND n=%d dg_bytes=%zu sent=%llu first_measure_seq=%llu last_seq=%llu measure_sent=%llu "
           "send_errs=%llu last_errno=%d elapsed_s=%.3f achieved_Bps=%.0f\n",
           n, len, (unsigned long long)seq, (unsigned long long)first_meas, (unsigned long long)(seq ? seq - 1 : 0),
           (unsigned long long)meas_sent, (unsigned long long)errs, last_errno, (double)(t1 - t0) / 1e9,
           (double)seq * UNIT * n / ((double)(t1 - t0) / 1e9));
    return 0;
}

static int do_recv(int argc, char** argv) {
    if (argc != 4)
        return 2;
    int port = atoi(argv[2]);
    double deadline = atof(argv[3]);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    int want = 64 * 1024 * 1024;
    /* FORCE needs CAP_NET_ADMIN (we run as root via ip netns exec); fall back to the capped one. */
    if (setsockopt(s, SOL_SOCKET, SO_RCVBUFFORCE, &want, sizeof want) != 0)
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, &want, sizeof want);
    int got = 0;
    socklen_t gl = sizeof got;
    getsockopt(s, SOL_SOCKET, SO_RCVBUF, &got, &gl);
    struct sockaddr_in sa = {.sin_family = AF_INET,
                             .sin_port = htons((uint16_t)port),
                             .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(s, (struct sockaddr*)&sa, sizeof sa) != 0) {
        perror("bind");
        return 1;
    }
    size_t cap = 1 << 20;
    uint8_t* bm = calloc(cap, 1);
    uint64_t maxseq = 0, rx = 0, dup = 0, bad = 0, any = 0;
    static char buf[MAXDG + 1];
    uint64_t t_end = now_ns() + (uint64_t)(deadline * 1e9);
    for (;;) {
        uint64_t t = now_ns();
        if (t >= t_end)
            break;
        struct pollfd pf = {.fd = s, .events = POLLIN};
        int ms = (int)((t_end - t) / 1000000ull) + 1;
        if (poll(&pf, 1, ms > 200 ? 200 : ms) <= 0)
            continue;
        for (;;) {
            ssize_t r = recv(s, buf, sizeof buf, MSG_DONTWAIT);
            if (r < 0)
                break;
            const struct hdr* h = (const struct hdr*)buf;
            if (r < HDR || h->magic != MAGIC || (r - HDR) % UNIT != 0) {
                bad++;
                continue;
            }
            uint64_t q = h->seq;
            while (q / 8 >= cap) {
                bm = realloc(bm, cap * 2);
                memset(bm + cap, 0, cap);
                cap *= 2;
            }
            if (bm[q / 8] & (1u << (q % 8))) {
                dup++;
            } else {
                bm[q / 8] |= (uint8_t)(1u << (q % 8));
                rx++;
            }
            if (!any || q > maxseq)
                maxseq = q;
            any = 1;
        }
    }
    size_t nbytes = any ? (size_t)(maxseq / 8 + 1) : 0;
    fwrite(bm, 1, nbytes, stdout);
    fflush(stdout);
    fprintf(stderr, "RECV rcvbuf=%d received=%llu dup=%llu bad=%llu maxseq=%llu\n", got, (unsigned long long)rx,
            (unsigned long long)dup, (unsigned long long)bad, (unsigned long long)maxseq);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "send") == 0)
        return do_send(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "recv") == 0)
        return do_recv(argc, argv);
    fprintf(stderr, "usage: send dst port n Bps warm meas | recv port deadline\n");
    return 2;
}
