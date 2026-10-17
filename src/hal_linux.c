/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// ppoll() (tt_receive(), below) is a GNU/Linux extension - glibc's <poll.h> only declares it when
// this is defined (or _DEFAULT_SOURCE, which the same feature-test-macros(7) family covers too;
// spelled out explicitly here rather than relying on whatever default dialect happens to define
// it, since that's exactly the kind of implicit dependency this whole file's own build shouldn't
// need to guess about). The name/spelling is glibc's own feature-test-macro convention
// (feature_test_macros(7)), not ours to rename - not tt_-prefixed, so this project's own
// bugprone-reserved-identifier/readability-identifier-naming allowlists (.clang-tidy) don't cover
// it; genuinely a reserved identifier, by design, by the C standard's own rules.
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming)
#define _GNU_SOURCE

#include <errno.h>
// fcntl(O_NONBLOCK): the sockets' mode (sockets_nonblocking()), in every build; and open() for the context-id registry
// and the segment, fcntl(F_DUPFD_CLOEXEC) for tt_tx_await_writable()'s watch.
#include <fcntl.h>
#include <ifaddrs.h>
#include <poll.h>
#include <stddef.h> // offsetof
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h> // getenv(): TT_UDP_OFFLOAD
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/timerfd.h> // the wait's deadline (rx_wait_lazy())
// struct iovec (tt_send_iov, below) - clang-tidy's IWYU mapping doesn't know this glibc symbol's
// real (portable, POSIX-specified) home, so it flags both this include and the struct itself as
// if neither provided/needed the other; this is the correct, portable header regardless (see
// readv(2)/writev(2)/sendmsg(2)).
#include <linux/sock_diag.h> // SK_MEMINFO_*: socket_drops()
#include <sys/uio.h>         // NOLINT(misc-include-cleaner)
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/hal_linux.h> // tt_RX_BATCH, struct tt_mmsghdr
// SYS_*: the hot path's system calls (tt_sys6()) and io_uring's; the numbers live in an architecture's own header.
#include <sys/syscall.h> // NOLINT(misc-include-cleaner)
#if tt_HAL_IO_URING      // after hal_linux.h, which derives it from tt_HAL_RX_HINT
// __NR_io_uring_*: <sys/syscall.h> is the portable spelling; the number itself lives in an architecture's own header
// (asm/unistd_64.h on x86-64, asm-generic/unistd.h on arm64), which include-cleaner would have us name instead.
#include <linux/io_uring.h> // tt_rx_maybe_ready()
#if tt_RX_DRAIN
#include <linux/time_types.h> // __kernel_timespec: the drain's stop wait
#include <sys/utsname.h>      // uname(): the kernel release a refused buffer ring is reported with
#endif
#endif
#if TT_HAL_UDP_GSO || TT_HAL_UDP_GRO // after hal_linux.h, which derives both: a build with neither uses none of it
#include <netinet/udp.h>             // SOL_UDP, UDP_GRO, UDP_SEGMENT
#endif
#include <tickle/tickle.h>
#include <tickle/trace.h>

#include "consts.h"
#include "log.h"

// Both /dev/shm users need these - the context-id registry and the segment - and they are
// independent settings, so the guard is the disjunction. Written this way after clang-tidy caught
// the duplicate: the segment's own include of <sys/stat.h> beside the registry's was not only
// redundant, it hid that a build with the registry off and the segment on would have lost them.
#if tt_CONTEXT_ID_CLAIM || tt_SEGMENT_ENABLED
#include <sys/file.h> // flock(): the registry's lock, and the segment owner's
#include <sys/stat.h> // fchmod(), fstat()
#endif

#if tt_CONTEXT_ID_CLAIM
#include <signal.h> // kill()

#include <sys/types.h> // pid_t
#endif

#include <sys/prctl.h> // PR_SET_TIMERSLACK (rx_timer_slack_once())

// The segment's mapping (SHM_PLAN.md stage 1) and io_uring's rings are both mmap()ed.
#if tt_SEGMENT_ENABLED || tt_HAL_IO_URING
#include <sys/mman.h>
#endif

// TT_RX_DROP_PERCENT - receive-side loss injection for experiments, 0 (off) by default and not
// something a deployment ever sets.
//
// Exists because the ordering work is only observable when it is stressed: with no loss, nothing
// arrives out of order, the reorder buffer never holds anything, and reorder_held_peak == 0 reads
// identically to a buffer that is not wired up at all.
//
// Injected here rather than with `tc netem loss` on lo, and the reason is not that tc needs root
// on this box (it does). tc on the loopback interface hits *every* loopback flow on the machine -
// another session's benchmark, anything else measuring at the same time - as loss it never asked
// for and cannot see. This drops only datagrams this process received, so it cannot reach past
// the experiment, needs no privilege, and leaves no global state that a crash could strand in a
// bad configuration. The first HIL sweep left tc at 20% loss when it was killed mid-run and would
// have silently corrupted every later measurement including CI's; the fix for that was a cleanup
// trap, but not needing the global state at all is better than cleaning it up reliably.
//
// Deterministic by default (a fixed seed) so two arms see the same drop pattern and differ only
// in what they do about it.
#ifndef TT_RX_DROP_PERCENT
#define TT_RX_DROP_PERCENT 0
#endif
#ifndef TT_RX_DROP_SEED
#define TT_RX_DROP_SEED 20260924u
#endif

#if TT_RX_DROP_PERCENT > 0
// xorshift32 rather than rand(): self-contained, identical across libc versions, and it cannot
// perturb an application that seeded rand() for its own purposes.
static uint32_t tt_rx_drop_state = TT_RX_DROP_SEED;

static bool tt_rx_should_drop(void) {
    tt_rx_drop_state ^= tt_rx_drop_state << 13;
    tt_rx_drop_state ^= tt_rx_drop_state >> 17;
    tt_rx_drop_state ^= tt_rx_drop_state << 5;
    return (tt_rx_drop_state % 100u) < (uint32_t)TT_RX_DROP_PERCENT;
}
#endif

// TT_RX_FIXED_PREFERENCE - an experiment arm for the "Data consistency violated" abort, off by
// default and not a knob anyone should turn in a deployment.
//
// A node reads two sockets: broadcasts land on the well-known one, unicast on its own data socket.
// Whichever is ready is read, and when both are they alternate. A stream that is half broadcast
// and half unicast - which is what a Publisher crossing tt_UNICAST_PEER_THRESHOLD mid-run produces
// - is therefore interleaved by the reader, and an alternating reader can take a later unicast
// before an earlier broadcast that was already waiting. That is a candidate cause of samples
// reaching the application out of order.
//
// Defining this to 1 pins the well-known socket first instead, which removes the interleaving and
// with it that candidate. It is a discriminator, not a fix: the alternation exists because a fixed
// preference does not merely delay the other socket but starves it outright under a sustained
// stream, and what starves is RELIABLE recovery (ACKNACKs are unicast while a Publisher over the
// threshold broadcasts its data). If the abort survives this arm, the interleaving is not the
// cause and the alternation keeps its reason for existing either way.
#ifndef TT_RX_FIXED_PREFERENCE
#define TT_RX_FIXED_PREFERENCE 0
#endif

// ---- The hot path's system calls without the C library's wrapper (2026-10-15)
// ------------------------------------------------------------------------------------
//
// glibc's sendto(), sendmsg(), recvfrom(), recvmmsg() and epoll_pwait2() are cancellation points: in a process with a
// second thread (a context with a receive drain, an rmw node) each goes through __syscall_cancel(), some 45 user
// instructions on x86-64 around the system call itself, for a pthread_cancel() no TickLE thread is ever sent. A small
// sample's round trip makes four or five of them on each side. Here they are made with the instruction itself
// (tt_sys6()) on x86-64 and aarch64: the same system call with the same arguments, its result returned and a failure's
// errno set as the library sets them. Elsewhere, under a sanitizer (its interceptors see the library's calls only),
// and in a build with TT_HAL_LIBC_CALLS (rx_direct_check, which puts its own sendto() and sendmsg() in front of the
// library's to refuse a send), the library's own.
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define TT_HAL_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define TT_HAL_SANITIZED 1
#endif
#if (defined(__x86_64__) || defined(__aarch64__)) && !defined(TT_HAL_SANITIZED) && !defined(TT_HAL_LIBC_CALLS)
#define TT_HAL_RAW_CALLS 1
#else
#define TT_HAL_RAW_CALLS 0
#endif

#if TT_HAL_RAW_CALLS
// The system call `number` with six arguments: its result, or -errno. The kernel's entry keeps every register but the
// result's (and on x86-64 rcx and r11, which the instruction itself overwrites).
static inline long tt_sys6(long number, long arg1, long arg2, long arg3, long arg4, long arg5, long arg6) {
#if defined(__x86_64__)
    long ret = 0;
    register long reg_r10 __asm__("r10") = arg4;
    register long reg_r8 __asm__("r8") = arg5;
    register long reg_r9 __asm__("r9") = arg6;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "0"(number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(reg_r10), "r"(reg_r8), "r"(reg_r9)
                     : "rcx", "r11", "memory");
    return ret;
#else
    register long reg_x8 __asm__("x8") = number;
    register long reg_x0 __asm__("x0") = arg1;
    register long reg_x1 __asm__("x1") = arg2;
    register long reg_x2 __asm__("x2") = arg3;
    register long reg_x3 __asm__("x3") = arg4;
    register long reg_x4 __asm__("x4") = arg5;
    register long reg_x5 __asm__("x5") = arg6;
    __asm__ volatile("svc 0"
                     : "+r"(reg_x0)
                     : "r"(reg_x8), "r"(reg_x1), "r"(reg_x2), "r"(reg_x3), "r"(reg_x4), "r"(reg_x5)
                     : "memory");
    return reg_x0;
#endif
}

// A result as the library returns it: -1 with errno for a failure (-4095..-1), the value otherwise.
#define TT_SYS_ERRNO_MAX 4095L // the kernel's MAX_ERRNO: a result from -4095 to -1 is a failure
static inline long tt_sys_result(long ret) {
    if (__builtin_expect(ret < 0 && ret >= -TT_SYS_ERRNO_MAX, 0)) {
        errno = (int)-ret;
        return -1;
    }
    return ret;
}
#endif

static inline ssize_t hal_sendto(int socket_fd, const void* buf, size_t len, int flags, const struct sockaddr* dest,
                                 socklen_t dest_len) {
#if TT_HAL_RAW_CALLS
    return tt_sys_result(tt_sys6(SYS_sendto, socket_fd, (long)buf, (long)len, flags, (long)dest, (long)dest_len));
#else
    return sendto(socket_fd, buf, len, flags, dest, dest_len);
#endif
}

static inline ssize_t hal_sendmsg(int socket_fd, const struct msghdr* msg, int flags) {
#if TT_HAL_RAW_CALLS
    return tt_sys_result(tt_sys6(SYS_sendmsg, socket_fd, (long)msg, flags, 0, 0, 0));
#else
    return sendmsg(socket_fd, msg, flags);
#endif
}

static inline ssize_t hal_recvfrom(int socket_fd, void* buf, size_t len, int flags, struct sockaddr* from,
                                   socklen_t* from_len) {
#if TT_HAL_RAW_CALLS
    return tt_sys_result(tt_sys6(SYS_recvfrom, socket_fd, (long)buf, (long)len, flags, (long)from, (long)from_len));
#else
    return recvfrom(socket_fd, buf, len, flags, from, from_len);
#endif
}

static inline int hal_recvmmsg(int socket_fd, struct mmsghdr* msgs, unsigned int count, int flags) {
#if TT_HAL_RAW_CALLS
    return (int)tt_sys_result(tt_sys6(SYS_recvmmsg, socket_fd, (long)msgs, (long)count, flags, 0, 0));
#else
    return recvmmsg(socket_fd, msgs, count, flags, NULL);
#endif
}

#define SEC_NS 1000000000LL

struct _tt_Config _tt_CONFIG = {
    .addr = _tt_CONTEXT_ADDRESS,
    .port = _tt_CONTEXT_PORT,
    .broadcast = _tt_CONTEXT_BROADCAST,
    .context_id = tt_CONTEXT_ID_INVALID, // auto-detect by default; see its own comment in config.h
    .large_fragmentation = tt_LARGE_FRAGMENTATION_AUTO,
    .ip_reassembly_max = _tt_IP_REASSEMBLY_MAX,
};

// Every deadline and duration: CLOCK_MONOTONIC, which a step of the date (NTP, settimeofday()) does not move. Like
// CLOCK_REALTIME it is read in the vDSO, without a system call.
uint64_t tt_get_ns(void) {
    struct timespec ts;
    // CLOCK_MONOTONIC lives in a glibc-private header; <time.h> (included above) is the correct public header.
    // NOLINTNEXTLINE(misc-include-cleaner)
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return ((uint64_t)ts.tv_sec * SEC_NS) + ts.tv_nsec;
}

// The wire's timestamps and the per-launch identity draws: the date, which hosts agree on (NTP/PTP).
uint64_t tt_get_wall_ns(void) {
    struct timespec ts;
    // NOLINTNEXTLINE(misc-include-cleaner) - CLOCK_REALTIME, as CLOCK_MONOTONIC above
    clock_gettime(CLOCK_REALTIME, &ts);

    return ((uint64_t)ts.tv_sec * SEC_NS) + ts.tv_nsec;
}

// clock_getres() of the clock tt_get_ns() reads. ppoll() and epoll_pwait2() wait on high-resolution timers, which
// end no more finely than that clock can tell; how much LATER they end (the timer slack, the scheduler) is what the
// context measures on top of this floor.
uint64_t tt_timer_resolution_ns(void) {
    struct timespec res;
    // NOLINTNEXTLINE(misc-include-cleaner) - CLOCK_MONOTONIC, as tt_get_ns()
    if (clock_getres(CLOCK_MONOTONIC, &res) != 0) {
        return 1;
    }
    uint64_t resolution_ns = ((uint64_t)res.tv_sec * SEC_NS) + (uint64_t)res.tv_nsec;
    return resolution_ns != 0 ? resolution_ns : 1;
}

int32_t tt_get_node_id(void) {
    // Get unique node ID in the network using IP address x.x.x.id
    uint32_t broadcast_ip = inet_addr(_tt_CONFIG.broadcast);

    struct ifaddrs* ifaddrs;
    if (getifaddrs(&ifaddrs) != 0) {
        TT_LOG_ERROR("Cannot get network interfaces: %s", strerror(errno));
        return -1;
    }

    uint8_t node_id = 0;

    struct ifaddrs* ifaddr = ifaddrs;
    while (ifaddr != NULL) {
        if (ifaddr->ifa_addr != NULL && ifaddr->ifa_netmask != NULL && ifaddr->ifa_addr->sa_family == AF_INET &&
            ifaddr->ifa_netmask->sa_family == AF_INET) {
            uint32_t addr = ((struct sockaddr_in*)ifaddr->ifa_addr)->sin_addr.s_addr;
            uint32_t netmask = ((struct sockaddr_in*)ifaddr->ifa_netmask)->sin_addr.s_addr;

            if (node_id == 0) {
                node_id = addr >> 24;
            } else if ((addr & netmask) == (broadcast_ip & netmask)) {
                node_id = (addr & ~netmask) >> BITS_IN_3BYTES;
            }
        }
        ifaddr = ifaddr->ifa_next;
    }

    freeifaddrs(ifaddrs);

    return node_id;
}

bool tt_resolve_link(const char* broadcast, uint32_t* addr, uint32_t* netmask, uint32_t* bcast) {
    if (broadcast == NULL || addr == NULL || netmask == NULL || bcast == NULL) {
        return false;
    }
    uint32_t want = ntohl(inet_addr(broadcast));
    // Reported even when no interface owns it, so a caller can still address the link. The return
    // value says whether *addr and *netmask are meaningful; *bcast is always the parsed string.
    *bcast = want;

    struct ifaddrs* ifaddrs = NULL;
    if (getifaddrs(&ifaddrs) != 0) {
        TT_LOG_ERROR("Cannot get network interfaces: %s", strerror(errno));
        return false;
    }

    bool found = false;
    for (struct ifaddrs* ifaddr = ifaddrs; ifaddr != NULL && !found; ifaddr = ifaddr->ifa_next) {
        if (ifaddr->ifa_addr == NULL || ifaddr->ifa_netmask == NULL || ifaddr->ifa_addr->sa_family != AF_INET ||
            ifaddr->ifa_netmask->sa_family != AF_INET) {
            continue;
        }
        uint32_t if_addr = ntohl(((struct sockaddr_in*)ifaddr->ifa_addr)->sin_addr.s_addr);
        uint32_t if_mask = ntohl(((struct sockaddr_in*)ifaddr->ifa_netmask)->sin_addr.s_addr);
        // Computed rather than read from ifa_broadaddr: that field is only valid when IFF_BROADCAST
        // is set, and a point-to-point interface reuses the same union member for its destination
        // address. The directed broadcast is (addr | ~netmask) by definition either way.
        uint32_t if_bcast = if_addr | ~if_mask;
        if (if_bcast != want) {
            continue;
        }
        *addr = if_addr;
        *netmask = if_mask;
        found = true;
    }

    freeifaddrs(ifaddrs);
    return found;
}

int32_t tt_link_mtu(uint32_t addr) {
    struct ifaddrs* ifaddrs = NULL;
    if (getifaddrs(&ifaddrs) != 0) {
        return -1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    bool found = false;
    for (struct ifaddrs* ifaddr = ifaddrs; ifaddr != NULL && !found; ifaddr = ifaddr->ifa_next) {
        if (ifaddr->ifa_addr == NULL || ifaddr->ifa_addr->sa_family != AF_INET ||
            ntohl(((struct sockaddr_in*)ifaddr->ifa_addr)->sin_addr.s_addr) != addr) {
            continue;
        }
        snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifaddr->ifa_name);
        found = true;
    }
    freeifaddrs(ifaddrs);
    if (!found) {
        return -1;
    }
    int query_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (query_fd < 0) {
        return -1;
    }
    // SIOCGIFMTU: glibc defines it in bits/ioctls.h, reached through <sys/ioctl.h> above
    int32_t mtu = ioctl(query_fd, SIOCGIFMTU, &ifr) == 0 ? (int32_t)ifr.ifr_mtu : -1; // NOLINT(misc-include-cleaner)
    close(query_fd);
    return mtu;
}

// Says what receive buffer the kernel actually granted, which is rarely what was asked for: an
// unprivileged setsockopt() is clamped to net.core.rmem_max (208 KB on many distros; the kernel
// then reports double what it keeps). Worth a warning only when the result holds few of this
// build's largest datagrams - the case rmw_tickle's 65507-byte buffer (2026-09-24) makes real,
// where a burst of full-size samples overflows the socket and is lost below TickLE, invisibly.
// Plan's review: name the sysctl, because nothing else will tell the user it is the limit.
#define tt_RCVBUF_WARN_DATAGRAMS 16
static void report_receive_buffer(int sock, const char* which) {
    int granted = 0;
    socklen_t length = sizeof(granted);
    // NOLINTNEXTLINE(misc-include-cleaner) - same as the setsockopt() calls below
    if (getsockopt(sock, SOL_SOCKET, SO_RCVBUF, (void*)&granted, &length) < 0) {
        return;
    }
    if ((long)granted < (long)tt_RCVBUF_WARN_DATAGRAMS * tt_MAX_BUFFER_LENGTH) {
        TT_LOG_WARNING("%s socket receive buffer is %d bytes (asked %d) - fewer than %d datagrams of %d bytes; raise "
                       "net.core.rmem_max to let it grow",
                       which, granted, (int)tt_SOCKET_BUFFER_SIZE, tt_RCVBUF_WARN_DATAGRAMS, tt_MAX_BUFFER_LENGTH);
    } else {
        TT_LOG_DEBUG("%s socket receive buffer is %d bytes (asked %d)", which, granted, (int)tt_SOCKET_BUFFER_SIZE);
    }
}

// Datagrams the kernel dropped at one socket because its receive buffer was full (SO_MEMINFO's SK_MEMINFO_DROPS, Linux
// 4.6), or 0 when it cannot say. The only witness of where a datagram the sender sent and the receiver never read was
// lost: rig cell I4s (RESULTS L4) lost 14-38 4 MB samples a run on the subscriber's side, with nothing in the run that
// could say whether the socket, the NIC or the wire took them.
static uint32_t socket_drops(int sock) {
    if (sock < 0) {
        return 0;
    }
    uint32_t info[SK_MEMINFO_VARS];
    socklen_t length = sizeof(info);
    memset(info, 0, sizeof(info));
    // NOLINTNEXTLINE(misc-include-cleaner) - SO_MEMINFO: same glibc-private header as the setsockopt() calls below
    if (getsockopt(sock, SOL_SOCKET, SO_MEMINFO, (void*)info, &length) < 0 ||
        length < (socklen_t)(sizeof(uint32_t) * (SK_MEMINFO_DROPS + 1))) {
        return 0;
    }
    return info[SK_MEMINFO_DROPS];
}

uint32_t tt_hal_rx_socket_drops(const struct tt_Context* node) {
    return socket_drops(node->hal.sock) + socket_drops(node->hal.data_sock);
}

uint32_t tt_rx_receiver_drops(const struct tt_Context* node) {
#if tt_RX_DRAIN
    return tt_hal_rx_socket_drops(node) + (uint32_t)node->hal.drain_truncated;
#else
    return tt_hal_rx_socket_drops(node);
#endif
}

#if tt_CONTEXT_ID_CLAIM
// ---- (g8, config.h's tt_CONTEXT_ID_CLAIM) context ids of their own for several contexts on one host.

static bool pid_alive(int32_t pid) {
    if (pid <= 0) {
        return false;
    }
    // EPERM (another user's process) or a pid from another namespace cannot be checked: counted alive, and the link
    // settles it if that was wrong.
    return kill((pid_t)pid, 0) == 0 || errno != ESRCH;
}

static bool id_avoided(const uint8_t* avoid, uint32_t id) {
    return avoid != NULL && ((avoid[id / 8] >> (id % 8)) & 1U) != 0;
}

// The id to take, given which are held: `preferred` if free, else the highest free one, or with `salt` the free one
// it picks. 0: none free.
static uint8_t pick_context_id(const bool held[tt_MAX_CONTEXT_IDS], const uint8_t* avoid, uint8_t preferred,
                               uint32_t salt) {
    if (preferred > tt_CONTEXT_ID_INVALID && preferred < tt_CONTEXT_ID_BROADCAST && !held[preferred] &&
        !id_avoided(avoid, preferred)) {
        return preferred;
    }
    uint32_t free_count = 0;
    for (uint32_t id = tt_CONTEXT_ID_BROADCAST - 1; id > tt_CONTEXT_ID_INVALID; id--) {
        free_count += !held[id] && !id_avoided(avoid, id) ? 1U : 0U;
    }
    if (free_count == 0) {
        return tt_CONTEXT_ID_INVALID;
    }
    uint32_t skip = salt != 0 ? salt % free_count : 0;
    for (uint32_t id = tt_CONTEXT_ID_BROADCAST - 1; id > tt_CONTEXT_ID_INVALID; id--) {
        if (!held[id] && !id_avoided(avoid, id) && skip-- == 0) {
            return (uint8_t)id;
        }
    }
    return tt_CONTEXT_ID_INVALID;
}

// Every user's contexts share the registry, so it is readable and writable by all: it decides nothing but which free
// id a context prefers, and the link settles any misuse.
#define REGISTRY_MODE 0666

// The registry file, open and locked, or -1 (no path, or it cannot be used).
static int lock_registry(const char* path) {
    if (path == NULL) {
        return -1;
    }
    int registry_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, REGISTRY_MODE);
    if (registry_fd < 0) {
        return -1;
    }
    (void)fchmod(registry_fd, REGISTRY_MODE); // every user's contexts share it; the umask would narrow it
    if (flock(registry_fd, LOCK_EX) != 0) {
        (void)close(registry_fd);
        return -1;
    }
    return registry_fd;
}

static void unlock_registry(int registry_fd) {
    (void)flock(registry_fd, LOCK_UN);
    (void)close(registry_fd);
}

uint8_t tt_id_registry_claim(const char* path, uint8_t preferred, const uint8_t* avoid, uint32_t salt, int32_t pid) {
    bool held[tt_MAX_CONTEXT_IDS] = {false};
    int32_t pids[tt_MAX_CONTEXT_IDS] = {0};
    int registry_fd = lock_registry(path);
    if (registry_fd < 0) {
        return pick_context_id(held, avoid, preferred, salt);
    }
    // A short file reads as zeros past its end: those ids are free.
    if (pread(registry_fd, pids, sizeof(pids), 0) < 0) {
        memset(pids, 0, sizeof(pids));
    }
    for (uint32_t id = 0; id < tt_MAX_CONTEXT_IDS; id++) {
        held[id] = pid_alive(pids[id]);
    }
    uint8_t id = pick_context_id(held, avoid, preferred, salt);
    if (id != tt_CONTEXT_ID_INVALID &&
        pwrite(registry_fd, &pid, sizeof(pid), (off_t)id * (off_t)sizeof(int32_t)) != (ssize_t)sizeof(pid)) {
        TT_LOG_WARNING("Cannot record context id %u in %s: %s", id, path, strerror(errno));
    }
    unlock_registry(registry_fd);
    return id;
}

void tt_id_registry_release(const char* path, uint8_t id, int32_t pid) {
    int registry_fd = lock_registry(path);
    if (registry_fd < 0) {
        return;
    }
    int32_t holder = 0;
    if (pread(registry_fd, &holder, sizeof(holder), (off_t)id * (off_t)sizeof(int32_t)) == (ssize_t)sizeof(holder) &&
        holder == pid) {
        int32_t none = 0;
        if (pwrite(registry_fd, &none, sizeof(none), (off_t)id * (off_t)sizeof(int32_t)) != (ssize_t)sizeof(none)) {
            TT_LOG_WARNING("Cannot release context id %u in %s: %s", id, path, strerror(errno));
        }
    }
    unlock_registry(registry_fd);
}

// One registry per well-known port and link, in /dev/shm: two network namespaces with different addresses share
// /dev/shm but never an address. TICKLE_ID_REGISTRY names another directory, or "off" for none.
static const char* registry_path(char* buf, size_t size) {
    const char* dir = getenv("TICKLE_ID_REGISTRY");
    if (dir != NULL && strcmp(dir, "off") == 0) {
        return NULL;
    }
    int written = snprintf(buf, size, "%s/tickle-context-ids-%d-%s-%s", dir != NULL ? dir : "/dev/shm", _tt_CONFIG.port,
                           _tt_CONFIG.addr, _tt_CONFIG.broadcast);
    return written > 0 && (size_t)written < size ? buf : NULL;
}

#define REGISTRY_PATH_LENGTH 256

uint8_t tt_claim_context_id(struct tt_Context* node, uint8_t preferred, const uint8_t* avoid, uint32_t salt) {
    char path[REGISTRY_PATH_LENGTH];
    const char* registry = registry_path(path, sizeof(path));
    int32_t pid = (int32_t)getpid();
    uint8_t id = tt_id_registry_claim(registry, preferred, avoid, salt, pid);
    if (id != tt_CONTEXT_ID_INVALID) {
        if (node->hal.claimed_id != tt_CONTEXT_ID_INVALID && node->hal.claimed_id != id) {
            tt_id_registry_release(registry, node->hal.claimed_id, pid);
        }
        node->hal.claimed_id = id;
    }
    return id;
}

// A context without an id of its own sends nothing: each send fails, and is counted (tt_Context.id_muted_drops).
static int32_t refuse_muted_send(struct tt_Context* node) {
    node->id_muted_drops++;
    errno = ENETDOWN;
    return -1;
}

