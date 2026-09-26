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
#include <tickle/tickle.h>
#include <tickle/trace.h>

#include "consts.h"
#include "log.h"

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
    .addr = _tt_NODE_ADDRESS,
    .port = _tt_NODE_PORT,
    .broadcast = _tt_NODE_BROADCAST,
    .node_id = tt_NODE_ID_INVALID, // auto-detect by default; see its own comment in config.h
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

tt_ret_t tt_bind(struct tt_Node* node) {
    // Set before anything below can fail into tt_close(): -1 says "nothing to close here yet",
    // the same convention node->hal.sock itself relies on implicitly (every failure that reaches
    // tt_close() below happens after sock was already created successfully).
    node->hal.wake_fd = -1;
    node->hal.data_sock = -1;
    node->hal.rx_prefer_data = false;
    node->hal.rx_idle = 0;
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

    // eventfd(2) tt_receive() also polls, purely so tt_wake_signal() has something to write to
    // that wakes it up - see hal_linux.h's own comment on wake_fd for why this (rather than a
    // loopback UDP socket, as hal_freertos.c uses) is what Linux needs specifically.
    node->hal.wake_fd = eventfd(0, EFD_NONBLOCK);
    if (node->hal.wake_fd < 0) {
        TT_LOG_ERROR("Cannot create wake eventfd: %s", strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    return tt_RET_OK;
}

void tt_close(struct tt_Node* node) {
    node->hal.rx_count = 0; // anything a batch still held belonged to the sockets closed below
    node->hal.rx_next = 0;
    if (node->hal.data_sock >= 0 && close(node->hal.data_sock) < 0) {
        TT_LOG_WARNING("Cannot close data socket: %s", strerror(errno));
    }
    node->hal.data_sock = -1;

    if (close(node->hal.sock) < 0) {
        TT_LOG_ERROR("Cannot close socket: %s", strerror(errno));
    }
    if (node->hal.wake_fd >= 0 && close(node->hal.wake_fd) < 0) {
        TT_LOG_ERROR("Cannot close wake eventfd: %s", strerror(errno));
    }
}

int32_t tt_send(struct tt_Node* node, const void* buf, size_t len) {
    int32_t sent = (int32_t)sendto(node->hal.data_sock, buf, len, 0, (struct sockaddr*)&node->hal.broadcast_addr,
                                   sizeof(struct sockaddr_in));
    TT_TRACE(tt_TRACE_TX_DONE);
    return sent;
}

int32_t tt_send_to(struct tt_Node* node, const void* buf, size_t len, uint32_t ip, uint16_t port) {
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(ip);
    addr.sin_port = htons(port);

    int32_t sent =
        (int32_t)sendto(node->hal.data_sock, buf, len, 0, (struct sockaddr*)&addr, sizeof(struct sockaddr_in));
    TT_TRACE(tt_TRACE_TX_DONE);
    return sent;
}

int32_t tt_send_iov(struct tt_Node* node, const void* hdr, size_t hdr_len, const void* body, size_t body_len,
                    uint32_t ip, uint16_t port) {
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

    return (int32_t)sendmsg(node->hal.data_sock, &msg, 0);
}

// Datagrams per sendmmsg() call. A sample's fragments (at most tt_FRAG_MAX_COUNT) always fit one call; a
// longer batch takes several, in order.
#define TT_SEND_BATCH_CHUNK 64

int32_t tt_send_batch(struct tt_Node* node, const struct tt_OutDatagram* datagrams, uint32_t count) {
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
        int result = sendmmsg(node->hal.data_sock, msgs, chunk, 0);
        TT_TRACE(tt_TRACE_TX_DONE);
        if (result <= 0) {
            return -1; // errno says why; a 0 would otherwise loop forever
        }
        sent += (uint32_t)result;
    }
    return (int32_t)count;
}

// Bits of struct tt_hal.rx_idle - see tt_try_receive().
#define TT_RX_IDLE_WELL_KNOWN 1U
#define TT_RX_IDLE_DATA 2U

// Hands out the next datagram the last recvmmsg() read and held back, or -1 when none is waiting.
static int32_t rx_take_pending(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
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

static int32_t rx_fill(struct tt_Node* node, int socket_fd, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    struct tt_hal* hal = &node->hal;
    node->rx_via_data_port = (socket_fd == hal->data_sock);
#if tt_RX_BATCH == 1
    // No batching: recvfrom(), which the control arm measured slightly cheaper than a one-slot recvmmsg().
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
#else
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

int32_t tt_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout) {
    // What the last batch read comes first, and without a wait: holding it behind ppoll() would delay
    // datagrams that have already arrived.
    int32_t pending = rx_take_pending(node, buf, len, ip, port);
    if (pending >= 0) {
        return pending;
    }
    node->hal.rx_idle = 0; // a timeout or an interrupt leaves no readiness to go on
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
    // timeout == 0 does but sees neither the data socket nor the wake fd. tt_Node_poll() never
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

        // struct pollfd/POLLIN/ppoll() live in a glibc-private header; <poll.h> (included above)
        // is the correct public header.
        // NOLINTNEXTLINE(misc-include-cleaner)
        struct pollfd pfd[3] = {
            {.fd = node->hal.sock, .events = POLLIN, .revents = 0},      // NOLINT(misc-include-cleaner)
            {.fd = node->hal.wake_fd, .events = POLLIN, .revents = 0},   // NOLINT(misc-include-cleaner)
            {.fd = node->hal.data_sock, .events = POLLIN, .revents = 0}, // NOLINT(misc-include-cleaner)
        };
        // sigmask=NULL: no signal-mask swap needed, only ppoll()'s own real (not
        // millisecond-rounded) timeout resolution is what's wanted here.
        // NOLINTNEXTLINE(misc-include-cleaner)
        int poll_ret = ppoll(pfd, 3, timeout_ts_ptr, NULL);
        if (poll_ret > 0 && ((pfd[0].revents | pfd[2].revents) & POLLIN) != 0) { // NOLINT(misc-include-cleaner)
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
        if (pfd[1].revents & POLLIN) { // NOLINT(misc-include-cleaner)
            // tt_wake_signal() - drain the counter (its value carries no meaning) and report the
            // interrupt. If the real socket also happens to be ready this same call, it's still
            // readable (poll() is level-triggered) and gets picked up on the very next call - no
            // data loss, just one extra round trip.
            uint64_t discard;
            (void)read(node->hal.wake_fd, &discard, sizeof(discard));
            return -3; // Interrupted
        }
        // Broadcasts arrive on the well-known socket and unicast on this node's own data socket.
        // Whichever is ready gets read; when both are, they alternate. A fixed preference would
        // not merely delay the other socket - under a sustained stream on the preferred one the
        // other is never read at all, and the path that starves would be RELIABLE recovery, whose
        // ACKNACKs come back as unicast while a Publisher above tt_UNICAST_PEER_THRESHOLD is
        // broadcasting its data. Alternating bounds the wait at one datagram either way.
        bool well_known_ready = (pfd[0].revents & POLLIN) != 0; // NOLINT(misc-include-cleaner)
        bool data_ready = (pfd[2].revents & POLLIN) != 0;       // NOLINT(misc-include-cleaner)
        // What the drain after this read may skip: a socket ppoll() did not report ready (tt_try_receive()).
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

    // ppoll() said read_fd is readable, so this reads at least one datagram without waiting - and every
    // other one already queued on that socket, up to tt_RX_BATCH, in the same syscall.
    int32_t ret = rx_fill(node, read_fd, buf, len, ip, port);
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
// found empty, is skipped until the next wait (struct tt_hal.rx_idle). A datagram that lands on a skipped
// socket in the meantime is not lost or delayed past the next poll: readiness is level-triggered, so the
// very next ppoll() reports it. When every socket is idle this answers without a syscall, and clears the
// bits so the next drain - one not preceded by a wait, like a non-blocking poll - asks both again.
int32_t tt_try_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    int32_t pending = rx_take_pending(node, buf, len, ip, port);
    if (pending >= 0) {
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
        return -1;             // Nothing waiting
    }
    TT_TRACE(tt_TRACE_RX_DATAGRAM);
    return ret;
}

tt_ret_t tt_wake_signal(struct tt_Node* node) {
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
