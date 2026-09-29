/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// zenoh-pico's best_effort_latency cell. The implementation is shared with its twin; only the transport differs.
#define BENCH_SCENARIO "best_effort_latency"
#define BENCH_RELIABLE 0
#include "../latency_common/server_impl.h"
