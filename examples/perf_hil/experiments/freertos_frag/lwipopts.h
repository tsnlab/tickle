/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The FreeRTOS test tier's lwIP configuration, unchanged, plus lwIP's own statistics (the reassembly
// witnesses: ip_frag.*, memp PBUF_POOL / REASSDATA). Nothing else differs from platform/freertos/lwipopts.h;
// frag_rx.c prints every reassembly-relevant value it was compiled with at boot, so the log carries the
// configuration that was measured.
#pragma once
#include "../../../../platform/freertos/lwipopts.h"
#undef LWIP_STATS
#define LWIP_STATS 1
#define LWIP_STATS_LARGE 1 // 32-bit counters: a cell receives more than 65535 frames

// Diagnostic arm only (make POOL=<k>): a larger pbuf pool, to tell which limit binds the largest datagram.
#ifdef FRAG_POOL_OVERRIDE
#undef PBUF_POOL_SIZE
#define PBUF_POOL_SIZE FRAG_POOL_OVERRIDE
#endif
