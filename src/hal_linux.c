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
#include <ifaddrs.h>
#include <poll.h>
#include <stddef.h> // offsetof
#include <stdint.h>
#include <stdio.h>
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
// struct iovec (tt_send_iov, below) - clang-tidy's IWYU mapping doesn't know this glibc symbol's
// real (portable, POSIX-specified) home, so it flags both this include and the struct itself as
// if neither provided/needed the other; this is the correct, portable header regardless (see
// readv(2)/writev(2)/sendmsg(2)).
#include <sys/uio.h> // NOLINT(misc-include-cleaner)
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/hal_linux.h> // tt_RX_BATCH, struct tt_mmsghdr
#if tt_HAL_IO_URING           // after hal_linux.h, which derives it from tt_HAL_RX_HINT
// __NR_io_uring_*: <sys/syscall.h> is the portable spelling; the number itself lives in an architecture's own header
// (asm/unistd_64.h on x86-64, asm-generic/unistd.h on arm64), which include-cleaner would have us name instead.
#include <linux/io_uring.h> // tt_rx_maybe_ready()
#include <sys/syscall.h>    // NOLINT(misc-include-cleaner)
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
#include <fcntl.h> // open()

#include <sys/stat.h> // fchmod(), fstat()
#endif

#if tt_CONTEXT_ID_CLAIM
#include <signal.h> // kill()
#include <stdlib.h> // getenv()

#include <sys/file.h>  // flock()
#include <sys/types.h> // pid_t
#endif

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

#define SEC_NS 1000000000LL

struct _tt_Config _tt_CONFIG = {
    .addr = _tt_CONTEXT_ADDRESS,
    .port = _tt_CONTEXT_PORT,
    .broadcast = _tt_CONTEXT_BROADCAST,
    .context_id = tt_CONTEXT_ID_INVALID, // auto-detect by default; see its own comment in config.h
};

uint64_t tt_get_ns(void) {
    struct timespec ts;
    // CLOCK_REALTIME lives in a glibc-private header; <time.h> (included above) is the correct public header.
    // NOLINTNEXTLINE(misc-include-cleaner)
    clock_gettime(CLOCK_REALTIME, &ts);

    return ((uint64_t)ts.tv_sec * SEC_NS) + ts.tv_nsec;
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

// The data socket's address, read back: bound to any address, it is the link's.
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
#else
    (void)node;
#endif
}

tt_ret_t tt_bind(struct tt_Context* node) {
    // Set before anything below can fail into tt_close(): -1 says "nothing to close here yet",
    // the same convention node->hal.sock itself relies on implicitly (every failure that reaches
    // tt_close() below happens after sock was already created successfully).
    node->hal.wake_fd = -1;
    node->hal.bell_fd_plus1 = 0;  // no segment yet, so no doorbell (tt_segment_bell_create())
    node->hal.epoll_fd_plus1 = 0; // none yet: tt_close() after a failed bind has nothing to close
#if tt_SEGMENT_ENABLED
    node->hal.bell_drain_every = 0;
    node->hal.bell_drained_at = 0;
    node->hal.bell_drains = 0;
#endif
    node->hal.data_sock = -1;
#if tt_HAL_IO_URING
    node->hal.uring_fd = -1; // tt_close() after a failed bind must not close what was never opened
#endif
    node->hal.rx_prefer_data = false;
    node->hal.rx_idle = 0;
    node->hal.rx_idle_since_ns = 0;
    node->hal.rx_count = 0;
    node->hal.rx_next = 0;
    node->hal.rx_headers_for = NULL;
    node->hal.rx_batch_calls = 0;
    node->hal.rx_batch_datagrams = 0;
    node->hal.rx_batch_full = 0;

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
    wait_set_setup(node); // without it tt_receive() builds a ppoll() set per call, as it always did

#if tt_HAL_IO_URING
    if (!uring_setup(node) && tt_HAL_RX_HINT == tt_RX_HINT_URING) {
        tt_close(node); // the reason was logged by uring_refused()
        return tt_RET_UNSUPPORTED;
    }
#endif
    return tt_RET_OK;
}

void tt_close(struct tt_Context* node) {
#if tt_CONTEXT_ID_CLAIM
    release_claimed_id(node);
#endif
#if tt_HAL_IO_URING
    uring_close(node); // before the sockets: a poll in flight on a socket being closed is cancelled with the ring
#endif
    node->hal.rx_count = 0; // anything a batch still held belonged to the sockets closed below
    node->hal.rx_next = 0;
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
    if (node->hal.epoll_fd_plus1 > 0) {
        (void)close(node->hal.epoll_fd_plus1 - 1);
        node->hal.epoll_fd_plus1 = 0;
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
    int32_t sent = (int32_t)sendto(node->hal.data_sock, buf, len, 0, (struct sockaddr*)&node->hal.broadcast_addr,
                                   sizeof(struct sockaddr_in));
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
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(ip);
    addr.sin_port = htons(port);

    TT_TRACE(tt_TRACE_TX_START);
    int32_t sent =
        (int32_t)sendto(node->hal.data_sock, buf, len, 0, (struct sockaddr*)&addr, sizeof(struct sockaddr_in));
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
    struct msghdr msg = {0};
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;
    if (ip != 0) {
        unicast_addr.sin_family = AF_INET;
        unicast_addr.sin_addr.s_addr = htonl(ip);
        unicast_addr.sin_port = htons(port);
        msg.msg_name = &unicast_addr;
        msg.msg_namelen = sizeof(unicast_addr);
    } else {
        msg.msg_name = &node->hal.broadcast_addr;
        msg.msg_namelen = sizeof(node->hal.broadcast_addr);
    }

    TT_TRACE(tt_TRACE_TX_START);
    return (int32_t)sendmsg(node->hal.data_sock, &msg, 0);
}

// Datagrams per sendmmsg() call. A sample's fragments (at most tt_FRAG_MAX_COUNT) always fit one call; a
// longer batch takes several, in order.
#define TT_SEND_BATCH_CHUNK 64

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
        int result = sendmmsg(node->hal.data_sock, msgs, chunk, 0);
        TT_TRACE(tt_TRACE_TX_DONE);
        if (result <= 0) {
            return -1; // errno says why; a 0 would otherwise loop forever
        }
        sent += (uint32_t)result;
    }
    return (int32_t)count;
}

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

