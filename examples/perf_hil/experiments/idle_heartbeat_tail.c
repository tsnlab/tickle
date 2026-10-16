/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// idle_heartbeat_tail.c - the idle HEARTBEAT (config.h's tt_IDLE_HEARTBEAT_PERIOD) on the real Linux HAL: a RELIABLE
// writer sends bursts of BURST samples 1 ms apart, one burst every GAP_MS, over a lossy veth (idle_heartbeat_tail.sh);
// the reader prints, per burst, how long after it was sent the burst's LAST sample was delivered. Before the idle
// HEARTBEAT a lost last sample waited for the next burst; with it, about one period.
//
// Usage: idle_heartbeat_tail pub <bursts> <gap_ms> <idle_ns|default>
//        idle_heartbeat_tail sub <seconds>
// One process per role, each in its own network namespace. Lines: "TAIL burst=<b> delay_us=<us>" (sub), and a final
// "SUB: ..." / "PUB: ..." summary.

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#define BURST 10U
#define DEPTH 64
#define WINDOW_WORDS 1
#define REORDER_SLOTS 64U
#define SETTLE_MS 500U
#define MATCH_WAIT_MS 10U

struct sample {
    uint32_t burst;
    uint32_t index;
    uint64_t sent_ns;
};

static int32_t sample_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(struct sample);
}

static int32_t sample_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    if (len < sizeof(struct sample)) {
        return -1;
    }
    memcpy(payload, data, sizeof(struct sample));
    return (int32_t)sizeof(struct sample);
}

static int32_t sample_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)native;
    if (len < sizeof(struct sample)) {
        return -1;
    }
    memcpy(data, payload, sizeof(struct sample));
    return (int32_t)sizeof(struct sample);
}

static void sample_free(struct tt_Data* data) {
    (void)data;
}

static struct tt_Topic topic = {
    .name = "idle_heartbeat_tail",
    .data_size = sizeof(struct sample),
    .data_encode_size = sample_encode_size,
    .data_encode = sample_encode,
    .data_decode = sample_decode,
    .data_free = sample_free,
};

static uint32_t tails;
static uint32_t delivered;

static void on_sample(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    const struct sample* got = (const struct sample*)data;
    delivered++;
    if (got->index == BURST - 1U) {
        tails++;
        printf("TAIL burst=%u delay_us=%llu\n", got->burst,
               (unsigned long long)((tt_get_ns() - got->sent_ns) / tt_MICROSECOND));
        fflush(stdout);
    }
}

static void poll_for(struct tt_Context* node, uint64_t ns) {
    uint64_t until = tt_get_ns() + ns;
    for (uint64_t now = tt_get_ns(); now < until; now = tt_get_ns()) {
        (void)tt_Context_poll(node, (int64_t)(until - now));
    }
}

static int run_sub(uint32_t seconds) {
    static struct tt_Context node;
    static struct tt_Subscriber sub;
    static uint64_t tracking[tt_MAX_PEER_COUNT * WINDOW_WORDS];
    static uint64_t reorder[REORDER_SLOTS * tt_REORDER_SLOT_SIZE(sizeof(struct sample)) / sizeof(uint64_t)];
    if (tt_Context_create(&node) != tt_RET_OK ||
        tt_Context_create_subscriber(&node, &sub, &topic, "tail", on_sample) != tt_RET_OK) {
        fprintf(stderr, "cannot create the subscriber\n");
        return 1;
    }
    sub.reliable = true;
    sub.tracking_bitmaps = tracking;
    sub.tracking_words = WINDOW_WORDS;
    sub.reorder_storage = reorder;
    sub.reorder_slots = REORDER_SLOTS;
    sub.reorder_slot_bytes = (uint32_t)tt_REORDER_SLOT_SIZE(sizeof(struct sample));
    poll_for(&node, (uint64_t)seconds * tt_SECOND);
    printf("SUB: delivered=%u tails=%u\n", delivered, tails);
    tt_Context_destroy(&node);
    return 0;
}

static int run_pub(uint32_t bursts, uint32_t gap_ms, const char* idle) {
    static struct tt_Context node;
    static struct tt_Publisher pub;
    static struct tt_ReliableCacheIndex index[DEPTH];
    static uint8_t arena[tt_RELIABLE_CACHE_ARENA_BYTES(DEPTH, tt_RELIABLE_RECORD_BYTES(sizeof(struct sample)))];
    static struct tt_ReliableCache cache;
    if (tt_Context_create(&node) != tt_RET_OK ||
        tt_Context_create_publisher(&node, &pub, &topic, "tail") != tt_RET_OK) {
        fprintf(stderr, "cannot create the publisher\n");
        return 1;
    }
    cache.index = index;
    cache.capacity = DEPTH;
    cache.depth = DEPTH;
    cache.arena = arena;
    cache.arena_size = (uint32_t)sizeof(arena);
    pub.reliable_cache = &cache;
    pub.reliable = true;
    if (strcmp(idle, "default") != 0) {
        pub.idle_heartbeat_ns = strtoull(idle, NULL, 10);
    }
    uint64_t waited = 0;
    while (tt_Publisher_matched_count(&pub) == 0 && waited < 10U * tt_SECOND) {
        poll_for(&node, (uint64_t)MATCH_WAIT_MS * tt_MILLISECOND);
        waited += (uint64_t)MATCH_WAIT_MS * tt_MILLISECOND;
    }
    if (tt_Publisher_matched_count(&pub) == 0) {
        printf("PUB: VOID no reader matched in 10 s\n");
        tt_Context_destroy(&node);
        return 1;
    }
    poll_for(&node, (uint64_t)SETTLE_MS * tt_MILLISECOND);
    uint32_t refused = 0;
    for (uint32_t burst = 0; burst < bursts; burst++) {
        for (uint32_t i = 0; i < BURST; i++) {
            struct sample one = {burst, i, tt_get_ns()};
            if (tt_Publisher_publish(&pub, (struct tt_Data*)&one) != tt_RET_OK) {
                refused++;
            }
            poll_for(&node, tt_MILLISECOND);
        }
        poll_for(&node, (uint64_t)gap_ms * tt_MILLISECOND);
    }
    poll_for(&node, 3U * tt_SECOND); // the last burst's tail, whatever repairs it
    printf("PUB: idle_heartbeat_ns=%llu bursts=%u published=%u refused=%u idle_heartbeats=%u retransmitted=%u "
           "tx_datagrams=%llu\n",
           (unsigned long long)pub.idle_heartbeat_ns, bursts, pub.seq_no, refused, pub.idle_heartbeats,
           pub.retransmitted, (unsigned long long)node.tx_datagrams);
    tt_Context_destroy(&node);
    return 0;
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    _tt_CONFIG.broadcast = "192.168.10.255";
    if (argc == 5 && strcmp(argv[1], "pub") == 0) {
        return run_pub((uint32_t)strtoul(argv[2], NULL, 10), (uint32_t)strtoul(argv[3], NULL, 10), argv[4]);
    }
    if (argc == 3 && strcmp(argv[1], "sub") == 0) {
        return run_sub((uint32_t)strtoul(argv[2], NULL, 10));
    }
    fprintf(stderr, "usage: %s pub <bursts> <gap_ms> <idle_ns|default> | sub <seconds>\n", argv[0]);
    return 2;
}
