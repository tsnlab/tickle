// Receive wake cost across two hosts, by how the receiver waits (2026-10-21, ROADMAP "rmw kernel wake", the 4(b)
// follow-up). The rig's wake split (docs/RESULTS.md, KH) put rmw_tickle's pong receive return ~4-5 us behind the
// vendors on BEST_EFFORT: its wait (epoll_pwait2) returns as early as their blocking recvmsg, and the gap is the second
// system call, the recvfrom() after the wait. recv_wake_cost.c measured the styles on one host's loopback; this is the
// same question on the rig's own path - a datagram from the other Pi over eth0, the NIC interrupt, the stack, the wake.
//
// What is measured, per datagram, on the RECEIVING host alone (one clock, no cross-host offset): the kernel's software
// receive stamp of the datagram (skb->tstamp, set where the stack takes the packet from the driver, read back with
// ioctl(SIOCGSTAMPNS) AFTER the receive returned, so the ioctl is outside the measured time) to the receive call
// returning to user space (CLOCK_REALTIME, the stamp's clock). Everything after the stack's entry is in it: the UDP
// delivery, the wake of the waiting thread, the system call(s) it makes to get the bytes. Everything before it (wire,
// interrupt, NAPI) is the same for every style and is not.
// The receiver then echoes the datagram's 16 bytes to the sender from a second socket, and the sender records the round
// trip on its own clock: a second instrument (it carries the same receive-side difference plus noise both ways), used
// only to check that the first is not an artefact of the stamp.
//
// Styles (one per run; the driver rotates them per rep):
//   recvfrom     a blocking recvfrom() on the socket (Fast DDS's wait). Run twice per rep as two arms, recvfrom and
//                recvfrom_aa - the A/A control: the same code under two names must agree.
//   recvmsg      a blocking recvmsg() (CycloneDDS's recvUC thread)
//   epoll3       a second idle socket, an idle eventfd and the data socket registered once in an epoll set;
//                epoll_pwait2(no timeout), then recvfrom(MSG_DONTWAIT) - rmw_tickle's receive today
//   uring        per datagram one IORING_OP_RECV submitted and waited for in the same io_uring_enter(); the idle
//                socket and eventfd each hold an IORING_OP_POLL_ADD that never completes
//   uring_dt     the same on a ring set up with IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN, so the copy
//                runs in the waiting thread's own io_uring_enter()
//   uring_ms     one multishot IORING_OP_RECV (IORING_RECV_MULTISHOT) with a provided-buffer ring, armed once; each
//                wait is io_uring_enter(0 submit, min_complete 1); the buffer goes back to the ring after the read.
//                The ring is registered as production's drain registers it: in this process's memory, else the
//                kernel's own (IOU_PBUF_RING_MMAP); a kernel that refuses both refuses the style
//   uring_ms_dt  the same on a DEFER_TASKRUN ring
// A style whose setup the kernel refuses prints "REFUSED <style> <reason>" and exits 3: not measured, not a failure of
// the others. `probe` tries every style's setup and prints AVAILABLE or REFUSED for each, then PBUF lines: which
// IORING_REGISTER_PBUF_RING variants the kernel accepts, against a control it must refuse, and a PBUF_CAUSE line that
// says whether a refusal is the harness's arguments or the kernel (2026-10-22: the rig's Pis, Linux 6.8.0-1064-raspi,
// refused the multishot styles with EINVAL; production's drain logs the same EINVAL there for both ring variants).
//
// The reading rules - which difference decides, the control, the sample counts - are code in recv_wake_xhost_read.py
// (--prereg prints them, --selftest checks them), written before the first run.
//
// Build: cc -O2 -o rwx recv_wake_xhost.c
// Run:   rwx rx <style> <port> <rounds> <warmup> <cpu> <echo_ip> <echo_port> <samples_file>
//        rwx tx <dst_ip> <port> <rounds> <warmup> <cpu> <gap_us> <own_port>
//        rwx probe
// The tx side sends warmup + rounds datagrams, each after the previous one's echo (or a 50 ms timeout) and a gap of
// gap_us, so the receiver is asleep each time, as a pong is between pings. Then STOP, a few times.
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming)
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <linux/io_uring.h>
#include <linux/sockios.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/utsname.h>
// __NR_io_uring_*: <sys/syscall.h> is the portable spelling (hal_linux.c); the numbers live in an architecture header.
#include <sys/syscall.h> // NOLINT(misc-include-cleaner)

