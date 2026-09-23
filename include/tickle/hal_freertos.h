/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#pragma once

#include <lwip/sockets.h>

// FreeRTOS+lwIP hardware abstraction layer structure - same shape as hal_linux.h's, since
// src/hal_freertos.c mirrors src/hal_linux.c almost line for line (lwIP's LWIP_COMPAT_SOCKETS
// option aliases socket()/bind()/sendto()/... straight onto lwip_socket()/lwip_bind()/... with
// the same signatures as the real POSIX calls).
struct tt_hal {
    // Mirrors hal_linux.h's own sock/data_sock split - see its comments for the measured reason
    // the second socket exists. The hazard is not Linux-specific: it is what happens whenever two
    // nodes share a host and therefore an address, so this target gets the same treatment rather
    // than an exemption it would have to be remembered.
    int sock;
    int data_sock;
    struct sockaddr_in broadcast_addr; // Precomputed once in tt_bind(), reused by every tt_send()
    // Mirrors hal_linux.h's own wake_sock/wake_addr - see its comment. lwIP has
    // LWIP_NETIF_LOOPBACK enabled (platform/freertos/lwipopts.h), so the same loopback-UDP-socket
    // trick works here unchanged, keeping this file's own tt_receive() select()-based rather than
    // needing a different, FreeRTOS-specific wake mechanism.
    int wake_sock;
    struct sockaddr_in wake_addr;
};
