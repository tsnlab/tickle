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
#include <stdlib.h> // getenv(): TT_UDP_OFFLOAD
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/udp.h> // SOL_UDP, UDP_GRO, UDP_SEGMENT
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

#include <sys/file.h> // flock(): the registry's lock, and the segment owner's
#include <sys/stat.h> // fchmod(), fstat()
#endif

#if tt_CONTEXT_ID_CLAIM
#include <signal.h> // kill()

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

// clock_getres() of the clock tt_get_ns() reads. ppoll() and epoll_pwait2() wait on high-resolution timers, which
// end no more finely than that clock can tell; how much LATER they end (the timer slack, the scheduler) is what the
// context measures on top of this floor.
uint64_t tt_timer_resolution_ns(void) {
    struct timespec res;
    // NOLINTNEXTLINE(misc-include-cleaner) - CLOCK_REALTIME, as tt_get_ns()
    if (clock_getres(CLOCK_REALTIME, &res) != 0) {
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
static bool udp_offload_allowed(void) {
    const char* setting = getenv("TT_UDP_OFFLOAD");
    return setting == NULL || strcmp(setting, "0") != 0;
}

#define TT_UDP_OFFLOAD_GSO 2U // struct tt_Context.udp_offload

// Send offload is decided per send (send_batch_flags()); here only whether the kernel knows UDP_SEGMENT at all. It
// must: a kernel before 4.18 ignores a SOL_UDP cmsg it does not know and would send the whole run as one datagram,
// which IP would then fragment. getsockopt(UDP_SEGMENT) exists from the same release, so it is the check.
static void gso_setup(struct tt_Context* node) {
    node->hal.gso_on = false;
    if (!TT_HAL_UDP_GSO || !udp_offload_allowed()) {
        return; // a build without large samples never sends a run (hal_linux.h TT_HAL_UDP_GSO)
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
#define TT_GRO_PLAIN 0U
#define TT_GRO_ON 1U
#define TT_GRO_SETTLING 2U
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

// A large sample's fragment, by its single-form framing (tt_SingleHeader).
static bool gro_large_fragment(const uint8_t* datagram, int32_t len) {
    return len >= (int32_t)sizeof(struct tt_SingleHeader) &&
           (datagram[0] == tt_SINGLE_MARKER_LE || datagram[0] == tt_SINGLE_MARKER_BE) &&
           (datagram[3] == tt_SUBMESSAGE_TYPE_FRAG_FIRST_L || datagram[3] == tt_SUBMESSAGE_TYPE_FRAG_CONT_L);
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

// rx_read_one() with UDP_GRO: recvmsg() with room for the segment size. A merged read returns its first datagram and
// keeps the rest for gro_take_pending().
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
    ssize_t got = recvmsg(socket_fd, &msg, MSG_DONTWAIT);
    if (got < 0) {
        // NOLINTNEXTLINE(misc-include-cleaner) - EAGAIN/EWOULDBLOCK: glibc-private headers
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
    }
    *ip = ntohl(addr.sin_addr.s_addr);
    *port = ntohs(addr.sin_port);
    int segment = 0;
    // NOLINTNEXTLINE(misc-include-cleaner) - CMSG_*: <sys/socket.h>
    for (struct cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm != NULL; cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level == SOL_UDP && cm->cmsg_type == UDP_GRO) {
            memcpy(&segment, CMSG_DATA(cm), sizeof(segment));
        }
    }
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
    node->hal.gso_on = false; // gso_setup(), once the data socket exists
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
    wait_set_setup(node); // without it tt_receive() builds a ppoll() set per call, as it always did

#if TT_HAL_UDP_GRO
    gro_setup(node);
#endif
    gso_setup(node);

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

// Send offload (UDP offload, above tt_bind()): a run of datagrams gso_run() accepts goes as one message, which the
// kernel cuts up below the socket layer - on the PC in software just before the device, on a NIC with UDP segmentation
// in hardware. One sendmmsg() carries runs and single datagrams alike, so a batch costs the calls it did before.
static int32_t send_batch_flags(struct tt_Context* node, const struct tt_OutDatagram* datagrams, uint32_t count,
                                int flags) {
    uint32_t sent = 0;
    while (sent < count) {
        struct mmsghdr msgs[TT_SEND_BATCH_CHUNK];
        // NOLINTNEXTLINE(misc-include-cleaner) - see <sys/uio.h>'s own include comment
        struct iovec iov[2 * TT_SEND_CALL_DATAGRAMS];
        struct sockaddr_in addrs[TT_SEND_BATCH_CHUNK];
        union gso_control control[TT_SEND_BATCH_CHUNK];
        uint8_t carried[TT_SEND_BATCH_CHUNK]; // datagrams in each message
        uint32_t messages = 0;
        uint32_t placed = 0; // datagrams in this call
        while (messages < TT_SEND_BATCH_CHUNK && sent + placed < count && placed < TT_SEND_CALL_DATAGRAMS) {
            const struct tt_OutDatagram* first = &datagrams[sent + placed];
            uint32_t room = count - sent - placed;
            room = room < TT_SEND_CALL_DATAGRAMS - placed ? room : TT_SEND_CALL_DATAGRAMS - placed;
            uint32_t run = TT_HAL_UDP_GSO && node->hal.gso_on ? gso_run(first, room) : 1U;
            send_message_setup(node, &msgs[messages], &iov[(size_t)2U * placed], &addrs[messages], &control[messages],
                               first, run);
            carried[messages++] = (uint8_t)run;
            placed += run;
        }
        TT_TRACE(tt_TRACE_TX_START);
        int result = sendmmsg(node->hal.data_sock, msgs, messages, flags);
        TT_TRACE(tt_TRACE_TX_DONE);
        if (result <= 0) {
            int err = errno;
            // NOLINTNEXTLINE(misc-include-cleaner) - EAGAIN/EWOULDBLOCK: glibc-private headers
            if (flags != 0 && result < 0 && (err == EAGAIN || err == EWOULDBLOCK)) {
                return (int32_t)sent; // the send buffer is full: what went, went; the caller sends the rest later
            }
            if (result < 0 && carried[0] > 1 && gso_refused(node, err)) {
                continue; // nothing of this call went: again from the same datagram, one at a time
            }
            return -1; // errno says why; a 0 would otherwise loop forever
        }
        // A short count (sendmmsg() stopped at a message that did not go) loops: the next call reports why - the
        // error, or with MSG_DONTWAIT the EAGAIN of a full buffer, answered above.
        sent += gso_count_sent(node, carried, result);
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
#if TT_HAL_UDP_GRO
    if (node->hal.gro_state != TT_GRO_PLAIN) {
        if (node->hal.gro_state == TT_GRO_SETTLING) {
            gro_turn_on(node); // step 2: never read what may be merged without the option on
        }
        int32_t got = gro_read(node, socket_fd, buf, len, ip, port);
        if (got >= 0) {
            bool busy = node->hal.gro_left > 0 || gro_large_fragment((const uint8_t*)buf, got);
            node->hal.gro_quiet = busy ? 0 : node->hal.gro_quiet + 1;
        }
        return got;
    }
#endif
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int32_t ret = (int32_t)recvfrom(socket_fd, buf, len, MSG_DONTWAIT, (struct sockaddr*)&addr, &addr_len);
    if (ret < 0) {
        // NOLINTNEXTLINE(misc-include-cleaner) - EAGAIN/EWOULDBLOCK: glibc-private headers
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
    }
    *ip = ntohl(addr.sin_addr.s_addr);
    *port = ntohs(addr.sin_port);
#if TT_HAL_UDP_GRO
    if (node->hal.gro_allowed && gro_large_fragment((const uint8_t*)buf, ret)) {
        gro_turn_on(node); // the rest of this sample may come merged
    }
#endif
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

// rx_wait() for `timeout` ns, 0 meaning no limit: this function's contract (hal.h) is "0 for no timeout", and a NULL
// timespec is the wait's own way to say it. While receive offload is settling, the wait first runs only to the
// settle's deadline, where gro_settle_decide() is made, and then for what is left of `timeout`.
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
#if TT_HAL_UDP_GRO
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
#endif
    return rx_wait_ns(node, timeout, ready);
}

int32_t tt_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout) {
    node->rx_offset = 0; // at the start of buf unless a merged read hands one out in place
    // What the last batch read comes first, and without a wait: holding it behind ppoll() would delay
    // datagrams that have already arrived.
#if TT_HAL_UDP_GRO
    int32_t merged = gro_take_pending(node, (uint8_t*)buf, len, ip, port);
    if (merged >= 0) {
        return merged;
    }
#endif
    int32_t pending = rx_take_pending(node, buf, len, ip, port);
    if (pending >= 0) {
        return pending;
    }
    node->hal.rx_idle = 0; // a timeout or an interrupt leaves no readiness to go on
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
        int poll_ret = rx_wait_for(node, timeout, &ready);
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
    uint32_t held = node->hal.rx_next < node->hal.rx_count ? (uint32_t)(node->hal.rx_count - node->hal.rx_next) : 0U;
#if TT_HAL_UDP_GRO
    held += node->hal.gro_left; // a merged read's datagrams, handed out without a system call too
#endif
    return held;
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
    node->rx_offset = 0;
#if TT_HAL_UDP_GRO
    gro_settle_decide(node); // a settle past its deadline is decided before anything is read
    int32_t merged = gro_take_pending(node, (uint8_t*)buf, len, ip, port);
    if (merged >= 0) {
        rx_idle_count_return(node);
        return merged;
    }
#endif
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
