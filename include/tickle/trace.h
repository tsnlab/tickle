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

// Latency trace stamps (2026-09-26, rmw_tickle/RMW_PERF_PLAN.md H2) - measurement only, compiled out
// entirely unless the library is built with -Dtt_TRACE (rmw_tickle: -DRMW_TICKLE_TRACE=ON). Each stamp is
// a CLOCK_MONOTONIC time, the stamping thread and a point, appended to one process-global ring that any
// thread may write: the index is taken with an atomic add, so stamps from the poll thread and an
// executor thread never share a slot. When the ring wraps the oldest stamps are overwritten.
//
// The points are the receive-to-reply path of a ping-pong responder, in the order one message crosses
// them. Core stamps the HAL's; rmw_tickle stamps its own.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum tt_TracePoint {
    tt_TRACE_RX_WAKE = 1, // ppoll() returned with a socket ready (hal_linux.c)
    tt_TRACE_RX_DATAGRAM, // a datagram handed to core, from a read or from a batch (hal_linux.c)
    tt_TRACE_DELIVER,     // rmw_tickle: core delivered a message to a subscription
    tt_TRACE_SIGNALED,    // rmw_tickle: message queued and the waiting executor signalled
    tt_TRACE_EXEC_WAKE,   // rmw_tickle: rmw_wait() returning with something ready
    tt_TRACE_TAKEN,       // rmw_tickle: rmw_take() returning a message
    tt_TRACE_PUBLISH,     // rmw_tickle: rmw_publish() entered
    tt_TRACE_TX_DONE,     // a send syscall returned (hal_linux.c)
};

#ifdef tt_TRACE

struct tt_TraceStamp {
    uint64_t ns;
    uint64_t thread; // tt_thread_self() of the stamping thread
    uint32_t point;  // enum tt_TracePoint
    uint32_t reserved;
};

#define tt_TRACE_CAPACITY 65536U // stamps kept; a power of two

void tt_trace_stamp(enum tt_TracePoint point);
// Copies up to max stamps, oldest first, into out and returns how many.
uint32_t tt_trace_read(struct tt_TraceStamp* out, uint32_t max);

#define TT_TRACE(point) tt_trace_stamp(point)
#else
#define TT_TRACE(point) ((void)0)
#endif

#ifdef __cplusplus
}
#endif