#define NS_PER_SEC 1000000000ULL
#define NS_PER_US 1000ULL
#define DATAGRAM_BYTES 76
#define MAGIC 0x52575848U // "RWXH"
#define SEQ_HELLO 0xFFFFFFFEU
#define SEQ_STOP 0xFFFFFFFFU
#define STOP_REPEATS 20
#define ECHO_TIMEOUT_US 50000
#define HELLO_TRIES 400 // x 50 ms: 20 s for the receiver to come up
#define BAD_STAMP_MAX_NS 1000000000ULL
#define PCT_10 10
#define PCT_50 50
#define PCT_99 99
#define PCT_DIV 100
#define EXIT_REFUSED 3
#define URING_ENTRIES 16U
#define URING_DATA_RECV 1U
#define PBUF_COUNT 8U
#define PBUF_BYTES 2048U
#define PBUF_GROUP 7U
// The buffer-ring constants of Linux 6.4 (IOU_PBUF_RING_MMAP), for headers older than the kernel they run on.
#ifndef IOU_PBUF_RING_MMAP
#define IOU_PBUF_RING_MMAP 1U
#endif
#ifndef IORING_OFF_PBUF_RING
#define IORING_OFF_PBUF_RING 0x80000000ULL
#define IORING_OFF_PBUF_SHIFT 16
#endif
#define WHY_BYTES 256
#define MAX_ROUNDS 1000000L
#define MAX_GAP_US 1000000L
#define MAX_PORT 65535L
#define MAX_CPU 1023L
#define EXIT_USAGE 64

// The clocks, from <time.h> above. The receive stamp's clock (skb->tstamp is CLOCK_REALTIME), the receiving
// thread's CPU time, the sender's round-trip clock.
static const clockid_t stamp_clock = CLOCK_REALTIME;        // NOLINT(misc-include-cleaner) - <time.h>
static const clockid_t cpu_clock = CLOCK_THREAD_CPUTIME_ID; // NOLINT(misc-include-cleaner) - <time.h>
static const clockid_t rtt_clock = CLOCK_MONOTONIC;         // NOLINT(misc-include-cleaner) - <time.h>

enum style { RECVFROM, RECVFROM_AA, RECVMSG, EPOLL3, URING, URING_DT, URING_MS, URING_MS_DT, STYLE_COUNT };
static const char* const style_names[STYLE_COUNT] = {"recvfrom", "recvfrom_aa", "recvmsg",  "epoll3",
                                                     "uring",    "uring_dt",    "uring_ms", "uring_ms_dt"};

static int is_uring(enum style style) {
    return style >= URING;
}
static int is_multishot(enum style style) {
    return style == URING_MS || style == URING_MS_DT;
}
static int is_deferred(enum style style) {
    return style == URING_DT || style == URING_MS_DT;
}

struct wire { // the first 16 bytes of every datagram, in host order (both ends are the same machine type)
    uint32_t magic;
    uint32_t seq;
    uint64_t tx_ns;
};

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
    struct io_uring_buf_ring* pbuf; // multishot: the provided buffers
    uint8_t* pbuf_mem;
    uint16_t pbuf_tail;
};

struct receiver {
    enum style style;
    int socket_fd;
    int other_fd;
    int event_fd;
    int epoll_fd;
    struct uring ring;
    uint8_t buf[DATAGRAM_BYTES];
    const uint8_t* last; // where the last datagram's bytes are (buf, or a provided buffer)
    long rearms;         // multishot: times the recv had to be armed again (a CQE without F_MORE)
    int pending_bid;     // multishot: the provided buffer last read from, given back at the next receive (-1: none)
};

static uint64_t clock_ns(clockid_t clock) {
    struct timespec now;
    clock_gettime(clock, &now);
    return ((uint64_t)now.tv_sec * NS_PER_SEC) + (uint64_t)now.tv_nsec;
}

static void pin(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        fprintf(stderr, "warning: cannot pin to CPU %d: %s\n", cpu, strerror(errno));
    }
}

static int compare_u64(const void* left, const void* right) {
    uint64_t lhs = *(const uint64_t*)left;
    uint64_t rhs = *(const uint64_t*)right;
    return (lhs > rhs) - (lhs < rhs);
}

// Nearest-rank percentile of a sorted array.
static uint64_t pct(const uint64_t* sorted, long count, int percent) {
    long rank = ((count * percent) + PCT_DIV - 1) / PCT_DIV;
    return sorted[rank < 1 ? 0 : rank - 1];
}

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

static void pbuf_give(struct uring* ring, uint16_t bid) {
    struct io_uring_buf* slot = &ring->pbuf->bufs[ring->pbuf_tail & (PBUF_COUNT - 1U)];
    slot->addr = (uint64_t)(uintptr_t)(ring->pbuf_mem + ((size_t)bid * PBUF_BYTES));
    slot->len = PBUF_BYTES;
    slot->bid = bid;
    ring->pbuf_tail++;
    __atomic_store_n(&ring->pbuf->tail, ring->pbuf_tail, __ATOMIC_RELEASE);
}

// One IORING_REGISTER_PBUF_RING: 0, or the errno the kernel refused it with.
static int pbuf_reg(int ring_fd, uint64_t ring_addr, uint32_t entries, uint16_t group, uint16_t flags, uint64_t resv0) {
    struct io_uring_buf_reg reg;
    memset(&reg, 0, sizeof(reg));
    reg.ring_addr = ring_addr;
    reg.ring_entries = entries;
    reg.bgid = group;
    reg.flags = flags;
    reg.resv[0] = resv0;
    // NOLINTNEXTLINE(misc-include-cleaner) - __NR_io_uring_register: <sys/syscall.h>, above
    return syscall(__NR_io_uring_register, ring_fd, IORING_REGISTER_PBUF_RING, &reg, 1) == 0 ? 0 : errno;
}

