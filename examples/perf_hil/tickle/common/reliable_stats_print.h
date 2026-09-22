/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Prints include/tickle/reliable_stats.h's counters as grep-able "RSTATS:" key=value lines at the
// end of a run. A no-op unless built with -Dtt_RELIABLE_STATS (build.sh's TICKLE_RELIABLE_STATS=1),
// which must match how libtickle.a itself was built.

#pragma once

#include <stdio.h>

#ifdef tt_RELIABLE_STATS
#include <tickle/reliable_stats.h>

static void print_reliable_hist(const char* role, const char* name, const uint64_t* hist) {
    printf("RSTATS: role=%s hist=%s", role, name);
    for (int i = 0; i < tt_RELIABLE_STATS_HIST_BUCKETS; i++) {
        if (i == 0) {
            printf(" lt1us=%llu", (unsigned long long)hist[i]);
        } else if (i == tt_RELIABLE_STATS_HIST_BUCKETS - 1) {
            printf(" ge%lluus=%llu", 1ULL << (i - 1), (unsigned long long)hist[i]);
        } else {
            printf(" lt%lluus=%llu", 1ULL << i, (unsigned long long)hist[i]);
        }
    }
    printf("\n");
}

static void print_reliable_stats(const char* role) {
    struct tt_ReliableStats s;
    tt_reliable_stats_get(&s);
#define U(x) ((unsigned long long)(x))
    printf("RSTATS: role=%s sub gaps_opened=%llu missing_opened=%llu gaps_opened_while_scheduled=%llu "
           "jump_data=%llu jump_heartbeat=%llu jump_abandoned_seq=%llu retry_giveups=%llu "
           "skip_backlog_calls=%llu skip_backlog_seq=%llu recovered=%llu recovered_after_request=%llu "
           "late_below_ack=%llu duplicates=%llu\n",
           role, U(s.gaps_opened), U(s.missing_opened), U(s.gaps_opened_while_scheduled), U(s.jump_data),
           U(s.jump_heartbeat), U(s.jump_abandoned_seq), U(s.retry_giveups), U(s.skip_backlog_calls),
           U(s.skip_backlog_seq), U(s.recovered), U(s.recovered_after_request), U(s.late_below_ack), U(s.duplicates));
    printf("RSTATS: role=%s acknack_tx immediate=%llu timer=%llu new_gap=%llu sent=%llu bits_sent=%llu\n", role,
           U(s.acknack_immediate), U(s.acknack_timer), U(s.acknack_new_gap), U(s.acknack_sent), U(s.acknack_bits_sent));
    printf("RSTATS: role=%s pub acknack_received=%llu bits_requested=%llu retransmitted=%llu null_evicted=%llu "
           "null_retry_cap=%llu null_lifespan=%llu retransmit_tx_fail=%llu\n",
           role, U(s.acknack_received), U(s.bits_requested), U(s.retransmitted), U(s.null_evicted), U(s.null_retry_cap),
           U(s.null_lifespan), U(s.retransmit_tx_fail));
    printf("RSTATS: role=%s tx datagrams=%llu datagrams_with_data=%llu data_in_datagrams=%llu "
           "avg_data_per_datagram=%.2f max_data_per_datagram=%llu\n",
           role, U(s.datagrams), U(s.datagrams_with_data), U(s.data_in_datagrams),
           s.datagrams_with_data > 0 ? (double)s.data_in_datagrams / (double)s.datagrams_with_data : 0.0,
           U(s.max_data_per_datagram));
#undef U
    print_reliable_hist(role, "detect_to_recover", s.detect_to_recover_hist);
    print_reliable_hist(role, "request_to_recover", s.request_to_recover_hist);
}
#else
static inline void print_reliable_stats(const char* role) {
    (void)role;
}
#endif
