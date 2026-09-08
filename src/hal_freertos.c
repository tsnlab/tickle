/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

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

tt_ret_t tt_bind(struct tt_Node* node) {
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
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    // inet_addr()/htons() come from lwip/sockets.h's LWIP_COMPAT_SOCKETS aliasing.
    addr.sin_addr.s_addr = inet_addr(_tt_CONFIG.addr);
    addr.sin_port = htons(_tt_CONFIG.port);
    addr.sin_len = sizeof(addr);

    if (bind(node->hal.sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_in)) < 0) {
        TT_LOG_ERROR("Cannot bind socket to %s:%d: %s", _tt_CONFIG.addr, _tt_CONFIG.port, strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    // Precompute the broadcast destination once instead of re-parsing _tt_CONFIG.broadcast with
    // inet_addr() on every single tt_send() call - same reasoning as hal_linux.c.
    memset(&node->hal.broadcast_addr, 0, sizeof(node->hal.broadcast_addr));
    node->hal.broadcast_addr.sin_family = AF_INET;
    node->hal.broadcast_addr.sin_addr.s_addr = inet_addr(_tt_CONFIG.broadcast);
    node->hal.broadcast_addr.sin_port = htons(_tt_CONFIG.port);
    node->hal.broadcast_addr.sin_len = sizeof(node->hal.broadcast_addr);

    return tt_RET_OK;
}

void tt_close(struct tt_Node* node) {
    if (close(node->hal.sock) < 0) {
        TT_LOG_ERROR("Cannot close socket: %s", strerror(errno));
    }
}

int32_t tt_send(struct tt_Node* node, const void* buf, size_t len) {
    return (int32_t)sendto(node->hal.sock, buf, len, 0, (struct sockaddr*)&node->hal.broadcast_addr,
                           sizeof(struct sockaddr_in));
}

int32_t tt_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout) {
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

        int select_ret = select(node->hal.sock + 1, &readfds, NULL, NULL, wait_time_ptr);
        if (select_ret == 0) {
            return -1; // Timeout
        }
        if (select_ret < 0) {
            if (errno == EINTR) {
                return -1; // Treat an interrupted wait like a timeout; the caller just polls again
            }
            return -2; // I/O error
        }
    }

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(struct sockaddr_in);
    int32_t ret = (int32_t)recvfrom(node->hal.sock, buf, len, 0, (struct sockaddr*)&addr, &addr_len);

    *ip = ntohl(addr.sin_addr.s_addr);
    *port = ntohs(addr.sin_port);

    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1; // Timeout
        }
        return -2; // I/O error
    }

    return ret;
}