// The multishot styles' buffer ring, the way production's drain asks for it (hal_linux.c drain_register_buffers()):
// the ring in this process's memory, else the kernel's own (IOU_PBUF_RING_MMAP, Linux 6.4), mapped here. Production
// then falls back to classic provided buffers; these styles measure a buffer ring, so they are refused instead, with
// both answers in *why (`probe` says which kernel check it is).
static int pbuf_register(struct uring* ring, size_t ring_bytes, const char** why) {
    static char reason[WHY_BYTES];
    int user_err = pbuf_reg(ring->fd, (uint64_t)(uintptr_t)ring->pbuf, PBUF_COUNT, PBUF_GROUP, 0, 0);
    if (user_err == 0) {
        return 0;
    }
    int kernel_err = pbuf_reg(ring->fd, 0, PBUF_COUNT, PBUF_GROUP, IOU_PBUF_RING_MMAP, 0);
    if (kernel_err == 0) {
        void* mapped = mmap(NULL, ring_bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring->fd,
                            (off_t)(IORING_OFF_PBUF_RING | ((uint64_t)PBUF_GROUP << IORING_OFF_PBUF_SHIFT)));
        if (mapped != MAP_FAILED) {
            (void)munmap(ring->pbuf, ring_bytes);
            ring->pbuf = mapped;
            return 0;
        }
        kernel_err = errno;
    }
    struct utsname host;
    if (uname(&host) != 0) {
        (void)snprintf(host.release, sizeof(host.release), "unknown");
    }
    (void)snprintf(
        reason, sizeof(reason),
        "IORING_REGISTER_PBUF_RING (user ring: %s; IOU_PBUF_RING_MMAP: %s; Linux %s; `probe` names the check)",
        strerror(user_err), strerror(kernel_err), host.release);
    *why = reason;
    errno = user_err;
    return -1;
}

static void arm_multishot(struct receiver* rcv) {
    struct io_uring_sqe* sqe = uring_sqe(&rcv->ring);
    sqe->opcode = IORING_OP_RECV;
    sqe->fd = rcv->socket_fd;
    sqe->ioprio = IORING_RECV_MULTISHOT;
    sqe->flags = IOSQE_BUFFER_SELECT;
    sqe->buf_group = PBUF_GROUP;
    sqe->user_data = URING_DATA_RECV;
}

