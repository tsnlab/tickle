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

#include <netinet/in.h>

// Linux-specific hardware abstraction layer structure
struct tt_hal {
    int sock;
    struct sockaddr_in broadcast_addr; // Precomputed once in tt_bind(), reused by every tt_send()
    // A private loopback UDP socket, bound to an ephemeral port and connected to nothing (its own
    // address, wake_addr, is both endpoints) - tt_receive()'s poll() watches this alongside sock,
    // and tt_wake_signal() sends it one byte to interrupt a blocked receive. -1 before tt_bind()
    // creates it (or if creation fails partway through), so tt_close() knows not to close it.
    int wake_sock;
    struct sockaddr_in wake_addr; // 127.0.0.1:<wake_sock's own ephemeral port>, from getsockname()
};
