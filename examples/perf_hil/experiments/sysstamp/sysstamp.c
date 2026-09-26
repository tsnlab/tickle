// sysstamp: an LD_PRELOAD layer that timestamps a process's socket and wait system calls, identically for
// any rmw implementation, without changing its code (2026-09-26, rmw_tickle/RMW_PERF_PLAN.md section 8).
//
// Joined with packet captures (rmw_pcap_split.py), it splits each side of a round trip at the kernel
// boundary: publish -> send syscall entry (the rmw's user-space transmit path), send entry -> capture tap
// (kernel transmit), tap -> receive syscall return (kernel receive and wake), receive return -> application.
// The user's question: which path, out to the kernel or in from it, holds rmw_tickle's latency.
//
// Recorded per call: entry and return times on CLOCK_REALTIME (the clock tcpdump stamps with, so the two
// line up on one host without any offset), the thread, the call, its return value and, per datagram, the
// first HEAD_BYTES of the payload - which the analysis searches for the Bench sample's send_ns, as it does
// in the pcaps. sendmmsg/recvmmsg give one record per datagram, sharing the call's times.
//
// Use: LD_PRELOAD=/path/libsysstamp.so SYSSTAMP_FILE=/tmp/x <program>  ->  /tmp/x.<pid>, one line per record:
//   tid call t_in_ns t_out_ns ret len hex_head
// Nothing is written for a process that recorded nothing (taskset, /usr/bin/time). The cost is two vDSO clock
// reads and a copy of at most HEAD_BYTES per call; rmw_crosshost_rtt.sh runs an arm without the layer to show
// what it adds.
//
// Build: cc -O2 -shared -fPIC -o libsysstamp.so sysstamp.c -ldl
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming)
#define _GNU_SOURCE
#include <dlfcn.h>
#include <poll.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>

#define HEAD_BYTES 192
#define MAX_RECORDS 65536
#define NS_PER_SEC 1000000000ULL
#define PATH_BYTES 512

enum call {
    CALL_SENDTO,
    CALL_SENDMSG,
    CALL_SENDMMSG,
    CALL_RECVFROM,
    CALL_RECVMSG,
    CALL_RECVMMSG,
    CALL_POLL,
    CALL_PPOLL,
    CALL_EPOLL_WAIT,
    CALL_EPOLL_PWAIT,
    CALL_SELECT,
    CALL_PSELECT,
};
static const char* const call_names[] = {"sendto", "sendmsg", "sendmmsg",   "recvfrom",    "recvmsg", "recvmmsg",
                                         "poll",   "ppoll",   "epoll_wait", "epoll_pwait", "select",  "pselect"};

struct record {
    uint64_t t_in;
    uint64_t t_out;
    int64_t ret;
    uint32_t tid;
    uint16_t call;
    uint16_t len; // bytes copied into head
    uint8_t head[HEAD_BYTES];
};

static struct record* records;
static atomic_size_t next_record;
static atomic_size_t dropped;

static ssize_t (*real_sendto)(int, const void*, size_t, int, const struct sockaddr*, socklen_t);
static ssize_t (*real_sendmsg)(int, const struct msghdr*, int);
static int (*real_sendmmsg)(int, struct mmsghdr*, unsigned int, int);
static ssize_t (*real_recvfrom)(int, void*, size_t, int, struct sockaddr*, socklen_t*);
static ssize_t (*real_recvmsg)(int, struct msghdr*, int);
static int (*real_recvmmsg)(int, struct mmsghdr*, unsigned int, int, struct timespec*);
// The checker maps pollfd, nfds_t, sigset_t and timeval to glibc-private headers; <poll.h> and <sys/select.h> are
// the public ones that provide them.
static int (*real_poll)(struct pollfd*, nfds_t, int); // NOLINT(misc-include-cleaner)
static int (*real_ppoll)(struct pollfd*, nfds_t, const struct timespec*,
                         const sigset_t*); // NOLINT(misc-include-cleaner)
static int (*real_epoll_wait)(int, struct epoll_event*, int, int);
static int (*real_epoll_pwait)(int, struct epoll_event*, int, int, const sigset_t*);
static int (*real_select)(int, fd_set*, fd_set*, fd_set*, struct timeval*); // NOLINT(misc-include-cleaner)
static int (*real_pselect)(int, fd_set*, fd_set*, fd_set*, const struct timespec*, const sigset_t*);

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts); // NOLINT(misc-include-cleaner) - <time.h> is the public header
    return ((uint64_t)ts.tv_sec * NS_PER_SEC) + (uint64_t)ts.tv_nsec;
}