static void release_claimed_id(struct tt_Context* node) {
    if (node->hal.claimed_id == tt_CONTEXT_ID_INVALID) {
        return;
    }
    char path[REGISTRY_PATH_LENGTH];
    tt_id_registry_release(registry_path(path, sizeof(path)), node->hal.claimed_id, (int32_t)getpid());
    node->hal.claimed_id = tt_CONTEXT_ID_INVALID;
}

// The source address the kernel puts on a datagram this host sends to `destination`, host order, or 0 when no route
// reaches it. A connected UDP socket is given exactly that address by the same route lookup sendto() makes, and
// connecting sends nothing.
static uint32_t route_source_address(const struct sockaddr_in* destination) {
    int probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe < 0) {
        return 0;
    }
    uint32_t source = 0;
    int optval = 1;
    struct sockaddr_in chosen;
    socklen_t length = sizeof(chosen);
    // SO_BROADCAST first: connect() to a broadcast address is refused with EACCES without it, as sendto() would be.
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (setsockopt(probe, SOL_SOCKET, SO_BROADCAST, (const void*)&optval, sizeof(optval)) == 0 &&
        connect(probe, (const struct sockaddr*)destination, sizeof(*destination)) == 0 &&
        getsockname(probe, (struct sockaddr*)&chosen, &length) == 0) {
        source = ntohl(chosen.sin_addr.s_addr);
    }
    close(probe);
    return source;
}

// The data socket's address, read back, as a peer sees it on what this context sends - same-host discovery
// (tickle.c, note_same_host_peer()) compares a peer's address with it, and the shared-memory segment is named from it.
// Bound to a specific address, it is that one. Bound to any address, it is the link's when an interface owns the
// configured broadcast, and otherwise - the limited broadcast 255.255.255.255, which no interface owns and which is
// rmw_tickle's default - the source address the kernel chooses for that broadcast, which is what every datagram
// tt_send() sends carries. Until 2026-10-06 that last case was left at 0: the context never knew its own address,
// treated no peer as same-host, and on a default configuration never used shared memory at all.
static void record_own_address(struct tt_Context* node) {
    struct sockaddr_in bound;
    socklen_t length = sizeof(bound);
    node->hal.own_ip = 0;
    node->hal.own_port = 0;
    if (getsockname(node->hal.data_sock, (struct sockaddr*)&bound, &length) == 0) {
        node->hal.own_ip = ntohl(bound.sin_addr.s_addr);
        node->hal.own_port = ntohs(bound.sin_port);
    }
    uint32_t addr = 0;
    uint32_t netmask = 0;
    uint32_t bcast = 0;
    if (node->hal.own_ip == 0 && tt_resolve_link(_tt_CONFIG.broadcast, &addr, &netmask, &bcast)) {
        node->hal.own_ip = addr;
    }
    if (node->hal.own_ip == 0) {
        node->hal.own_ip = route_source_address(&node->hal.broadcast_addr);
    }
}

#define LOOPBACK_NET 127U
#define BITS_IN_3BYTES_HOST 24U

bool tt_is_own_address(const struct tt_Context* node, uint32_t ip, uint16_t port) {
    // Two contexts on one host never share a port; on one address, a port is enough. The address only tells this
    // host's own loopback copy, or the link's, from another host whose context took the same port number.
    return port == node->hal.own_port &&
           (ip == node->hal.own_ip || node->hal.own_ip == 0 || (ip >> BITS_IN_3BYTES_HOST) == LOOPBACK_NET);
}

void tt_own_address(const struct tt_Context* node, uint32_t* ip, uint16_t* port) {
    *ip = node->hal.own_ip;
    *port = node->hal.own_port;
}
#endif

#if tt_HAL_IO_URING
static bool uring_setup(struct tt_Context* node);
static void uring_close(struct tt_Context* node);
#endif

// The wait set (ROADMAP "Now" 4, 2026-10-06). A same-host reader at a low rate sleeps and is woken once per sample,
// and ppoll() made every one of those sleeps register on, and then leave, the wait queue of each descriptor in its set
// - both sockets, the wake eventfd and the bell - and ask each for its state: perf on the PC put that machinery at
// about a third of a reader's ppoll() outside the sleep itself. The set never changes between sleeps, so it is kept in
// the kernel, registered once, and a sleep is one epoll_pwait2(). Sockets and the eventfd stay level-triggered, so
// what tt_receive() reports is exactly what ppoll() reported; the bell is edge-triggered (tt_segment_bell_create()).
//
// epoll_pwait2() because it is the only epoll wait that takes a timespec: epoll_wait()'s millisecond int would round
// every sub-millisecond wait up, the defect ppoll() was chosen to avoid. It is Linux 5.11 and glibc 2.35; on anything
// older, or when the kernel refuses, there is no set and tt_receive() uses ppoll() as before.
#if defined(__GLIBC__) && __GLIBC_PREREQ(2, 35) // NOLINT(misc-include-cleaner) - <features.h>, via every libc header
#define TT_HAL_EPOLL 1
#else
#define TT_HAL_EPOLL 0
#endif

// What a wait reports, one bit per member of the set - in epoll_event.data.u32 as registered, and built from revents
// on the ppoll() path, so the code after the wait reads one thing either way.
#define RX_READY_WELL_KNOWN 1U
#define RX_READY_WAKE 2U
#define RX_READY_DATA 4U
#define RX_READY_BELL 8U
#define RX_READY_WRITABLE 16U // data_sock has room again (tt_tx_await_writable())
#define RX_READY_TIMER 32U    // the wait's deadline timer fired (rx_wait_lazy())

// The deadline timer edge-triggered: a fire needs no read() (rx_timer_take()). 0: level-triggered and read, as before
// 2026-10-17 (the A/B control).
#ifndef TT_RX_TIMER_EDGE
#define TT_RX_TIMER_EDGE 1
#endif
// A wait in a poll longer than this takes the deadline timer whatever the run of waits that ended for data
// (rx_wait_epoll()): past it the kernel's slack on a poll()-family timeout - 0.1% of the wait, 0.5% for a thread with a
// positive nice value, against the thread's 50 us timer slack for a timerfd - makes the wait end measurably late.
// 0: never (the rule before 2026-10-17, the A/B control).
// A long wait for an entry sets the timer to its own deadline, later as well as earlier (rx_wait_lazy()). 0: only
// earlier, as the streak rule's waits do (the A/B control).
#ifndef TT_RX_TIMER_LONG_EXACT
#define TT_RX_TIMER_LONG_EXACT 1
#endif
#ifndef TT_RX_TIMER_LONG_NS
#define TT_RX_TIMER_LONG_NS (10LL * 1000000LL)
#endif
// The polling thread's timer slack, lowered to this many nanoseconds the first time the thread waits in tt_receive()
// (rx_timer_slack_once(); never raised). A wait of TT_RX_TIMER_LONG_NS or less takes the relative timeout, whose
// slack is 0.1% of the wait but never less than the thread's timer slack - 50 us by default, so a ping at a 5 ms
// interval left ~60 us after its due time, the DDS clients' figure (same-host run, 2026-10-17). At 1 ns the slack is
// the 0.1% (5 us at 5 ms), at no cost per wait; arming the timerfd for those waits instead cost a timerfd_settime()
// or two a round trip, +6-7% client kernel instructions (PC, sched_wake_ab_pc.sh, 2026-10-18). It applies to every
// timer of that thread, as PR_SET_TIMERSLACK does. 0: leave the thread's slack alone (the A/B control).
#ifndef TT_RX_TIMER_SLACK_NS
#define TT_RX_TIMER_SLACK_NS 1
#endif

#if TT_HAL_EPOLL
static bool wait_set_add(int epoll_fd, int member, uint32_t events, uint32_t tag) {
    struct epoll_event event = {.events = events, .data = {.u32 = tag}};
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, member, &event) == 0;
}
#endif

static void wait_set_setup(struct tt_Context* node) {
#if TT_HAL_EPOLL
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        TT_LOG_WARNING("epoll unavailable (%s) - every wait will build its poll set again", strerror(errno));
        return;
    }
    struct epoll_event probe;
    const struct timespec now = {0, 0};
    // The zero-timeout wait first: ENOSYS from a kernel older than 5.11 is refused here, not on the first real wait.
    if (epoll_pwait2(epoll_fd, &probe, 1, &now, NULL) < 0 ||
        !wait_set_add(epoll_fd, node->hal.sock, EPOLLIN, RX_READY_WELL_KNOWN) ||
        !wait_set_add(epoll_fd, node->hal.wake_fd, EPOLLIN, RX_READY_WAKE) ||
        !wait_set_add(epoll_fd, node->hal.data_sock, EPOLLIN, RX_READY_DATA)) {
        TT_LOG_WARNING("epoll_pwait2 unavailable (%s) - every wait will build its poll set again", strerror(errno));
        (void)close(epoll_fd);
        return;
    }
    node->hal.epoll_fd_plus1 = epoll_fd + 1;
    // The wait's deadline timer (rx_wait_lazy()). Without it, or with TT_RX_LAZY_TIMER=0 (the A/B control), each wait
    // takes its timeout as before.
    const char* lazy = getenv("TT_RX_LAZY_TIMER");
    if (lazy != NULL && strcmp(lazy, "0") == 0) {
        return;
    }
    int timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC); // tt_get_ns()'s clock
    if (timer < 0 || !wait_set_add(epoll_fd, timer, TT_RX_TIMER_EDGE ? (EPOLLIN | EPOLLET) : EPOLLIN, RX_READY_TIMER)) {
        TT_LOG_DEBUG("No deadline timer in the wait set (%s) - each wait takes its timeout", strerror(errno));
        if (timer >= 0) {
            (void)close(timer);
        }
        return;
    }
    node->hal.timer_fd_plus1 = timer + 1;
    // The timer only after a wait that ended for a datagram (rx_wait_epoll()); TT_RX_LAZY_ADAPT=0: after every wait.
    const char* adapt = getenv("TT_RX_LAZY_ADAPT");
    node->hal.timer_adapt = adapt == NULL || strcmp(adapt, "0") != 0;
#else
    (void)node;
#endif
}

// UDP offload (2026-10-10). A large sample is hundreds of 1472-byte datagrams to one destination, and every one of them
// paid the socket layer once on each side. The kernel can pay it once per run instead, without anything on the wire
// changing:
//   - receive, UDP_GRO: same-flow datagrams that arrive together are handed over in one read, back to back, with their
//     size in a cmsg (all the same size but the last). They are given to core one at a time, in the buffer they were
//     read into (struct tt_Context.rx_offset), so a merged read costs core no copy. Alignment: segment k starts at
//     k x size. Core's codec needs 4 (CDR-4), so a size that is not a multiple of 4 has every segment but the first
//     copied to the start of the buffer (gro_copied). Receive-buffer lending wants 8 (03737b57: a lone DATA's payload
//     at 4 mod 8), which a size of 4 mod 8 gives only every other segment - and that is the main case: a large
//     sample's fragment datagram is 1468 B (4 framing + 12 FRAG_CONT_L + 1452). Those odd segments are still handed
//     out in place (gro_off8), because what being off 8 costs is narrow: a fragment is copied into its assembly
//     either way, and a lone DATA there is still correct, only no longer readable in place by a loaned take - rmw
//     decodes it instead, the copy the HAL would otherwise have made here for every such segment, loaned or not.
//     Measured on the PC (1 MB samples): copying them was half of every merged datagram, 54150 of 108450. Off in
//     builds whose buffer cannot hold a merged read (TT_HAL_UDP_GRO, hal_linux.h). A kernel that refuses the option
//     leaves every read as it was. On only while large samples arrive: see "When receive offload is on", below.
//   - send, UDP_SEGMENT (tt_send_batch()): see send_batch_flags().
// Rig probe 2026-10-10 (experiments/rx_gro_probe_rig.sh, Pi 5 x2, 1472 B): receiver 3.99 ms/MB with recvfrom(), 3.40
// with UDP_GRO; sender 4.76 plain, 4.19 with UDP_SEGMENT. TT_UDP_OFFLOAD=0 in the environment turns both off, for the
// A/B control: the same binary, every datagram sent and read one at a time as before.
//
// A build with neither (core's default: 1472-byte datagrams, no large samples) compiles all of it out, the state and
// the counters' writes included, so its send and receive paths are the ones it had before offload existed - gate v2 on
// the rig (2026-10-12) read the always-compiled version +65 instructions a round trip at reliable_latency p1.
#if TT_HAL_UDP_GSO || TT_HAL_UDP_GRO
static bool udp_offload_allowed(void) {
    const char* setting = getenv("TT_UDP_OFFLOAD");
    return setting == NULL || strcmp(setting, "0") != 0;
}
#endif

#if TT_HAL_UDP_GSO
#define TT_UDP_OFFLOAD_GSO 2U // struct tt_Context.udp_offload

// Send offload is decided per send (send_batch_flags()); here only whether the kernel knows UDP_SEGMENT at all. It
// must: a kernel before 4.18 ignores a SOL_UDP cmsg it does not know and would send the whole run as one datagram,
// which IP would then fragment. getsockopt(UDP_SEGMENT) exists from the same release, so it is the check.
static void gso_setup(struct tt_Context* node) {
    node->hal.gso_on = false;
    if (!udp_offload_allowed()) {
        return;
    }
    int size = 0;
    socklen_t size_len = sizeof(size);
    if (getsockopt(node->hal.data_sock, SOL_UDP, UDP_SEGMENT, &size, &size_len) != 0) {
        TT_LOG_DEBUG("UDP_SEGMENT unknown to this kernel (%s) - every datagram is sent alone", strerror(errno));
        return;
    }
    node->hal.gso_on = true;
    node->udp_offload |= TT_UDP_OFFLOAD_GSO;
}
#endif

#if TT_HAL_UDP_GRO
#define TT_UDP_OFFLOAD_GRO 1U // struct tt_Context.udp_offload

// When receive offload is on (2026-10-10). A read with UDP_GRO costs more than one without it whether or not anything
// was merged - recvmsg() with a control buffer in place of recvfrom(), and the kernel's GRO paths for the socket: +2.3%
// receiver instructions per MB for a stream of 1 KB samples on the PC, which never form a run. So the sockets read as
// they always did (TT_GRO_PLAIN) until a large sample's fragment (FRAG_FIRST_L/FRAG_CONT_L, a single-form datagram -
// byte 0 the marker, byte 3 the type) arrives, and go back after TT_GRO_QUIET_READS reads with neither a merged read
// nor such a fragment. A stream of small samples never turns it on.
//
// Going back is the delicate half. Turning UDP_GRO off does not split what is already queued merged, and a merged
// datagram read with the option off comes back as one block with no size: core would take a run for one datagram
// (measured: two 10-datagram runs queued, the option turned off, two 10000-byte reads and no cmsg). What arrives after
// the switch is split by the kernel - a socket that no longer accepts merged datagrams has them cut at enqueue. So the
// switch is made only across a moment the queues are seen empty, past the instant the option went off:
//   1. TT_GRO_ON, quiet long enough, at a tt_receive() about to wait - every read before it found the queues empty:
//      the option goes off on both sockets, and the state is TT_GRO_SETTLING until TT_GRO_SETTLE_NS later.
//   2. Any read while settling turns the option back on first (TT_GRO_ON again: aborted), so nothing that may be
//      merged is ever read without the cmsg that sizes it.
//   3. At the deadline (tt_receive() shortens its wait to reach it), both queues empty (SIOCINQ 0): TT_GRO_PLAIN,
//      committed. Anything queued then may have been merged before the option went off: aborted.
// TT_GRO_SETTLE_NS covers a datagram the kernel had already tested against the option when it went off and had not yet
// queued: the two are a few hundred instructions apart in one section run with bottom halves off, well under a
// microsecond; 100 us is two orders beyond it (tt_RECEIVE_TIMEOUT, the poll loop's own slice). Only a reader whose
// traffic never pauses that long stays on - an aborted settle is tried again after half the quiet reads.
//
// TT_GRO_QUIET_READS: switching back costs four setsockopt() calls and two ioctl() calls, and turning it on again two
// more; after 128 reads of small traffic those are under 5% of the calls they save (and an abort's retry after 64,
// under 10%). It counts reads of small datagrams
// only - every datagram of a large sample is a fragment, and resets it - so it is derived from the switch's cost, not
// from any testbed. Both overridable for udp_offload_check's flapping arm.
// TT_GRO_PLAIN, TT_GRO_ON, TT_GRO_SETTLING: hal_linux.h, beside struct tt_hal's gro_state.
#ifndef TT_GRO_QUIET_READS
#define TT_GRO_QUIET_READS 128U
#endif
#ifndef TT_GRO_SETTLE_NS
#define TT_GRO_SETTLE_NS 100000ULL
#endif

// UDP_GRO on or off on both sockets. False if either refused.
static bool gro_option(struct tt_Context* node, int value) {
    bool well_known = setsockopt(node->hal.sock, SOL_UDP, UDP_GRO, &value, sizeof(value)) == 0;
    bool data = setsockopt(node->hal.data_sock, SOL_UDP, UDP_GRO, &value, sizeof(value)) == 0;
    return well_known && data;
}

// Whether the kernel takes UDP_GRO on both sockets (ENOPROTOOPT before Linux 5.0: not an error, reads stay as they
// were). Asked once and left off: it goes on only for large samples (above).
static void gro_setup(struct tt_Context* node) {
    node->hal.gro_allowed = false;
    node->hal.gro_state = TT_GRO_PLAIN;
    node->hal.gro_quiet = 0;
    node->hal.gro_left = 0;
    node->hal.gro_next = 0;
    node->hal.gro_end = 0;
    node->hal.gro_base = NULL;
    if (!udp_offload_allowed()) {
        return;
    }
    if (!gro_option(node, 1)) {
        TT_LOG_DEBUG("UDP_GRO refused (%s) - reads stay one datagram each", strerror(errno));
        (void)gro_option(node, 0);
        return;
    }
    (void)gro_option(node, 0);
    node->hal.gro_allowed = true;
    node->udp_offload |= TT_UDP_OFFLOAD_GRO;
}

// A large sample's MTU-mode fragment, by its single-form framing (tt_SingleHeader): FRAG_FIRST_L, or FRAG_CONT_L but
// the sample's last unit. With the hybrid transport (DESIGN.md section 8) a sample sent in runs ends on one MTU
// fragment, its last unit alone, and nothing can merge with it - the runs before it are each one IP-reassembled
// datagram, which the kernel's GRO never sees (IPv4 fragments bypass it; udp_offload_check.sh pattern 8 shows it). So
// that tail neither turns receive offload on nor keeps it on: only MTU-mode traffic - a sample sent at n = 1, MTU
// repairs - does, and an AUTO stream of runs reads plain.
static bool gro_large_fragment(const uint8_t* datagram, int32_t len) {
    if (len < (int32_t)sizeof(struct tt_SingleHeader) ||
        (datagram[0] != tt_SINGLE_MARKER_LE && datagram[0] != tt_SINGLE_MARKER_BE)) {
        return false;
    }
    if (datagram[3] == tt_SUBMESSAGE_TYPE_FRAG_FIRST_L) {
        return true;
    }
    const uint32_t index_at = sizeof(struct tt_SingleHeader) + offsetof(struct tt_FragContLHeader, frag_index);
    const uint32_t count_at = sizeof(struct tt_SingleHeader) + offsetof(struct tt_FragContLHeader, frag_count);
    if (datagram[3] != tt_SUBMESSAGE_TYPE_FRAG_CONT_L || len < (int32_t)(count_at + sizeof(uint16_t))) {
        return false;
    }
    const bool little = datagram[0] == tt_SINGLE_MARKER_LE;
    const uint32_t index = little ? (uint32_t)datagram[index_at] | ((uint32_t)datagram[index_at + 1U] << 8U)
                                  : ((uint32_t)datagram[index_at] << 8U) | (uint32_t)datagram[index_at + 1U];
    const uint32_t count = little ? (uint32_t)datagram[count_at] | ((uint32_t)datagram[count_at + 1U] << 8U)
                                  : ((uint32_t)datagram[count_at] << 8U) | (uint32_t)datagram[count_at + 1U];
    return index + 1U != count; // the last unit alone merges with nothing
}

// The option back on, from TT_GRO_PLAIN (a large fragment came) or TT_GRO_SETTLING (aborted). A refusal now - which
// the check at bind did not see - leaves the sockets as they are and receive offload off for good.
static void gro_turn_on(struct tt_Context* node) {
    if (!gro_option(node, 1)) {
        (void)gro_option(node, 0);
        node->hal.gro_allowed = false;
        node->hal.gro_state = TT_GRO_PLAIN;
        node->udp_offload &= (uint8_t)~TT_UDP_OFFLOAD_GRO;
        return;
    }
    if (node->hal.gro_state == TT_GRO_SETTLING) {
        // Aborted: try again after half the quiet reads, not all of them - what arrived may be small traffic still,
        // and a merged read or a large fragment resets the count anyway.
        node->hal.gro_aborts++;
        node->hal.gro_quiet = TT_GRO_QUIET_READS / 2U;
    } else {
        node->hal.gro_enables++;
        node->hal.gro_quiet = 0;
    }
    node->hal.gro_state = TT_GRO_ON;
}

// Step 1 of going back (above): at a tt_receive() about to wait, with nothing of a merged read left to hand out.
static void gro_maybe_settle(struct tt_Context* node) {
    if (node->hal.gro_state != TT_GRO_ON || node->hal.gro_quiet < TT_GRO_QUIET_READS) {
        return;
    }
    (void)gro_option(node, 0);
    node->hal.gro_state = TT_GRO_SETTLING;
    node->hal.gro_settle_at_ns = tt_get_ns() + TT_GRO_SETTLE_NS;
}

static bool gro_queue_empty(int socket_fd) {
    int queued = 0;
    // NOLINTNEXTLINE(misc-include-cleaner) - SIOCINQ is FIONREAD, from <sys/ioctl.h>
    return ioctl(socket_fd, FIONREAD, &queued) == 0 && queued == 0;
}

// Step 3: the deadline reached - by tt_get_ns(), or by a wait that ran to it (whose clock may end it a little before
// tt_get_ns() agrees) - commit if both queues are empty, otherwise abort.
static void gro_settle_end(struct tt_Context* node) {
    if (node->hal.gro_state != TT_GRO_SETTLING) {
        return;
    }
    if (gro_queue_empty(node->hal.sock) && gro_queue_empty(node->hal.data_sock)) {
        node->hal.gro_state = TT_GRO_PLAIN;
        node->hal.gro_commits++;
        return;
    }
    gro_turn_on(node);
}

static void gro_settle_decide(struct tt_Context* node) {
    if (node->hal.gro_state == TT_GRO_SETTLING && tt_get_ns() >= node->hal.gro_settle_at_ns) {
        gro_settle_end(node);
    }
}

// Hands out the next datagram of the last merged read, or -1 when none is left. In place when `buf` is still the
// buffer it was read into and the datagram starts 4-aligned there (UDP offload, above, for 8); copied to the start of
// `buf` otherwise. That overwrites only datagrams already handed out: the next one starts at least one segment in. And
// a buffer core has moved off (a sample in it was retained, tt_Sample_retain()) is only read from.
static int32_t gro_take_pending(struct tt_Context* node, uint8_t* buf, size_t len, uint32_t* ip, uint16_t* port) {
    struct tt_hal* hal = &node->hal;
    if (hal->gro_left == 0) {
        return -1;
    }
    uint32_t offset = hal->gro_next;
    uint32_t size = hal->gro_end - offset < hal->gro_segment ? hal->gro_end - offset : hal->gro_segment;
    hal->gro_next = offset + size;
    hal->gro_left--;
    if (buf == hal->gro_base && offset % 4U == 0) {
        node->rx_offset = offset;
        if (offset % 8U != 0) {
            node->udp_gro_off8++;
        }
    } else {
        size = size < len ? size : (uint32_t)len;
        memcpy(buf, hal->gro_base + offset, size);
        node->udp_gro_copied++;
    }
    *ip = hal->gro_ip;
    *port = hal->gro_port;
    node->rx_via_data_port = hal->gro_from_data;
    TT_TRACE(tt_TRACE_RX_DATAGRAM);
    return (int32_t)size;
}

#if TT_HAL_RX_DIRECT
static ssize_t direct_recvmsg(struct tt_Context* node, int socket_fd, struct msghdr* msg);
#endif

// UDP_GRO's segment size from a read's control messages: 0 when the kernel attached none (the read is one datagram).
static int gro_segment_of(struct msghdr* msg) {
    int segment = 0;
    // NOLINTNEXTLINE(misc-include-cleaner) - CMSG_*: <sys/socket.h>
    for (struct cmsghdr* cm = CMSG_FIRSTHDR(msg); cm != NULL; cm = CMSG_NXTHDR(msg, cm)) {
        if (cm->cmsg_level == SOL_UDP && cm->cmsg_type == UDP_GRO) {
            memcpy(&segment, CMSG_DATA(cm), sizeof(segment));
        }
    }
    return segment;
}