// tt_receive()'s wait set: the two sockets, the wake eventfd, and - with the segment - its doorbell FIFO.
#if tt_SEGMENT_ENABLED
#define RX_WAIT_FDS 4
#else
#define RX_WAIT_FDS 3
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

// One datagram with recvfrom(), without waiting: its length, -1 when nothing is waiting, -2 on an I/O error.
static int32_t rx_read_one(struct tt_Context* node, int socket_fd, void* buf, size_t len, uint32_t* ip,
                           uint16_t* port) {
    node->rx_via_data_port = (socket_fd == node->hal.data_sock);
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int32_t ret = (int32_t)recvfrom(socket_fd, buf, len, MSG_DONTWAIT, (struct sockaddr*)&addr, &addr_len);
    if (ret < 0) {
        // NOLINTNEXTLINE(misc-include-cleaner) - EAGAIN/EWOULDBLOCK: glibc-private headers
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
    }
    *ip = ntohl(addr.sin_addr.s_addr);
    *port = ntohs(addr.sin_port);
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
    int got = recvmmsg(socket_fd, (struct mmsghdr*)hal->rx_msgs, tt_RX_BATCH, MSG_DONTWAIT, NULL);
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
static int rx_wait(struct tt_Context* node, const struct timespec* timeout_ts, uint32_t* ready) {
    *ready = 0;
#if TT_HAL_EPOLL
    if (node->hal.epoll_fd_plus1 > 0) {
        struct epoll_event events[RX_WAIT_FDS];
        int count = epoll_pwait2(node->hal.epoll_fd_plus1 - 1, events, RX_WAIT_FDS, timeout_ts, NULL);
        for (int i = 0; i < count; i++) {
            // A descriptor reported only for an error or a hangup counts towards the return value, as it did with
            // ppoll(), and is read like one: the read says what the error was.
            if ((events[i].events & EPOLLIN) != 0) {
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
        {.fd = node->hal.sock, .events = POLLIN, .revents = 0},      // NOLINT(misc-include-cleaner)
        {.fd = node->hal.wake_fd, .events = POLLIN, .revents = 0},   // NOLINT(misc-include-cleaner)
        {.fd = node->hal.data_sock, .events = POLLIN, .revents = 0}, // NOLINT(misc-include-cleaner)
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
    return count;
}

int32_t tt_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout) {
    // What the last batch read comes first, and without a wait: holding it behind ppoll() would delay
    // datagrams that have already arrived.
    int32_t pending = rx_take_pending(node, buf, len, ip, port);
    if (pending >= 0) {
        return pending;
    }
    node->hal.rx_idle = 0; // a timeout or an interrupt leaves no readiness to go on
    node->hal.rx_idle_since_ns = 0;
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
        struct timespec* timeout_ts_ptr = NULL;
        struct timespec timeout_ts;
        if (timeout > 0) {
            // This function's contract (see hal.h) is "0 for no timeout", i.e. block until data
            // arrives - timeout_ts_ptr staying NULL is ppoll()'s own way to say exactly that, no
            // special-cased sentinel value needed (unlike poll()'s own timeout=-1 convention).
            timeout_ts.tv_sec = (time_t)(timeout / SEC_NS);
            timeout_ts.tv_nsec = (long)(timeout % SEC_NS);
            timeout_ts_ptr = &timeout_ts;
        }

        uint32_t ready = 0;
        int poll_ret = rx_wait(node, timeout_ts_ptr, &ready);
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
        return -1;
    }
#endif

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
    return node->hal.rx_next < node->hal.rx_count ? (uint32_t)(node->hal.rx_count - node->hal.rx_next) : 0U;
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
    int32_t pending = rx_take_pending(node, buf, len, ip, port);
    if (pending >= 0) {
        rx_idle_count_return(node);
        return pending;
    }
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
            continue;
        }
        ret = rx_fill(node, order[i], buf, len, ip, port);
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

void* tt_segment_create(const char* path, size_t bytes) {
    // Unlinked first, so a segment left behind by a dead context of this name is replaced rather
    // than inherited. Anyone still holding the old mapping keeps it and sees the old incarnation,
    // which is precisely what lets them notice they are stale.
    (void)unlink(path);
    int segment_fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, SEGMENT_MODE);
    if (segment_fd < 0) {
        TT_LOG_WARNING("Cannot create segment %s: %s", path, strerror(errno));
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

int32_t tt_segment_bell_open(const char* path) {
    // O_WRONLY|O_NONBLOCK fails with ENXIO when nobody holds the read end - an owner that has gone - and with ENOENT
    // for a peer from before the bell existed. Both are answered by ringing over UDP.
    return open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
}

void tt_segment_bell_ring(int32_t bell) {
    const uint8_t one = 1;
    (void)write(bell, &one, sizeof(one)); // EAGAIN: the pipe is full of rings the reader has not drained yet
}

void tt_segment_bell_close(int32_t bell) {
    (void)close(bell);
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