static void* resolve(const char* name) {
    void* func = dlsym(RTLD_NEXT, name);
    if (func == NULL) {
        (void)fprintf(stderr, "sysstamp: %s not found\n", name);
        abort();
    }
    return func;
}

// Resolved on first use as well as in the constructor: a library's own constructor may make a call before
// ours has run.
#define RESOLVE(name)                     \
    do {                                  \
        if (real_##name == NULL) {        \
            real_##name = resolve(#name); \
        }                                 \
    } while (0)

static struct record* claim(uint16_t call, uint64_t t_in, uint64_t t_out, int64_t ret) {
    if (records == NULL) {
        return NULL;
    }
    size_t idx = atomic_fetch_add(&next_record, 1);
    if (idx >= MAX_RECORDS) {
        atomic_fetch_add(&dropped, 1);
        return NULL;
    }
    struct record* rec = &records[idx];
    rec->t_in = t_in;
    rec->t_out = t_out;
    rec->ret = ret;
    rec->tid = (uint32_t)gettid();
    rec->call = call;
    rec->len = 0;
    return rec;
}

// Copies up to HEAD_BYTES of a scatter list, limited to `valid` bytes (what was sent or received).
static void copy_iov(struct record* rec, const struct iovec* iov, size_t iovlen, size_t valid) {
    size_t copied = 0;
    for (size_t i = 0; i < iovlen && copied < HEAD_BYTES && copied < valid; i++) {
        size_t take = iov[i].iov_len;
        if (take > HEAD_BYTES - copied) {
            take = HEAD_BYTES - copied;
        }
        if (take > valid - copied) {
            take = valid - copied;
        }
        memcpy(rec->head + copied, iov[i].iov_base, take);
        copied += take;
    }
    rec->len = (uint16_t)copied;
}

static void copy_flat(struct record* rec, const void* buf, size_t valid) {
    size_t take = valid < HEAD_BYTES ? valid : HEAD_BYTES;
    memcpy(rec->head, buf, take);
    rec->len = (uint16_t)take;
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) - glibc names these __fd etc., reserved
ssize_t sendto(int sock, const void* buf, size_t len, int flags, const struct sockaddr* addr, socklen_t addrlen) {
    RESOLVE(sendto);
    uint64_t t_in = now_ns();
    ssize_t ret = real_sendto(sock, buf, len, flags, addr, addrlen);
    struct record* rec = claim(CALL_SENDTO, t_in, now_ns(), ret);
    if (rec != NULL && ret > 0) {
        copy_flat(rec, buf, (size_t)ret);
    }
    return ret;
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) - glibc names these __fd etc., reserved
ssize_t sendmsg(int sock, const struct msghdr* msg, int flags) {
    RESOLVE(sendmsg);
    uint64_t t_in = now_ns();
    ssize_t ret = real_sendmsg(sock, msg, flags);
    struct record* rec = claim(CALL_SENDMSG, t_in, now_ns(), ret);
    if (rec != NULL && ret > 0) {
        copy_iov(rec, msg->msg_iov, msg->msg_iovlen, (size_t)ret);
    }
    return ret;
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) - glibc names these __fd etc., reserved
int sendmmsg(int sock, struct mmsghdr* vec, unsigned int vlen, int flags) {
    RESOLVE(sendmmsg);
    uint64_t t_in = now_ns();
    int ret = real_sendmmsg(sock, vec, vlen, flags);
    uint64_t t_out = now_ns();
    for (int i = 0; i < ret; i++) {
        struct record* rec = claim(CALL_SENDMMSG, t_in, t_out, ret);
        if (rec != NULL) {
            copy_iov(rec, vec[i].msg_hdr.msg_iov, vec[i].msg_hdr.msg_iovlen, vec[i].msg_len);
        }
    }
    if (ret <= 0) {
        (void)claim(CALL_SENDMMSG, t_in, t_out, ret);
    }
    return ret;
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) - glibc names these __fd etc., reserved
ssize_t recvfrom(int sock, void* buf, size_t len, int flags, struct sockaddr* addr, socklen_t* addrlen) {
    RESOLVE(recvfrom);
    uint64_t t_in = now_ns();
    ssize_t ret = real_recvfrom(sock, buf, len, flags, addr, addrlen);
    struct record* rec = claim(CALL_RECVFROM, t_in, now_ns(), ret);
    if (rec != NULL && ret > 0) {
        copy_flat(rec, buf, (size_t)ret);
    }
    return ret;
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) - glibc names these __fd etc., reserved
ssize_t recvmsg(int sock, struct msghdr* msg, int flags) {
    RESOLVE(recvmsg);
    uint64_t t_in = now_ns();
    ssize_t ret = real_recvmsg(sock, msg, flags);
    struct record* rec = claim(CALL_RECVMSG, t_in, now_ns(), ret);
    if (rec != NULL && ret > 0) {
        copy_iov(rec, msg->msg_iov, msg->msg_iovlen, (size_t)ret);
    }
    return ret;
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) - glibc names these __fd etc., reserved
int recvmmsg(int sock, struct mmsghdr* vec, unsigned int vlen, int flags, struct timespec* timeout) {
    RESOLVE(recvmmsg);
    uint64_t t_in = now_ns();
    int ret = real_recvmmsg(sock, vec, vlen, flags, timeout);
    uint64_t t_out = now_ns();
    for (int i = 0; i < ret; i++) {
        struct record* rec = claim(CALL_RECVMMSG, t_in, t_out, ret);
        if (rec != NULL) {
            copy_iov(rec, vec[i].msg_hdr.msg_iov, vec[i].msg_hdr.msg_iovlen, vec[i].msg_len);
        }
    }
    if (ret <= 0) {
        (void)claim(CALL_RECVMMSG, t_in, t_out, ret);
    }
    return ret;
}