// rx_read_one() with UDP_GRO: recvmsg() with room for the segment size. A merged read returns its first datagram and
// keeps the rest for gro_take_pending(). A run core hints is read in place here too (direct_recvmsg()): a run is one
// IP-reassembled datagram, which GRO never merges, so receive offload left on by MTU-mode traffic does not take the
// read in place away from the runs (on the rig's 1 MB cell it stayed on all run, and no run was read in place).
static int32_t gro_read(struct tt_Context* node, int socket_fd, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    struct tt_hal* hal = &node->hal;
    struct sockaddr_in addr;
    // NOLINTNEXTLINE(misc-include-cleaner) - see <sys/uio.h>'s own include comment
    struct iovec iov = {.iov_base = buf, .iov_len = len};
    union {
        char bytes[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align; // NOLINT(misc-include-cleaner) - <sys/socket.h>
    } control;
    struct msghdr msg = {0};
    msg.msg_name = &addr;
    msg.msg_namelen = sizeof(addr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.bytes;
    msg.msg_controllen = sizeof(control.bytes);
#if TT_HAL_RX_DIRECT
    // Only the polling thread makes a hint, so a 0 seen here is current (rx_read_one()).
    ssize_t got = __atomic_load_n(&node->large.direct_bytes, __ATOMIC_RELAXED) != 0
                      ? direct_recvmsg(node, socket_fd, &msg)
                      : recvmsg(socket_fd, &msg, MSG_DONTWAIT);
#else
    ssize_t got = recvmsg(socket_fd, &msg, MSG_DONTWAIT);
#endif
    if (got < 0) {
        // NOLINTNEXTLINE(misc-include-cleaner) - EAGAIN/EWOULDBLOCK: glibc-private headers
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
    }
    *ip = ntohl(addr.sin_addr.s_addr);
    *port = ntohs(addr.sin_port);
    int segment = gro_segment_of(&msg);
    if (segment <= 0 || got <= segment) {
        return (int32_t)got; // one datagram
    }
    uint32_t end = (uint32_t)got;
    // NOLINTNEXTLINE(misc-include-cleaner) - MSG_TRUNC: <sys/socket.h>
    if ((msg.msg_flags & MSG_TRUNC) != 0) {
        // Larger than the buffer, which TT_HAL_UDP_GRO's size rule excludes: the cut datagram is dropped, as loss.
        end -= end % (uint32_t)segment;
    }
    uint32_t count = (end + (uint32_t)segment - 1U) / (uint32_t)segment;
    node->udp_gro_reads++;
    node->udp_gro_merged += count;
    hal->gro_base = (uint8_t*)buf;
    hal->gro_segment = (uint32_t)segment;
    hal->gro_next = (uint32_t)segment;
    hal->gro_end = end;
    hal->gro_left = (uint16_t)(count - 1U);
    hal->gro_ip = *ip;
    hal->gro_port = *port;
    hal->gro_from_data = (socket_fd == hal->data_sock);
    return segment;
}
#endif

// ---- Non-blocking sockets (2026-10-13)
// ---------------------------------------------------------------------------------
//
// Both sockets are O_NONBLOCK. Every read was non-blocking already (MSG_DONTWAIT, after a wait), so reads do not
// change; what changes is the wait. Linux's udp_poll() checks, for a socket NOT in non-blocking mode, that the first
// queued datagram's checksum is good before it reports the socket readable (first_packet_length(): a blocking read must
// not wake for a datagram it would then drop) - a separate pass over every datagram, cold, before the read copies it.
// On the Pi 5 that pass was the checksum row of the receive gap to Fast DDS (+0.068 / +0.082 ms/MB at 1 / 4 MB,
// kallsyms-named,
// ~/claude_reports/rx_perf_gap2_2026-10-13.md section 9). Non-blocking, the poll does not look, and the checksum is
// made once, during the copy to user (skb_copy_and_csum_datagram_msg()) - as Fast DDS's blocking recvfrom() makes it. A
// datagram with a bad checksum then wakes the wait and the read finds nothing (-1, as an empty read).
//
// Sends were blocking (flags 0) where the caller wants that: tt_send(), tt_send_to(), tt_send_iov() and
// tt_send_batch(). They keep it: a send the full buffer refuses waits for room (POLLOUT, the same threshold the
// writable watch uses) and is made again, as the kernel would have waited inside the call. TT_RX_NONBLOCK=0 in the
// environment leaves both sockets blocking (the A/B control).
static void sockets_nonblocking(struct tt_Context* node) {
    const char* nonblock = getenv("TT_RX_NONBLOCK");
    node->hal.nonblocking = nonblock == NULL || strcmp(nonblock, "0") != 0;
    if (!node->hal.nonblocking) {
        return;
    }
    int fds[2] = {node->hal.sock, node->hal.data_sock};
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(fds[i], F_GETFL);
        if (flags < 0 || fcntl(fds[i], F_SETFL, flags | O_NONBLOCK) < 0) {
            TT_LOG_WARNING("O_NONBLOCK refused on a socket (%s) - its wait checks checksums twice", strerror(errno));
        }
    }
}

// A send that may block found the buffer full (EAGAIN on a non-blocking socket): wait for room, then the caller sends
// again. False for any other failure, which the caller reports as before.
static bool send_wait_for_room(int socket_fd) {
    // NOLINTNEXTLINE(misc-include-cleaner) - EAGAIN/EWOULDBLOCK/EINTR: glibc-private headers
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        return false;
    }
    // NOLINTNEXTLINE(misc-include-cleaner) - struct pollfd/POLLOUT: <poll.h>
    struct pollfd room = {.fd = socket_fd, .events = POLLOUT, .revents = 0};
    // NOLINTNEXTLINE(misc-include-cleaner) - EINTR: glibc-private header
    while (poll(&room, 1, -1) < 0 && errno == EINTR) {
    }
    return true;
}

// A blocking send the buffer refused, made again (sendto_again(), sendmsg_again()) from what the sender wrote into its
// own frame before the call. So the call's usual return - it went - has only the result to test, and keeps nothing live
// across the call: until 2026-10-14 the wait was a loop around the call, which kept the node, the buffer, its length
// and the address in saved registers on every send, +15 instructions a tt_send_to() on x86-64 whether or not the
// buffer ever filled - the whole of the publisher's +19 user instructions a sample on the PC (srv_gap_pc.sh, step
// 1d10c89e, every TickLE throughput cell sending one datagram a sample, and both roles of the latency cells) and its
// share of the rig's (gate v2, ab/rx-gap3b against main: client insn_u +0.5..+1.8%).
struct tt_SendAgain {
    const void* buf;
    size_t len;
    int fd;
    struct sockaddr_in to;
};

static __attribute__((cold, noinline)) int32_t sendto_again(const struct tt_SendAgain* again) {
    int32_t sent = -1;
    while (send_wait_for_room(again->fd)) {
        sent = (int32_t)hal_sendto(again->fd, again->buf, again->len, 0, (const struct sockaddr*)&again->to,
                                   sizeof(again->to));
        if (sent >= 0) {
            break;
        }
    }
    return sent;
}

// tt_send_iov()'s message, beside the socket it goes to: the same as above for a sendmsg().
struct tt_SendMsgAgain {
    int fd;
    struct msghdr msg;
};

static __attribute__((cold, noinline)) int32_t sendmsg_again(const struct tt_SendMsgAgain* again) {
    int32_t sent = -1;
    while (send_wait_for_room(again->fd)) {
        sent = (int32_t)hal_sendmsg(again->fd, &again->msg, 0);
        if (sent >= 0) {
            break;
        }
    }
    return sent;
}

#if TT_HAL_UDP_CSUM0
static void csum0_setup(struct tt_Context* node); // "UDP checksum 0", beside the send path
#endif