// Sets the style's waiting machinery up. 0, or -1 with *why naming what the kernel refused.
static int receiver_setup(struct receiver* rcv, const char** why) {
    rcv->epoll_fd = epoll_create1(0);
    const int watched[3] = {rcv->other_fd, rcv->event_fd, rcv->socket_fd};
    for (int i = 0; i < 3; i++) {
        struct epoll_event interest = {.events = EPOLLIN, .data = {.fd = watched[i]}};
        if (epoll_ctl(rcv->epoll_fd, EPOLL_CTL_ADD, watched[i], &interest) != 0) {
            *why = "epoll_ctl";
            return -1;
        }
    }
    if (!is_uring(rcv->style)) {
        return 0;
    }
    unsigned flags = is_deferred(rcv->style) ? (IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN) : 0U;
    if (uring_setup(&rcv->ring, flags) != 0) {
        *why = is_deferred(rcv->style) ? "io_uring_setup(SINGLE_ISSUER|DEFER_TASKRUN)" : "io_uring_setup";
        return -1;
    }
    const int idle[2] = {rcv->other_fd, rcv->event_fd}; // armed once, never complete
    for (int i = 0; i < 2; i++) {
        struct io_uring_sqe* sqe = uring_sqe(&rcv->ring);
        sqe->opcode = IORING_OP_POLL_ADD;
        sqe->fd = idle[i];
        sqe->poll32_events = POLLIN; // NOLINT(misc-include-cleaner) - <poll.h>
        sqe->user_data = 2U + (unsigned)i;
    }
    unsigned submit = 2;
    if (is_multishot(rcv->style)) {
        size_t ring_bytes = PBUF_COUNT * sizeof(struct io_uring_buf);
        rcv->ring.pbuf = mmap(NULL, ring_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        rcv->ring.pbuf_mem = malloc((size_t)PBUF_COUNT * PBUF_BYTES);
        if (rcv->ring.pbuf == MAP_FAILED || rcv->ring.pbuf_mem == NULL) {
            *why = "provided buffer memory";
            return -1;
        }
        memset(rcv->ring.pbuf, 0, ring_bytes);
        if (pbuf_register(&rcv->ring, ring_bytes, why) != 0) {
            return -1;
        }
        for (uint16_t bid = 0; bid < PBUF_COUNT; bid++) {
            pbuf_give(&rcv->ring, bid);
        }
        arm_multishot(rcv);
        submit = 3;
    }
    if (uring_enter(&rcv->ring, submit, 0) != (long)submit) {
        *why = "io_uring submission of the idle polls";
        return -1;
    }
    return 0;
}

// Takes the next completion off the ring into *out, entering the kernel to wait for one (submitting *submit SQEs with
// that wait, once) while there is none. 0, or -1 when the kernel refuses or an idle poll completed.
static int uring_next(struct receiver* rcv, unsigned* submit, struct io_uring_cqe* out) {
    for (;;) {
        uint32_t head = *rcv->ring.cq_head;
        if (head != __atomic_load_n(rcv->ring.cq_tail, __ATOMIC_ACQUIRE)) {
            *out = rcv->ring.cqes[head & *rcv->ring.cq_mask];
            __atomic_store_n(rcv->ring.cq_head, head + 1, __ATOMIC_RELEASE);
            return out->user_data == URING_DATA_RECV ? 0 : -1; // an idle poll: nothing should have made it ready
        }
        long entered = uring_enter(&rcv->ring, *submit, 1);
        if (entered < 0 && errno != EINTR) {
            return -1;
        }
        if (entered > 0) {
            *submit = 0; // the SQE is in: never submitted twice
        }
    }
}

// uring, uring_dt: one IORING_OP_RECV submitted and waited for in the same io_uring_enter().
static ssize_t receive_uring_single(struct receiver* rcv) {
    struct io_uring_sqe* sqe = uring_sqe(&rcv->ring);
    sqe->opcode = IORING_OP_RECV;
    sqe->fd = rcv->socket_fd;
    sqe->addr = (uint64_t)(uintptr_t)rcv->buf;
    sqe->len = (uint32_t)sizeof(rcv->buf);
    sqe->user_data = URING_DATA_RECV;
    unsigned submit = 1;
    struct io_uring_cqe cqe;
    if (uring_next(rcv, &submit, &cqe) != 0) {
        return -1;
    }
    return cqe.res;
}

// uring_ms, uring_ms_dt: the armed multishot recv's next completion; the bytes are read in place in the provided buffer
// (no copy in the timed path), which goes back to the kernel at the next call.
static ssize_t receive_uring_multishot(struct receiver* rcv) {
    if (rcv->pending_bid >= 0) {
        pbuf_give(&rcv->ring, (uint16_t)rcv->pending_bid);
        rcv->pending_bid = -1;
    }
    for (;;) {
        unsigned submit = 0;
        struct io_uring_cqe cqe;
        if (uring_next(rcv, &submit, &cqe) != 0) {
            return -1;
        }
        if ((cqe.flags & IORING_CQE_F_MORE) == 0U) { // the multishot ended (e.g. ENOBUFS): arm it again
            rcv->rearms++;
            arm_multishot(rcv);
            if (uring_enter(&rcv->ring, 1, 0) != 1) {
                return -1;
            }
        }
        if (cqe.res > 0 && (cqe.flags & IORING_CQE_F_BUFFER) != 0U) {
            uint16_t bid = (uint16_t)(cqe.flags >> IORING_CQE_BUFFER_SHIFT);
            rcv->last = rcv->ring.pbuf_mem + ((size_t)bid * PBUF_BYTES);
            rcv->pending_bid = bid;
            return cqe.res;
        }
    }
}

// recvfrom, recvfrom_aa, recvmsg, epoll3: the socket calls.
static ssize_t receive_socket(struct receiver* rcv) {
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    if (rcv->style == EPOLL3) {
        struct epoll_event ready;
        int got = 0;
        do {
            got = epoll_pwait2(rcv->epoll_fd, &ready, 1, NULL, NULL);
        } while (got < 0 && errno == EINTR);
        if (got <= 0) {
            return -1;
        }
        return recvfrom(rcv->socket_fd, rcv->buf, sizeof(rcv->buf), MSG_DONTWAIT, (struct sockaddr*)&from, &from_len);
    }
    if (rcv->style == RECVFROM || rcv->style == RECVFROM_AA) {
        return recvfrom(rcv->socket_fd, rcv->buf, sizeof(rcv->buf), 0, (struct sockaddr*)&from, &from_len);
    }
    struct iovec iov = {.iov_base = rcv->buf, .iov_len = sizeof(rcv->buf)}; // NOLINT(misc-include-cleaner)
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &from;
    msg.msg_namelen = sizeof(from);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    return recvmsg(rcv->socket_fd, &msg, 0);
}

// One blocking receive in the receiver's style. The byte count, or -1; rcv->last points at the bytes.
static ssize_t receive_one(struct receiver* rcv) {
    rcv->last = rcv->buf;
    if (is_multishot(rcv->style)) {
        return receive_uring_multishot(rcv);
    }
    if (is_uring(rcv->style)) {
        return receive_uring_single(rcv);
    }
    return receive_socket(rcv);
}

static int parse_style(const char* name) {
    for (int i = 0; i < STYLE_COUNT; i++) {
        if (strcmp(name, style_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static int open_receiver(struct receiver* rcv, enum style style, int port, const char** why) {
    memset(rcv, 0, sizeof(*rcv));
    rcv->style = style;
    rcv->pending_bid = -1;
    rcv->socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    rcv->other_fd = socket(AF_INET, SOCK_DGRAM, 0);
    rcv->event_fd = eventfd(0, EFD_NONBLOCK);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (rcv->socket_fd < 0 || rcv->other_fd < 0 || rcv->event_fd < 0 ||
        bind(rcv->socket_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        *why = "socket/bind";
        return -1;
    }
    // The first SIOCGSTAMPNS turns the socket's receive stamps on (SOCK_TIMESTAMP); nothing has arrived yet, so it
    // answers ENOENT. From here every datagram read on the socket leaves its skb->tstamp in sk_stamp.
    struct timespec stamp;
    (void)ioctl(rcv->socket_fd, SIOCGSTAMPNS, &stamp);
    return receiver_setup(rcv, why);
}

// One registration variant on a ring of its own (so no earlier group is in the way): the errno, 0 when accepted.
static int pbuf_variant(uint64_t ring_addr, uint32_t entries, uint16_t group, uint16_t flags, uint64_t resv0) {
    struct uring ring;
    memset(&ring, 0, sizeof(ring));
    if (uring_setup(&ring, 0) != 0) {
        return errno;
    }
    int err = pbuf_reg(ring.fd, ring_addr, entries, group, flags, resv0);
    (void)close(ring.fd); // the mappings go with the process
    return err;
}

// Classic provided buffers (IORING_OP_PROVIDE_BUFFERS, Linux 5.7) - what production's drain falls back to: the CQE's
// result, or the errno of a refused setup or submission.
static int pbuf_classic(const uint8_t* memory) {
    struct uring ring;
    memset(&ring, 0, sizeof(ring));
    if (uring_setup(&ring, 0) != 0) {
        return errno;
    }
    struct io_uring_sqe* sqe = uring_sqe(&ring);
    sqe->opcode = IORING_OP_PROVIDE_BUFFERS;
    sqe->fd = (int32_t)PBUF_COUNT;
    sqe->addr = (uint64_t)(uintptr_t)memory;
    sqe->len = PBUF_BYTES;
    sqe->buf_group = PBUF_GROUP;
    if (uring_enter(&ring, 1, 1) < 0) {
        int err = errno;
        (void)close(ring.fd);
        return err;
    }
    uint32_t head = *ring.cq_head;
    int res = head != __atomic_load_n(ring.cq_tail, __ATOMIC_ACQUIRE) ? ring.cqes[head & *ring.cq_mask].res : -EAGAIN;
    (void)close(ring.fd);
    return res < 0 ? -res : 0;
}

// Why IORING_REGISTER_PBUF_RING is refused, decided by variants rather than read off one errno: a control the kernel
// must refuse (resv[0] set: EINVAL on every kernel since 5.19) proves the call reaches the argument checks; then the
// harness's own registration, the same with one entry and with group 0, page-aligned memory each time (so neither
// alignment nor a power of two nor the group can be the reason), and the kernel's own ring (IOU_PBUF_RING_MMAP, which
// takes no user memory at all). Printed as PBUF lines and one PBUF_CAUSE line.
static void pbuf_diagnose(void) {
    struct utsname host;
    if (uname(&host) != 0) {
        (void)snprintf(host.release, sizeof(host.release), "unknown");
    }
    long page = sysconf(_SC_PAGESIZE);
    size_t bytes = (size_t)page;
    uint8_t* user = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    uint8_t* memory = malloc((size_t)PBUF_COUNT * PBUF_BYTES);
    if (user == MAP_FAILED || memory == NULL) {
        printf("PBUF_CAUSE unknown: no memory for the variants\n");
        return;
    }
    const uint64_t addr = (uint64_t)(uintptr_t)user;
    const int control = pbuf_variant(addr, PBUF_COUNT, PBUF_GROUP, 0, 1);
    const int harness = pbuf_variant(addr, PBUF_COUNT, PBUF_GROUP, 0, 0);
    const int one = pbuf_variant(addr, 1, PBUF_GROUP, 0, 0);
    const int group0 = pbuf_variant(addr, PBUF_COUNT, 0, 0, 0);
    const int kernel = pbuf_variant(0, PBUF_COUNT, PBUF_GROUP, IOU_PBUF_RING_MMAP, 0);
    const int classic = pbuf_classic(memory);
    printf("PBUF Linux %s, page %ld bytes, user ring at page offset %lu\n", host.release, page,
           (unsigned long)(addr % (uint64_t)page));
    printf("PBUF control resv[0]=1 (must be refused, EINVAL): %s\n", control == 0 ? "accepted" : strerror(control));
    printf("PBUF user ring, entries=%u bgid=%u (the harness's): %s\n", PBUF_COUNT, PBUF_GROUP,
           harness == 0 ? "accepted" : strerror(harness));
    printf("PBUF user ring, entries=1: %s\n", one == 0 ? "accepted" : strerror(one));
    printf("PBUF user ring, bgid=0: %s\n", group0 == 0 ? "accepted" : strerror(group0));
    printf("PBUF kernel ring (IOU_PBUF_RING_MMAP, no user memory): %s\n", kernel == 0 ? "accepted" : strerror(kernel));
    printf("PBUF classic IORING_OP_PROVIDE_BUFFERS (production's drain fallback): %s\n",
           classic == 0 ? "accepted" : strerror(classic));
    if (control != EINVAL) {
        printf("PBUF_CAUSE unknown: the control was not refused with EINVAL (%s) - the variants cannot be read\n",
               control == 0 ? "accepted" : strerror(control));
    } else if (harness == 0) {
        printf("PBUF_CAUSE none: the harness's registration is accepted\n");
    } else if (one == 0 || group0 == 0) {
        printf("PBUF_CAUSE harness: a variant of the harness's own arguments is accepted - fix the harness\n");
    } else if (kernel == 0) {
        printf("PBUF_CAUSE kernel: Linux %s refuses buffer rings in user memory (any size, any group, page-aligned) "
               "and accepts its own (IOU_PBUF_RING_MMAP) - the harness falls back to that\n",
               host.release);
    } else {
        printf("PBUF_CAUSE kernel: Linux %s refuses every buffer ring registration - user memory (page-aligned, power "
               "of two, any group) and IOU_PBUF_RING_MMAP alike - with a control that shows the call reaches the "
               "argument checks; not the harness. Multishot styles stay REFUSED; classic provided buffers: %s\n",
               host.release, classic == 0 ? "accepted" : strerror(classic));
    }
    free(memory);
}

static int run_probe(void) {
    int refused = 0;
    for (int i = 0; i < STYLE_COUNT; i++) {
        struct receiver rcv;
        const char* why = "";
        if (open_receiver(&rcv, (enum style)i, 0, &why) != 0) {
            printf("REFUSED %s %s: %s\n", style_names[i], why, strerror(errno));
            refused++;
        } else {
            printf("AVAILABLE %s\n", style_names[i]);
        }
        // the process exits soon: the descriptors and rings go with it
    }
    pbuf_diagnose();
    return refused == STYLE_COUNT ? 1 : 0;
}

struct tally { // what the receiver counted
    uint64_t* samples;
    unsigned char* seen;
    long rounds;
    long warmup;
    long kept;
    long bad_stamps;
    long duplicates;
    long foreign;
    long hellos;
    uint64_t cpu_start;
    int counting;
};

enum take_result { TAKE_ON, TAKE_STOP, TAKE_FAILED };

// One datagram: received, timed, echoed, counted.
static enum take_result take_one(struct tally* tally, struct receiver* rcv, int echo_fd,
                                 const struct sockaddr_in* echo_addr) {
    ssize_t got = receive_one(rcv);
    uint64_t returned = clock_ns(stamp_clock);
    if (got < (ssize_t)sizeof(struct wire)) {
        fprintf(stderr, "receive failed (%zd): %s\n", got, strerror(errno));
        return TAKE_FAILED;
    }
    struct timespec stamp = {0, 0};
    int stamped = ioctl(rcv->socket_fd, SIOCGSTAMPNS, &stamp) == 0; // after the time is taken: outside it
    struct wire head;
    memcpy(&head, rcv->last, sizeof(head));
    if (head.magic != MAGIC) {
        tally->foreign++;
        return TAKE_ON;
    }
    if (head.seq == SEQ_STOP) {
        return TAKE_STOP;
    }
    (void)sendto(echo_fd, rcv->last, sizeof(head), 0, (const struct sockaddr*)echo_addr, sizeof(*echo_addr));
    if (head.seq == SEQ_HELLO) {
        tally->hellos++;
        return TAKE_ON;
    }
    if (head.seq < (uint32_t)tally->warmup || head.seq >= (uint32_t)(tally->warmup + tally->rounds)) {
        return TAKE_ON;
    }
    if (!tally->counting) {
        tally->cpu_start = clock_ns(cpu_clock);
        tally->counting = 1;
    }
    long index = (long)head.seq - tally->warmup;
    if (tally->seen[index]) {
        tally->duplicates++;
        return TAKE_ON;
    }
    tally->seen[index] = 1;
    uint64_t stamp_ns = ((uint64_t)stamp.tv_sec * NS_PER_SEC) + (uint64_t)stamp.tv_nsec;
    if (!stamped || stamp_ns == 0 || returned <= stamp_ns || returned - stamp_ns > BAD_STAMP_MAX_NS) {
        tally->bad_stamps++;
        return TAKE_ON;
    }
    tally->samples[tally->kept++] = returned - stamp_ns;
    return TAKE_ON;
}

static int report_rx(struct tally* tally, enum style style, long rearms, const char* samples_path) {
    uint64_t cpu_ns = tally->counting ? clock_ns(cpu_clock) - tally->cpu_start : 0;
    FILE* out = fopen(samples_path, "w");
    if (out == NULL) {
        fprintf(stderr, "cannot write %s\n", samples_path);
        return 1;
    }
    for (long i = 0; i < tally->kept; i++) {
        fprintf(out, "%llu\n", (unsigned long long)tally->samples[i]);
    }
    fclose(out);
    long missing = 0;
    for (long i = 0; i < tally->rounds; i++) {
        missing += tally->seen[i] ? 0 : 1;
    }
    long kept = tally->kept;
    uint64_t* sorted = tally->samples;
    qsort(sorted, (size_t)kept, sizeof(sorted[0]), compare_u64);
    printf("RXRESULT style=%s n=%ld expected=%ld missing=%ld bad_stamps=%ld duplicates=%ld foreign=%ld hellos=%ld "
           "rearms=%ld p10_ns=%llu p50_ns=%llu p99_ns=%llu cpu_ns_per=%llu\n",
           style_names[style], kept, tally->rounds, missing, tally->bad_stamps, tally->duplicates, tally->foreign,
           tally->hellos, rearms, kept > 0 ? (unsigned long long)pct(sorted, kept, PCT_10) : 0ULL,
           kept > 0 ? (unsigned long long)pct(sorted, kept, PCT_50) : 0ULL,
           kept > 0 ? (unsigned long long)pct(sorted, kept, PCT_99) : 0ULL,
           kept > 0 ? (unsigned long long)(cpu_ns / (uint64_t)kept) : 0ULL);
    return 0;
}

static int run_rx(enum style style, int port, long rounds, long warmup, int cpu, const char* echo_ip, int echo_port,
                  const char* samples_path) {
    pin(cpu);
    struct receiver rcv;
    const char* why = "";
    if (open_receiver(&rcv, style, port, &why) != 0) {
        printf("REFUSED %s %s: %s\n", style_names[style], why, strerror(errno));
        return EXIT_REFUSED;
    }
    int echo_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in echo_addr;
    memset(&echo_addr, 0, sizeof(echo_addr));
    echo_addr.sin_family = AF_INET;
    echo_addr.sin_port = htons((uint16_t)echo_port);
    if (echo_fd < 0 || inet_pton(AF_INET, echo_ip, &echo_addr.sin_addr) != 1) {
        fprintf(stderr, "bad echo address %s\n", echo_ip);
        return 1;
    }
    struct tally tally;
    memset(&tally, 0, sizeof(tally));
    tally.rounds = rounds;
    tally.warmup = warmup;
    tally.samples = calloc((size_t)rounds, sizeof(uint64_t));
    tally.seen = calloc((size_t)rounds, 1);
    if (tally.samples == NULL || tally.seen == NULL) {
        return 1;
    }
    printf("RXREADY style=%s port=%d rounds=%ld warmup=%ld cpu=%d\n", style_names[style], port, rounds, warmup, cpu);
    fflush(stdout);
    enum take_result result = TAKE_ON;
    while (result == TAKE_ON) {
        result = take_one(&tally, &rcv, echo_fd, &echo_addr);
    }
    if (result == TAKE_FAILED) {
        return 1;
    }
    return report_rx(&tally, style, rcv.rearms, samples_path);
}

// Sends one datagram and waits for its echo. The round trip in ns, or 0 when no echo came within the timeout.
static uint64_t ping(int sock_fd, const struct sockaddr_in* dst, uint32_t seq) {
    uint8_t payload[DATAGRAM_BYTES] = {0};
    struct wire head = {.magic = MAGIC, .seq = seq, .tx_ns = clock_ns(rtt_clock)};
    memcpy(payload, &head, sizeof(head));
    if (sendto(sock_fd, payload, sizeof(payload), 0, (const struct sockaddr*)dst, sizeof(*dst)) < 0) {
        return 0;
    }
    for (;;) {
        struct wire back;
        ssize_t got = recv(sock_fd, &back, sizeof(back), 0);
        uint64_t now = clock_ns(rtt_clock);
        if (got < 0) {
            return 0; // SO_RCVTIMEO: lost
        }
        if (got == (ssize_t)sizeof(back) && back.magic == MAGIC && back.seq == seq) {
            return now > head.tx_ns ? now - head.tx_ns : 1;
        }
        // a late echo of an earlier datagram: wait on (the timeout restarts; bounded by the sender's run anyway)
    }
}

static int run_tx(const char* dst_ip, int port, long rounds, long warmup, int cpu, long gap_us, int own_port) {
    pin(cpu);
    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in own;
    memset(&own, 0, sizeof(own));
    own.sin_family = AF_INET;
    own.sin_port = htons((uint16_t)own_port);
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)port);
    struct timeval timeout = {0, ECHO_TIMEOUT_US}; // NOLINT(misc-include-cleaner) - <sys/socket.h>
    if (sock_fd < 0 || bind(sock_fd, (struct sockaddr*)&own, sizeof(own)) != 0 ||
        // NOLINTNEXTLINE(misc-include-cleaner) - SOL_SOCKET, SO_RCVTIMEO: <sys/socket.h>, above
        setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        inet_pton(AF_INET, dst_ip, &dst.sin_addr) != 1) {
        fprintf(stderr, "tx setup failed: %s\n", strerror(errno));
        return 1;
    }
    int hello = 0;
    for (; hello < HELLO_TRIES; hello++) {
        if (ping(sock_fd, &dst, SEQ_HELLO) != 0) {
            break;
        }
    }
    if (hello == HELLO_TRIES) {
        printf("TXRESULT status=no_receiver\n");
        return 1;
    }
    uint64_t* rtt = calloc((size_t)rounds, sizeof(uint64_t));
    if (rtt == NULL) {
        return 1;
    }
    long answered = 0;
    long lost = 0;
    const struct timespec gap = {gap_us / 1000000L, (gap_us % 1000000L) * (long)NS_PER_US};
    uint64_t start = clock_ns(rtt_clock);
    for (long seq = 0; seq < warmup + rounds; seq++) {
        nanosleep(&gap, NULL);
        uint64_t took = ping(sock_fd, &dst, (uint32_t)seq);
        if (seq < warmup) {
            continue;
        }
        if (took == 0) {
            lost++;
        } else {
            rtt[answered++] = took;
        }
    }
    uint64_t elapsed = clock_ns(rtt_clock) - start;
    for (int i = 0; i < STOP_REPEATS; i++) {
        struct wire stop = {.magic = MAGIC, .seq = SEQ_STOP, .tx_ns = 0};
        (void)sendto(sock_fd, &stop, sizeof(stop), 0, (const struct sockaddr*)&dst, sizeof(dst));
    }
    qsort(rtt, (size_t)answered, sizeof(rtt[0]), compare_u64);
    printf("TXRESULT status=ok sent=%ld answered=%ld lost=%ld hellos=%d elapsed_ms=%llu rtt_p10_ns=%llu "
           "rtt_p50_ns=%llu rtt_p99_ns=%llu\n",
           rounds, answered, lost, hello + 1, elapsed / NS_PER_US / NS_PER_US,
           answered > 0 ? (unsigned long long)pct(rtt, answered, PCT_10) : 0ULL,
           answered > 0 ? (unsigned long long)pct(rtt, answered, PCT_50) : 0ULL,
           answered > 0 ? (unsigned long long)pct(rtt, answered, PCT_99) : 0ULL);
    return 0;
}

static long whole(const char* text, long max) {
    char* end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 0 || value > max) {
        fprintf(stderr, "not a whole number in [0, %ld]: %s\n", max, text);
        exit(EXIT_USAGE);
    }
    return value;
}

// argv positions: <mode> then the mode's arguments, in the order of the usage line.
enum { ARG_MODE = 1, ARG_A, ARG_B, ARG_C, ARG_D, ARG_E, ARG_F, ARG_G, ARG_H, ARGC_RX };
enum { ARGC_TX = ARG_H };

int main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[ARG_MODE], "probe") == 0) {
        return run_probe();
    }
    if (argc == ARGC_RX &&
        strcmp(argv[ARG_MODE], "rx") == 0) { // rx style port rounds warmup cpu echo_ip echo_port file
        int style = parse_style(argv[ARG_A]);
        if (style < 0) {
            fprintf(stderr, "unknown style %s\n", argv[ARG_A]);
            return EXIT_USAGE;
        }
        long rounds = whole(argv[ARG_C], MAX_ROUNDS);
        if (rounds < 1) {
            return EXIT_USAGE;
        }
        return run_rx((enum style)style, (int)whole(argv[ARG_B], MAX_PORT), rounds, whole(argv[ARG_D], MAX_ROUNDS),
                      (int)whole(argv[ARG_E], MAX_CPU), argv[ARG_F], (int)whole(argv[ARG_G], MAX_PORT), argv[ARG_H]);
    }
    if (argc == ARGC_TX && strcmp(argv[ARG_MODE], "tx") == 0) { // tx dst_ip port rounds warmup cpu gap_us own_port
        long rounds = whole(argv[ARG_C], MAX_ROUNDS);
        if (rounds < 1) {
            return EXIT_USAGE;
        }
        return run_tx(argv[ARG_A], (int)whole(argv[ARG_B], MAX_PORT), rounds, whole(argv[ARG_D], MAX_ROUNDS),
                      (int)whole(argv[ARG_E], MAX_CPU), whole(argv[ARG_F], MAX_GAP_US),
                      (int)whole(argv[ARG_G], MAX_PORT));
    }
    fprintf(stderr,
            "usage: %s probe | rcv <style> <port> <rounds> <warmup> <cpu> <echo_ip> <echo_port> <samples> | "
            "tx <dst_ip> <port> <rounds> <warmup> <cpu> <gap_us> <own_port>\n",
            argv[0]);
    return EXIT_USAGE;
}
