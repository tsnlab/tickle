/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// NOLINTNEXTLINE(misc-include-cleaner) -- picolibc routes EINTR/EAGAIN/EWOULDBLOCK through <sys/errno.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <lwip/netif.h>
#include <lwip/sockets.h>
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "log.h"

struct _tt_Config _tt_CONFIG = {
    .addr = _tt_NODE_ADDRESS,
    .port = _tt_NODE_PORT,
    .broadcast = _tt_NODE_BROADCAST,
    .node_id = tt_NODE_ID_INVALID, // auto-detect by default; see its own comment in config.h
};

// QEMU's `-machine virt` CLINT counts at a fixed 10MHz (RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ) -
// must match FreeRTOSConfig.h's configCPU_CLOCK_HZ, which platform/freertos/FreeRTOSConfig.h
// sets from the same constant (kept separate, rather than including FreeRTOS.h here, so this
// HAL file's only real dependency stays lwIP + raw CLINT registers). There's no RTC on this
// target, so this reads the same free-running hardware counter FreeRTOS's own tick interrupt is
// derived from, rather than tracking wall-clock time the way hal_linux.c's
// clock_gettime(CLOCK_REALTIME) does.
#define CLINT_MTIME ((volatile uint64_t*)0x0200BFF8UL)
#define CLINT_TIMEBASE_HZ 10000000ULL
#define CLINT_NS_PER_TICK (tt_SECOND / CLINT_TIMEBASE_HZ)

uint64_t tt_get_ns(void) {
    return *CLINT_MTIME * CLINT_NS_PER_TICK;
}

int32_t tt_get_node_id(void) {
    // Unlike hal_linux.c (which enumerates every interface with getifaddrs() to find the one on
    // the broadcast network), this target has exactly one netif, brought up statically by
    // net_init.c before tt_Node_create() ever runs - so there's nothing to search.
    if (netif_default == NULL) {
        TT_LOG_ERROR("No default netif - net_init() must run before tt_Node_create()");
        return -1;
    }

    // ip4_addr_get_u32() returns the address in network byte order (like sin_addr.s_addr on
    // Linux); on this little-endian target, shifting that raw value right by 24 bits lands on
    // the last transmitted octet - the same "node ID = last byte of the IP" convention
    // hal_linux.c uses for the (simpler, single-interface) common case.
    uint32_t addr = ip4_addr_get_u32(netif_ip4_addr(netif_default));
    return (int32_t)(addr >> 24);
}

bool tt_resolve_link(const char* broadcast, uint32_t* addr, uint32_t* netmask, uint32_t* bcast) {
    // One netif on this target (see tt_get_node_id()'s own comment), so there is nothing to search:
    // either the configured broadcast is this interface's, or no local interface owns it.
    if (broadcast == NULL || addr == NULL || netmask == NULL || bcast == NULL || netif_default == NULL) {
        return false;
    }
    uint32_t want = ntohl(inet_addr(broadcast));
    // See hal_linux.c: always reported, so a caller can address a link the stack does not own.
    *bcast = want;
    uint32_t if_addr = ntohl(ip4_addr_get_u32(netif_ip4_addr(netif_default)));
    uint32_t if_mask = ntohl(ip4_addr_get_u32(netif_ip4_netmask(netif_default)));
    uint32_t if_bcast = if_addr | ~if_mask;
    if (if_bcast != want) {
        return false;
    }
    *addr = if_addr;
    *netmask = if_mask;
    return true;
}

