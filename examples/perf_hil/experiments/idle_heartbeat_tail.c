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

static struct tt_TypeSupport topic = {
    .name = "idle_heartbeat_tail",
    .data_size = sizeof(struct sample),
    .data_encode_size = sample_encode_size,
    .data_encode = sample_encode,
    .data_decode = sample_decode,
    .data_free = sample_free,
    .max_encoded_size = sizeof(struct sample),
};

// The reader: RELIABLE with a WINDOW_WORDS-word ACK window and REORDER_SLOTS held samples (each slot sized for one
// sample from max_encoded_size: tt_REORDER_SLOT_SIZE(sizeof(struct sample)), as before); KEEP_ALL, so keep_last_depth
// stays 0 and the segment drain never skips, as the legacy create left it. VOLATILE, AUTOMATIC, infinite durations.
static const struct tt_QosProfile sub_qos = {
    .history = {.kind = tt_QOS_HISTORY_KEEP_ALL, .depth = 0, .bytes = 0},
    .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE,
                    .tracking_words = WINDOW_WORDS,
                    .reorder_slots = REORDER_SLOTS,
                    .max_blocking_ns = 0},
    .durability = {.kind = tt_QOS_DURABILITY_VOLATILE},
    .deadline = {.period_ns = tt_QOS_DURATION_INFINITE},
    .lifespan = {.duration_ns = tt_QOS_DURATION_INFINITE},
    .liveliness = {.kind = tt_QOS_LIVELINESS_AUTOMATIC, .lease_ns = tt_QOS_DURATION_INFINITE},
};

// The writer: RELIABLE KEEP_LAST DEPTH, its arena the (DEPTH + 1) records the hand-built cache had. VOLATILE,
// AUTOMATIC, infinite durations.
static const struct tt_QosProfile pub_qos = {
    .history = {.kind = tt_QOS_HISTORY_KEEP_LAST,
                .depth = DEPTH,
                .bytes = tt_RELIABLE_CACHE_ARENA_BYTES(DEPTH, tt_RELIABLE_RECORD_BYTES(sizeof(struct sample)))},
    .reliability = {.kind = tt_QOS_RELIABILITY_RELIABLE, .tracking_words = 0, .reorder_slots = 0, .max_blocking_ns = 0},
    .durability = {.kind = tt_QOS_DURABILITY_VOLATILE},
    .deadline = {.period_ns = tt_QOS_DURATION_INFINITE},
    .lifespan = {.duration_ns = tt_QOS_DURATION_INFINITE},
    .liveliness = {.kind = tt_QOS_LIVELINESS_AUTOMATIC, .lease_ns = tt_QOS_DURATION_INFINITE},
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

static void poll_for(struct tt_Context* node, uint64_t span_ns) {
    uint64_t until = tt_get_ns() + span_ns;
    for (uint64_t now = tt_get_ns(); now < until; now = tt_get_ns()) {
        (void)tt_Context_poll(node, (int64_t)(until - now));
    }
}

static int run_sub(uint32_t seconds) {
    static struct tt_Context node;
    static struct tt_Subscriber sub;
    static const struct tt_SubscriberOptions options = {.callback = on_sample};
    const size_t storage_size = tt_Subscriber_storage_size(&topic, &sub_qos);
    void* storage = calloc(1, storage_size); // the ACK window, then the reorder slots
    if (storage == NULL || tt_Context_create(&node) != tt_RET_OK ||
        tt_Node_create_subscriber(tt_Context_default_node(&node), &sub, &topic, "tail", &sub_qos, &options, storage,
                                  storage_size) != tt_RET_OK) {
        fprintf(stderr, "cannot create the subscriber\n");
        return 1;
    }
    poll_for(&node, (uint64_t)seconds * tt_SECOND);
    printf("SUB: delivered=%u tails=%u\n", delivered, tails);
    tt_Context_destroy(&node);
    free(storage);
    return 0;
}

static int run_pub(uint32_t bursts, uint32_t gap_ms, const char* idle) {
    static struct tt_Context node;
    static struct tt_Publisher pub;
    // "default": the build's tt_IDLE_HEARTBEAT_PERIOD (options 0); a number: that period, 0 being off, which the
    // options spell tt_QOS_DURATION_INFINITE. The PUB line prints the field, 0 when off, as before.
    struct tt_PublisherOptions options = {.idle_heartbeat_ns = 0};
    if (strcmp(idle, "default") != 0) {
        const uint64_t period_ns = strtoull(idle, NULL, 10);
        options.idle_heartbeat_ns = period_ns == 0 ? tt_QOS_DURATION_INFINITE : period_ns;
    }
    const size_t storage_size = tt_Publisher_storage_size(&topic, &pub_qos);
    void* storage = calloc(1, storage_size); // the cache, its index ring and its arena
    if (storage == NULL || tt_Context_create(&node) != tt_RET_OK ||
        tt_Node_create_publisher(tt_Context_default_node(&node), &pub, &topic, "tail", &pub_qos, &options, storage,
                                 storage_size) != tt_RET_OK) {
        fprintf(stderr, "cannot create the publisher\n");
        return 1;
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
    free(storage);
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
