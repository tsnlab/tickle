/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// discovery_interval_netns.c - one context of discovery_interval_netns.sh: created with the given discovery update
// interval on the given broadcast address, polled for the given time, and reporting what it made of its one peer.
//
// Usage: discovery_interval_netns <label> <broadcast> <update_interval_ms> <duration_s>
//
// Watched once per poll (every 20 ms at least): update_seen[] of every other context id. A rise is an arrival, a fall a
// death (presume_node_dead() is the only place it falls during a run). The last line is always
//   RESULT label=<label> interval_ms=<n> own_silence_ms=<n> peer=<id> arrivals=<n> deaths=<n> alive_at_end=<0|1>
//          peer_silence_ms=<n> polled_s=<n>
// peer_silence_ms is what the peer's summaries told this context (tt_Context.peer_silence_ns, 0 = none yet): the
// treatment as realized, printed beside the verdict it drives.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

#define POLL_NS (20 * tt_MILLISECOND) // tt_Context_poll() takes nanoseconds

int main(int argc, char** argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s <label> <broadcast> <update_interval_ms> <duration_s>\n", argv[0]);
        return 2;
    }
    const char* label = argv[1];
    uint64_t interval_ms = strtoull(argv[3], NULL, 10);
    uint64_t duration_s = strtoull(argv[4], NULL, 10);

    struct tt_InitOptions opts;
    tt_InitOptions_default(&opts);
    opts.net.broadcast = argv[2];
    opts.discovery.update_interval_ns = interval_ms * tt_MILLISECOND;

    static struct tt_Context node;
    tt_ret_t ret = tt_Context_create_with(&node, &opts);
    if (ret != tt_RET_OK) {
        printf("RESULT label=%s create_failed=%d\n", label, (int)ret);
        return 1;
    }
    printf("START label=%s id=%u interval_ms=%llu\n", label, (unsigned)node.id, (unsigned long long)interval_ms);
    fflush(stdout);

    static bool seen[tt_MAX_CONTEXT_IDS];
    unsigned arrivals = 0;
    unsigned deaths = 0;
    int peer = -1;
    uint64_t start = tt_get_ns();
    uint64_t end = start + (duration_s * tt_SECOND);
    uint64_t now = start;
    while (now < end) {
        ret = tt_Context_poll(&node, (int64_t)POLL_NS);
        if (ret != tt_RET_OK && ret != tt_RET_TIMEOUT && ret != tt_RET_INTERRUPTED) {
            printf("POLL_ERROR label=%s ret=%d\n", label, (int)ret);
            break;
        }
        now = tt_get_ns();
        for (int i = 0; i < tt_MAX_CONTEXT_IDS; i++) {
            if (i == node.id || node.update_seen[i] == seen[i]) {
                continue;
            }
            seen[i] = node.update_seen[i];
            peer = i;
            if (seen[i]) {
                arrivals++;
            } else {
                deaths++;
            }
            printf("%s label=%s t_ms=%llu peer=%d peer_silence_ms=%llu\n", seen[i] ? "ARRIVAL" : "DEATH", label,
                   (unsigned long long)((now - start) / tt_MILLISECOND), i,
                   (unsigned long long)(node.peer_silence_ns[i] / tt_MILLISECOND));
            fflush(stdout);
        }
    }
    uint64_t own_silence =
        ((((uint64_t)node.opts.discovery.miss_threshold * 2U) + 1U) * node.opts.discovery.update_interval_ns) / 2U;
    printf("RESULT label=%s interval_ms=%llu own_silence_ms=%llu peer=%d arrivals=%u deaths=%u alive_at_end=%d "
           "peer_silence_ms=%llu polled_s=%llu\n",
           label, (unsigned long long)interval_ms, (unsigned long long)(own_silence / tt_MILLISECOND), peer, arrivals,
           deaths, peer >= 0 && seen[peer] ? 1 : 0,
           (unsigned long long)(peer >= 0 ? node.peer_silence_ns[peer] / tt_MILLISECOND : 0),
           (unsigned long long)((now - start) / tt_SECOND));
    fflush(stdout);
    tt_Context_destroy(&node);
    return 0;
}
