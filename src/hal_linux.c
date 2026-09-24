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
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
// struct iovec (tt_send_iov, below) - clang-tidy's IWYU mapping doesn't know this glibc symbol's
// real (portable, POSIX-specified) home, so it flags both this include and the struct itself as
// if neither provided/needed the other; this is the correct, portable header regardless (see
// readv(2)/writev(2)/sendmsg(2)).
#include <sys/uio.h> // NOLINT(misc-include-cleaner)
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "consts.h"
#include "log.h"

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

tt_ret_t tt_bind(struct tt_Node* node) {
    // Set before anything below can fail into tt_close(): -1 says "nothing to close here yet",
    // the same convention node->hal.sock itself relies on implicitly (every failure that reaches
    // tt_close() below happens after sock was already created successfully).
    node->hal.wake_fd = -1;
    node->hal.data_sock = -1;
    node->hal.rx_prefer_data = false;

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
    return (int32_t)sendto(node->hal.data_sock, buf, len, 0, (struct sockaddr*)&node->hal.broadcast_addr,
                           sizeof(struct sockaddr_in));
}

int32_t tt_send_to(struct tt_Node* node, const void* buf, size_t len, uint32_t ip, uint16_t port) {
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(ip);
    addr.sin_port = htons(port);

    return (int32_t)sendto(node->hal.data_sock, buf, len, 0, (struct sockaddr*)&addr, sizeof(struct sockaddr_in));
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

int32_t tt_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout) {
    // Wait for readability with ppoll() instead of arming SO_RCVTIMEO via setsockopt() before
    // every recvfrom(): the timeout here changes on nearly every call (it tracks whatever
    // scheduled event is due next), and re-arming a socket option that often is pure overhead -
    // ppoll() just takes the timeout as a plain argument, no socket mutation needed. ppoll(), not
    // plain poll(): poll()'s own timeout is a whole millisecond int, which silently rounds any
    // shorter wait *up* to 1ms (a caller asking for e.g. tt_Node_poll()'s own default 100us
    // effectively got throttled to roughly 10x that instead) - found the hard way benchmarking a
    // real publish/subscribe round trip (rmw_tickle/PLAN.md's rmw-perf.yml). ppoll() takes a real
    // struct timespec, so nothing shorter than a millisecond gets rounded at all.
    // Always poll, including for a negative timeout. Negative used to skip the poll and go
    // straight to a blocking recvfrom() on the well-known socket, which blocks exactly as
    // timeout == 0 does but sees neither the data socket nor the wake fd. tt_Node_poll() never
    // takes that path (it normalises a negative timeout to tt_RECEIVE_TIMEOUT first), so nothing
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
        if (data_ready && (!well_known_ready || node->hal.rx_prefer_data)) {
            read_fd = node->hal.data_sock;
        }
        if (well_known_ready && data_ready) {
            node->hal.rx_prefer_data = !node->hal.rx_prefer_data;
        }
    }

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(struct sockaddr_in);
    node->rx_via_data_port = (read_fd == node->hal.data_sock);
    int32_t ret = (int32_t)recvfrom(read_fd, buf, len, 0, (struct sockaddr*)&addr, &addr_len);

    *ip = ntohl(addr.sin_addr.s_addr);
    *port = ntohs(addr.sin_port);

    if (ret < 0) {
        // EAGAIN/EWOULDBLOCK live in glibc-private headers; <errno.h> (included above) is the correct public header.
        // NOLINTNEXTLINE(misc-include-cleaner)
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1; // Timeout
        }
        return -2; // I/O error
    }

    return ret;
}

int32_t tt_try_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(struct sockaddr_in);
    // MSG_DONTWAIT makes just this call non-blocking regardless of the socket's own mode - no
    // poll() first, no socket-option re-arm.
    // Both sockets, because either can have something waiting: broadcasts land on the well-known
    // one and unicast on this node's own data socket. Draining only one of them would leave the
    // other's backlog to the next poll(), which is exactly the per-packet round trip drain_rx()
    // exists to avoid - and a fixed order here would starve the second socket outright while the
    // first has a sustained stream on it, so the same alternation tt_receive() uses applies.
    int first = node->hal.rx_prefer_data ? node->hal.data_sock : node->hal.sock;
    int second = node->hal.rx_prefer_data ? node->hal.sock : node->hal.data_sock;
    node->hal.rx_prefer_data = !node->hal.rx_prefer_data;

    node->rx_via_data_port = (first == node->hal.data_sock);
    int32_t ret = (int32_t)recvfrom(first, buf, len, MSG_DONTWAIT, (struct sockaddr*)&addr, &addr_len);
    if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { // NOLINT(misc-include-cleaner)
        addr_len = sizeof(struct sockaddr_in);
        node->rx_via_data_port = (second == node->hal.data_sock);
        ret = (int32_t)recvfrom(second, buf, len, MSG_DONTWAIT, (struct sockaddr*)&addr, &addr_len);
    }

    *ip = ntohl(addr.sin_addr.s_addr);
    *port = ntohs(addr.sin_port);

    if (ret < 0) {
        // NOLINTNEXTLINE(misc-include-cleaner)
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1; // Nothing waiting
        }
        return -2; // I/O error
    }

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