int poll(struct pollfd* fds, nfds_t nfds, int timeout) {
    RESOLVE(poll);
    uint64_t t_in = now_ns();
    int ret = real_poll(fds, nfds, timeout);
    (void)claim(CALL_POLL, t_in, now_ns(), ret);
    return ret;
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) - glibc names these __fd etc., reserved
int ppoll(struct pollfd* fds, nfds_t nfds, const struct timespec* timeout, const sigset_t* sigmask) {
    RESOLVE(ppoll);
    uint64_t t_in = now_ns();
    int ret = real_ppoll(fds, nfds, timeout, sigmask);
    (void)claim(CALL_PPOLL, t_in, now_ns(), ret);
    return ret;
}

int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout) {
    RESOLVE(epoll_wait);
    uint64_t t_in = now_ns();
    int ret = real_epoll_wait(epfd, events, maxevents, timeout);
    (void)claim(CALL_EPOLL_WAIT, t_in, now_ns(), ret);
    return ret;
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) - glibc names these __fd etc., reserved
int epoll_pwait(int epfd, struct epoll_event* events, int maxevents, int timeout, const sigset_t* sigmask) {
    RESOLVE(epoll_pwait);
    uint64_t t_in = now_ns();
    int ret = real_epoll_pwait(epfd, events, maxevents, timeout, sigmask);
    (void)claim(CALL_EPOLL_PWAIT, t_in, now_ns(), ret);
    return ret;
}

int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, struct timeval* timeout) {
    RESOLVE(select);
    uint64_t t_in = now_ns();
    int ret = real_select(nfds, readfds, writefds, exceptfds, timeout);
    (void)claim(CALL_SELECT, t_in, now_ns(), ret);
    return ret;
}

int pselect(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, const struct timespec* timeout,
            const sigset_t* sigmask) {
    RESOLVE(pselect);
    uint64_t t_in = now_ns();
    int ret = real_pselect(nfds, readfds, writefds, exceptfds, timeout, sigmask);
    (void)claim(CALL_PSELECT, t_in, now_ns(), ret);
    return ret;
}

__attribute__((constructor)) static void sysstamp_init(void) {
    void* mem =
        mmap(NULL, sizeof(struct record) * MAX_RECORDS, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem != MAP_FAILED) {
        records = mem;
    }
}

__attribute__((destructor)) static void sysstamp_dump(void) {
    size_t count = atomic_load(&next_record);
    if (records == NULL || count == 0) {
        return;
    }
    if (count > MAX_RECORDS) {
        count = MAX_RECORDS;
    }
    const char* base = getenv("SYSSTAMP_FILE");
    if (base == NULL) {
        return;
    }
    char path[PATH_BYTES];
    (void)snprintf(path, sizeof(path), "%s.%d", base, (int)getpid());
    FILE* out = fopen(path, "w");
    if (out == NULL) {
        return;
    }
    (void)fprintf(out, "# sysstamp pid %d records %zu dropped %zu\n", (int)getpid(), count, atomic_load(&dropped));
    for (size_t i = 0; i < count; i++) {
        const struct record* rec = &records[i];
        (void)fprintf(out, "%u %s %llu %llu %lld %u ", rec->tid, call_names[rec->call], (unsigned long long)rec->t_in,
                      (unsigned long long)rec->t_out, (long long)rec->ret, rec->len);
        for (unsigned j = 0; j < rec->len; j++) {
            (void)fprintf(out, "%02x", rec->head[j]);
        }
        (void)fputc('\n', out);
    }
    (void)fclose(out);
}