tt_ret_t tt_bind(struct tt_Node* node) {
    // See hal_linux.c's own tt_bind() comment on this same line - node->hal.sock relies on the
    // identical "only touched after it's known-good" convention.
    node->hal.wake_sock = -1;
    node->hal.data_sock = -1;
    node->hal.rx_prefer_data = false;

    node->hal.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (node->hal.sock < 0) {
        TT_LOG_ERROR("Cannot create UDP socket: %s", strerror(errno));
        return tt_RET_IO_ERROR;
    }

    int optval = 1;
    if (setsockopt(node->hal.sock, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int)) < 0) {
        TT_LOG_ERROR("Cannot set socket reuseaddr: %s", strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    optval = 1;
    if (setsockopt(node->hal.sock, SOL_SOCKET, SO_BROADCAST, (const void*)&optval, sizeof(int)) < 0) {
        TT_LOG_ERROR("Cannot set socket broadcast: %s", strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    // Unlike hal_linux.c, there's no SO_SNDBUF/SO_RCVBUF tuning here: lwIP's socket layer has no
    // SO_SNDBUF at all (UDP send never queues), and SO_RCVBUF support is compiled out by default
    // (LWIP_SO_RCVBUF, off in platform/freertos/lwipopts.h) - its rx buffering is sized instead by
    // lwipopts.h's own compile-time knobs (e.g. the UDP recvmbox size). Not an oversight.
    // The well-known socket always binds the wildcard, never _tt_CONFIG.addr - see hal_linux.c's
    // own comment on this same bind for the measured reason: a socket bound to a unicast address
    // receives no broadcasts, so binding this one would stop every announce arriving while unicast
    // kept working. _tt_CONFIG.addr scopes the data socket instead, below.
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    // inet_addr()/htons() come from lwip/sockets.h's LWIP_COMPAT_SOCKETS aliasing.
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(_tt_CONFIG.port);
    addr.sin_len = sizeof(addr);

    if (bind(node->hal.sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_in)) < 0) {
        TT_LOG_ERROR("Cannot bind socket to 0.0.0.0:%d: %s", _tt_CONFIG.port, strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    // Precompute the broadcast destination once instead of re-parsing _tt_CONFIG.broadcast with
    // inet_addr() on every single tt_send() call - same reasoning as hal_linux.c.
    memset(&node->hal.broadcast_addr, 0, sizeof(node->hal.broadcast_addr));
    node->hal.broadcast_addr.sin_family = AF_INET;
    node->hal.broadcast_addr.sin_addr.s_addr = inet_addr(_tt_CONFIG.broadcast);
    node->hal.broadcast_addr.sin_port = htons(_tt_CONFIG.port);

    // This node's own data port - mirrors hal_linux.c's own data_sock, for the same measured
    // reason (see struct tt_hal.data_sock, hal_linux.h): everything is sent from here so a peer
    // records this node at a port that belongs to it alone, which is what lets a unicast reach
    // this node rather than whichever node on the host the stack happens to pick. Port 0 lets the
    // stack choose, and no SO_REUSEADDR, because a second node sharing this port is exactly the
    // failure being prevented.
    node->hal.data_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (node->hal.data_sock < 0) {
        TT_LOG_ERROR("Cannot create UDP data socket: %d", errno);
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    optval = 1;
    if (setsockopt(node->hal.data_sock, SOL_SOCKET, SO_BROADCAST, (const void*)&optval, sizeof(int)) < 0) {
        TT_LOG_ERROR("Cannot set data socket broadcast: %d", errno);
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    // This one does honour _tt_CONFIG.addr - see hal_linux.c's own comment on the same bind.
    struct sockaddr_in data_addr;
    data_addr.sin_family = AF_INET;
    data_addr.sin_addr.s_addr = inet_addr(_tt_CONFIG.addr);
    data_addr.sin_port = 0; // stack-assigned
    if (bind(node->hal.data_sock, (struct sockaddr*)&data_addr, sizeof(struct sockaddr_in)) < 0) {
        TT_LOG_ERROR("Cannot bind data socket to %s:0: %d", _tt_CONFIG.addr, errno);
        tt_close(node);
        return tt_RET_IO_ERROR;
    }
    node->hal.broadcast_addr.sin_len = sizeof(node->hal.broadcast_addr);

    // See hal_linux.c's own tt_bind() comment - a private loopback socket purely so
    // tt_wake_signal() has something to write to that wakes up a blocked tt_receive().
    // LWIP_NETIF_LOOPBACK is on (platform/freertos/lwipopts.h), so this works the same way here.
    node->hal.wake_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (node->hal.wake_sock < 0) {
        TT_LOG_ERROR("Cannot create wake socket: %s", strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    struct sockaddr_in wake_bind_addr;
    memset(&wake_bind_addr, 0, sizeof(wake_bind_addr));
    wake_bind_addr.sin_family = AF_INET;
    wake_bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    wake_bind_addr.sin_port = 0;
    wake_bind_addr.sin_len = sizeof(wake_bind_addr);

    if (bind(node->hal.wake_sock, (struct sockaddr*)&wake_bind_addr, sizeof(wake_bind_addr)) < 0) {
        TT_LOG_ERROR("Cannot bind wake socket: %s", strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    socklen_t wake_addr_len = sizeof(node->hal.wake_addr);
    if (getsockname(node->hal.wake_sock, (struct sockaddr*)&node->hal.wake_addr, &wake_addr_len) < 0) {
        TT_LOG_ERROR("Cannot get wake socket address: %s", strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    return tt_RET_OK;
}

void tt_close(struct tt_Node* node) {
    if (node->hal.data_sock >= 0 && close(node->hal.data_sock) < 0) {
        TT_LOG_WARNING("Cannot close data socket: %d", errno);
    }
    node->hal.data_sock = -1;

    if (close(node->hal.sock) < 0) {
        TT_LOG_ERROR("Cannot close socket: %s", strerror(errno));
    }
    if (node->hal.wake_sock >= 0 && close(node->hal.wake_sock) < 0) {
        TT_LOG_ERROR("Cannot close wake socket: %s", strerror(errno));
    }
}

int32_t tt_send(struct tt_Node* node, const void* buf, size_t len) {
    return (int32_t)sendto(node->hal.data_sock, buf, len, 0, (struct sockaddr*)&node->hal.broadcast_addr,
                           sizeof(struct sockaddr_in));
}

int32_t tt_send_to(struct tt_Node* node, const void* buf, size_t len, uint32_t ip, uint16_t port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(ip);
    addr.sin_port = htons(port);
    addr.sin_len = sizeof(addr);

    return (int32_t)sendto(node->hal.data_sock, buf, len, 0, (struct sockaddr*)&addr, sizeof(struct sockaddr_in));
}

int32_t tt_send_iov(struct tt_Node* node, const void* hdr, size_t hdr_len, const void* body, size_t body_len,
                    uint32_t ip, uint16_t port) {
    // iovec.iov_base is non-const by POSIX, but sendmsg() only reads it - casting away const here
    // is the standard idiom, not a real int-to-pointer round trip.
    struct iovec iov[2] = {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        {.iov_base = (void*)(uintptr_t)hdr, .iov_len = hdr_len},
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        {.iov_base = (void*)(uintptr_t)body, .iov_len = body_len},
    };

    struct sockaddr_in unicast_addr;
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;
    if (ip != 0) {
        memset(&unicast_addr, 0, sizeof(unicast_addr));
        unicast_addr.sin_family = AF_INET;
        unicast_addr.sin_addr.s_addr = htonl(ip);
        unicast_addr.sin_port = htons(port);
        unicast_addr.sin_len = sizeof(unicast_addr);
        msg.msg_name = &unicast_addr;
        msg.msg_namelen = sizeof(unicast_addr);
    } else {
        msg.msg_name = &node->hal.broadcast_addr;
        msg.msg_namelen = sizeof(node->hal.broadcast_addr);
    }

    return (int32_t)sendmsg(node->hal.data_sock, &msg, 0);
}

int32_t tt_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout) {
    // Which socket this call will read from - see the select() below. Defaults to the well-known
    // one so the non-polling path behaves exactly as it did before data_sock existed.
    int read_fd = node->hal.sock;
    // Same "0 for no timeout" (block until data arrives) contract fix as hal_linux.c's poll()
    // rewrite, using lwIP's select() (LWIP_COMPAT_SOCKETS aliases it the same as the real thing)
    // since lwIP doesn't provide poll().
    if (timeout >= 0) {
        struct timeval wait_time;
        struct timeval* wait_time_ptr;
        if (timeout == 0) {
            wait_time_ptr = NULL; // select()'s own NULL timeout means "block until data arrives"
        } else {
            wait_time.tv_sec = (long)(timeout / tt_SECOND);
            wait_time.tv_usec = (long)((timeout % tt_SECOND) / tt_MICROSECOND);
            wait_time_ptr = &wait_time;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(node->hal.sock, &readfds);
        FD_SET(node->hal.wake_sock, &readfds);
        FD_SET(node->hal.data_sock, &readfds);
        int maxfd = node->hal.sock > node->hal.wake_sock ? node->hal.sock : node->hal.wake_sock;
        if (node->hal.data_sock > maxfd) {
            maxfd = node->hal.data_sock;
        }

        int select_ret = select(maxfd + 1, &readfds, NULL, NULL, wait_time_ptr);
        if (select_ret == 0) {
            return -1; // Timeout
        }
        if (select_ret < 0) {
            // NOLINTNEXTLINE(misc-include-cleaner)
            if (errno == EINTR) {
                return -1; // Treat an interrupted wait like a timeout; the caller just polls again
            }
            return -2; // I/O error
        }
        if (FD_ISSET(node->hal.wake_sock, &readfds)) {
            // tt_wake_signal() - see hal_linux.c's tt_receive() for the reasoning (identical here,
            // just select() instead of poll()).
            uint8_t discard;
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            (void)recvfrom(node->hal.wake_sock, &discard, sizeof(discard), 0, (struct sockaddr*)&from, &from_len);
            return -3; // Interrupted
        }
        // Broadcasts land on the well-known socket, unicast on this node's own. They alternate
        // when both are ready - see hal_linux.c's own comment on why a fixed preference starves
        // the other socket outright rather than merely delaying it.
        bool well_known_ready = FD_ISSET(node->hal.sock, &readfds) != 0;
        bool data_ready = FD_ISSET(node->hal.data_sock, &readfds) != 0;
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
        // NOLINTNEXTLINE(misc-include-cleaner)
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1; // Timeout
        }
        return -2; // I/O error
    }

    return ret;
}

int32_t tt_try_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    // lwIP does NOT honor MSG_DONTWAIT per recvfrom() call the way Linux does - without O_NONBLOCK
    // on the socket it can still block on an empty socket, which hangs drain_rx()'s "keep calling
    // until nothing's left" loop forever. So gate the read on a zero-timeout select() (the same
    // lwIP primitive tt_receive() uses, just non-blocking): only recvfrom() when it reports the
    // socket readable, otherwise report "nothing waiting" immediately.
    // Both sockets, because either can have something waiting: broadcasts land on the well-known
    // one and unicast on this node's own data socket. Draining only one leaves the other's backlog
    // to the next select(), which is the per-packet round trip drain_rx() exists to avoid.
    struct timeval no_wait = {0, 0};
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(node->hal.sock, &readfds);
    FD_SET(node->hal.data_sock, &readfds);
    int maxfd = node->hal.sock > node->hal.data_sock ? node->hal.sock : node->hal.data_sock;

    int select_ret = select(maxfd + 1, &readfds, NULL, NULL, &no_wait);
    if (select_ret == 0) {
        return -1; // Nothing waiting
    }
    if (select_ret < 0) {
        if (errno == EINTR) {
            return -1;
        }
        return -2; // I/O error
    }

    bool well_known_ready = FD_ISSET(node->hal.sock, &readfds) != 0;
    bool data_ready = FD_ISSET(node->hal.data_sock, &readfds) != 0;
    int read_fd = node->hal.sock;
    if (data_ready && (!well_known_ready || node->hal.rx_prefer_data)) {
        read_fd = node->hal.data_sock;
    }
    if (well_known_ready && data_ready) {
        node->hal.rx_prefer_data = !node->hal.rx_prefer_data;
    }

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(struct sockaddr_in);
    node->rx_via_data_port = (read_fd == node->hal.data_sock);
    int32_t ret = (int32_t)recvfrom(read_fd, buf, len, 0, (struct sockaddr*)&addr, &addr_len);

    *ip = ntohl(addr.sin_addr.s_addr);
    *port = ntohs(addr.sin_port);

    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1; // Nothing waiting
        }
        return -2; // I/O error
    }

    return ret;
}

tt_ret_t tt_wake_signal(struct tt_Node* node) {
    uint8_t one = 1;
    // See hal_linux.c's tt_wake_signal() - identical reasoning (a send to our own loopback
    // address never blocks, so this is safe from any task).
    if (sendto(node->hal.wake_sock, &one, sizeof(one), 0, (struct sockaddr*)&node->hal.wake_addr,
               sizeof(node->hal.wake_addr)) < 0) {
        TT_LOG_ERROR("Cannot send wake signal: %s", strerror(errno));
        return tt_RET_IO_ERROR;
    }
    return tt_RET_OK;
}
