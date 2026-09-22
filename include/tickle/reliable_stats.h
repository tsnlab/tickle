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

// RELIABLE recovery instrumentation (experiment/reliable-recovery-instrumentation, 2026-09-22) -
// measurement only, compiled out entirely unless the library itself is built with
// -Dtt_RELIABLE_STATS (and the consumer reading these is built with the same define). Counters are
// process-global, not per-node/per-WriterProxy: every consumer this exists for
// (examples/perf_hil/tickle/reliable_throughput) runs exactly one node with one writer/reader pair,
// and TickLE drives each node from a single thread, so no locking is attempted.

#ifdef tt_RELIABLE_STATS

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Latency histograms: bucket 0 = <1us, bucket i (1..N-2) = [2^(i-1), 2^i) us, last bucket = the rest.
#define tt_RELIABLE_STATS_HIST_BUCKETS 18

struct tt_ReliableStats {
    // --- Subscriber side (update_reliable_ack() and friends) ---
    uint64_t gaps_opened;                 // DATA arrivals that left >=1 new missing seq_no below them
    uint64_t missing_opened;              // seq_nos newly marked missing by those arrivals
    uint64_t gaps_opened_while_scheduled; // H2: ...of which acknack_scheduled was already true (no immediate ACKNACK)
    uint64_t jump_data;               // H1: jump_ack_baseline() from the DATA path (offset >= tt_RELIABLE_BITMAP_BITS)
    uint64_t jump_heartbeat;          // jump_ack_baseline() from the Heartbeat path
    uint64_t jump_abandoned_seq;      // still-missing seq_nos abandoned by those jumps
    uint64_t retry_giveups;           // acknack_retry() give-ups (retry > tt_RELIABLE_RETRY)
    uint64_t heartbeat_advances;      // Phase 1-c: Heartbeats that moved ack_seq_no up to first_available_seq_no
    uint64_t heartbeat_abandoned_seq; // ...still-missing seq_nos those advances skipped (gone at the Publisher)
    uint64_t acknack_immediate;       // ACKNACK send attempts from maybe_arm_acknack_retry()
    uint64_t acknack_timer;           // ACKNACK send attempts from the acknack_retry() timer
    uint64_t acknack_new_gap;         // Phase 1-a: narrow ACKNACK attempts for a gap opened while armed
    uint64_t acknack_sent;            // ...of which actually encoded and flushed
    uint64_t acknack_bits_sent;       // total "please resend" bits across those ACKNACKs
    uint64_t recovered;               // arrivals filling a tracked gap below the highest seen
    uint64_t recovered_after_request; // ...of which the seq_no had been named in an ACKNACK
    uint64_t late_below_ack;          // arrivals with seq_no < ack_seq_no (after a jump/skip/give-up)
    uint64_t duplicates;              // arrivals whose bitmap bit was already set
    uint64_t detect_to_recover_hist[tt_RELIABLE_STATS_HIST_BUCKETS];  // gap detected -> recovered, us
    uint64_t request_to_recover_hist[tt_RELIABLE_STATS_HIST_BUCKETS]; // first ACKNACK naming it -> recovered, us

    // --- Publisher side (process_acknack() / retransmit_reliable_samples()) ---
    uint64_t acknack_received;       // ACKNACKs reaching a reliable Publisher's retransmit loop
    uint64_t bits_requested;         // total set bits across them
    uint64_t retransmitted;          // samples actually re-encoded and flushed
    uint64_t null_evicted;           // not resent: slot empty or overwritten by a newer seq_no
    uint64_t null_retry_cap;         // not resent: cache_entry->retry >= tt_RELIABLE_RETRY
    uint64_t null_lifespan;          // not resent: aged out of lifespan
    uint64_t retransmit_tx_fail;     // not resent: tx buffer full or end_encode() failed
    uint64_t eviction_heartbeats;    // Phase 1-c: FINAL Heartbeats sent back for an ACKNACK naming a gone sample
    uint64_t evicted_by_count;       // B1: samples evicted because the index was full (HISTORY depth)
    uint64_t evicted_by_bytes;       // B1: samples evicted to make contiguous arena room
    uint64_t not_cached_oversize;    // B1: samples larger than the whole arena - sent, never cached
    uint64_t ack_solicit_sent;       // Phase 3 (d): watermark-triggered ACK solicitations sent
    uint64_t ack_solicit_suppressed; // Phase 3 (d): ...and ones the min-gap throttle dropped

    // --- tx path (flush_tx()) ---
    uint64_t datagrams;           // flush_tx() calls that sent something (counted once per call, not per peer)
    uint64_t datagrams_with_data; // ...of which carried >=1 DATA submessage
    uint64_t data_in_datagrams;   // DATA submessages across those
    uint64_t max_data_per_datagram;
};

void tt_reliable_stats_get(struct tt_ReliableStats* out);
void tt_reliable_stats_reset(void);

#ifdef __cplusplus
}
#endif

#endif // tt_RELIABLE_STATS
