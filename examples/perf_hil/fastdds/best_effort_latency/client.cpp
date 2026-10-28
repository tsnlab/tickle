/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// HIL 3-way QoS-matrix comparison, scenario "best_effort_latency" - FastDDS native client/ping role.
// reliable_latency's client with BEST_EFFORT and nothing else changed (../../tickle/common/LatencyReliability.h), so
// the two scenarios share their windowing, pacing and statistics and cannot drift apart.
#define BENCH_LATENCY_BEST_EFFORT 1
#include "../reliable_latency/client.cpp" // NOLINT(bugprone-suspicious-include): one source for both scenarios
