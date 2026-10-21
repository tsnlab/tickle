/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The service the rig's service cells call (examples/perf_hil/tickle/service_call/): request and response are both a
// BenchData of the payload shape the build chose (p1 for the campaign cells), encoded by the generated BenchData
// codec, so a call moves exactly the bytes a sample of that shape moves. One definition for the client and the server,
// so the two cannot disagree on the codec or on the retry settings.
//
// The retry settings stay 0 on both sides (core's auto schedule, tt_CALL_RETRY_*), as an application that sets nothing
// gets: the server then keeps an answer for the same seed schedule the client retries on. fb065d65's duplicate came
// from rmw_tickle giving its client an explicit 100 ms x 50 and its server nothing; that mismatch is not this cell's
// subject, and a cell that built it in would measure the mismatch rather than the service path.
//
// What travels in the BenchData of a request (the server reads it, the response echoes it whole):
//   seq        the client's call number k, 1, 2, 3, ... - the request id the server counts executions by. 32 bits,
//              so it does not wrap inside a run, unlike the 16-bit wire seq_no core numbers calls with.
//   send_ns    the client's own clock when it made the call; the round trip is taken on that same clock.
//   payload[0] the client's phase when it made the call (BENCH_SVC_PHASE_*), so the server can count the measured
//              window's calls without a clock of its own on the client's timeline.
//   payload[1] BENCH_SVC_PROBE on a probe call (service_call/client.c: the duplicate counter's own control).

#pragma once

#include <stdint.h>

#include <tickle/tickle.h>

#include "Bench.h"

#define BENCH_SVC_NAME "bench_service"
#define BENCH_SVC_PHASE_WARMUP 0U
#define BENCH_SVC_PHASE_MEASURED 1U
#define BENCH_SVC_PHASE_COOLDOWN 2U
#define BENCH_SVC_PHASE_BYTE 0
#define BENCH_SVC_PROBE_BYTE 1
#define BENCH_SVC_PROBE 1U

_Static_assert(sizeof(((struct BenchData*)0)->payload) > BENCH_SVC_PROBE_BYTE, "the payload carries the call's flags");

static struct tt_Service bench_service = {
    .name = "BenchService",
    .request_size = sizeof(struct BenchData),
    .response_size = sizeof(struct BenchData),
    .request_encode_size = (tt_REQUEST_ENCODE_SIZE)BenchData_encode_size,
    .request_encode = (tt_REQUEST_ENCODE)BenchData_encode,
    .request_decode = (tt_REQUEST_DECODE)BenchData_decode,
    .request_free = (tt_REQUEST_FREE)BenchData_free,
    .response_encode_size = (tt_RESPONSE_ENCODE_SIZE)BenchData_encode_size,
    .response_encode = (tt_RESPONSE_ENCODE)BenchData_encode,
    .response_decode = (tt_RESPONSE_DECODE)BenchData_decode,
    .response_free = (tt_RESPONSE_FREE)BenchData_free,
    .call_retry_interval = 0, // core's auto schedule, on both sides (above)
    .call_retry_count = 0,
};