tt_ret_t tt_bind(struct tt_Context* node) {
    // Set before anything below can fail into tt_close(): -1 says "nothing to close here yet",
    // the same convention node->hal.sock itself relies on implicitly (every failure that reaches
    // tt_close() below happens after sock was already created successfully).
    node->hal.wake_fd = -1;
    node->hal.bell_fd_plus1 = 0;  // no segment yet, so no doorbell (tt_segment_bell_create())
    node->hal.epoll_fd_plus1 = 0; // none yet: tt_close() after a failed bind has nothing to close
    node->hal.timer_fd_plus1 = 0;
    node->hal.timer_armed_ns = 0;
    node->hal.timer_waits = 0;
    node->hal.timer_sets = 0;
    node->hal.timer_fires = 0;
    node->hal.timer_data_streak = 0;
    node->hal.timer_adapt = false; // set with the timer (wait_set_setup())
    node->hal.timer_plain_waits = 0;
    node->hal.tx_watch_fd_plus1 = 0;
    node->hal.tx_watch_armed = false;
#if tt_SEGMENT_ENABLED
    node->hal.bell_drain_every = 0;
    node->hal.bell_drained_at = 0;
    node->hal.bell_drains = 0;
#endif
    node->hal.data_sock = -1;
#if tt_RX_DRAIN
    node->hal.drain_fd = -1; // set up at the first drain (drain_setup())
    node->hal.drain_running = false;
    node->hal.drain_armed = 0;
    node->hal.drain_pending = 0;
    node->hal.drain_holdoff_until = 0;
    node->hal.drain_starts = 0;
    node->hal.drain_datagrams = 0;
    node->hal.drain_truncated = 0;
    node->hal.drain_overruns = 0;
    node->hal.drain_ring = tt_RX_DRAIN_RING_NONE;
    node->hal.drain_group = 0;
    node->hal.drain_rearm = 0;
    node->hal.drain_returned = 0;
    node->hal.drain_rearms = 0;
    node->hal.drain_budget = tt_RX_DRAIN_BYTES;
    node->hal.drain_overran = false;
    node->hal.drain_grows = 0;
#if TT_HAL_UDP_GRO
    node->hal.drain_split_left = 0;
    node->hal.drain_merged = 0;
#endif
#endif
#if tt_HAL_IO_URING
    node->hal.uring_fd = -1; // tt_close() after a failed bind must not close what was never opened
#endif
    node->hal.rx_prefer_data = false;
    node->hal.rx_idle = 0;
    node->hal.rx_idle_since_ns = 0;
    node->hal.rx_empty_reads = 0;
    node->hal.rx_idle_skips = 0;
    node->hal.rx_lone_streak = 0;
    node->hal.rx_lone_skips = 0;
    node->hal.rx_probe_bit = 0;
    node->hal.rx_count = 0;
    node->hal.rx_next = 0;
    node->hal.rx_headers_for = NULL;
    node->hal.rx_batch_calls = 0;
    node->hal.rx_batch_datagrams = 0;
    node->hal.rx_batch_full = 0;
#if TT_HAL_UDP_GSO
    node->hal.gso_on = false; // gso_setup(), once the data socket exists
#endif
#if TT_HAL_UDP_CSUM0
    node->hal.csum0 = 0; // csum0_setup(), once the data socket exists
    node->hal.csum0_on = false;
    node->hal.csum0_switches = 0;
    node->hal.csum0_calls = 0;
    node->hal.csum0_datagrams = 0;
    node->hal.csum0_retries = 0;
#endif
#if TT_HAL_UDP_GRO
    node->hal.gro_allowed = false; // gro_setup(), once both sockets exist
    node->hal.gro_state = TT_GRO_PLAIN;
    node->hal.gro_enables = 0;
    node->hal.gro_aborts = 0;
    node->hal.gro_commits = 0;
    node->hal.gro_left = 0;
#endif

    node->hal.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (node->hal.sock < 0) {
        TT_LOG_ERROR("Cannot create UDP socket: %s", strerror(errno));
        return tt_RET_IO_ERROR;
    }

    int optval = 1;
    // SOL_SOCKET/SO_* live in glibc-private headers; <sys/socket.h> (included above) is the correct public header.
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (setsockopt(node->hal.sock, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int)) < 0) {
        TT_LOG_ERROR("Cannot set socket reuseaddr: %s", strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    optval = 1;
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (setsockopt(node->hal.sock, SOL_SOCKET, SO_BROADCAST, (const void*)&optval, sizeof(int)) < 0) {
        TT_LOG_ERROR("Cannot set socket broadcast: %s", strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    // Best-effort: the kernel clamps this to net.core.[rw]mem_max for an unprivileged process,
    // so a failure or a smaller-than-requested result here isn't fatal, just less headroom
    // against bursty drops.
    int buffer_size = tt_SOCKET_BUFFER_SIZE;
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (setsockopt(node->hal.sock, SOL_SOCKET, SO_SNDBUF, (const void*)&buffer_size, sizeof(int)) < 0) {
        TT_LOG_WARNING("Cannot set socket send buffer size: %s", strerror(errno));
    }
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (setsockopt(node->hal.sock, SOL_SOCKET, SO_RCVBUF, (const void*)&buffer_size, sizeof(int)) < 0) {
        TT_LOG_WARNING("Cannot set socket receive buffer size: %s", strerror(errno));
    }
    report_receive_buffer(node->hal.sock, "Well-known");

    // The well-known socket always binds the wildcard, never _tt_CONFIG.addr. Measured on Linux:
    // a socket bound to a unicast address receives no broadcasts at all, directed or limited - so
    // binding this one to a specific address would stop every announce from arriving while unicast
    // kept working, which is a node that hears nobody and is heard by nobody with every send
    // reporting success. _tt_CONFIG.addr scopes the data socket instead, below.
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(_tt_CONFIG.port);

    if (bind(node->hal.sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_in)) < 0) {
        TT_LOG_ERROR("Cannot bind socket to 0.0.0.0:%d: %s", _tt_CONFIG.port, strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    // Precompute the broadcast destination once instead of re-parsing _tt_CONFIG.broadcast with
    // inet_addr() on every single tt_send() call.
    node->hal.broadcast_addr.sin_family = AF_INET;
    node->hal.broadcast_addr.sin_addr.s_addr = inet_addr(_tt_CONFIG.broadcast);
    node->hal.broadcast_addr.sin_port = htons(_tt_CONFIG.port);

    // This node's own data port. Everything is sent from here so that every peer records this
    // node at a port that belongs to it alone - see struct tt_hal.data_sock (hal_linux.h) for the
    // measured failure that made this necessary. Port 0 lets the kernel choose; nothing needs to
    // know the number in advance, because a peer learns it from the source port of the first
    // packet it hears. No SO_REUSEADDR: this port is this node's alone, and a second node
    // silently sharing it is precisely what must not happen.
    node->hal.data_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (node->hal.data_sock < 0) {
        TT_LOG_ERROR("Cannot create UDP data socket: %s", strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    optval = 1;
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (setsockopt(node->hal.data_sock, SOL_SOCKET, SO_BROADCAST, (const void*)&optval, sizeof(int)) < 0) {
        TT_LOG_ERROR("Cannot set data socket broadcast: %s", strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    buffer_size = tt_SOCKET_BUFFER_SIZE;
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (setsockopt(node->hal.data_sock, SOL_SOCKET, SO_SNDBUF, (const void*)&buffer_size, sizeof(int)) < 0) {
        TT_LOG_WARNING("Cannot set data socket send buffer size: %s", strerror(errno));
    }
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (setsockopt(node->hal.data_sock, SOL_SOCKET, SO_RCVBUF, (const void*)&buffer_size, sizeof(int)) < 0) {
        TT_LOG_WARNING("Cannot set data socket receive buffer size: %s", strerror(errno));
    }
    report_receive_buffer(node->hal.data_sock, "Data");

    // This one does honour _tt_CONFIG.addr. Left at 0.0.0.0 it behaves as before; set to a local
    // address it scopes this node's sends to the link that owns that address, which is the
    // unprivileged way to pin a limited broadcast to one interface - SO_BINDTODEVICE is the
    // obvious tool and needs CAP_NET_RAW. Safe here and not on the socket above because this one
    // only ever needs to receive unicast.
    struct sockaddr_in data_addr;
    data_addr.sin_family = AF_INET;
    data_addr.sin_addr.s_addr = inet_addr(_tt_CONFIG.addr);
    data_addr.sin_port = 0; // kernel-assigned
    if (bind(node->hal.data_sock, (struct sockaddr*)&data_addr, sizeof(struct sockaddr_in)) < 0) {
        TT_LOG_ERROR("Cannot bind data socket to %s:0: %s", _tt_CONFIG.addr, strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }
#if tt_CONTEXT_ID_CLAIM
    record_own_address(node);
#endif

    // eventfd(2) tt_receive() also polls, purely so tt_wake_signal() has something to write to
    // that wakes it up - see hal_linux.h's own comment on wake_fd for why this (rather than a
    // loopback UDP socket, as hal_freertos.c uses) is what Linux needs specifically.
    node->hal.wake_fd = eventfd(0, EFD_NONBLOCK);
    if (node->hal.wake_fd < 0) {
        TT_LOG_ERROR("Cannot create wake eventfd: %s", strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }
    sockets_nonblocking(node);
    wait_set_setup(node); // without it tt_receive() builds a ppoll() set per call, as it always did
#if TT_HAL_RX_DIRECT
    {
        const char* direct = getenv("TT_RX_DIRECT");
        node->large.direct_allowed = direct == NULL || strcmp(direct, "0") != 0;
    }
#endif

#if TT_HAL_UDP_GRO
    gro_setup(node);
#endif
#if TT_HAL_UDP_GSO
    gso_setup(node);
#endif
#if TT_HAL_UDP_CSUM0
    csum0_setup(node);
#endif

#if tt_HAL_IO_URING
    if (!uring_setup(node) && tt_HAL_RX_HINT == tt_RX_HINT_URING) {
        tt_close(node); // the reason was logged by uring_refused()
        return tt_RET_UNSUPPORTED;
    }
#endif
    return tt_RET_OK;
}

#if tt_RX_DRAIN
static void drain_stop(struct tt_Context* node);
static void drain_release(struct tt_hal* hal);

static const char* drain_ring_name(uint8_t ring) {
    switch (ring) {
    case tt_RX_DRAIN_RING_USER:
        return "user";
    case tt_RX_DRAIN_RING_KERNEL:
        return "kernel";
    case tt_RX_DRAIN_RING_CLASSIC:
        return "classic";
    default:
        return "none";
    }
}
#endif

#define DIRECT_LINE_BYTES 160U // tt_close()'s direct_* fields on the rx line
#define GRO_LINE_BYTES 256U    // and its empty_reads=, idle_skips=, gro_* and timer_* fields
#define CSUM0_LINE_BYTES 160U  // and its csum0_* fields

void tt_close(struct tt_Context* node) {
    // Read while both sockets are still open: what the kernel dropped at each, beside core's traffic line.
    // Runs read in place (direct_read()), on the same line: a rig arm's treatment check.
    char direct[DIRECT_LINE_BYTES] = "";
#if TT_HAL_RX_DIRECT
    (void)snprintf(direct, sizeof(direct),
                   " direct=%d direct_reads=%lu direct_runs=%lu direct_missed=%lu direct_cancelled=%lu",
                   node->large.direct_allowed ? 1 : 0, (unsigned long)node->large.direct_reads,
                   (unsigned long)node->large.direct_runs, (unsigned long)node->large.direct_missed,
                   (unsigned long)node->large.direct_cancelled);
#endif
    // The sockets' mode, empty reads, receive offload's switches (gro_on: its state at the end) and the wait's deadline
    // timer, on the same line: a rig arm's treatment check.
    char gro[GRO_LINE_BYTES] = "";
#if TT_HAL_UDP_GRO
    (void)snprintf(gro, sizeof(gro),
                   " nonblock=%d empty_reads=%lu idle_skips=%lu gro_on=%d gro_enables=%lu gro_aborts=%lu "
                   "gro_commits=%lu timer_waits=%lu timer_sets=%lu timer_fires=%lu timer_plain=%lu",
                   node->hal.nonblocking ? 1 : 0, (unsigned long)node->hal.rx_empty_reads,
                   (unsigned long)node->hal.rx_idle_skips, node->hal.gro_state != TT_GRO_PLAIN ? 1 : 0,
                   (unsigned long)node->hal.gro_enables, (unsigned long)node->hal.gro_aborts,
                   (unsigned long)node->hal.gro_commits, (unsigned long)node->hal.timer_waits,
                   (unsigned long)node->hal.timer_sets, (unsigned long)node->hal.timer_fires,
                   (unsigned long)node->hal.timer_plain_waits);
#else
    (void)snprintf(gro, sizeof(gro),
                   " nonblock=%d empty_reads=%lu idle_skips=%lu timer_waits=%lu timer_sets=%lu timer_fires=%lu "
                   "timer_plain=%lu",
                   node->hal.nonblocking ? 1 : 0, (unsigned long)node->hal.rx_empty_reads,
                   (unsigned long)node->hal.rx_idle_skips, (unsigned long)node->hal.timer_waits,
                   (unsigned long)node->hal.timer_sets, (unsigned long)node->hal.timer_fires,
                   (unsigned long)node->hal.timer_plain_waits);
#endif
    // UDP checksum 0 (an option, "UDP checksum 0"), on the same line: the arm's treatment check. Not on a build without
    // large samples, which compiles it out.
    char csum0[CSUM0_LINE_BYTES] = "";
#if TT_HAL_UDP_CSUM0
    (void)snprintf(csum0, sizeof(csum0),
                   " csum0=%d csum0_switches=%lu csum0_calls=%lu csum0_datagrams=%lu csum0_retries=%lu",
                   (int)node->hal.csum0, (unsigned long)node->hal.csum0_switches, (unsigned long)node->hal.csum0_calls,
                   (unsigned long)node->hal.csum0_datagrams, (unsigned long)node->hal.csum0_retries);
#endif
#if tt_RX_DRAIN
    drain_stop(node);
#if TT_HAL_UDP_GRO
    const unsigned long drain_merged = (unsigned long)node->hal.drain_merged;
#else
    const unsigned long drain_merged = 0;
#endif
    TT_LOG_INFO("Node %u rx: socket_drops=%u well_known_drops=%u data_drops=%u drain=%d drain_starts=%lu "
                "drain_datagrams=%lu drain_truncated=%lu drain_overruns=%lu drain_buffers=%u x %u drain_ring=%s "
                "drain_merged=%lu drain_rearms=%lu drain_grows=%lu%s%s%s",
                node->id, (unsigned)tt_hal_rx_socket_drops(node), (unsigned)socket_drops(node->hal.sock),
                (unsigned)socket_drops(node->hal.data_sock), node->hal.drain_fd == -2 ? -1 : 1,
                (unsigned long)node->hal.drain_starts, (unsigned long)node->hal.drain_datagrams,
                (unsigned long)node->hal.drain_truncated, (unsigned long)node->hal.drain_overruns,
                (unsigned)(node->hal.drain_fd >= 0 ? node->hal.drain_count : 0U),
                (unsigned)(node->hal.drain_fd >= 0 ? node->hal.drain_size : 0U), drain_ring_name(node->hal.drain_ring),
                drain_merged, (unsigned long)node->hal.drain_rearms, (unsigned long)node->hal.drain_grows, direct, gro,
                csum0);
    drain_release(&node->hal); // before the sockets: its requests go with its ring
#else
    TT_LOG_INFO("Node %u rx: socket_drops=%u well_known_drops=%u data_drops=%u drain=0%s%s%s", node->id,
                (unsigned)tt_hal_rx_socket_drops(node), (unsigned)socket_drops(node->hal.sock),
                (unsigned)socket_drops(node->hal.data_sock), direct, gro, csum0);
#endif
#if tt_CONTEXT_ID_CLAIM
    release_claimed_id(node);
#endif
#if tt_HAL_IO_URING
    uring_close(node); // before the sockets: a poll in flight on a socket being closed is cancelled with the ring
#endif
    node->hal.rx_count = 0; // anything a batch still held belonged to the sockets closed below
    node->hal.rx_next = 0;
#if TT_HAL_UDP_GRO
    node->hal.gro_left = 0; // and so did anything a merged read still held
#endif
    if (node->hal.data_sock >= 0 && close(node->hal.data_sock) < 0) {
        TT_LOG_WARNING("Cannot close data socket: %s", strerror(errno));
    }
    node->hal.data_sock = -1;

    if (close(node->hal.sock) < 0) {
        TT_LOG_ERROR("Cannot close socket: %s", strerror(errno));
    }
#if tt_SEGMENT_ENABLED
    if (node->hal.bell_fd_plus1 > 0) { // normally closed with the segment (release_own_segment()); the backstop
        (void)close(node->hal.bell_fd_plus1 - 1);
        node->hal.bell_fd_plus1 = 0;
    }
#endif
    if (node->hal.tx_watch_fd_plus1 > 0) {
        (void)close(node->hal.tx_watch_fd_plus1 - 1);
        node->hal.tx_watch_fd_plus1 = 0;
    }
    if (node->hal.epoll_fd_plus1 > 0) {
        (void)close(node->hal.epoll_fd_plus1 - 1);
        node->hal.epoll_fd_plus1 = 0;
    }
    if (node->hal.timer_fd_plus1 > 0) {
        (void)close(node->hal.timer_fd_plus1 - 1);
        node->hal.timer_fd_plus1 = 0;
    }
    if (node->hal.wake_fd >= 0 && close(node->hal.wake_fd) < 0) {
        TT_LOG_ERROR("Cannot close wake eventfd: %s", strerror(errno));
    }
}

int32_t tt_send(struct tt_Context* node, const void* buf, size_t len) {
#if tt_DISCOVERY_OPTIONS
    if (_tt_CONFIG.discovery_range == tt_DISCOVERY_RANGE_OFF) {
        return (int32_t)len; // (g6) discovery off, the user's choice: nothing goes to the link
    }
#endif
#if tt_CONTEXT_ID_CLAIM
    if (node->id_muted) {
        return refuse_muted_send(node); // (g8) see refuse_muted_send()
    }
#endif
    TT_TRACE(tt_TRACE_TX_START);
    // Field by field, not an initializer: one zeroes the padding too (-fzero-init-padding-bits=all), a store a send.
    struct tt_SendAgain again;
    again.buf = buf;
    again.len = len;
    again.fd = node->hal.data_sock;
    again.to = node->hal.broadcast_addr;
    int32_t sent = (int32_t)hal_sendto(again.fd, buf, len, 0, (const struct sockaddr*)&again.to, sizeof(again.to));
    if (__builtin_expect(sent < 0, 0)) {
        sent = sendto_again(&again); // a blocking send: room, then again (struct tt_SendAgain)
    }
    TT_TRACE(tt_TRACE_TX_DONE);
    return sent;
}

int32_t tt_send_to(struct tt_Context* node, const void* buf, size_t len, uint32_t ip, uint16_t port) {
#if tt_DISCOVERY_OPTIONS
    if (_tt_CONFIG.discovery_range == tt_DISCOVERY_RANGE_OFF) {
        return (int32_t)len; // (g6) discovery off, the user's choice: nothing goes to the link
    }
#endif
#if tt_CONTEXT_ID_CLAIM
    if (node->id_muted) {
        return refuse_muted_send(node); // (g8) see refuse_muted_send()
    }
#endif
    struct tt_SendAgain again; // field by field (tt_send())
    again.to.sin_family = AF_INET;
    again.to.sin_addr.s_addr = htonl(ip);
    again.to.sin_port = htons(port);

    TT_TRACE(tt_TRACE_TX_START);
    again.buf = buf;
    again.len = len;
    again.fd = node->hal.data_sock;
    int32_t sent = (int32_t)hal_sendto(again.fd, buf, len, 0, (const struct sockaddr*)&again.to, sizeof(again.to));
    if (__builtin_expect(sent < 0, 0)) {
        sent = sendto_again(&again); // a blocking send: room, then again (struct tt_SendAgain)
    }
    TT_TRACE(tt_TRACE_TX_DONE);
    return sent;
}

int32_t tt_send_iov(struct tt_Context* node, const void* hdr, size_t hdr_len, const void* body, size_t body_len,
                    uint32_t ip, uint16_t port) {
#if tt_DISCOVERY_OPTIONS
    if (_tt_CONFIG.discovery_range == tt_DISCOVERY_RANGE_OFF) {
        return (int32_t)(hdr_len + body_len); // (g6) see tt_send()
    }
#endif
#if tt_CONTEXT_ID_CLAIM
    if (node->id_muted) {
        return refuse_muted_send(node); // (g8) see refuse_muted_send()
    }
#endif
    // NOLINTNEXTLINE(misc-include-cleaner) - see <sys/uio.h>'s own include comment
    struct iovec iov[2] = {
        {.iov_base = (void*)hdr, .iov_len = hdr_len},
        {.iov_base = (void*)body, .iov_len = body_len},
    };

    struct sockaddr_in unicast_addr;
    struct tt_SendMsgAgain again; // the message zeroed as before, the socket beside it (tt_send())
    again.msg = (struct msghdr) {0};
    again.fd = node->hal.data_sock;
    struct msghdr* msg = &again.msg;
    msg->msg_iov = iov;
    msg->msg_iovlen = 2;
    if (ip != 0) {
        unicast_addr.sin_family = AF_INET;
        unicast_addr.sin_addr.s_addr = htonl(ip);
        unicast_addr.sin_port = htons(port);
        msg->msg_name = &unicast_addr;
        msg->msg_namelen = sizeof(unicast_addr);
    } else {
        msg->msg_name = &node->hal.broadcast_addr;
        msg->msg_namelen = sizeof(node->hal.broadcast_addr);
    }

    TT_TRACE(tt_TRACE_TX_START);
    int32_t sent = (int32_t)hal_sendmsg(again.fd, msg, 0);
    if (__builtin_expect(sent < 0, 0)) {
        sent = sendmsg_again(&again); // a blocking send: room, then again (struct tt_SendMsgAgain)
    }
    return sent;
}

// Messages per sendmmsg() call, and datagrams: a message is one datagram, or with UDP_SEGMENT a run of them
// (gso_run()). A sample's fragments (at most tt_FRAG_MAX_COUNT) always fit one call; a longer batch takes several, in
// order. Without send offload a call carries 64 datagrams, as it always did.
#define TT_SEND_BATCH_CHUNK 64
#define TT_SEND_CALL_DATAGRAMS 128
// The kernel's bounds on one UDP_SEGMENT send: UDP_MAX_SEGMENTS (64 before Linux 6.9, 128 after - the lower one), and
// the payload of the one IPv4 UDP datagram it is cut from.
#define TT_GSO_MAX_SEGMENTS 64U
#define TT_GSO_MAX_BYTES 65507U

// Which runs go as one (2026-10-11): a large sample's fragments (FRAG_FIRST_L/FRAG_CONT_L) only. The first rig A/B
// (Pi 5 x2, a5747a85 against f3c948ba, ~/rig_results_safe/udpoffload_l2x_compare.txt) sent every run of same-size
// datagrams so, and core's p4 sample - two DATA_FRAG datagrams, a run of 2 - came out worse: the publisher's CPU per
// sample +3.5% at throughput (c4), +3.2% best effort (c15); and at latency (c17) 3% of pings unanswered within the
// client's 500 ms with no datagram lost (each side read every datagram the other sent, nothing was retransmitted) and
// up to 475 ms late: runs that left the host only when something else was sent after them. Not an instruction cost:
// on the PC a run of 2 takes fewer instructions as one message than as two (experiments/gso_run_cost.sh: break-even
// 1.4 datagrams, the Pi's NIC as the veth, segmenting in software). Where it paid on the rig was the 1 MB sample, runs
// of 44 (publisher -19%, subscriber -10.5%). So a run is made of large fragments and nothing else - what the rig
// showed paying - and a build without large samples sends none (TT_HAL_UDP_GSO).
// A large fragment by its framing (tickle.c large_write_framing()): the single form (addressed to everyone - byte 0
// the marker, byte 3 the type) or the full one (a retransmission to one node - the magic, then the submessage header's
// type at byte 4).
#if TT_HAL_UDP_GSO
static bool gso_type_large(uint8_t type) {
    return type == tt_SUBMESSAGE_TYPE_FRAG_FIRST_L || type == tt_SUBMESSAGE_TYPE_FRAG_CONT_L;
}
static bool gso_large_fragment(const struct tt_OutDatagram* datagram) {
    const uint8_t* head = (const uint8_t*)datagram->head;
    if (datagram->head_len >= sizeof(struct tt_SingleHeader) &&
        (head[0] == tt_SINGLE_MARKER_LE || head[0] == tt_SINGLE_MARKER_BE)) {
        return gso_type_large(head[3]);
    }
    return datagram->head_len >= sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader) &&
           ((head[0] == 'K' && head[1] == 'T') || (head[0] == 'T' && head[1] == 'K')) &&
           gso_type_large(head[sizeof(struct tt_Header)]);
}

// How many datagrams from `first` (of `available`) go as one UDP_SEGMENT send, which the kernel - or the NIC - cuts
// back into datagrams of the first one's length: large fragments (above), the same destination, the same length but
// the last (which may be shorter and ends the run), none empty and none longer than tt_CONTROL_MAX_LENGTH. Each cut is
// then exactly the datagram core built, so the wire carries what it carried without offload, and no datagram of more
// than 1472 bytes (the IP fragmentation DESIGN.md section 8 keeps out) can come from a run. 1 when nothing can follow.
static uint32_t gso_run(const struct tt_OutDatagram* first, uint32_t available) {
    const size_t size = first->head_len + first->body_len;
    if (size == 0 || size > tt_CONTROL_MAX_LENGTH || available < 2 || !gso_large_fragment(first)) {
        return 1;
    }
    uint32_t run = 1;
    size_t total = size;
    while (run < available && run < TT_GSO_MAX_SEGMENTS) {
        const struct tt_OutDatagram* next = &first[run];
        const size_t length = next->head_len + next->body_len;
        if (next->ip != first->ip || (first->ip != 0 && next->port != first->port) || length == 0 || length > size ||
            total + length > TT_GSO_MAX_BYTES || !gso_large_fragment(next)) {
            break;
        }
        total += length;
        run++;
        if (length < size) {
            break; // only the last may be short
        }
    }
    return run;
}

// A UDP_SEGMENT send refused for what it is rather than for what it carries: EIO, no checksum offload on the route's
// device; EINVAL, a segment longer than the route's MTU allows; the others, a kernel or socket without it. Send offload
// goes off for the context, once, and the run goes again datagram by datagram.
static bool gso_refused(struct tt_Context* node, int err) {
    // NOLINTNEXTLINE(misc-include-cleaner) - the errno values: glibc-private headers
    if (err != EIO && err != EINVAL && err != ENOPROTOOPT && err != EOPNOTSUPP) {
        return false;
    }
    TT_LOG_WARNING("UDP_SEGMENT refused (%s) - send offload off for context %u, every datagram sent alone",
                   strerror(err), node->id);
    node->hal.gso_on = false;
    node->udp_offload &= (uint8_t)~TT_UDP_OFFLOAD_GSO;
    return true;
}
#endif

// ---- UDP checksum 0 (2026-10-14; the default since 2026-10-15, TT_UDP_CSUM0=0 keeps every checksum)
// --------------------------------------------------------------------------------
//
// Where the checksum is made (the rig: Pi 5, macb, Linux 6.8). A datagram of one frame has it made by the NIC on send
// (tx-checksum-ip-generic, CHECKSUM_PARTIAL) and checked by it on receipt (rx-checksumming), so it costs no CPU on
// either side. A hybrid run is one datagram IP fragments into up to 45 frames: no NIC can check a fragment's UDP
// checksum, so the receiver checks it in software over the reassembled datagram - in recvmsg(), as the copy to user
// (skb_copy_and_csum_datagram_msg(); on arm64 a csum_partial() pass over the cold data, then the copy). The kallsyms
// profile of the Pi 5 subscriber put that checksum at 0.31 / 0.30 ms per MB of TickLE's kernel time at 1 / 4 MB and
// 0.24 / 0.22 of Fast DDS's (~/claude_reports/rx_perf_gap2_2026-10-13.md, section 9). An IPv4 UDP checksum of 0 means
// "none": the receiving kernel then marks the datagram checked (udp4_csum_init()) and reads it with a plain copy.
// The sender makes the checksum of an IP-fragmented datagram in software as it copies from user
// (ip_generic_getfrag()) whether or not SO_NO_CHECK is set - the option only leaves it out of the header - so the
// sender is not expected to save anything.
//
// Scope: SO_NO_CHECK goes on on the data socket before a sendmmsg() call that carries a datagram larger than one
// frame's payload (tt_ETHERNET_UDP_PAYLOAD), and stays on - until a call that carries a UDP_SEGMENT run, which the
// kernel refuses with it on (EINVAL; measured on the PC, Linux 7.0), so it goes off before such a call. A batch is cut
// where the kind changes (a short datagram between them joins either). Turning it on and off around every call cost
// two setsockopt() calls a call, ~9 calls a 1 MB sample under the send buffer's back-pressure (udp_csum0_pc.sh, first
// session); left on, an AUTO stream of runs switches it once. So while the option is on (the default), the data
// socket's other datagrams (tt_send*(), and a batch's short datagrams) may go with checksum 0 too: datagrams of one
// frame, which the link's FCS covers as it covers every frame. A UDP_SEGMENT call that meets it on anyway (another
// thread switched it) turns it off and goes again (csum0_retries); a second refusal is send offload's own
// (gso_refused()).
#if TT_HAL_UDP_CSUM0
static void csum0_setup(struct tt_Context* node) {
    node->hal.csum0 = 0;
    const char* setting = getenv("TT_UDP_CSUM0");
    if (setting != NULL && strcmp(setting, "0") == 0) {
        return; // every datagram keeps its checksum
    }
    int enable = 1;
    int disable = 0;
    // NOLINTNEXTLINE(misc-include-cleaner) - SO_NO_CHECK: glibc-private header behind <sys/socket.h>
    if (setsockopt(node->hal.data_sock, SOL_SOCKET, SO_NO_CHECK, &enable, sizeof(enable)) != 0 ||
        // NOLINTNEXTLINE(misc-include-cleaner) - SO_NO_CHECK: as above
        setsockopt(node->hal.data_sock, SOL_SOCKET, SO_NO_CHECK, &disable, sizeof(disable)) != 0) {
        TT_LOG_WARNING("SO_NO_CHECK refused (%s) - no UDP checksum 0, every datagram keeps its checksum",
                       strerror(errno));
        node->hal.csum0 = -1;
        return;
    }
    node->hal.csum0 = 1;
}

// The data socket's SO_NO_CHECK to `enable`, unless it is already (csum0_on); a refusal leaves it as it was.
static void csum0_set(struct tt_Context* node, bool enable) {
    if (node->hal.csum0_on == enable) {
        return;
    }
    int value = enable ? 1 : 0;
    // NOLINTNEXTLINE(misc-include-cleaner) - SO_NO_CHECK: glibc-private header behind <sys/socket.h>
    if (setsockopt(node->hal.data_sock, SOL_SOCKET, SO_NO_CHECK, &value, sizeof(value)) == 0) {
        node->hal.csum0_on = enable;
        node->hal.csum0_switches++;
    }
}
#endif

// The body of tt_send_batch() and tt_send_batch_nonblocking(): `flags` is 0 or MSG_DONTWAIT. Returns how many went;
// with MSG_DONTWAIT a full send buffer ends the batch early instead of failing it.
static int32_t send_batch_flags(struct tt_Context* node, const struct tt_OutDatagram* datagrams, uint32_t count,
                                int flags);

int32_t tt_send_batch_nonblocking(struct tt_Context* node, const struct tt_OutDatagram* datagrams, uint32_t count) {
#if tt_DISCOVERY_OPTIONS
    if (_tt_CONFIG.discovery_range == tt_DISCOVERY_RANGE_OFF) {
        return (int32_t)count; // (g6) see tt_send_batch()
    }
#endif
#if tt_CONTEXT_ID_CLAIM
    if (node->id_muted) {
        return refuse_muted_send(node); // (g8) see refuse_muted_send()
    }
#endif
    return send_batch_flags(node, datagrams, count, MSG_DONTWAIT);
}

int32_t tt_send_batch(struct tt_Context* node, const struct tt_OutDatagram* datagrams, uint32_t count) {
#if tt_DISCOVERY_OPTIONS
    if (_tt_CONFIG.discovery_range == tt_DISCOVERY_RANGE_OFF) {
        return (int32_t)count; // (g6) discovery off, the user's choice: nothing goes to the link
    }
#endif
#if tt_CONTEXT_ID_CLAIM
    if (node->id_muted) {
        return refuse_muted_send(node); // (g8) see refuse_muted_send()
    }
#endif
    int32_t sent = send_batch_flags(node, datagrams, count, 0);
    return sent == (int32_t)count ? sent : -1;
}

#if tt_LARGE_SAMPLES
// The next wait ends, once, when data_sock - every large send's socket (send_batch_flags()) - has room again: a UDP
// socket reports EPOLLOUT/POLLOUT once half its send buffer is free, which is when a blocking sendmsg() is woken. With
// the wait set, its duplicate (struct tt_hal.tx_watch_fd_plus1) is armed one-shot; without it, the next ppoll() asks
// for POLLOUT. Safe from the publishing thread while another waits: epoll_ctl() wakes a waiter on a socket already
// writable, and a ppoll() already asleep is woken by the fallback timer core schedules beside it.
bool tt_tx_await_writable(struct tt_Context* node) {
    struct tt_hal* hal = &node->hal;
#if TT_HAL_EPOLL
    if (hal->epoll_fd_plus1 > 0) {
        struct epoll_event event = {.events = EPOLLOUT | EPOLLONESHOT, .data = {.u32 = RX_READY_WRITABLE}};
        if (hal->tx_watch_fd_plus1 > 0) {
            return epoll_ctl(hal->epoll_fd_plus1 - 1, EPOLL_CTL_MOD, hal->tx_watch_fd_plus1 - 1, &event) == 0;
        }
        int watch = fcntl(hal->data_sock, F_DUPFD_CLOEXEC, 0);
        if (watch < 0) {
            return false;
        }
        if (epoll_ctl(hal->epoll_fd_plus1 - 1, EPOLL_CTL_ADD, watch, &event) != 0) {
            (void)close(watch);
            return false;
        }
        hal->tx_watch_fd_plus1 = watch + 1;
        return true;
    }
#endif
    __atomic_store_n(&hal->tx_watch_armed, true, __ATOMIC_RELEASE);
    return true;
}
#endif

#if TT_HAL_UDP_GSO
// One message of a sendmmsg() call: `run` datagrams from `first`, their pieces in `iov` (room for two each), to their
// destination (`addr`, unless broadcast), with the UDP_SEGMENT cmsg in `control` when run > 1.
union gso_control {
    char bytes[CMSG_SPACE(sizeof(uint16_t))];
    struct cmsghdr align; // NOLINT(misc-include-cleaner) - <sys/socket.h>
};
// NOLINTNEXTLINE(misc-include-cleaner) - struct iovec: see <sys/uio.h>'s own include comment
static void send_message_setup(struct tt_Context* node, struct mmsghdr* msg, struct iovec* iov,
                               struct sockaddr_in* addr, union gso_control* control, const struct tt_OutDatagram* first,
                               uint32_t run) {
    memset(msg, 0, sizeof(*msg));
    size_t pieces = 0;
    for (uint32_t k = 0; k < run; k++) {
        iov[pieces].iov_base = (void*)first[k].head;
        iov[pieces++].iov_len = first[k].head_len;
        if (first[k].body_len != 0) {
            iov[pieces].iov_base = (void*)first[k].body;
            iov[pieces++].iov_len = first[k].body_len;
        }
    }
    msg->msg_hdr.msg_iov = iov;
    msg->msg_hdr.msg_iovlen = pieces;
    if (first->ip != 0) {
        memset(addr, 0, sizeof(*addr));
        addr->sin_family = AF_INET;
        addr->sin_addr.s_addr = htonl(first->ip);
        addr->sin_port = htons(first->port);
        msg->msg_hdr.msg_name = addr;
        msg->msg_hdr.msg_namelen = sizeof(*addr);
    } else {
        msg->msg_hdr.msg_name = &node->hal.broadcast_addr;
        msg->msg_hdr.msg_namelen = sizeof(node->hal.broadcast_addr);
    }
    if (run > 1) {
        const uint16_t segment = (uint16_t)(first->head_len + first->body_len); // <= tt_CONTROL_MAX_LENGTH: gso_run()
        msg->msg_hdr.msg_control = control->bytes;
        msg->msg_hdr.msg_controllen = sizeof(control->bytes);
        // NOLINTNEXTLINE(misc-include-cleaner) - CMSG_*: <sys/socket.h>
        struct cmsghdr* header = CMSG_FIRSTHDR(&msg->msg_hdr);
        header->cmsg_level = SOL_UDP;
        header->cmsg_type = UDP_SEGMENT;
        header->cmsg_len = CMSG_LEN(sizeof(segment));
        memcpy(CMSG_DATA(header), &segment, sizeof(segment));
    }
}

// The datagrams the first `messages` messages of a call carried (`carried`, per message), counting the runs among them.
static uint32_t gso_count_sent(struct tt_Context* node, const uint8_t* carried, int messages) {
    uint32_t datagrams = 0;
    for (int i = 0; i < messages; i++) {
        datagrams += carried[i];
        if (carried[i] > 1) {
            node->udp_gso_sends++;
            node->udp_gso_datagrams += carried[i];
        }
    }
    return datagrams;
}

#if !TT_HAL_UDP_CSUM0
#error "the UDP_SEGMENT send path carries UDP checksum 0: TT_HAL_UDP_CSUM0 is TT_HAL_UDP_GSO (hal_linux.h)"
#endif
// One sendmmsg() call of send_batch_flags(): its messages, their pieces and destinations, and what the call carries.
struct send_call {
    struct mmsghdr msgs[TT_SEND_BATCH_CHUNK];
    // NOLINTNEXTLINE(misc-include-cleaner) - see <sys/uio.h>'s own include comment
    struct iovec iov[2 * TT_SEND_CALL_DATAGRAMS];
    struct sockaddr_in addrs[TT_SEND_BATCH_CHUNK];
    union gso_control control[TT_SEND_BATCH_CHUNK];
    uint8_t carried[TT_SEND_BATCH_CHUNK]; // datagrams in each message
    uint32_t messages;
    uint32_t placed; // datagrams in the call
    bool zero;       // it carries an IP-fragmented datagram: with UDP checksum 0 on, it goes with checksum 0
    bool segmented;  // it carries a UDP_SEGMENT run, which the kernel refuses with checksum 0 on
};

// UDP checksum 0 ("UDP checksum 0", above): whether the next message, `run` datagrams from `first`, may join the call.
// It may not when the call's kind and its differ - an IP-fragmented datagram after a UDP_SEGMENT run, or a run after an
// IP-fragmented datagram - and then starts the next call. Always with the option off.
static bool csum0_joins(const struct tt_Context* node, struct send_call* call, const struct tt_OutDatagram* first,
                        uint32_t run) {
    if (node->hal.csum0 <= 0) {
        return true;
    }
    const bool fragmented = first->head_len + first->body_len > tt_ETHERNET_UDP_PAYLOAD;
    if ((fragmented && call->segmented) || (run > 1 && call->zero)) {
        return false;
    }
    call->zero = call->zero || fragmented;
    call->segmented = call->segmented || run > 1;
    return true;
}

// The next call of a batch: as many of `available` datagrams from `first` as one call takes.
static void send_call_build(struct tt_Context* node, struct send_call* call, const struct tt_OutDatagram* first,
                            uint32_t available) {
    call->messages = 0;
    call->placed = 0;
    call->zero = false;
    call->segmented = false;
    while (call->messages < TT_SEND_BATCH_CHUNK && call->placed < available && call->placed < TT_SEND_CALL_DATAGRAMS) {
        const struct tt_OutDatagram* next = &first[call->placed];
        uint32_t room = available - call->placed;
        room = room < TT_SEND_CALL_DATAGRAMS - call->placed ? room : TT_SEND_CALL_DATAGRAMS - call->placed;
        uint32_t run = node->hal.gso_on ? gso_run(next, room) : 1U;
        if (!csum0_joins(node, call, next, run)) {
            break; // the other kind: the next call
        }
        send_message_setup(node, &call->msgs[call->messages], &call->iov[(size_t)2U * call->placed],
                           &call->addrs[call->messages], &call->control[call->messages], next, run);
        call->carried[call->messages++] = (uint8_t)run;
        call->placed += run;
    }
}

// Before the call: SO_NO_CHECK on for a call of IP-fragmented datagrams, off for one with a UDP_SEGMENT run, and as it
// was for any other. A refusal to turn it on: the call goes with checksums (and is counted as such, csum0_sent()).
static void csum0_before(struct tt_Context* node, const struct send_call* call) {
    if (call->segmented || call->zero) { // both false with the option off (csum0_joins())
        csum0_set(node, call->zero);
    }
}

// After the call: the datagrams of its first `result` messages went with checksum 0 if SO_NO_CHECK was on.
static void csum0_sent(struct tt_Context* node, const struct send_call* call, int result) {
    if (!node->hal.csum0_on || result <= 0) {
        return;
    }
    node->hal.csum0_calls++;
    for (int i = 0; i < result; i++) {
        node->hal.csum0_datagrams += call->carried[i];
    }
}

// A UDP_SEGMENT call refused with `err`: true when SO_NO_CHECK may have been the cause (EINVAL with the option on,
// another thread having turned it on) and the call should go again - once (`retried`) - with it now off.
static bool csum0_retry(struct tt_Context* node, int err, bool* retried) {
    // NOLINTNEXTLINE(misc-include-cleaner) - EINVAL: glibc-private header
    if (err != EINVAL || node->hal.csum0 <= 0 || *retried) {
        return false;
    }
    *retried = true;
    node->hal.csum0_on = true; // what the kernel said, whatever this thread last set
    csum0_set(node, false);
    node->hal.csum0_retries++;
    return true;
}

// Send offload (UDP offload, above tt_bind()): a run of datagrams gso_run() accepts goes as one message, which the
// kernel cuts up below the socket layer - on the PC in software just before the device, on a NIC with UDP segmentation
// in hardware. One sendmmsg() carries runs and single datagrams alike, so a batch costs the calls it did before.
static int32_t send_batch_flags(struct tt_Context* node, const struct tt_OutDatagram* datagrams, uint32_t count,
                                int flags) {
    uint32_t sent = 0;
    bool retried = false; // this call's UDP_SEGMENT send met SO_NO_CHECK on, which was then turned off (csum0_retry())
    while (sent < count) {
        struct send_call call;
        send_call_build(node, &call, &datagrams[sent], count - sent);
        csum0_before(node, &call);
        TT_TRACE(tt_TRACE_TX_START);
        int result = sendmmsg(node->hal.data_sock, call.msgs, call.messages, flags);
        TT_TRACE(tt_TRACE_TX_DONE);
        csum0_sent(node, &call, result);
        if (result <= 0) {
            int err = errno;
            // NOLINTNEXTLINE(misc-include-cleaner) - EAGAIN/EWOULDBLOCK: glibc-private headers
            if (flags != 0 && result < 0 && (err == EAGAIN || err == EWOULDBLOCK)) {
                return (int32_t)sent; // the send buffer is full: what went, went; the caller sends the rest later
            }
            errno = err;
            if (flags == 0 && result < 0 && send_wait_for_room(node->hal.data_sock)) {
                continue; // a blocking batch on the non-blocking socket: room, then the rest
            }
            if (result < 0 && call.carried[0] > 1 && (csum0_retry(node, err, &retried) || gso_refused(node, err))) {
                continue; // nothing of this call went: again from the same datagram (checksums on, or one at a time)
            }
            return -1; // errno says why; a 0 would otherwise loop forever
        }
        retried = false;
        // A short count (sendmmsg() stopped at a message that did not go) loops: the next call reports why - the
        // error, or with MSG_DONTWAIT the EAGAIN of a full buffer, answered above.
        sent += gso_count_sent(node, call.carried, result);
    }
    return (int32_t)count;
}
#else
// Without send offload (a build without large samples, TT_HAL_UDP_GSO): one message a datagram, the loop it always was.
static int32_t send_batch_flags(struct tt_Context* node, const struct tt_OutDatagram* datagrams, uint32_t count,
                                int flags) {
    uint32_t sent = 0;
    while (sent < count) {
        uint32_t chunk = count - sent < TT_SEND_BATCH_CHUNK ? count - sent : TT_SEND_BATCH_CHUNK;
        struct mmsghdr msgs[TT_SEND_BATCH_CHUNK];
        // NOLINTNEXTLINE(misc-include-cleaner) - see <sys/uio.h>'s own include comment
        struct iovec iov[TT_SEND_BATCH_CHUNK][2];
        struct sockaddr_in addrs[TT_SEND_BATCH_CHUNK];
        memset(msgs, 0, sizeof(msgs[0]) * chunk);
        for (uint32_t i = 0; i < chunk; i++) {
            const struct tt_OutDatagram* datagram = &datagrams[sent + i];
            iov[i][0].iov_base = (void*)datagram->head;
            iov[i][0].iov_len = datagram->head_len;
            iov[i][1].iov_base = (void*)datagram->body;
            iov[i][1].iov_len = datagram->body_len;
            msgs[i].msg_hdr.msg_iov = iov[i];
            msgs[i].msg_hdr.msg_iovlen = datagram->body_len != 0 ? 2 : 1;
            if (datagram->ip != 0) {
                memset(&addrs[i], 0, sizeof(addrs[i]));
                addrs[i].sin_family = AF_INET;
                addrs[i].sin_addr.s_addr = htonl(datagram->ip);
                addrs[i].sin_port = htons(datagram->port);
                msgs[i].msg_hdr.msg_name = &addrs[i];
                msgs[i].msg_hdr.msg_namelen = sizeof(addrs[i]);
            } else {
                msgs[i].msg_hdr.msg_name = &node->hal.broadcast_addr;
                msgs[i].msg_hdr.msg_namelen = sizeof(node->hal.broadcast_addr);
            }
        }
        TT_TRACE(tt_TRACE_TX_START);
        int result = sendmmsg(node->hal.data_sock, msgs, chunk, flags);
        TT_TRACE(tt_TRACE_TX_DONE);
        if (result <= 0) {
            // NOLINTNEXTLINE(misc-include-cleaner) - EAGAIN/EWOULDBLOCK: glibc-private headers
            if (flags != 0 && result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return (int32_t)sent; // the send buffer is full: what went, went; the caller sends the rest later
            }
            if (flags == 0 && result < 0 && send_wait_for_room(node->hal.data_sock)) {
                continue; // a blocking batch on the non-blocking socket: room, then the rest
            }
            return -1; // errno says why; a 0 would otherwise loop forever
        }
        // A short count (sendmmsg() stopped at a datagram that did not go) loops: the next call reports why - the
        // error, or with MSG_DONTWAIT the EAGAIN of a full buffer, answered above.
        sent += (uint32_t)result;
    }
    return (int32_t)count;
}
#endif

// Bits of struct tt_hal.rx_idle (TT_RX_IDLE_WELL_KNOWN, TT_RX_IDLE_DATA, hal_linux.h) - see tt_try_receive().
// How long tt_try_receive() goes on returning datagrams while a socket is skipped before it asks that socket again
// (struct tt_hal.rx_idle_since_ns): tt_RECEIVE_TIMEOUT, the same bound the poll loop puts on scheduler work before it
// looks at the socket at all, so neither can keep the other from the data socket for longer. It costs one empty read
// per tt_RECEIVE_TIMEOUT, only while one socket is busy and the other idle - a fixed share of time on any hardware.
// It was a count, 64 datagrams (2026-10-05), whose period was 64 times whatever handling a datagram cost: ~64 us of
// starvation on a fast receiver, 64 ms behind a 1 ms callback, and an empty read every 64 datagrams however fast they
// came (ROADMAP.md 5a). The time is the one the running poll already read (struct tt_Context.rx_clock_ns,
// refreshed every tt_RX_CLOCK_REFRESH datagrams of a drain), so the bound is tt_RECEIVE_TIMEOUT or tt_RX_CLOCK_REFRESH
// datagrams, whichever ends later, and the check reads no clock of its own inside a poll.
#define TT_RX_IDLE_RECHECK_NS ((uint64_t)tt_RECEIVE_TIMEOUT)

// The read after a wake-up's datagram (struct tt_hal.rx_lone_streak). The wake-up read is one recvfrom(), kept
// single because it is on the latency path (tt_receive()), so whether more waited is learnt only by reading again, in
// the drain: on the core p1 ping-pong that read found nothing on every round trip, at both ends (lat_gap_pc.sh,
// affea967: recvmmsg() 1.004 calls and 1.002 EAGAIN a round trip on the client). Skipping it loses nothing - every
// wait is level-triggered for the sockets, so a datagram left in one ends the next wait at once - but costs, when one
// did wait, a wait and a single read where a batch would have taken it. So it is skipped only after
// TT_RX_LONE_STREAK of these reads in a row found the socket empty, and one wake-up in TT_RX_LONE_RECHECK still reads:
// a node whose traffic turns into a backlog reads one datagram per wait for at most TT_RX_LONE_RECHECK - 1 wake-ups,
// and one whose traffic stays lone pays 1/TT_RX_LONE_RECHECK of the reads it paid. Counts of wake-ups, not a time:
// what a wrong guess costs is one wait per wake-up on any hardware. TT_RX_LONE_STREAK 0 turns the skip off.
#ifndef TT_RX_LONE_STREAK
#define TT_RX_LONE_STREAK 8U
#endif
#define TT_RX_LONE_RECHECK 16U

static inline void rx_lone_after_wake(struct tt_hal* hal, int socket_fd) {
    const uint8_t bit = socket_fd == hal->data_sock ? TT_RX_IDLE_DATA : TT_RX_IDLE_WELL_KNOWN;
    if (TT_RX_LONE_STREAK != 0 && hal->rx_lone_streak >= TT_RX_LONE_STREAK &&
        ++hal->rx_lone_skips < TT_RX_LONE_RECHECK) {
        hal->rx_idle |= bit; // the drain does not ask this socket: the next wait reports what came
        hal->rx_probe_bit = 0;
        return;
    }
    hal->rx_lone_skips = 0;
    hal->rx_probe_bit = bit;
}

// The drain's read of the socket the wake-up read (rx_probe_bit): `found` when it returned a datagram.
static inline void rx_lone_probed(struct tt_hal* hal, bool found) {
    hal->rx_probe_bit = 0;
    if (found) {
        hal->rx_lone_streak = 0;
    } else if (hal->rx_lone_streak < TT_RX_LONE_STREAK) {
        hal->rx_lone_streak++;
    }
}

// tt_receive()'s wait set: the two sockets, the wake eventfd, and - with the segment - its doorbell FIFO.
#if tt_SEGMENT_ENABLED
#define RX_WAIT_FDS 4
#else
#define RX_WAIT_FDS 3
#endif

#if tt_RX_DRAIN
// ---- The drain (tt_rx_drain_start(), hal.h)
// --------------------------------------------------------------------------
//
// While core is away from the sockets - in a callback, or returned to the application - an io_uring multishot recvmsg
// on each socket takes every datagram that arrives into a buffer of a ring this HAL provides. The kernel does the copy
// as task work of the thread that started it, which runs whenever that thread is interrupted or enters the kernel, so
// a thread busy in user space for milliseconds still has its socket emptied (rx_drain_pc.sh: a 5 ms or 10 ms stop per
// 4 MB sample lost 0 datagrams over a saturated link, against 7% and 18% without). tt_receive() and tt_try_receive()
// hand out what it took (rx_take_drained()), oldest first, while it goes on taking, and cancel it (drain_stop()) once
// it holds nothing, before they read a socket again: everything it took left the socket before anything still there,
// so order is kept.
//
// Its own ring, apart from tt_rx_maybe_ready()'s: that one is four entries of one-shot polls, and this one needs a
// completion entry per buffer.
//
// The drain and receive offload (UDP_GRO, "When receive offload is on" above). Who reads when: the drain reads the
// sockets only between tt_rx_drain_start() and the drain_stop() that rx_take_drained() makes once it holds nothing;
// core reads them (gro_read(), rx_read_one()) only outside that, and only core's own reads move the receive-offload
// state - a datagram handed out from the drain neither turns UDP_GRO on nor counts towards turning it off, and a settle
// is not decided while a drain runs or holds datagrams (drain_active()). So the option stays what it was when the
// drain started, and the drain takes what that option makes: its recvmsg asks for UDP_GRO's segment-size cmsg as
// gro_read() does, and a completion that holds a merged read is handed out one datagram at a time, in order, before
// the next completion (drain_split_*). Unless the sockets read plain, its buffers hold a merged read whole
// (tt_rx_drain_start()), so nothing merged is ever cut.
//
// Its buffers' length (tt_rx_drain_start()): the longest datagram core says may arrive, and core asks for the
// reassembly ceiling this context advertises (tickle.c large_rx_drain()) - a hybrid run is one IP-reassembled
// datagram of up to 65,507 bytes, and no writer sends one longer than the ceiling it was told. A run cut by the drain
// would be lost to a receiver that could have taken it (drain_truncated, which must stay 0).
#define DRAIN_SOCKETS 2U
#define DRAIN_RECV 0x5243560000000000ULL    // user_data of a recvmsg, | the socket's index (0 well-known, 1 data)
#define DRAIN_CANCEL 0x43414e0000000000ULL  // user_data of a cancellation
#define DRAIN_REMOVE 0x524d560000000000ULL  // user_data of a classic buffer removal (tt_RX_DRAIN_RING_CLASSIC)
#define DRAIN_PROVIDE 0x5056440000000000ULL // user_data of a classic buffer provision (tt_RX_DRAIN_RING_CLASSIC)
#define DRAIN_GROUP 1U                      // the buffer group of a ring
#define DRAIN_GROUP_CLASSIC 2U // of classic buffers: never the group a refused or unmappable ring may still hold
#define DRAIN_ENTRIES 4U
#define DRAIN_STOP_TRIES 100          // waits of DRAIN_STOP_WAIT_NS for both recvmsgs to end
#define DRAIN_STOP_WAIT_NS 10000000LL // 10 ms
#define DRAIN_ALIGN 64U               // a buffer's length is a multiple of this
// What a buffer holds before the datagram: the completion's own header, the sender's address and, where receive
// offload is compiled, room for UDP_GRO's segment-size cmsg (the multishot layout reserves both whether used or not).
#if TT_HAL_UDP_GRO
#define DRAIN_CONTROL ((uint32_t)CMSG_SPACE(sizeof(int))) // NOLINT(misc-include-cleaner) - <sys/socket.h>
#else
#define DRAIN_CONTROL 0U
#endif
#define DRAIN_HEADER ((uint32_t)(sizeof(struct io_uring_recvmsg_out) + sizeof(struct sockaddr_in)) + DRAIN_CONTROL)
#define DRAIN_ASKED_BYTES 256U // drain_describe()'s line
#define DRAIN_REARM_SHARE 8U   // an overrun's recvmsg goes again once 1/8 of the buffers is back (drain_rearm_now())

static void drain_release(struct tt_hal* hal) {
    if (hal->drain_buffers != NULL) {
        (void)munmap(hal->drain_buffers, (size_t)hal->drain_count * hal->drain_size);
    }
    if (hal->drain_buf_ring != NULL) {
        (void)munmap(hal->drain_buf_ring, hal->drain_buf_ring_len);
    }
    if (hal->drain_sqes != NULL) {
        (void)munmap(hal->drain_sqes, hal->drain_sqes_len);
    }
    if (hal->drain_ring_map != NULL) {
        (void)munmap(hal->drain_ring_map, hal->drain_ring_map_len);
    }
    if (hal->drain_fd >= 0) {
        (void)close(hal->drain_fd); // with the ring go its requests and the registered buffer ring
    }
    hal->drain_buffers = NULL;
    hal->drain_buf_ring = NULL;
    hal->drain_sqes = NULL;
    hal->drain_ring_map = NULL;
    hal->drain_fd = -1;
    hal->drain_running = false;
    hal->drain_armed = 0;
    hal->drain_pending = 0;
    hal->drain_ring = tt_RX_DRAIN_RING_NONE;
}

// Said once per process: why large samples on a saturated link may lose what the socket's buffer cannot hold.
static void drain_refused(struct tt_hal* hal, const char* step, int err) {
    static bool told = false;
    if (!told) {
        told = true;
        TT_LOG_WARNING("Receive drain unavailable (%s: %s) - while a callback runs, a burst larger than the socket's "
                       "receive buffer is dropped. See README.md, \"io_uring\".",
                       step, strerror(err));
    }
    drain_release(hal);
    hal->drain_fd = -2;
}

// Puts buffer `bid` back on the ring for the kernel to fill. A ring entry is written field by field, never whole: the
// ring's tail lives in the first entry's last field.
static void drain_give_back(struct tt_hal* hal, uint16_t bid) {
    if (hal->drain_ring == tt_RX_DRAIN_RING_CLASSIC) {
        return; // all of them are handed over again at the next start, when none is in use (tt_rx_drain_start())
    }
    struct io_uring_buf_ring* ring = (struct io_uring_buf_ring*)hal->drain_buf_ring;
    struct io_uring_buf* entry = &ring->bufs[hal->drain_buf_tail & (hal->drain_count - 1U)];
    entry->addr = (uint64_t)(uintptr_t)(hal->drain_buffers + ((size_t)bid * hal->drain_size));
    entry->len = hal->drain_size;
    entry->bid = bid;
    hal->drain_buf_tail++;
    hal->drain_returned++;
    __atomic_store_n(&ring->tail, hal->drain_buf_tail, __ATOMIC_RELEASE);
}

// One IORING_REGISTER_PBUF_RING of `count` entries in group `group`: 0, or the errno the kernel refused it with.
static int drain_register_ring(int ring_fd, uint64_t ring_addr, uint32_t count, uint16_t group, uint16_t flags) {
    struct io_uring_buf_reg reg;
    memset(&reg, 0, sizeof(reg)); // resv[] zero: every kernel refuses anything else with EINVAL
    reg.ring_addr = ring_addr;
    reg.ring_entries = count;
    reg.bgid = group;
    reg.flags = flags;
    // NOLINTNEXTLINE(misc-include-cleaner) - see the <sys/syscall.h> include
    return syscall(__NR_io_uring_register, ring_fd, IORING_REGISTER_PBUF_RING, &reg, 1) < 0 ? errno : 0;
}

// What a refused registration asked for, beside the kernel that refused it: EINVAL alone names none of the reasons
// Linux 6.8 has for it (io_uring/kbuf.c: resv[] nonzero, an unknown flag, ring_addr not page-aligned or given with
// IOU_PBUF_RING_MMAP, entries not a power of two or above 65535, a highmem or miscoloured page of the ring).
static void drain_describe(char* out, size_t len, uint64_t ring_addr, uint32_t count, uint16_t group) {
    struct utsname host;
    if (uname(&host) != 0) {
        (void)snprintf(host.release, sizeof(host.release), "unknown");
    }
    long page = sysconf(_SC_PAGESIZE);
    (void)snprintf(out, len,
                   "asked entries=%u (a power of two: %s) ring_addr page offset=%lu of %ld bgid=%u flags=0 resv=0, "
                   "Linux %s",
                   (unsigned)count, (count & (count - 1U)) == 0 ? "yes" : "no",
                   page > 0 ? (unsigned long)(ring_addr % (uint64_t)page) : 0UL, page, (unsigned)group, host.release);
}

// Hands the kernel drain_count buffers the first way it accepts (tt_RX_DRAIN_RING_*): a ring in this process's
// memory; else the same ring in the kernel's, mapped here; else classic provided buffers, which tt_rx_drain_start()
// hands over again at each start, and whose refusal drain_look() reports. What was refused is said once, with why.
static void drain_register_buffers(struct tt_hal* hal, int ring_fd, uint32_t count) {
    static bool told = false;
    void* user =
        mmap(NULL, hal->drain_buf_ring_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    int user_err =
        user == MAP_FAILED ? ENOMEM : drain_register_ring(ring_fd, (uint64_t)(uintptr_t)user, count, DRAIN_GROUP, 0);
    if (user_err == 0) {
        hal->drain_buf_ring = user;
        hal->drain_ring = tt_RX_DRAIN_RING_USER;
        hal->drain_group = DRAIN_GROUP;
        return;
    }
    char asked[DRAIN_ASKED_BYTES];
    drain_describe(asked, sizeof(asked), user == MAP_FAILED ? 0 : (uint64_t)(uintptr_t)user, count, DRAIN_GROUP);
    if (user != MAP_FAILED) {
        (void)munmap(user, hal->drain_buf_ring_len);
    }
    int kernel_err = drain_register_ring(ring_fd, 0, count, DRAIN_GROUP, IOU_PBUF_RING_MMAP);
    if (kernel_err == 0) {
        void* mapped = mmap(NULL, hal->drain_buf_ring_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring_fd,
                            (off_t)(IORING_OFF_PBUF_RING | ((uint64_t)DRAIN_GROUP << IORING_OFF_PBUF_SHIFT)));
        if (mapped != MAP_FAILED) {
            if (!told) {
                told = true;
                TT_LOG_WARNING("Receive drain: IORING_REGISTER_PBUF_RING refused a ring in this process's memory "
                               "(%s; %s) - the kernel's own ring (IOU_PBUF_RING_MMAP) is used instead",
                               strerror(user_err), asked);
            }
            hal->drain_buf_ring = mapped;
            hal->drain_ring = tt_RX_DRAIN_RING_KERNEL;
            hal->drain_group = DRAIN_GROUP;
            return;
        }
        kernel_err = errno; // registered and not mappable: the classic buffers take a group of their own
    }
    if (!told) {
        told = true;
        TT_LOG_WARNING("Receive drain: IORING_REGISTER_PBUF_RING refused a ring in this process's memory (%s; %s) and "
                       "the kernel's own (IOU_PBUF_RING_MMAP: %s) - classic provided buffers are used instead, handed "
                       "over again at each drain start",
                       strerror(user_err), asked, strerror(kernel_err));
    }
    hal->drain_buf_ring = NULL;
    hal->drain_ring = tt_RX_DRAIN_RING_CLASSIC;
    hal->drain_group = DRAIN_GROUP_CLASSIC;
}

// The ring and drain_count buffers of drain_size bytes, registered as one buffer group. False, having said why once
// and given everything back, when the kernel refuses any of it (no io_uring). How the buffers reach the kernel is
// drain_register_buffers()'s.
static bool drain_setup(struct tt_hal* hal, uint32_t size, uint32_t count) {
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_CQSIZE; // a completion per buffer and room for the ends and cancellations
    params.cq_entries = 2U * count > 2U * DRAIN_ENTRIES ? 2U * count : 2U * DRAIN_ENTRIES; // never below the SQ's
    // NOLINTNEXTLINE(misc-include-cleaner) - see the <sys/syscall.h> include
    int ring_fd = (int)syscall(__NR_io_uring_setup, DRAIN_ENTRIES, &params);
    if (ring_fd < 0) {
        drain_refused(hal, "io_uring_setup", errno);
        return false;
    }
    hal->drain_fd = ring_fd;
    hal->drain_size = size;
    hal->drain_count = count;
    if ((params.features & IORING_FEAT_SINGLE_MMAP) == 0) {
        drain_refused(hal, "IORING_FEAT_SINGLE_MMAP", EOPNOTSUPP); // Linux < 5.4, which has no buffer ring either
        return false;
    }
    size_t sq_len = params.sq_off.array + (params.sq_entries * sizeof(uint32_t));
    size_t cq_len = params.cq_off.cqes + (params.cq_entries * sizeof(struct io_uring_cqe));
    hal->drain_ring_map_len = sq_len > cq_len ? sq_len : cq_len;
    void* map = mmap(NULL, hal->drain_ring_map_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring_fd,
                     IORING_OFF_SQ_RING);
    if (map == MAP_FAILED) {
        drain_refused(hal, "mmap ring", errno);
        return false;
    }
    hal->drain_ring_map = map;
    hal->drain_sqes_len = params.sq_entries * sizeof(struct io_uring_sqe);
    void* sqes =
        mmap(NULL, hal->drain_sqes_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring_fd, IORING_OFF_SQES);
    if (sqes == MAP_FAILED) {
        drain_refused(hal, "mmap SQEs", errno);
        return false;
    }
    hal->drain_sqes = sqes;
    uint8_t* base = (uint8_t*)map;
    hal->drain_sq_tail = (uint32_t*)(base + params.sq_off.tail);
    hal->drain_sq_mask = (uint32_t*)(base + params.sq_off.ring_mask);
    hal->drain_sq_array = (uint32_t*)(base + params.sq_off.array);
    hal->drain_cq_head = (uint32_t*)(base + params.cq_off.head);
    hal->drain_cq_tail = (uint32_t*)(base + params.cq_off.tail);
    hal->drain_cq_mask = (uint32_t*)(base + params.cq_off.ring_mask);
    hal->drain_cqes = base + params.cq_off.cqes;
    hal->drain_scan = *hal->drain_cq_head;

    hal->drain_buf_ring_len = (size_t)count * sizeof(struct io_uring_buf);
    void* buffers = mmap(NULL, (size_t)count * size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    hal->drain_buffers = buffers == MAP_FAILED ? NULL : (uint8_t*)buffers;
    if (hal->drain_buffers == NULL) {
        drain_refused(hal, "mmap buffers", ENOMEM);
        return false;
    }
    drain_register_buffers(hal, ring_fd, count);
    if (hal->drain_ring != tt_RX_DRAIN_RING_CLASSIC) {
        hal->drain_buf_tail = 0;
        for (uint32_t bid = 0; bid < count; bid++) {
            drain_give_back(hal, (uint16_t)bid);
        }
    }
    memset(&hal->drain_msghdr, 0, sizeof(hal->drain_msghdr));
    hal->drain_msghdr.msg_namelen = sizeof(struct sockaddr_in);
    hal->drain_msghdr.msg_controllen = DRAIN_CONTROL;
    return true;
}

// Queues one submission; the caller submits them all with one io_uring_enter().
static struct io_uring_sqe* drain_sqe(struct tt_hal* hal, uint32_t* added) {
    uint32_t tail = *hal->drain_sq_tail + *added;
    uint32_t index = tail & *hal->drain_sq_mask;
    struct io_uring_sqe* sqe = &((struct io_uring_sqe*)hal->drain_sqes)[index];
    memset(sqe, 0, sizeof(*sqe));
    hal->drain_sq_array[index] = index;
    (*added)++;
    return sqe;
}

static bool drain_submit(struct tt_hal* hal, uint32_t added) {
    __atomic_store_n(hal->drain_sq_tail, *hal->drain_sq_tail + added, __ATOMIC_RELEASE);
    // NOLINTNEXTLINE(misc-include-cleaner) - see the <sys/syscall.h> include
    return syscall(__NR_io_uring_enter, hal->drain_fd, added, 0U, 0U, NULL, 0) >= 0;
}

// The datagram a completion took, or NULL for one that took none (an end, a cancellation, an error).
static bool drain_holds_datagram(const struct io_uring_cqe* cqe) {
    return (cqe->user_data & ~(uint64_t)(DRAIN_SOCKETS - 1U)) == DRAIN_RECV && cqe->res >= 0 &&
           (cqe->flags & IORING_CQE_F_BUFFER) != 0;
}

// Looks at the completions that came since the last look: one with a datagram is pending, one without
// IORING_CQE_F_MORE ends its socket's recvmsg - for want of a buffer (an overrun: the socket's own buffer takes over),
// cancelled, or refused by a kernel without multishot recvmsg (Linux < 6.0), which turns the drain off for good.
static void drain_look(struct tt_hal* hal) {
    uint32_t tail = __atomic_load_n(hal->drain_cq_tail, __ATOMIC_ACQUIRE);
    const struct io_uring_cqe* cqes = (const struct io_uring_cqe*)hal->drain_cqes;
    for (; hal->drain_scan != tail; hal->drain_scan++) {
        const struct io_uring_cqe* cqe = &cqes[hal->drain_scan & *hal->drain_cq_mask];
        if (cqe->user_data == DRAIN_PROVIDE && cqe->res < 0) {
            static bool told = false;
            if (!told) {
                told = true;
                TT_LOG_WARNING("Receive drain unavailable (IORING_OP_PROVIDE_BUFFERS: %s, after the buffer ring was "
                               "refused) - while a callback runs, a burst larger than the socket's receive buffer is "
                               "dropped. See README.md, \"io_uring\".",
                               strerror(-cqe->res));
            }
            hal->drain_holdoff_until = UINT64_MAX; // its recvmsgs end at once for want of a buffer: never again
        }
        if ((cqe->user_data & ~(uint64_t)(DRAIN_SOCKETS - 1U)) != DRAIN_RECV) {
            continue;
        }
        if (drain_holds_datagram(cqe)) {
            hal->drain_pending++;
        }
        if ((cqe->flags & IORING_CQE_F_MORE) == 0) {
            hal->drain_armed &= (uint8_t)~(1U << (cqe->user_data & (DRAIN_SOCKETS - 1U)));
            if (cqe->res == -ENOBUFS) {
                hal->drain_overruns++;
                hal->drain_overran = true;
                if (hal->drain_running && hal->drain_ring != tt_RX_DRAIN_RING_CLASSIC) {
                    hal->drain_rearm |= (uint8_t)(1U << (cqe->user_data & (DRAIN_SOCKETS - 1U)));
                    hal->drain_returned = 0;
                }
            } else if (cqe->res == -EINVAL || cqe->res == -EOPNOTSUPP) {
                hal->drain_holdoff_until = UINT64_MAX; // this kernel cannot do it: never again
            }
        }
    }
    // Completions without a datagram at the head - ends, cancellations, provisions - are let go at once. Left there,
    // the ring never looks empty, so drain_stop()'s wait for the next completion returned at once, its tries ran out in
    // microseconds and the drain was turned off (the PC preflight under rmw_tickle, 2026-10-12, once re-arms made such
    // completions common). A datagram's completion stays until it is handed out, and those behind it with it.
    uint32_t head = *hal->drain_cq_head;
    while (head != hal->drain_scan && !drain_holds_datagram(&cqes[head & *hal->drain_cq_mask])) {
        head++;
    }
    __atomic_store_n(hal->drain_cq_head, head, __ATOMIC_RELEASE);
}

// Queues a multishot recvmsg on each socket in `sockets` (bit 0 the well-known one, bit 1 the data one) that is open;
// returns the bits queued.
static uint8_t drain_queue_recvmsg(struct tt_hal* hal, uint8_t sockets, uint32_t* added) {
    const int fds[DRAIN_SOCKETS] = {hal->sock, hal->data_sock};
    uint8_t armed = 0;
    for (uint32_t i = 0; i < DRAIN_SOCKETS; i++) {
        if (fds[i] < 0 || (sockets & (1U << i)) == 0) {
            continue;
        }
        struct io_uring_sqe* sqe = drain_sqe(hal, added);
        sqe->opcode = IORING_OP_RECVMSG;
        sqe->fd = fds[i];
        sqe->addr = (uint64_t)(uintptr_t)&hal->drain_msghdr;
        sqe->ioprio = IORING_RECV_MULTISHOT;
        sqe->flags = IOSQE_BUFFER_SELECT;
        sqe->buf_group = hal->drain_group;
        sqe->user_data = DRAIN_RECV | i;
        armed |= (uint8_t)(1U << i);
    }
    return armed;
}

// A running drain whose recvmsg an overrun ended (every buffer held, IORING_CQE_F_MORE off with ENOBUFS) takes again
// once an eighth of its buffers is back, so it goes on draining for as long as it runs rather than leaving the rest of
// that time to the socket's own buffer - a few 64 KB runs. Not at the first buffer back: a recvmsg picks its buffer
// before it looks for a datagram, so one submitted with a single buffer free takes one datagram and ends again, a
// system call a datagram. The datagram the overrun left is still first in the socket, so order is kept. Called after
// each buffer goes back, it first looks for an overrun not seen yet. Not with classic buffers, which come back only at
// the next start.
static void drain_rearm_now(struct tt_hal* hal) {
    if (!hal->drain_running) {
        return;
    }
    drain_look(hal);
    if (hal->drain_rearm == 0 ||
        hal->drain_returned < (hal->drain_count + DRAIN_REARM_SHARE - 1U) / DRAIN_REARM_SHARE) {
        return;
    }
    uint32_t added = 0;
    uint8_t armed = drain_queue_recvmsg(hal, hal->drain_rearm, &added);
    hal->drain_rearm = 0;
    if (armed != 0 && drain_submit(hal, added)) {
        hal->drain_armed |= armed;
        hal->drain_rearms++;
    }
}

// Ends a running drain: each recvmsg still in flight is cancelled and waited for, so nothing is taken from a socket
// after this returns. What it took stays in the completion ring for rx_take_drained().
static void drain_stop(struct tt_Context* node) {
    struct tt_hal* hal = &node->hal;
    if (!hal->drain_running) {
        return;
    }
    hal->drain_running = false;
    hal->drain_rearm = 0;
    drain_look(hal);
    uint32_t added = 0;
    for (uint32_t i = 0; i < DRAIN_SOCKETS; i++) {
        if ((hal->drain_armed & (1U << i)) != 0) {
            struct io_uring_sqe* sqe = drain_sqe(hal, &added);
            sqe->opcode = IORING_OP_ASYNC_CANCEL;
            sqe->addr = DRAIN_RECV | i;
            sqe->user_data = DRAIN_CANCEL;
        }
    }
    // The cancellations go in with the first wait - one system call where a submit and then a wait were two, and a
    // multishot recvmsg's cancellation usually completes as it is submitted. Then until both recvmsgs have ended, by
    // the clock: a wait returns at once while a datagram's completion is still held (drain_look() lets the others go),
    // so a count of waits would not be a time.
    __atomic_store_n(hal->drain_sq_tail, *hal->drain_sq_tail + added, __ATOMIC_RELEASE);
    uint32_t to_submit = added;
    const uint64_t deadline = tt_get_ns() + ((uint64_t)DRAIN_STOP_TRIES * (uint64_t)DRAIN_STOP_WAIT_NS);
    while ((hal->drain_armed != 0 || to_submit != 0) && tt_get_ns() < deadline) {
        struct __kernel_timespec wait = {0, DRAIN_STOP_WAIT_NS};
        struct io_uring_getevents_arg arg;
        memset(&arg, 0, sizeof(arg));
        arg.ts = (uint64_t)(uintptr_t)&wait;
        // NOLINTNEXTLINE(misc-include-cleaner) - see the <sys/syscall.h> include
        long entered = syscall(__NR_io_uring_enter, hal->drain_fd, to_submit, 1U,
                               IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &arg, sizeof(arg));
        if (entered > 0) {
            to_submit -= (uint32_t)entered < to_submit ? (uint32_t)entered : to_submit; // what it took of them
        } else if (entered < 0 && to_submit != 0 && errno != ETIME && errno != EINTR) {
            TT_LOG_ERROR("Cannot cancel the receive drain: %s - turned off", strerror(errno));
            drain_refused(hal, "cancel", errno);
            return;
        }
        drain_look(hal);
    }
    if (hal->drain_armed != 0) {
        // A recvmsg that will not end could still take a datagram after a read of its socket, out of order. Rather
        // than that, the drain goes: what it holds is lost with it.
        TT_LOG_ERROR("Receive drain did not stop within %d ms - turned off", (int)(DRAIN_STOP_TRIES * 10));
        drain_refused(hal, "stop", ETIMEDOUT);
    }
}

// Hands out the oldest datagram a drain took, as rx_take_pending() does a batch's, or -1 when none is waiting. One
// longer than a buffer arrived cut short: it is dropped, counted, and no drain starts for tt_RX_DRAIN_HOLDOFF_NS.
#if TT_HAL_UDP_GRO
// The next datagram of a merged read the drain took (drain_split_*), copied to `buf`; its buffer goes back with the
// last one.
static int32_t drain_take_split(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    struct tt_hal* hal = &node->hal;
    uint32_t offset = hal->drain_split_next;
    uint32_t size = hal->drain_split_end - offset < hal->drain_split_segment ? hal->drain_split_end - offset
                                                                             : hal->drain_split_segment;
    const uint8_t* payload = hal->drain_buffers + ((size_t)hal->drain_split_bid * hal->drain_size) + DRAIN_HEADER;
    size_t copy = (size_t)size < len ? (size_t)size : len;
    memcpy(buf, payload + offset, copy);
    hal->drain_split_next = offset + size;
    hal->drain_split_left--;
    if (hal->drain_split_left == 0) {
        drain_give_back(hal, hal->drain_split_bid);
        drain_rearm_now(hal);
    }
    *ip = hal->drain_split_ip;
    *port = hal->drain_split_port;
    node->rx_via_data_port = hal->drain_split_from_data;
    hal->drain_datagrams++;
    TT_TRACE(tt_TRACE_RX_DATAGRAM);
    return (int32_t)copy;
}

// UDP_GRO's segment size in a drained completion's control area, 0 when there is none (a datagram read alone).
static uint32_t drain_gro_segment(const uint8_t* buffer, const struct io_uring_recvmsg_out* out) {
    if (out->controllen == 0) {
        return 0;
    }
    struct msghdr control = {0};
    control.msg_control = (void*)(buffer + sizeof(*out) + sizeof(struct sockaddr_in));
    control.msg_controllen = out->controllen;
    int segment = 0;
    // NOLINTNEXTLINE(misc-include-cleaner) - CMSG_*: <sys/socket.h>
    for (struct cmsghdr* cm = CMSG_FIRSTHDR(&control); cm != NULL; cm = CMSG_NXTHDR(&control, cm)) {
        if (cm->cmsg_level == SOL_UDP && cm->cmsg_type == UDP_GRO) {
            memcpy(&segment, CMSG_DATA(cm), sizeof(segment));
        }
    }
    return segment > 0 ? (uint32_t)segment : 0U;
}
#endif

#if TT_HAL_UDP_GRO
// A completion that holds a merged read (UDP_GRO's size in its control area, and more than one datagram of it): its
// datagrams are handed out one at a time from this buffer, before the next completion. False for one datagram.
static bool drain_split_begin(struct tt_Context* node, uint16_t bid, const uint8_t* buffer,
                              const struct io_uring_recvmsg_out* out, uint32_t ip, uint16_t port) {
    struct tt_hal* hal = &node->hal;
    uint32_t segment = drain_gro_segment(buffer, out);
    if (segment == 0 || out->payloadlen <= segment) {
        return false;
    }
    hal->drain_merged++;
    hal->drain_split_bid = bid;
    hal->drain_split_segment = segment;
    hal->drain_split_next = 0;
    hal->drain_split_end = out->payloadlen;
    hal->drain_split_left = (uint16_t)((out->payloadlen + segment - 1U) / segment);
    hal->drain_split_ip = ip;
    hal->drain_split_port = port;
    hal->drain_split_from_data = node->rx_via_data_port;
    return true;
}
#endif

// keep: the caller is a drain pass (tt_try_receive()), not a wait. A drain whose recvmsgs are still in flight on every
// open socket takes whatever arrives there, so such a caller has nothing to read meanwhile and the drain is left
// running - through the rest of the poll and past its return, where tt_Context_poll() would start it again
// (large_rx_drain()). Only a wait (tt_receive()) stops it. A drain an overrun cut short on a socket stops as before,
// so that socket is read.
static uint8_t drain_sockets_open(const struct tt_hal* hal) {
    return (uint8_t)((hal->sock >= 0 ? 1U : 0U) | (hal->data_sock >= 0 ? 2U : 0U));
}

// rx_take_drained() with nothing held: whether there is more to hand out. A running drain may have taken more since it
// was last looked at; when it has not, it stops here - unless kept (above) - and whatever it took while stopping is
// handed out before the caller reads a socket.
static bool drain_took_more(struct tt_Context* node, bool keep) {
    struct tt_hal* hal = &node->hal;
    if (!hal->drain_running) {
        return false;
    }
    drain_look(hal);
    if (hal->drain_pending != 0) {
        return true;
    }
    if (keep && hal->drain_armed == drain_sockets_open(hal)) {
        return false;
    }
    drain_stop(node);
    return hal->drain_pending != 0;
}

static int32_t rx_take_drained(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port,
                               bool keep) {
    struct tt_hal* hal = &node->hal;
    const struct io_uring_cqe* cqes = (const struct io_uring_cqe*)hal->drain_cqes;
#if TT_HAL_UDP_GRO
    if (hal->drain_split_left != 0) {
        return drain_take_split(node, buf, len, ip, port); // a merged read's next; nothing else starts one meanwhile
    }
#endif
    for (;;) {
        while (hal->drain_pending != 0) {
            uint32_t head = *hal->drain_cq_head;
            const struct io_uring_cqe* cqe = &cqes[head & *hal->drain_cq_mask];
            bool holds = drain_holds_datagram(cqe);
            uint64_t user_data = cqe->user_data;
            uint32_t flags = cqe->flags;
            uint32_t written = (uint32_t)cqe->res;
            __atomic_store_n(hal->drain_cq_head, head + 1U, __ATOMIC_RELEASE);
            if (!holds) {
                continue;
            }
            hal->drain_pending--;
            uint16_t bid = (uint16_t)(flags >> IORING_CQE_BUFFER_SHIFT);
            const uint8_t* buffer = hal->drain_buffers + ((size_t)bid * hal->drain_size);
            const struct io_uring_recvmsg_out* out = (const struct io_uring_recvmsg_out*)buffer;
            const uint8_t* payload = buffer + DRAIN_HEADER;
            // NOLINTNEXTLINE(misc-include-cleaner) - MSG_TRUNC: <sys/socket.h>
            if ((out->flags & MSG_TRUNC) != 0 || written < DRAIN_HEADER || out->payloadlen > written - DRAIN_HEADER) {
                hal->drain_truncated++;
                hal->drain_holdoff_until = tt_get_ns() + tt_RX_DRAIN_HOLDOFF_NS;
                drain_give_back(hal, bid);
                drain_stop(node); // and the retry this datagram's sender will make is read from the socket, whole
                continue;
            }
            struct sockaddr_in from;
            memcpy(&from, buffer + sizeof(*out), sizeof(from));
            *ip = ntohl(from.sin_addr.s_addr);
            *port = ntohs(from.sin_port);
            node->rx_via_data_port = (user_data & 1U) != 0;
#if TT_HAL_UDP_GRO
            if (drain_split_begin(node, bid, buffer, out, *ip, *port)) {
                return drain_take_split(node, buf, len, ip, port);
            }
#endif
            size_t copy = (size_t)out->payloadlen < len ? (size_t)out->payloadlen : len;
            memcpy(buf, payload, copy);
            drain_give_back(hal, bid);
            drain_rearm_now(hal);
            hal->drain_datagrams++;
            TT_TRACE(tt_TRACE_RX_DATAGRAM);
            return (int32_t)copy;
        }
        if (!drain_took_more(node, keep)) {
            return -1;
        }
    }
}

static uint32_t drain_held(const struct tt_Context* node);

// A buffer's length for datagrams of up to datagram_bytes: unless the sockets read plain, a read may be a merged one,
// up to the receive buffer core reads merged reads into (TT_GRO_SETTLING too: what was queued before the option went
// off may be merged). Plain, nothing queued is.
static uint32_t drain_size_for(const struct tt_hal* hal, uint32_t datagram_bytes) {
#if TT_HAL_UDP_GRO
    if (hal->gro_state != TT_GRO_PLAIN && datagram_bytes < (uint32_t)tt_MAX_BUFFER_LENGTH) {
        datagram_bytes = (uint32_t)tt_MAX_BUFFER_LENGTH;
    }
#else
    (void)hal;
#endif
    return ((datagram_bytes + DRAIN_HEADER + DRAIN_ALIGN - 1U) / DRAIN_ALIGN) * DRAIN_ALIGN;
}

// A drain asked to start while one runs: true when the running one serves. Left running by a drain pass
// (rx_take_drained()'s keep) with buffers shorter than now asked for, or after an overrun that earns it more, it is
// stopped instead, to be set up again once what it took is handed out.
static bool drain_running_serves(struct tt_Context* node, uint32_t datagram_bytes) {
    struct tt_hal* hal = &node->hal;
    const bool grows = hal->drain_overran && drain_held(node) == 0 && hal->drain_count < tt_RX_DRAIN_BUFFERS &&
                       hal->drain_budget < tt_RX_DRAIN_BYTES_MAX;
    if (datagram_bytes == 0 || (drain_size_for(hal, datagram_bytes) <= hal->drain_size && !grows)) {
        return true;
    }
    drain_stop(node);
    return false;
}

bool tt_rx_drain_start(struct tt_Context* node, uint32_t datagram_bytes) {
    struct tt_hal* hal = &node->hal;
    if (hal->drain_running && drain_running_serves(node, datagram_bytes)) {
        return true;
    }
    if (hal->drain_fd == -2 || datagram_bytes == 0 ||
        (hal->drain_holdoff_until != 0 && tt_get_ns() < hal->drain_holdoff_until)) {
        return false;
    }
    uint32_t size = drain_size_for(hal, datagram_bytes);
    if (hal->drain_fd >= 0 && size > hal->drain_size) {
        if (drain_held(node) != 0) {
            return false; // longer datagrams than the buffers hold: rebuilt once what they took is handed out
        }
        drain_release(hal);
    }
    if (hal->drain_fd >= 0 && hal->drain_overran && drain_held(node) == 0 && hal->drain_count < tt_RX_DRAIN_BUFFERS &&
        hal->drain_budget < tt_RX_DRAIN_BYTES_MAX) {
        // The last drain overran: twice the buffers (tt_RX_DRAIN_BYTES_MAX, hal_linux.h).
        hal->drain_budget =
            hal->drain_budget <= tt_RX_DRAIN_BYTES_MAX / 2U ? hal->drain_budget * 2U : tt_RX_DRAIN_BYTES_MAX;
        hal->drain_grows++;
        TT_LOG_INFO("Receive drain: its %u buffers of %u bytes overran - %u KB of them from now on",
                    (unsigned)hal->drain_count, (unsigned)hal->drain_size, (unsigned)(hal->drain_budget / 1024U));
        drain_release(hal);
    }
    hal->drain_overran = false;
    if (hal->drain_fd < 0) {
        uint32_t count = tt_RX_DRAIN_BUFFERS;
        while (count > 1U && (uint64_t)count * size > hal->drain_budget) {
            count /= 2U;
        }
        if (!drain_setup(hal, size, count)) {
            return false;
        }
    }
    uint32_t added = 0;
    if (hal->drain_ring == tt_RX_DRAIN_RING_CLASSIC) {
        // Classic buffers an earlier drain took are not given back one by one (drain_give_back()): all of them are
        // handed over again here, which is right only while none is held. Issued in order and inline, before the
        // recvmsgs: what is left of the last provision is removed (ENOENT the first time), then every buffer provided.
        if (drain_held(node) != 0) {
            return false;
        }
        struct io_uring_sqe* sqe = drain_sqe(hal, &added);
        sqe->opcode = IORING_OP_REMOVE_BUFFERS;
        sqe->fd = (int32_t)hal->drain_count; // how many
        sqe->buf_group = hal->drain_group;
        sqe->user_data = DRAIN_REMOVE;
        sqe = drain_sqe(hal, &added);
        sqe->opcode = IORING_OP_PROVIDE_BUFFERS;
        sqe->fd = (int32_t)hal->drain_count; // how many, of len bytes each from addr, the first numbered off
        sqe->addr = (uint64_t)(uintptr_t)hal->drain_buffers;
        sqe->len = hal->drain_size;
        sqe->off = 0;
        sqe->buf_group = hal->drain_group;
        sqe->user_data = DRAIN_PROVIDE;
    }
    uint8_t armed = drain_queue_recvmsg(hal, (uint8_t)((1U << DRAIN_SOCKETS) - 1U), &added);
    if (armed == 0 || !drain_submit(hal, added)) {
        return false;
    }
    hal->drain_armed = armed;
    hal->drain_running = true;
    hal->drain_starts++;
    return true;
}

static uint32_t drain_held(const struct tt_Context* node) {
#if TT_HAL_UDP_GRO
    return node->hal.drain_pending + node->hal.drain_split_left;
#else
    return node->hal.drain_pending;
#endif
}

#if TT_HAL_UDP_GRO
// A drain running, or holding what it took: receive offload's state stays as it is ("The drain and receive offload").
static bool drain_active(const struct tt_Context* node) {
    return node->hal.drain_running || drain_held(node) != 0;
}
#endif
#else
bool tt_rx_drain_start(struct tt_Context* node, uint32_t datagram_bytes) {
    (void)node;
    (void)datagram_bytes;
    return false;
}

// NOLINTNEXTLINE(readability-non-const-parameter) - the drain's own version writes them
static int32_t rx_take_drained(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port,
                               bool keep) {
    (void)node;
    (void)buf;
    (void)len;
    (void)ip;
    (void)port;
    (void)keep;
    return -1;
}

static uint32_t drain_held(const struct tt_Context* node) {
    (void)node;
    return 0;
}

#if TT_HAL_UDP_GRO
static bool drain_active(const struct tt_Context* node) {
    (void)node;
    return false;
}
#endif
#endif

// Hands out the next datagram the last recvmmsg() read and held back, or -1 when none is waiting.
static int32_t rx_take_pending(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    struct tt_hal* hal = &node->hal;
    if (hal->rx_next >= hal->rx_count) {
        return -1;
    }
    uint16_t slot = hal->rx_next++;
    size_t copy = (size_t)hal->rx_len[slot] < len ? (size_t)hal->rx_len[slot] : len;
    memcpy(buf, hal->rx_batch[slot], copy);
    *ip = hal->rx_ip[slot];
    *port = hal->rx_port[slot];
    node->rx_via_data_port = hal->rx_from_data;
    TT_TRACE(tt_TRACE_RX_DATAGRAM);
    return (int32_t)copy;
}

// One recvmmsg() on fd, without waiting: the first datagram into buf, up to tt_RX_BATCH - 1 more held in
// struct tt_hal.rx_batch for rx_take_pending(). Returns the first datagram's length, -1 when nothing is
// waiting, -2 on an I/O error.
_Static_assert(sizeof(struct tt_mmsghdr) == sizeof(struct mmsghdr), "struct tt_mmsghdr must match struct mmsghdr");
_Static_assert(offsetof(struct tt_mmsghdr, msg_len) == offsetof(struct mmsghdr, msg_len),
               "struct tt_mmsghdr must match struct mmsghdr");

#if tt_RX_BATCH > 1
// Points recvmmsg()'s headers at this node's own buffers. Once per node, or again if it has moved.
static void rx_headers_setup(struct tt_hal* hal) {
    memset(hal->rx_msgs, 0, sizeof(hal->rx_msgs));
    for (int i = 0; i < tt_RX_BATCH; i++) {
        if (i > 0) {
            hal->rx_iov[i].iov_base = hal->rx_batch[i - 1];
            hal->rx_iov[i].iov_len = sizeof(hal->rx_batch[0]);
        }
        hal->rx_msgs[i].msg_hdr.msg_name = &hal->rx_addr[i];
        hal->rx_msgs[i].msg_hdr.msg_namelen = sizeof(hal->rx_addr[i]);
        hal->rx_msgs[i].msg_hdr.msg_iov = &hal->rx_iov[i];
        hal->rx_msgs[i].msg_hdr.msg_iovlen = 1;
    }
    hal->rx_headers_for = hal;
}
#endif

#if TT_HAL_RX_DIRECT
// ---- Reading a run in place (DESIGN.md section 8, "Receiving runs in place")
// -------------------------------------------
//
// Core's hint (struct tt_LargeState.direct_*, tickle.c large_direct_aim()) names the run it expects next: its framing
// to the byte and where its payload belongs in the sample's assembly. The read is then one recvmsg() of three pieces -
// the framing's bytes into `buf`, the payload's into the assembly, anything longer after them in `buf` - so the run's
// payload is copied once, by the kernel, to where it stays. A datagram that is that run (or a shorter one of the same
// sample from the same place: direct_count_at) is handed to core as it lies, with direct_split set; anything else is
// put back together - the bytes that went to the assembly copied after the framing in `buf` - and is an ordinary
// datagram. The stretch it overwrote holds units not landed yet, which is all a hint ever names.
//
// A thread that frees the hinted assembly meanwhile (core, under the state lock) clears direct_bytes and, seeing
// direct_busy, leaves the buffer to the polling thread (direct_parked), so the read never writes into a buffer that
// went back; a run read in place for an assembly freed after the read is dropped by core (direct_cancelled).
static bool direct_is_hinted_run(const struct tt_LargeState* large, const uint8_t* buf, size_t got, uint32_t bytes) {
    const uint32_t framing = large->direct_framing_len;
    const uint32_t count_at = large->direct_count_at;
    const uint32_t after = count_at + (uint32_t)sizeof(uint16_t);
    uint16_t count = 0;
    memcpy(&count, buf + count_at, sizeof(count)); // native order: core hints only runs from a native-endian writer
    const uint32_t payload = (uint32_t)count * large->direct_unit;
    return count != 0 && payload <= bytes && got == (size_t)framing + payload &&
           memcmp(buf, large->direct_framing, count_at) == 0 &&
           memcmp(buf + after, large->direct_framing + after, framing - after) == 0;
}

// The aimed read itself, for a `msg` whose one piece is the caller's buffer (and, with receive offload, whose control
// buffer takes UDP_GRO's segment size): the hinted run handed over in place (direct_split), anything else put back
// together in that buffer. A merged read (UDP_GRO) is never the run - a run is one IP-reassembled datagram, which GRO
// never merges - and is put back together too, for gro_read() to hand out. Returns recvmsg()'s result; msg's piece,
// address, flags and control are as a plain recvmsg() would have left them.
static ssize_t direct_recvmsg(struct tt_Context* node, int socket_fd, struct msghdr* msg) {
    struct tt_LargeState* large = &node->large;
    __atomic_store_n(&large->direct_busy, 1U, __ATOMIC_SEQ_CST);
    const uint32_t bytes = __atomic_load_n(&large->direct_bytes, __ATOMIC_SEQ_CST);
    // NOLINTNEXTLINE(misc-include-cleaner) - see <sys/uio.h>'s own include comment
    struct iovec* whole = msg->msg_iov;
    uint8_t* buf = (uint8_t*)whole->iov_base;
    const size_t len = whole->iov_len;
    const uint32_t framing = large->direct_framing_len;
    if (bytes == 0 || len < (size_t)framing + bytes) {
        ssize_t plain = recvmsg(socket_fd, msg, MSG_DONTWAIT);
        __atomic_store_n(&large->direct_busy, 0U, __ATOMIC_SEQ_CST);
        return plain;
    }
    uint8_t* dest = large->direct_dest;
    // NOLINTNEXTLINE(misc-include-cleaner) - see <sys/uio.h>'s own include comment
    struct iovec iov[3] = {{.iov_base = buf, .iov_len = framing},
                           {.iov_base = dest, .iov_len = bytes},
                           {.iov_base = buf + framing + bytes, .iov_len = len - framing - bytes}};
    msg->msg_iov = iov;
    msg->msg_iovlen = 3;
    ssize_t got = recvmsg(socket_fd, msg, MSG_DONTWAIT);
    msg->msg_iov = whole;
    msg->msg_iovlen = 1;
    if (got < 0) {
        __atomic_store_n(&large->direct_busy, 0U, __ATOMIC_SEQ_CST);
        return got;
    }
    large->direct_reads++;
#if TT_HAL_UDP_GRO
    const int segment = msg->msg_control != NULL ? gro_segment_of(msg) : 0;
    const bool merged = segment > 0 && got > segment;
#else
    const bool merged = false;
#endif
    // Still hinted after the read: a hint withdrawn during it leaves the payload in a parked buffer that goes back once
    // this read is over, so it is copied out like any other.
    if (!merged && direct_is_hinted_run(large, buf, (size_t)got, bytes) &&
        __atomic_load_n(&large->direct_bytes, __ATOMIC_SEQ_CST) == bytes) {
        large->direct_split = dest;
        __atomic_store_n(&large->direct_busy, 0U, __ATOMIC_SEQ_CST);
        return got;
    }
    large->direct_missed++;
    if ((size_t)got > framing) {
        size_t back = (size_t)got - framing < bytes ? (size_t)got - framing : bytes;
        memcpy(buf + framing, dest, back);
    }
    __atomic_store_n(&large->direct_busy, 0U, __ATOMIC_SEQ_CST);
    return got;
}

// rx_read_one() with a hint and receive offload off: one datagram, its run payload in place when it is the hinted run.
static int32_t direct_read(struct tt_Context* node, int socket_fd, void* buf, size_t len, uint32_t* ip,
                           uint16_t* port) {
    struct sockaddr_in addr;
    // NOLINTNEXTLINE(misc-include-cleaner) - see <sys/uio.h>'s own include comment
    struct iovec iov = {.iov_base = buf, .iov_len = len};
    struct msghdr msg = {0};
    msg.msg_name = &addr;
    msg.msg_namelen = sizeof(addr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    ssize_t got = direct_recvmsg(node, socket_fd, &msg);
    if (got < 0) {
        // NOLINTNEXTLINE(misc-include-cleaner) - EAGAIN/EWOULDBLOCK: glibc-private headers
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
    }
    *ip = ntohl(addr.sin_addr.s_addr);
    *port = ntohs(addr.sin_port);
    return (int32_t)got;
}
#endif

// After a read that took a hybrid run (FRAG_RUN_L, one IP-reassembled datagram of a large sample, read in place or
// not, with receive offload on or off): the drain after it does not ask that socket again. A run is up to ~64 KB, so
// the next one is at least its wire time away (0.5 ms at 1 GbE), far more than core takes over this one, and the read
// that would find the socket empty - a system call a wake-up - is skipped; anything that did come is reported by the
// next wait, which is level-triggered (tt_try_receive()'s rx_idle). Until 2026-10-13 only a run read in place was
// marked, and on the rig's 1 MB cell, where receive offload stayed on, no run was: every wake-up kept its empty read.
static void rx_note_run(struct tt_Context* node, int socket_fd, const uint8_t* buf, int32_t got) {
    if (got > (int32_t)sizeof(struct tt_SingleHeader) && buf[3] == tt_SUBMESSAGE_TYPE_FRAG_RUN_L &&
        (buf[0] == tt_SINGLE_MARKER_LE || buf[0] == tt_SINGLE_MARKER_BE)) {
        node->hal.rx_idle |= socket_fd == node->hal.data_sock ? TT_RX_IDLE_DATA : TT_RX_IDLE_WELL_KNOWN;
    }
}

// One datagram, without waiting: its length, -1 when nothing is waiting (counted, rx_empty_reads), -2 on an I/O error.
static int32_t rx_read_one(struct tt_Context* node, int socket_fd, void* buf, size_t len, uint32_t* ip,
                           uint16_t* port) {
    node->rx_via_data_port = (socket_fd == node->hal.data_sock);
    int32_t ret = -1;
#if TT_HAL_UDP_GRO
    if (node->hal.gro_state != TT_GRO_PLAIN) {
        if (node->hal.gro_state == TT_GRO_SETTLING) {
            gro_turn_on(node); // step 2: never read what may be merged without the option on
        }
        ret = gro_read(node, socket_fd, buf, len, ip, port);
        if (ret >= 0) {
            bool busy = node->hal.gro_left > 0 || gro_large_fragment((const uint8_t*)buf, ret);
            node->hal.gro_quiet = busy ? 0 : node->hal.gro_quiet + 1;
            if (node->hal.gro_left == 0) {
                rx_note_run(node, socket_fd, (const uint8_t*)buf, ret);
            }
        } else if (ret == -1) {
            node->hal.rx_empty_reads++;
        }
        return ret;
    }
#endif
#if TT_HAL_RX_DIRECT
    // Only the polling thread makes a hint (direct_bytes != 0), so a 0 seen here is current: no fence for the reads
    // that have none.
    if (__atomic_load_n(&node->large.direct_bytes, __ATOMIC_RELAXED) != 0) {
        ret = direct_read(node, socket_fd, buf, len, ip, port);
        if (ret >= 0) {
            rx_note_run(node, socket_fd, (const uint8_t*)buf, ret);
        } else if (ret == -1) {
            node->hal.rx_empty_reads++;
        }
        return ret;
    }
#endif
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    ret = (int32_t)hal_recvfrom(socket_fd, buf, len, MSG_DONTWAIT, (struct sockaddr*)&addr, &addr_len);
    if (ret < 0) {
        // NOLINTNEXTLINE(misc-include-cleaner) - EAGAIN/EWOULDBLOCK: glibc-private headers
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            node->hal.rx_empty_reads++;
            return -1;
        }
        return -2;
    }
    *ip = ntohl(addr.sin_addr.s_addr);
    *port = ntohs(addr.sin_port);
#if TT_HAL_UDP_GRO
    if (node->hal.gro_allowed && gro_large_fragment((const uint8_t*)buf, ret)) {
        gro_turn_on(node); // the rest of this sample may come merged
    }
#endif
    rx_note_run(node, socket_fd, (const uint8_t*)buf, ret);
    return ret;
}

static int32_t rx_fill(struct tt_Context* node, int socket_fd, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    struct tt_hal* hal = &node->hal;
#if tt_RX_BATCH == 1
    // No batching: recvfrom(), which the control arm measured slightly cheaper than a one-slot recvmmsg().
    return rx_read_one(node, socket_fd, buf, len, ip, port);
#else
    node->rx_via_data_port = (socket_fd == hal->data_sock);
    if (hal->rx_headers_for != hal) {
        rx_headers_setup(hal);
    }
    hal->rx_iov[0].iov_base = buf; // the caller's buffer takes the first datagram, saving it a copy
    hal->rx_iov[0].iov_len = len;
    struct sockaddr_in* addrs = hal->rx_addr;
    int got = hal_recvmmsg(socket_fd, (struct mmsghdr*)hal->rx_msgs, tt_RX_BATCH, MSG_DONTWAIT);
    if (got < 0) {
        // NOLINTNEXTLINE(misc-include-cleaner) - EAGAIN/EWOULDBLOCK: glibc-private headers, see below
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
    }
    if (got == 0) {
        return -1;
    }
    hal->rx_batch_calls++;
    hal->rx_batch_datagrams += (uint64_t)got;
    if (got == tt_RX_BATCH) {
        hal->rx_batch_full++;
    } else {
        // A batch that came back short emptied the socket: the drain need not spend a syscall on it
        // to find that out (tt_try_receive()'s rx_idle). Anything arriving since is still reported by
        // the next ppoll(), which is level-triggered.
        hal->rx_idle |= (socket_fd == hal->data_sock) ? TT_RX_IDLE_DATA : TT_RX_IDLE_WELL_KNOWN;
    }
    for (int i = 1; i < got; i++) {
        hal->rx_len[i - 1] = (int32_t)hal->rx_msgs[i].msg_len;
        hal->rx_ip[i - 1] = ntohl(addrs[i].sin_addr.s_addr);
        hal->rx_port[i - 1] = ntohs(addrs[i].sin_port);
        hal->rx_msgs[i].msg_hdr.msg_namelen = sizeof(addrs[i]); // the kernel wrote it; ready it for next time
    }
    hal->rx_msgs[0].msg_hdr.msg_namelen = sizeof(addrs[0]);
    hal->rx_count = (uint16_t)(got - 1);
    hal->rx_next = 0;
    hal->rx_from_data = (socket_fd == hal->data_sock);
    *ip = ntohl(addrs[0].sin_addr.s_addr);
    *port = ntohs(addrs[0].sin_port);
    return (int32_t)hal->rx_msgs[0].msg_len;
#endif
}

#if tt_SEGMENT_ENABLED
static void bell_rung(struct tt_Context* node);
#endif

// One wait on tt_receive()'s set: ppoll()'s return convention (ready count, 0 on timeout, < 0 with errno), and in
// *ready the RX_READY_* bits of what is readable. From the kept set when there is one (wait_set_setup()), otherwise
// from a ppoll() set built for this call, as every wait was until 2026-10-06.
static inline __attribute__((always_inline)) int rx_wait(struct tt_Context* node, const struct timespec* timeout_ts,
                                                         uint32_t* ready) {
    *ready = 0;
#if TT_HAL_EPOLL
    if (node->hal.epoll_fd_plus1 > 0) {
        struct epoll_event events[RX_WAIT_FDS];
#if TT_HAL_RAW_CALLS
        int count = (int)tt_sys_result(
            tt_sys6(SYS_epoll_pwait2, node->hal.epoll_fd_plus1 - 1, (long)events, RX_WAIT_FDS, (long)timeout_ts, 0, 0));
#else
        int count = epoll_pwait2(node->hal.epoll_fd_plus1 - 1, events, RX_WAIT_FDS, timeout_ts, NULL);
#endif
        for (int i = 0; i < count; i++) {
            // A descriptor reported only for an error or a hangup counts towards the return value, as it did with
            // ppoll(), and is read like one: the read says what the error was.
            if ((events[i].events & EPOLLIN) != 0 ||
                (events[i].data.u32 == RX_READY_WRITABLE && (events[i].events & (EPOLLOUT | EPOLLERR)) != 0)) {
                *ready |= events[i].data.u32;
            }
        }
        return count;
    }
#endif
    // struct pollfd/POLLIN/ppoll() live in a glibc-private header; <poll.h> (included above)
    // is the correct public header.
    // NOLINTNEXTLINE(misc-include-cleaner)
    struct pollfd pfd[RX_WAIT_FDS] = {
        {.fd = node->hal.sock, .events = POLLIN, .revents = 0},    // NOLINT(misc-include-cleaner)
        {.fd = node->hal.wake_fd, .events = POLLIN, .revents = 0}, // NOLINT(misc-include-cleaner)
        {.fd = node->hal.data_sock,
         // NOLINTNEXTLINE(misc-include-cleaner) - POLLOUT: the same private header as POLLIN
         .events = (short)(POLLIN | (__atomic_load_n(&node->hal.tx_watch_armed, __ATOMIC_ACQUIRE) ? POLLOUT : 0)),
         .revents = 0}, // NOLINT(misc-include-cleaner)
#if tt_SEGMENT_ENABLED
        {.fd = node->hal.bell_fd_plus1 - 1, .events = POLLIN, .revents = 0}, // NOLINT(misc-include-cleaner) -1: ignored
#endif
    };
    // sigmask=NULL: no signal-mask swap needed, only ppoll()'s own real (not
    // millisecond-rounded) timeout resolution is what's wanted here.
    // NOLINTNEXTLINE(misc-include-cleaner)
    int count = ppoll(pfd, RX_WAIT_FDS, timeout_ts, NULL);
    static const uint32_t tags[RX_WAIT_FDS] = {
        RX_READY_WELL_KNOWN,
        RX_READY_WAKE,
        RX_READY_DATA,
#if tt_SEGMENT_ENABLED
        RX_READY_BELL,
#endif
    };
    for (int i = 0; count > 0 && i < RX_WAIT_FDS; i++) {
        if ((pfd[i].revents & POLLIN) != 0) { // NOLINT(misc-include-cleaner)
            *ready |= tags[i];
        }
    }
    if (count > 0 && (pfd[2].revents & POLLOUT) != 0) { // NOLINT(misc-include-cleaner) - data_sock: room again
        __atomic_store_n(&node->hal.tx_watch_armed, false, __ATOMIC_RELAXED);
        *ready |= RX_READY_WRITABLE;
    }
    return count;
}

#if TT_HAL_EPOLL
// ---- The wait's deadline (2026-10-13)
// --------------------------------------------------------------------------------
//
// epoll_pwait2() with a timeout starts and cancels an hrtimer on every wait - on the Pi the timed wait's share of the
// receive gap to Fast DDS, which blocks in recvfrom() with no timeout. The deadline itself moves on nearly every wait
// (4,094 of 4,096 on the bench subscriber, whose polls each have their own budget), so re-arming a timer per wait would
// cost a system call each. Instead the deadline is a timerfd in the wait set, armed for an absolute time and moved only
// EARLIER: a wait whose deadline is later than the armed one leaves it (no call), and the wait itself has no timeout
// (no hrtimer). When the armed time comes first - a deadline that has since moved later - the timer is read, re-armed
// for this wait's deadline and the wait goes on; only a deadline that really came ends the wait. A wait that ends for
// data, the usual case on a stream, costs nothing for its deadline. The deadline is the poll loop's own absolute one
// (struct tt_Context.wait_until, which the polling thread alone writes); a tt_receive() outside a poll (none set) waits
// as before. On the monotonic clock, as tt_get_ns() and every deadline core sets, so a step of the date neither fires
// it nor delays it.

// The poll loop's deadline for this wait (poll_wait_io()): 0 when tt_receive() runs outside a poll, UINT64_MAX for
// none.
static uint64_t rx_wait_until(const struct tt_Context* node) {
    return ((uint64_t)__atomic_load_n(&node->wait_until_hi, __ATOMIC_RELAXED) << 32U) |
           __atomic_load_n(&node->wait_until_lo, __ATOMIC_RELAXED);
}

static void rx_timer_arm(struct tt_hal* hal, uint64_t until) {
    // NOLINTNEXTLINE(misc-include-cleaner) - struct itimerspec: <sys/timerfd.h> (via <time.h>), a glibc-private header
    struct itimerspec deadline = {{0, 0}, {(time_t)(until / SEC_NS), (long)(until % SEC_NS)}};
    if (timerfd_settime(hal->timer_fd_plus1 - 1, TFD_TIMER_ABSTIME, &deadline, NULL) == 0) {
        hal->timer_armed_ns = until;
        hal->timer_sets++;
    }
}

// The timer fired: disarmed, and - registered level-triggered - read, so the set stops reporting it. Edge-triggered
// (TT_RX_TIMER_EDGE, the default since 2026-10-17) it needs no read: epoll reports each expiry once, and the next
// timerfd_settime() clears the count, so a fire costs no system call. A fire also starts the run of waits that ended
// for data again (rx_wait_epoll()).
static void rx_timer_take(struct tt_hal* hal) {
#if !TT_RX_TIMER_EDGE
    uint64_t expirations = 0;
    (void)read(hal->timer_fd_plus1 - 1, &expirations, sizeof(expirations));
#endif
    hal->timer_armed_ns = 0;
    hal->timer_fires++;
    hal->timer_data_streak = 0;
}

// rx_wait() until `until` (absolute, UINT64_MAX: none) with the deadline timer: the same return convention. `exact`
// (a long wait for a scheduler entry, TT_RX_TIMER_LONG_EXACT) also moves an armed deadline LATER, at the cost of a
// timerfd_settime(), rather than waking early at it and setting the timer then: a wake-up costs more than the call.
static int rx_wait_lazy(struct tt_Context* node, uint64_t until, bool exact, uint32_t* ready) {
    struct tt_hal* hal = &node->hal;
    hal->timer_waits++;
    for (;;) {
        if (until != UINT64_MAX &&
            (hal->timer_armed_ns == 0 || hal->timer_armed_ns > until || (exact && hal->timer_armed_ns != until))) {
            rx_timer_arm(hal, until);
        }
        int count = rx_wait(node, NULL, ready);
        if (count <= 0 || (*ready & RX_READY_TIMER) == 0) {
            return count;
        }
        rx_timer_take(hal);
        *ready &= ~RX_READY_TIMER;
        if (*ready != 0) {
            return count - 1;
        }
        if (until != UINT64_MAX && tt_get_ns() >= until) {
            return 0; // this wait's deadline: a timeout
        }
        // An earlier deadline's timer: wait on, for this one.
    }
}

#endif

// rx_wait() for `timeout` ns, 0 meaning no limit: this function's contract (hal.h) is "0 for no timeout", and a NULL
// timespec is the wait's own way to say it. While receive offload is settling, the wait first runs only to the
// settle's deadline, where gro_settle_decide() is made, and then for what is left of `timeout`. Only in a build with
// receive offload: without it tt_receive() builds the timespec in line, as before offload existed - split out,
// rx_wait() stopped being inlined into tt_receive() and every wait cost ~65 instructions more on aarch64 (gate v2,
// 2026-10-12).
#if TT_HAL_UDP_GRO
static int rx_wait_ns(struct tt_Context* node, int64_t timeout, uint32_t* ready) {
    struct timespec timeout_ts;
    if (timeout <= 0) {
        return rx_wait(node, NULL, ready);
    }
    timeout_ts.tv_sec = (time_t)(timeout / SEC_NS);
    timeout_ts.tv_nsec = (long)(timeout % SEC_NS);
    return rx_wait(node, &timeout_ts, ready);
}

static int rx_wait_for(struct tt_Context* node, int64_t timeout, uint32_t* ready) {
    if (node->hal.gro_state == TT_GRO_SETTLING) {
        const uint64_t start = tt_get_ns();
        const int64_t to_deadline = (int64_t)(node->hal.gro_settle_at_ns - start);
        if (to_deadline > 0 && (timeout <= 0 || to_deadline < timeout)) {
            int ret = rx_wait_ns(node, to_deadline, ready);
            if (ret != 0) {
                return ret; // woken before the deadline: whatever it was, a read will abort the settle
            }
            if (timeout > 0) {
                timeout -= (int64_t)(tt_get_ns() - start);
                if (timeout <= 0) {
                    gro_settle_end(node);
                    return 0;
                }
            }
            gro_settle_end(node);
        } else {
            gro_settle_decide(node);
        }
    }
    return rx_wait_ns(node, timeout, ready);
}
#endif

#if TT_HAL_EPOLL
// The deadline timer only where it pays (2026-10-13). It saves the hrtimer epoll_pwait2() starts inside its own call,
// and costs a system call each time it is set (a deadline earlier than the armed one, or none armed) and each time it
// fires (read()): it pays only on a run of waits that end for data before a deadline that rarely moves earlier - a
// stream subscriber's. Elsewhere it cost what it saved several times over. The core ping-pong client (lat_gap_pc.sh,
// reliable_latency p1, 1 kHz): ae5defc8 added 2.0 timerfd_settime() and 1.0 read() a round trip, client instructions
// +4.4% user, +5% kernel (on the rig's Pi: kernel +22..+24% a sample, gate v2 WORSE in every latency cell); the PC
// bench publisher (tx_gap_pc.sh, 1 MB BEST_EFFORT): ~2.8 sets, ~2.8 reads and ~1 more wake-up a sample. So a wait uses
// the timer only after TT_RX_LAZY_STREAK waits in a row ended for data (a datagram, or a rung segment bell); a wait in
// which the timer fired (a read) starts the count again, and the waits in between take their timeout as before
// (rx_wait_plain(), counted timer_plain=). TT_RX_LAZY_ADAPT=0: every wait in a poll uses the timer, as ab/rx-gap2 did
// (the A/B control).
#define TT_RX_LAZY_STREAK 8U
#define RX_READY_FOR_DATA (RX_READY_WELL_KNOWN | RX_READY_DATA | RX_READY_BELL) // a wait that ended for data

static bool rx_timer_lazy_next(const struct tt_hal* hal) {
    return !hal->timer_adapt || hal->timer_data_streak >= TT_RX_LAZY_STREAK;
}

// A wait in a poll that takes its timeout (the deadline `until`) saw the timer fire: an earlier wait armed it, and it
// is in the set either way. The fire is read - level-triggered, it would end every wait after it at once - and the
// wait goes on to its own deadline: an early return would read to the poll loop as a wait cut short by a signal. Out
// of rx_wait_epoll()'s line, as rx_wait_lazy_run() is: the usual wait, which takes its timeout and sees no fire, then
// keeps rx_wait() in line in tt_receive() as before the timer (2026-10-14: with the timer's waits beside it in line,
// rx_wait() was called out of line - the latency client +123 user instructions a round trip on the PC, the server
// +49, over two and one waits; srv_gap_pc.sh, ae5defc8 to 723ec1f0).
static __attribute__((noinline)) int rx_wait_plain_fired(struct tt_Context* node, int64_t timeout, uint64_t until,
                                                         int count, uint32_t* ready) {
    for (;;) {
        rx_timer_take(&node->hal);
        *ready &= ~RX_READY_TIMER;
        if (*ready != 0) {
            return count - 1;
        }
        if (until != UINT64_MAX) {
            const uint64_t now = tt_get_ns();
            if (now >= until) {
                return 0;
            }
            timeout = (int64_t)(until - now);
        }
#if TT_HAL_UDP_GRO
        count = rx_wait_for(node, timeout, ready);
#else
        struct timespec timeout_ts;
        timeout_ts.tv_sec = (time_t)(timeout / SEC_NS);
        timeout_ts.tv_nsec = (long)(timeout % SEC_NS);
        count = rx_wait(node, timeout > 0 ? &timeout_ts : NULL, ready);
#endif
        if (count <= 0 || (*ready & RX_READY_TIMER) == 0) {
            return count; // the run of waits that ended for data is not grown: this wait cost a read
        }
    }
}

// A wait in a poll with the timer (rx_wait_lazy()), and the run of waits that ended for data ended by one that did not.
static __attribute__((noinline)) int rx_wait_lazy_run(struct tt_Context* node, uint64_t until, bool exact,
                                                      uint32_t* ready) {
    int count = rx_wait_lazy(node, until, exact, ready);
    if ((*ready & RX_READY_FOR_DATA) == 0 && count >= 0) {
        node->hal.timer_data_streak = 0; // a timeout, a wake, only room to send: not a wait that ended for data
    }
    return count;
}

// tt_receive()'s wait with the wait set: in a poll (a deadline set), with the timer after TT_RX_LAZY_STREAK waits in a
// row ended for data and with the timeout before; outside a poll, with the timeout. The run of waits that ended for
// data grows here and only here, at a wait that takes its timeout: a wait that uses the timer comes once the run is
// long enough, and needs nothing more (2026-10-14: counted after every wait, the rule cost the latency server, whose
// every wait uses the timer, +17..+19 user instructions a round trip on the PC and +25..+29 on the rig's Pi). The run
// ends at a fire (rx_timer_take(): a stale fire read costs the wait a read) and at a wait that ended without data.
static int rx_wait_epoll(struct tt_Context* node, int64_t timeout, uint32_t* ready) {
    struct tt_hal* hal = &node->hal;
    const uint64_t until = hal->timer_fd_plus1 > 0 ? rx_wait_until(node) : 0;
    // A long wait takes the timer too (TT_RX_TIMER_LONG_NS): with its timeout it would end up to 0.1-0.5% of it late,
    // which ran a scheduled entry ~0.5 ms late after a 500 ms wait (sched_wake_check.c, 2026-10-17). One wake-up at
    // the deadline, and a timerfd_settime() only when the deadline is earlier than the one armed or none is. Only for
    // a wait that ends for a scheduler entry (wait_until_entry): a poll's own budget needs no precision, and the
    // server's 500 ms budget, moving later at every poll, left an earlier one armed that fired early - ~0.4 more
    // epoll_pwait2() a round trip for the core ping server (PC, 2026-10-17).
    const bool long_wait = TT_RX_TIMER_LONG_NS > 0 && timeout > (int64_t)TT_RX_TIMER_LONG_NS && node->wait_until_entry;
#if TT_HAL_UDP_GRO
    if (until != 0 && (rx_timer_lazy_next(hal) || long_wait) && hal->gro_state != TT_GRO_SETTLING) {
#else
    if (until != 0 && (rx_timer_lazy_next(hal) || long_wait)) {
#endif
        return rx_wait_lazy_run(node, until, TT_RX_TIMER_LONG_EXACT && long_wait, ready);
    }
#if TT_HAL_UDP_GRO
    int count = rx_wait_for(node, timeout, ready);
#else
    struct timespec* timeout_ts_ptr = NULL;
    struct timespec timeout_ts;
    if (timeout > 0) {
        timeout_ts.tv_sec = (time_t)(timeout / SEC_NS);
        timeout_ts.tv_nsec = (long)(timeout % SEC_NS);
        timeout_ts_ptr = &timeout_ts;
    }
    int count = rx_wait(node, timeout_ts_ptr, ready);
#endif
    if (until == 0) {
        return count; // outside a poll: no timer bookkeeping
    }
    hal->timer_plain_waits++;
    if (__builtin_expect(count > 0 && (*ready & RX_READY_TIMER) != 0, 0)) {
        count = rx_wait_plain_fired(node, timeout, until, count, ready);
    } else if (count > 0 && (*ready & RX_READY_FOR_DATA) != 0) {
        if (hal->timer_data_streak < UINT8_MAX) {
            hal->timer_data_streak++;
        }
        return count;
    }
    if ((*ready & RX_READY_FOR_DATA) == 0 && count >= 0) {
        hal->timer_data_streak = 0; // a timeout, a wake, only room to send: not a wait that ended for data
    }
    return count;
}
#endif

#if TT_RX_TIMER_SLACK_NS > 0
static _Thread_local bool rx_timer_slack_done = false;

// TT_RX_TIMER_SLACK_NS, once per thread: lowers the thread's timer slack, never raises it (a thread that asked for a
// smaller one keeps it).
static __attribute__((noinline)) void rx_timer_slack_set(void) {
    rx_timer_slack_done = true;
    const int current = prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0); // NOLINT(misc-include-cleaner) - <sys/prctl.h>
    if (current < 0 || current > TT_RX_TIMER_SLACK_NS) {
        // NOLINTNEXTLINE(misc-include-cleaner) - PR_SET_TIMERSLACK: <sys/prctl.h>
        (void)prctl(PR_SET_TIMERSLACK, (unsigned long)TT_RX_TIMER_SLACK_NS, 0, 0, 0);
    }
}

static inline void rx_timer_slack_once(void) {
    if (__builtin_expect(!rx_timer_slack_done, 0)) {
        rx_timer_slack_set();
    }
}
#endif

int32_t tt_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout) {
    // What the last batch read comes first, and without a wait: holding it behind ppoll() would delay
    // datagrams that have already arrived. Then what a drain took while core was away, which left the sockets after
    // the batch and before anything still in them; it keeps draining while it holds any (rx_take_drained()).
#if TT_HAL_UDP_GRO
    node->rx_offset = 0; // at the start of buf unless a merged read hands one out in place
    int32_t merged = gro_take_pending(node, (uint8_t*)buf, len, ip, port);
    if (merged >= 0) {
        return merged;
    }
#endif
    int32_t pending = rx_take_pending(node, buf, len, ip, port);
    if (pending < 0) {
        pending = rx_take_drained(node, buf, len, ip, port, false);
    }
    if (pending >= 0) {
        return pending;
    }
    node->hal.rx_idle = 0; // a timeout or an interrupt leaves no readiness to go on
#if TT_RX_TIMER_SLACK_NS > 0
    rx_timer_slack_once(); // about to wait: with the thread's timer slack at TT_RX_TIMER_SLACK_NS
#endif
    node->hal.rx_idle_since_ns = 0;
#if TT_HAL_UDP_GRO
    gro_maybe_settle(node); // about to wait: what came before was read
#endif
    // Wait for readability with ppoll() instead of arming SO_RCVTIMEO via setsockopt() before
    // every recvfrom(): the timeout here changes on nearly every call (it tracks whatever
    // scheduled event is due next), and re-arming a socket option that often is pure overhead -
    // ppoll() just takes the timeout as a plain argument, no socket mutation needed. ppoll(), not
    // plain poll(): poll()'s own timeout is a whole millisecond int, which silently rounds any
    // shorter wait *up* to 1ms (a caller asking for a 100us slice effectively got throttled to
    // roughly 10x that instead) - found the hard way benchmarking a
    // real publish/subscribe round trip (rmw_tickle/PLAN.md's rmw-perf.yml). ppoll() takes a real
    // struct timespec, so nothing shorter than a millisecond gets rounded at all.
    // Always poll, including for a negative timeout. Negative used to skip the poll and go
    // straight to a blocking recvfrom() on the well-known socket, which blocks exactly as
    // timeout == 0 does but sees neither the data socket nor the wake fd. tt_Context_poll() never
    // takes that path (it turns a negative timeout into a positive wait first), so nothing
    // relied on it, and leaving a path that reads only one of the two sockets would be a trap for
    // the next direct caller.
    int read_fd = node->hal.sock;
    {
        uint32_t ready = 0;
#if TT_HAL_EPOLL
        int poll_ret = rx_wait_epoll(node, timeout, &ready);
#elif TT_HAL_UDP_GRO
        int poll_ret = rx_wait_for(node, timeout, &ready);
#else
        struct timespec* timeout_ts_ptr = NULL;
        struct timespec timeout_ts;
        if (timeout > 0) {
            // This function's contract (see hal.h) is "0 for no timeout", i.e. block until data
            // arrives - timeout_ts_ptr staying NULL is ppoll()'s own way to say exactly that.
            timeout_ts.tv_sec = (time_t)(timeout / SEC_NS);
            timeout_ts.tv_nsec = (long)(timeout % SEC_NS);
            timeout_ts_ptr = &timeout_ts;
        }
        int poll_ret = rx_wait(node, timeout_ts_ptr, &ready);
#endif
        if ((ready & (RX_READY_WELL_KNOWN | RX_READY_DATA)) != 0) {
            TT_TRACE(tt_TRACE_RX_WAKE);
        }
        if (poll_ret == 0) {
            return -1; // Timeout
        }
        if (poll_ret < 0) {
            // NOLINTNEXTLINE(misc-include-cleaner) -- EINTR lives in the same private header as EAGAIN below
            if (errno == EINTR) {
                return -1; // Treat an interrupted wait like a timeout; the caller just polls again
            }
            return -2; // I/O error
        }
#if tt_LARGE_SAMPLES
        if ((ready & RX_READY_WRITABLE) != 0) {
            node->large.tx_writable = true; // core resumes the large send waiting for room (tt_tx_await_writable())
            ready &= ~RX_READY_WRITABLE;
            if (ready == 0) {
                return -1; // nothing to read: a timeout to the caller, which looks at tx_writable
            }
        }
#endif
#if tt_SEGMENT_ENABLED
        // Before the interrupt below, which returns: an edge-triggered bell is reported once, and a ring it reports
        // must be counted towards the bell's next read (bell_rung()) whichever way this wait ends. What the ring
        // means needs no handling on that path - core drains the segment before it sleeps again, whatever woke it.
        if ((ready & RX_READY_BELL) != 0) {
            bell_rung(node);
        }
#endif
        if ((ready & RX_READY_WAKE) != 0) {
            // tt_wake_signal() - drain the counter (its value carries no meaning) and report the
            // interrupt. If the real socket also happens to be ready this same call, it's still
            // readable (both waits are level-triggered for the sockets) and gets picked up on the very next call - no
            // data loss, just one extra round trip.
            uint64_t discard;
            (void)read(node->hal.wake_fd, &discard, sizeof(discard));
            return -3; // Interrupted
        }
#if tt_SEGMENT_ENABLED
        if (ready == RX_READY_BELL) {
            // Only the bell: a record is in the segment and neither socket has anything, so the drain that
            // follows must not ask them (the empty recvmmsg per wake-up the UDP doorbell cost).
            node->hal.rx_idle = TT_RX_IDLE_WELL_KNOWN | TT_RX_IDLE_DATA;
            *ip = 0;
            *port = 0;
            return 0; // a rung bell is a zero-length datagram to core, which is what a doorbell always was
        }
#endif
        // Broadcasts arrive on the well-known socket and unicast on this node's own data socket.
        // Whichever is ready gets read; when both are, they alternate. A fixed preference would
        // not merely delay the other socket - under a sustained stream on the preferred one the
        // other is never read at all, and the path that starves would be RELIABLE recovery, whose
        // ACKNACKs come back as unicast while a Publisher above tt_UNICAST_PEER_THRESHOLD is
        // broadcasting its data. Alternating bounds the wait at one datagram either way.
        bool well_known_ready = (ready & RX_READY_WELL_KNOWN) != 0;
        bool data_ready = (ready & RX_READY_DATA) != 0;
        // What the drain after this read may skip: a socket the wait did not report ready (tt_try_receive()).
        node->hal.rx_idle =
            (uint8_t)((well_known_ready ? 0U : TT_RX_IDLE_WELL_KNOWN) | (data_ready ? 0U : TT_RX_IDLE_DATA));
#if TT_RX_FIXED_PREFERENCE
        // EXPERIMENT ARM, not a proposed behaviour. See the note at tt_RX_FIXED_PREFERENCE below.
        if (data_ready && !well_known_ready) {
            read_fd = node->hal.data_sock;
        }
#else
        if (data_ready && (!well_known_ready || node->hal.rx_prefer_data)) {
            read_fd = node->hal.data_sock;
        }
        if (well_known_ready && data_ready) {
            node->hal.rx_prefer_data = !node->hal.rx_prefer_data;
        }
#endif
    }

    // ppoll() said read_fd is readable: one datagram with recvfrom(), not a batch. This read is on the
    // latency path - the datagram that woke the node is processed, and often answered, before anything
    // else - and a recvmmsg() does not return after its first datagram: it tries the next slot and only
    // then sees EAGAIN, a probe that recv_single_cost.c measured at ~0.3 us on x86 (0.99 against 0.69)
    // and that recvfrom() leaves until after processing, in the drain. Whatever else is queued, the drain
    // (tt_try_receive()) takes in batches.
    int32_t ret = rx_read_one(node, read_fd, buf, len, ip, port);
    if (ret < 0) {
        return ret; // -1 nothing after all (Timeout), -2 I/O error
    }

#if TT_RX_DROP_PERCENT > 0
    // Reported as a timeout rather than an error: a dropped datagram is indistinguishable from one
    // that never arrived, which is the whole point, and an error would make the caller tear the
    // node down instead of carrying on the way real loss makes it carry on.
    if (tt_rx_should_drop()) {
#if TT_HAL_RX_DIRECT
        node->large.direct_split = NULL; // dropped unseen: core never takes it
#endif
        return -1;
    }
#endif

    rx_lone_after_wake(&node->hal, read_fd);
    TT_TRACE(tt_TRACE_RX_DATAGRAM);
    return ret;
}

// Reads one datagram if one is waiting, without blocking - drain_rx() calls it until it says nothing is.
//
// Only sockets that can have something are asked (2026-09-26). This used to probe both sockets blindly,
// alternating which went first: with traffic on one socket, every other call spent a recvfrom() on the
// empty one, and the call that ended each drain spent two. Measured on the rig's server, 1.58 receive
// syscalls per delivered sample against CycloneDDS's 0.74, and 34% of them returned nothing - 95% of the
// server's syscall time was receiving. Now a socket ppoll() did not report ready, or one a read here has
// found empty, is skipped until the next wait (struct tt_hal.rx_idle) - or until TT_RX_IDLE_RECHECK_NS has
// passed, because a drain session need not end: a socket refilled as fast as it is read keeps drain_rx() going,
// and without the recheck the skipped socket was never read again (struct tt_hal.rx_idle_since_ns). Otherwise a
// datagram that lands on a skipped socket waits at most until the next ppoll(), which is level-triggered. When every
// socket is idle this answers without a syscall, and clears the bits so the next drain - one not preceded by a wait,
// like a non-blocking poll - asks both again.
uint32_t tt_rx_buffered(const struct tt_Context* node) {
    uint32_t held = node->hal.rx_next < node->hal.rx_count ? (uint32_t)(node->hal.rx_count - node->hal.rx_next) : 0U;
#if TT_HAL_UDP_GRO
    held += node->hal.gro_left; // a merged read's datagrams, handed out without a system call too
#endif
    return held + drain_held(node);
}

// Notes a datagram returned while a socket is being skipped, and asks every socket again once TT_RX_IDLE_RECHECK_NS
// has passed since the first such return. "Now" is the running poll's reading; outside a poll (a caller draining
// with tt_try_receive() itself) there is none, and the clock is read, as rx_now() does in core for the same reason.
static void rx_idle_count_return(struct tt_Context* node) {
    if (node->hal.rx_idle == 0) {
        return;
    }
    const uint64_t now = node->rx_clock_ns != 0 ? node->rx_clock_ns : tt_get_ns();
    if (node->hal.rx_idle_since_ns == 0) {
        node->hal.rx_idle_since_ns = now;
        return;
    }
    if (now - node->hal.rx_idle_since_ns >= TT_RX_IDLE_RECHECK_NS) {
        node->hal.rx_idle = 0;
        node->hal.rx_idle_since_ns = 0;
    }
}

int32_t tt_try_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
#if TT_HAL_UDP_GRO
    node->rx_offset = 0;
    if (!drain_active(node)) {
        gro_settle_decide(node); // a settle past its deadline is decided before anything is read
    }
    int32_t merged = gro_take_pending(node, (uint8_t*)buf, len, ip, port);
    if (merged >= 0) {
        rx_idle_count_return(node);
        return merged;
    }
#endif
    int32_t pending = rx_take_pending(node, buf, len, ip, port);
    if (pending < 0) {
        pending = rx_take_drained(node, buf, len, ip, port, true);
    }
    if (pending >= 0) {
        rx_idle_count_return(node);
        return pending;
    }
#if tt_RX_DRAIN
    if (node->hal.drain_running) {
        return -1; // the drain is taking what comes on every socket, and has taken nothing since (rx_take_drained())
    }
#endif
    // MSG_DONTWAIT makes just this call non-blocking regardless of the socket's own mode - no
    // poll() first, no socket-option re-arm. Both sockets are candidates, because broadcasts land on
    // the well-known one and unicast on this node's own data socket, and the same alternation
    // tt_receive() uses decides which goes first, so a sustained stream on one cannot starve the other.
#if TT_RX_FIXED_PREFERENCE
    int order[2] = {node->hal.sock, node->hal.data_sock};
#else
    int order[2] = {node->hal.rx_prefer_data ? node->hal.data_sock : node->hal.sock,
                    node->hal.rx_prefer_data ? node->hal.sock : node->hal.data_sock};
    node->hal.rx_prefer_data = !node->hal.rx_prefer_data;
#endif

    int32_t ret = -1;
    for (int i = 0; i < 2 && ret < 0; i++) {
        uint8_t bit = order[i] == node->hal.data_sock ? TT_RX_IDLE_DATA : TT_RX_IDLE_WELL_KNOWN;
        if ((node->hal.rx_idle & bit) != 0) {
            node->hal.rx_idle_skips++; // a read not made: the rx line's idle_skips=
            continue;
        }
        ret = rx_fill(node, order[i], buf, len, ip, port);
        if (bit == node->hal.rx_probe_bit) {
            rx_lone_probed(&node->hal, ret >= 0);
        }
        if (ret == -2) {
            return -2; // I/O error
        }
        if (ret < 0) {
            node->hal.rx_idle |= bit;
        }
    }
    if (ret < 0) {
        node->hal.rx_idle = 0; // drained: the next drain session asks every socket again
        node->hal.rx_idle_since_ns = 0;
        return -1; // Nothing waiting
    }
    rx_idle_count_return(node);
    TT_TRACE(tt_TRACE_RX_DATAGRAM);
    return ret;
}

#if tt_HAL_IO_URING
// tt_rx_maybe_ready()'s machinery. Deliberately the smallest io_uring there is: no liburing, no buffers, no
// multishot - one-shot POLLIN on each receive socket, submitted only after a check found that socket unarmed, and
// completed by the kernel the first time a datagram makes it readable. So a socket nobody writes to costs one
// submission ever, and a quiet one is checked by reading a counter in shared memory.
//
// Why one-shot and not multishot: a multishot poll posts a completion for every wake-up, which on a busy receiver
// is one per datagram - work added to the kernel path, where the cross-host figures are made. One-shot posts once
// per arm, and arming happens only from the busy-loop check, so a context that never runs that check pays nothing.
// It also needs Linux 5.1 rather than 5.13.
#define URING_ENTRIES 4U
#define URING_SOCKETS 2U

static void uring_unmap(struct tt_hal* hal) {
    if (hal->uring_sqes != NULL) {
        (void)munmap(hal->uring_sqes, hal->uring_sqes_len);
    }
    if (hal->uring_cq_map != NULL && hal->uring_cq_map != hal->uring_sq_map) {
        (void)munmap(hal->uring_cq_map, hal->uring_cq_map_len);
    }
    if (hal->uring_sq_map != NULL) {
        (void)munmap(hal->uring_sq_map, hal->uring_sq_map_len);
    }
    hal->uring_sqes = NULL;
    hal->uring_cq_map = NULL;
    hal->uring_sq_map = NULL;
}

static void uring_close(struct tt_Context* node) {
    struct tt_hal* hal = &node->hal;
    if (hal->uring_fd < 0) {
        return;
    }
    uring_unmap(hal);
    (void)close(hal->uring_fd);
    hal->uring_fd = -1;
    hal->uring_armed = 0;
}

// Refused is not an error: Docker's default seccomp profile, Kubernetes' RuntimeDefault and
// kernel.io_uring_disabled all refuse it, and the context works without it - every check then reads the socket, as
// it did before. Said once per process, so the reason a deployment is slower is in its log (README, "io_uring").
static void uring_refused(const char* step, int err) {
#if tt_HAL_RX_HINT == tt_RX_HINT_URING
    TT_LOG_ERROR("io_uring refused (%s: %s) and this build requires it (tt_HAL_RX_HINT=tt_RX_HINT_URING) - not "
                 "creating the context. Allow io_uring (README.md, \"io_uring\") or build with tt_RX_HINT_AUTO.",
                 step, strerror(err));
#else
    static bool told = false;
    if (!told) {
        told = true;
        TT_LOG_WARNING("io_uring unavailable (%s: %s) - a busy context will check its sockets with a read each time. "
                       "See README.md, \"io_uring\", to allow it.",
                       step, strerror(err));
    }
#endif
}

static bool uring_setup(struct tt_Context* node) {
    struct tt_hal* hal = &node->hal;
    hal->uring_fd = -1;
    hal->uring_armed = 0;
    hal->uring_sq_map = NULL;
    hal->uring_cq_map = NULL;
    hal->uring_sqes = NULL;

    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    // NOLINTNEXTLINE(misc-include-cleaner) - see the <sys/syscall.h> include
    int ring_fd = (int)syscall(__NR_io_uring_setup, URING_ENTRIES, &params);
    if (ring_fd < 0) {
        uring_refused("io_uring_setup", errno);
        return false;
    }
    hal->uring_fd = ring_fd;
    hal->uring_sq_map_len = params.sq_off.array + (params.sq_entries * sizeof(uint32_t));
    hal->uring_cq_map_len = params.cq_off.cqes + (params.cq_entries * sizeof(struct io_uring_cqe));
    const bool single = (params.features & IORING_FEAT_SINGLE_MMAP) != 0;
    if (single && hal->uring_cq_map_len > hal->uring_sq_map_len) {
        hal->uring_sq_map_len = hal->uring_cq_map_len;
    }
    void* sq_map = mmap(NULL, hal->uring_sq_map_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring_fd,
                        IORING_OFF_SQ_RING);
    if (sq_map == MAP_FAILED) {
        uring_refused("mmap SQ", errno);
        uring_close(node);
        return false;
    }
    hal->uring_sq_map = sq_map;
    void* cq_map = sq_map;
    if (!single) {
        cq_map = mmap(NULL, hal->uring_cq_map_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring_fd,
                      IORING_OFF_CQ_RING);
        if (cq_map == MAP_FAILED) {
            uring_refused("mmap CQ", errno);
            uring_close(node);
            return false;
        }
    }
    hal->uring_cq_map = cq_map;
    hal->uring_sqes_len = params.sq_entries * sizeof(struct io_uring_sqe);
    void* sqes =
        mmap(NULL, hal->uring_sqes_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring_fd, IORING_OFF_SQES);
    if (sqes == MAP_FAILED) {
        uring_refused("mmap SQEs", errno);
        uring_close(node);
        return false;
    }
    hal->uring_sqes = sqes;
    uint8_t* sq_base = (uint8_t*)sq_map;
    uint8_t* cq_base = (uint8_t*)cq_map;
    hal->uring_sq_tail = (uint32_t*)(sq_base + params.sq_off.tail);
    hal->uring_sq_mask = (uint32_t*)(sq_base + params.sq_off.ring_mask);
    hal->uring_sq_array = (uint32_t*)(sq_base + params.sq_off.array);
    hal->uring_cq_head = (uint32_t*)(cq_base + params.cq_off.head);
    hal->uring_cq_tail = (uint32_t*)(cq_base + params.cq_off.tail);
    hal->uring_cq_mask = (uint32_t*)(cq_base + params.cq_off.ring_mask);
    hal->uring_cqes = cq_base + params.cq_off.cqes;
    return true;
}

// Completions in, armed bits out. A completion means its socket became readable (or the poll ended for another
// reason - an error, a cancellation), and either way that socket is no longer armed, which is what makes the next
// check read it.
static void uring_harvest(struct tt_hal* hal) {
    uint32_t head = *hal->uring_cq_head; // ours to move
    uint32_t tail = __atomic_load_n(hal->uring_cq_tail, __ATOMIC_ACQUIRE);
    if (head == tail) {
        return;
    }
    const struct io_uring_cqe* cqes = (const struct io_uring_cqe*)hal->uring_cqes;
    for (; head != tail; head++) {
        uint64_t which = cqes[head & *hal->uring_cq_mask].user_data;
        if (which < URING_SOCKETS) {
            hal->uring_armed &= (uint8_t)~(1U << which);
        }
    }
    __atomic_store_n(hal->uring_cq_head, head, __ATOMIC_RELEASE);
}

// One submission for every socket in `bits`, in one io_uring_enter(). A poll on a socket that is already readable
// completes at once, so arming before the read that will empty it loses nothing.
static void uring_arm(struct tt_hal* hal, uint8_t bits) {
    const int fds[URING_SOCKETS] = {hal->sock, hal->data_sock};
    uint32_t tail = *hal->uring_sq_tail; // ours to move
    uint32_t added = 0;
    struct io_uring_sqe* sqes = (struct io_uring_sqe*)hal->uring_sqes;
    for (uint32_t i = 0; i < URING_SOCKETS; i++) {
        if ((bits & (1U << i)) == 0 || fds[i] < 0) {
            continue;
        }
        uint32_t index = (tail + added) & *hal->uring_sq_mask;
        struct io_uring_sqe* sqe = &sqes[index];
        memset(sqe, 0, sizeof(*sqe));
        sqe->opcode = IORING_OP_POLL_ADD;
        sqe->fd = fds[i];
        uint32_t mask = POLLIN; // NOLINT(misc-include-cleaner) - <poll.h>
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        mask = (mask << 16) | (mask >> 16); // the kernel reads poll32_events half-word swapped on big-endian
#endif
        sqe->poll32_events = mask;
        sqe->user_data = i;
        hal->uring_sq_array[index] = index;
        added++;
    }
    if (added == 0) {
        return;
    }
    __atomic_store_n(hal->uring_sq_tail, tail + added, __ATOMIC_RELEASE);
    // NOLINTNEXTLINE(misc-include-cleaner) - see the <sys/syscall.h> include
    int submitted = (int)syscall(__NR_io_uring_enter, hal->uring_fd, added, 0U, 0U, NULL, 0);
    if (submitted < 0) {
        return; // nothing armed: the next check reads the socket, as without the ring
    }
    hal->uring_arms++;
    hal->uring_armed |= bits;
}

bool tt_rx_maybe_ready(struct tt_Context* node) {
    struct tt_hal* hal = &node->hal;
#if tt_RX_DRAIN
    if (hal->drain_running) {
        return true; // what it took is known only once it stops, which the read does
    }
#endif
    if (hal->uring_fd < 0 || tt_rx_buffered(node) != 0) {
        return true;
    }
    uring_harvest(hal);
    const uint8_t all = (uint8_t)((1U << URING_SOCKETS) - 1U);
    if (hal->uring_armed == all) {
        hal->uring_skipped++;
        return false; // both polls still waiting: nothing has arrived on either socket since they were armed
    }
    uring_arm(hal, (uint8_t)(all & ~hal->uring_armed));
    return true; // unarmed means unknown: let the caller read, the poll now in flight covers what comes after
}
#else
bool tt_rx_maybe_ready(struct tt_Context* node) {
    (void)node;
    return true;
}
#endif

tt_ret_t tt_wake_signal(struct tt_Context* node) {
    uint64_t one = 1;
    // eventfd's write() only ever blocks if the counter would overflow (~2^64 unconsumed
    // signals) - never a real concern here, so this is safe to call from any thread, or from
    // within a signal handler, without risking a deadlock against whatever tt_receive() might be
    // doing.
    if (write(node->hal.wake_fd, &one, sizeof(one)) < 0) {
        TT_LOG_ERROR("Cannot send wake signal: %s", strerror(errno));
        return tt_RET_IO_ERROR;
    }
    return tt_RET_OK;
}

#if tt_SEGMENT_ENABLED
// The segment's platform half (SHM_PLAN.md stage 1). Only the mapping lives here: the ring, the
// naming and the header checks are core's, so they are the same code and the same tests everywhere
// and only the way pages are obtained differs.
//
// Plain open()/ftruncate()/mmap() on a /dev/shm path rather than shm_open(), matching
// registry_path() above - same directory, same visibility across network namespaces, and one fewer
// library to link.
// The segment's own mode constant rather than the registry's REGISTRY_MODE, which sits inside the
// registry's #if and would make this module silently depend on that one being enabled.
#define SEGMENT_MODE 0666

// Whether a context still owns the segment file open as `segment_fd`: its owner holds an exclusive flock() on it for
// as long as the region is mapped (tt_segment_create()), so a shared non-blocking attempt that SUCCEEDS means nobody
// does - the owner died, or exited without unlinking - and the ring is one nobody will ever drain. Found 2026-10-09:
// a writer attached to such a file, left by a killed process under the same (address, port, id), before the new
// owner had built its own, and wrote into it while the two sides reported different segment ids (~2% of
// test_loaned_messages runs). Nothing read through the mapping could tell: the header was valid and named the peer.
//
// Shared, so two writers asking at once do not refuse each other. What it took goes with the descriptor, which every
// caller closes straight after. An error other than EWOULDBLOCK is "cannot tell", answered as alive: the check then
// decides nothing, which is the behaviour before it existed.
static bool segment_owner_gone(int segment_fd) {
    return flock(segment_fd, LOCK_SH | LOCK_NB) == 0;
}

void* tt_segment_create(const char* path, size_t bytes) {
    // Unlinked first, so a segment left behind by a dead context of this name is replaced rather
    // than inherited. Anyone still holding the old mapping keeps it and sees the old incarnation,
    // which is precisely what lets them notice they are stale.
    //
    // A file that a LIVE context still owns is replaced too, as before, but said: it means two contexts share
    // (address, port, id), which one data port per address should make impossible, and the one whose file this was
    // keeps a ring its writers will leave at their next revalidation. Asked only here, once per segment built.
    int previous_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (previous_fd >= 0) {
        if (!segment_owner_gone(previous_fd)) {
            TT_LOG_WARNING("Replacing segment %s although a live context owns it - two contexts share its name", path);
        }
        (void)close(previous_fd);
    }
    (void)unlink(path);
    int segment_fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, SEGMENT_MODE);
    if (segment_fd < 0) {
        TT_LOG_WARNING("Cannot create segment %s: %s", path, strerror(errno));
        return NULL;
    }
    // The owner's mark, taken before the file has a size and so before it can carry a header a writer believes. A
    // writer asks only once the size is right (tt_segment_attach()), so nothing else holds this file yet; blocking
    // anyway, because the only lock anyone else ever takes on it is a momentary shared one.
    //
    // It is held by the MAPPING, not by this descriptor. A flock() belongs to the open file description, which lives
    // until its last reference goes, and a MAP_SHARED mapping is one: the descriptor is closed below and the lock
    // stays until tt_segment_detach() unmaps the region or the process ends, whichever way it ends. So "owned" means
    // exactly "mapped by the context that built it", with no descriptor to keep and no HAL state to forget
    // (tests/test_segment_owner.c checks both edges on the running kernel). A child forked without exec inherits the
    // mapping and with it the lock; the dead-reader rule (tt_SEGMENT_DEAD_READER_NS) still covers that one.
    if (flock(segment_fd, LOCK_EX) != 0) {
        TT_LOG_WARNING("Cannot lock segment %s: %s", path, strerror(errno));
        (void)close(segment_fd);
        (void)unlink(path);
        return NULL;
    }
    if (ftruncate(segment_fd, (off_t)bytes) != 0) {
        TT_LOG_WARNING("Cannot size segment %s to %zu: %s", path, bytes, strerror(errno));
        (void)close(segment_fd);
        (void)unlink(path);
        return NULL;
    }
    void* mapping = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, segment_fd, 0);
    (void)close(segment_fd); // the mapping keeps the region alive; the descriptor has no further use
    if (mapping == MAP_FAILED) {
        TT_LOG_WARNING("Cannot map segment %s: %s", path, strerror(errno));
        (void)unlink(path);
        return NULL;
    }
    return mapping;
}

void* tt_segment_attach(const char* path, size_t bytes, uint8_t* why) {
    int segment_fd = open(path, O_RDWR | O_CLOEXEC);
    if (segment_fd < 0) {
        // ENOENT is the ordinary case and not a failure: a peer on another host, or one built
        // without the module. Anything else is a real refusal and is worth telling apart, because
        // a permission problem that reads as "not same host" is a module that is inert for a
        // reason nobody can see.
        *why = (uint8_t)(errno == ENOENT ? tt_SEGMENT_ABSENT : tt_SEGMENT_REFUSED);
        return NULL;
    }
    // A region smaller than expected is refused rather than mapped short: the caller is about to
    // index slots inside it, and a short mapping would fault on a slot that is legitimately there
    // by the header's own numbers.
    struct stat info;
    if (fstat(segment_fd, &info) != 0 || (size_t)info.st_size < bytes) {
        *why = (uint8_t)tt_SEGMENT_BAD_HEADER;
        (void)close(segment_fd);
        return NULL;
    }
    // A file nobody owns: refused before it is mapped, and not unlinked. Removing it by name could remove a
    // successor's file instead - one created at this name between this check and the unlink - and nothing in
    // unlink() can be made conditional on which file the name holds. The successor's own create replaces it.
    // One flock() per attach and per revalidation (every tt_SEGMENT_REVALIDATE_SENDS sends), none per datagram.
    if (segment_owner_gone(segment_fd)) {
        *why = (uint8_t)tt_SEGMENT_ORPHANED;
        (void)close(segment_fd);
        return NULL;
    }
    void* mapping = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, segment_fd, 0);
    (void)close(segment_fd);
    if (mapping == MAP_FAILED) {
        *why = (uint8_t)tt_SEGMENT_REFUSED;
        return NULL;
    }
    *why = (uint8_t)tt_SEGMENT_ATTACHED;
    return mapping;
}

void tt_segment_detach(void* mapping, size_t bytes) {
    if (mapping != NULL) {
        (void)munmap(mapping, bytes);
    }
}

void tt_segment_unlink(const char* path) {
    (void)unlink(path);
}

#define BELL_DRAIN_BYTES 64 // rings read per call: any size empties the pipe, a larger one in fewer reads

#if TT_HAL_EPOLL
// Whether this kernel reports EVERY write to a pipe to an edge-triggered epoll, the second one included, while the
// first is still unread. Linux has done so since 5.14 (and since 5.16 only for a pipe somebody polls, which this one
// is); 5.5 to 5.13 reported a write only into an empty pipe, and a bell that is never read would then ring exactly
// once. Asked of the running kernel rather than assumed from a version, on a scratch epoll so the context's own set
// sees none of it. The bell is the owner's own, open read-write, so both ends are at hand; it is left empty.
static bool bell_edge_works(int bell) {
    int probe = epoll_create1(EPOLL_CLOEXEC);
    if (probe < 0) {
        return false;
    }
    bool works = false;
    struct epoll_event event = {.events = EPOLLIN | EPOLLET, .data = {.u32 = RX_READY_BELL}};
    if (epoll_ctl(probe, EPOLL_CTL_ADD, bell, &event) == 0) {
        const uint8_t one = 1;
        const struct timespec now = {0, 0};
        int reported = 0;
        for (int ring = 0; ring < 2; ring++) {
            if (write(bell, &one, sizeof(one)) == (ssize_t)sizeof(one) &&
                epoll_pwait2(probe, &event, 1, &now, NULL) == 1) {
                reported++;
            }
        }
        works = reported == 2;
    }
    (void)close(probe);
    uint8_t discard[BELL_DRAIN_BYTES];
    while (read(bell, discard, sizeof(discard)) > 0) {
    }
    return works;
}
#endif

// Puts the bell in the context's wait set: edge-triggered when the kernel reports every ring (bell_edge_works()), so
// a ring costs the reader no read(); level-triggered and read on every ring otherwise. Without a set, tt_receive()'s
// ppoll() carries it, level-triggered, as before.
//
// How often an edge-triggered bell is read, and why that is not a tuned interval: every writer rings a reader at most
// once per sleep generation (struct tt_SegmentPeer.doorbell_generation), and a context has at most tt_MAX_CONTEXT_IDS
// peers, so G generations leave at most G x tt_MAX_CONTEXT_IDS bytes - plus one generation's worth from rings already
// on their way when the bell is read, and one for the generation being slept in. Reading it every
// capacity / (4 x tt_MAX_CONTEXT_IDS) generations keeps that under half the pipe's own capacity, read from the pipe,
// on any hardware: 64 generations at Linux's default 64 KiB. A full pipe would refuse a ring - a lost wakeup - so the
// bound is on the worst case, not on what is typical.
static void bell_join_wait_set(struct tt_Context* node, int bell) {
    node->hal.bell_drain_every = 0;
#if TT_HAL_EPOLL
    if (node->hal.epoll_fd_plus1 == 0) {
        return;
    }
    int capacity = fcntl(bell, F_GETPIPE_SZ);
    uint32_t every = capacity > 0 ? (uint32_t)capacity / (4U * (uint32_t)tt_MAX_CONTEXT_IDS) : 0U;
    bool edge = every > 0 && bell_edge_works(bell);
    struct epoll_event event = {.events = EPOLLIN | (edge ? (uint32_t)EPOLLET : 0U), .data = {.u32 = RX_READY_BELL}};
    if (epoll_ctl(node->hal.epoll_fd_plus1 - 1, EPOLL_CTL_ADD, bell, &event) != 0) {
        // Not in the set, so no wait would ever see it: give the set up and let ppoll() carry all four.
        TT_LOG_WARNING("Cannot add the segment doorbell to the wait set (%s) - waiting with ppoll()", strerror(errno));
        (void)close(node->hal.epoll_fd_plus1 - 1);
        node->hal.epoll_fd_plus1 = 0;
        return;
    }
    if (edge) {
        node->hal.bell_drain_every = every;
        node->hal.bell_drained_at = node->segment_sleep_generation;
    } else {
        TT_LOG_WARNING("This kernel does not report every ring of an unread doorbell - reading it on every ring");
    }
#else
    (void)node;
    (void)bell;
#endif
}

// The doorbell FIFO (hal.h). Its name is the segment's plus a suffix, built by core, so the two cannot drift.
//
// The owner opens its own FIFO read-write rather than read-only: a FIFO with no writer reports POLLHUP to a reader
// for as long as nobody has it open for writing, which would turn every wait into a busy loop between peers. Holding
// both ends keeps it quiet. Non-blocking both ways - a reader must never sleep in read(), and a writer never waits on
// a full pipe. A full pipe refuses the ring: level-triggered that is a bell already rung, edge-triggered it would be a
// wake-up lost, which is why an edge-triggered bell is read before it can fill (bell_join_wait_set()).
int32_t tt_segment_bell_create(struct tt_Context* node, const char* path) {
    (void)unlink(path); // a bell left by a dead context of this name is replaced, as its segment is
    if (mkfifo(path, SEGMENT_MODE) != 0) {
        TT_LOG_WARNING("Cannot create segment doorbell %s: %s - peers will ring over UDP", path, strerror(errno));
        return -1;
    }
    // The umask applied to mkfifo(); the peer may run as another user, exactly as for the segment itself.
    (void)chmod(path, SEGMENT_MODE);
    int bell = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (bell < 0) {
        TT_LOG_WARNING("Cannot open segment doorbell %s: %s - peers will ring over UDP", path, strerror(errno));
        (void)unlink(path);
        return -1;
    }
    node->hal.bell_fd_plus1 = bell + 1;
    bell_join_wait_set(node, bell);
    return 0;
}

void tt_segment_bell_destroy(struct tt_Context* node, const char* path) {
    if (node->hal.bell_fd_plus1 > 0) {
#if TT_HAL_EPOLL
        if (node->hal.epoll_fd_plus1 > 0) {
            (void)epoll_ctl(node->hal.epoll_fd_plus1 - 1, EPOLL_CTL_DEL, node->hal.bell_fd_plus1 - 1, NULL);
        }
#endif
        node->hal.bell_drain_every = 0;
        (void)close(node->hal.bell_fd_plus1 - 1);
        node->hal.bell_fd_plus1 = 0;
    }
    (void)unlink(path);
}

// Read-write, not write-only, so that this end always counts as a reader of the pipe. A write-only end outlives the
// owner that held the read end, and write() on a FIFO with no reader fails with EPIPE AND raises SIGPIPE, whose
// default action ends the process: a same-host subscriber that exited left its reader_waiting flag set, the
// publisher rang it and died with status 141 (tests/test_segment_bell.c; CI never saw it because the GitHub runner
// starts steps with SIGPIPE ignored). Holding a read end costs the ring nothing - it stays one write() - where
// blocking the signal around each ring would have cost two or three syscalls per wake-up. Linux defines O_RDWR on a
// FIFO (fifo(7)); POSIX leaves it undefined, and this HAL is Linux's.
//
// What this end gives up is ENXIO at open for an owner that has gone: that bell now opens, and its rings fill a pipe
// nobody reads until write() returns EAGAIN, which is as useless and as harmless as the UDP doorbell it used to
// fall back to - a dead owner is found by its ring refusing records (segment_deliver_ringing() in core), not by its
// bell. ENOENT, a peer from before the bell existed, still falls back to UDP. This end never reads, so it takes
// nothing from the owner's wait, edge-triggered or not.
int32_t tt_segment_bell_open(const char* path) {
    return open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
}

void tt_segment_bell_ring(int32_t bell) {
    const uint8_t one = 1;
    (void)write(bell, &one, sizeof(one)); // EAGAIN: the pipe is full of rings no reader has drained
}

void tt_segment_bell_close(int32_t bell) {
    (void)close(bell);
}

uint64_t tt_thread_cpu_ns(void) {
    struct timespec ts;
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * SEC_NS) + (uint64_t)ts.tv_nsec;
}

// Empties the bell so a level-triggered wait stops reporting it. Rings carry no content: how many were written
// does not matter, only that the reader is now awake and about to drain the segment.

static void bell_drain(struct tt_Context* node) {
    uint8_t discard[BELL_DRAIN_BYTES];
    while (read(node->hal.bell_fd_plus1 - 1, discard, sizeof(discard)) == (ssize_t)sizeof(discard)) {
    }
    node->hal.bell_drains++;
}

// A ring the wait reported. Level-triggered, it must be read now or every later wait returns at once. Edge-triggered
// (bell_join_wait_set()), the report was the whole of it, and the bytes rings leave are read only when enough sleep
// generations have passed that they could approach the pipe's capacity.
static void bell_rung(struct tt_Context* node) {
    if (node->hal.bell_drain_every == 0) {
        bell_drain(node);
        return;
    }
    uint32_t generation = node->segment_sleep_generation;
    if (generation - node->hal.bell_drained_at >= node->hal.bell_drain_every) {
        bell_drain(node);
        node->hal.bell_drained_at = generation;
    }
}
#endif
