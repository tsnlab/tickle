/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// shm_netns_collision.c - one context of shm_netns_collision.sh: a BEST_EFFORT publisher or subscriber of one UInt64
// topic, at a given address and context id, run for a given time.
//
// Usage: shm_netns_collision <label> <pub|sub> <addr> <broadcast> <context_id> <tag> <duration_ms>
//
// A publisher sends (tag << 32 | n) every 2 ms. A subscriber counts what it receives as its own namespace's (the
// value's tag is its own) or foreign (any other tag: a sample that crossed from the other namespace). Every context
// prints
//   START label=<l> role=<r> id=<n> port=<data port>
// once created, and last
//   RESULT label=<l> role=<r> id=<n> port=<p> own=<n> foreign=<n> sent=<n> tx_shm=<n> tx_udp=<n> rx_shm=<n>
//          rx_udp=<n> attach=<count per tt_SegmentAttach reason, comma-separated>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "UInt64.h"

#define POLL_NS (1 * tt_MILLISECOND)
#define TAG_SHIFT 32 // a value is (tag << TAG_SHIFT) | n
#define DECIMAL 10
enum { ARG_LABEL = 1, ARG_ROLE, ARG_ADDR, ARG_BROADCAST, ARG_ID, ARG_TAG, ARG_DURATION_MS, ARG_COUNT };
#define PUBLISH_EVERY_NS (2 * tt_MILLISECOND)

static uint64_t own_tag;
static uint64_t own_count;
static uint64_t foreign_count;

static void on_sample(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct UInt64Data* data) {
    (void)sub;
    (void)timestamp;
    (void)seq_no;
    if ((data->data >> TAG_SHIFT) == own_tag) {
        own_count++;
    } else {
        foreign_count++;
    }
}

int main(int argc, char** argv) {
    if (argc != ARG_COUNT) {
        fprintf(stderr, "usage: %s <label> <pub|sub> <addr> <broadcast> <context_id> <tag> <duration_ms>\n", argv[0]);
        return 2;
    }
    const char* label = argv[ARG_LABEL];
    const char* role = argv[ARG_ROLE];
    const bool publisher = strcmp(role, "pub") == 0;
    own_tag = strtoull(argv[ARG_TAG], NULL, DECIMAL);
    const uint64_t duration_ns = strtoull(argv[ARG_DURATION_MS], NULL, DECIMAL) * tt_MILLISECOND;

    struct tt_InitOptions opts;
    tt_InitOptions_default(&opts);
    opts.net.addr = argv[ARG_ADDR];
    opts.net.broadcast = argv[ARG_BROADCAST];
    opts.net.context_id = (int32_t)strtol(argv[ARG_ID], NULL, DECIMAL);

    static struct tt_Context node;
    tt_ret_t ret = tt_Context_create_with(&node, &opts);
    if (ret != tt_RET_OK) {
        printf("RESULT label=%s role=%s create_failed=%d\n", label, role, (int)ret);
        return 1;
    }
    uint32_t own_ip = 0;
    uint16_t own_port = 0;
    tt_own_address(&node, &own_ip, &own_port);
    printf("START label=%s role=%s id=%u port=%u\n", label, role, (unsigned)node.id, (unsigned)own_port);
    fflush(stdout);

    const struct tt_QosProfile qos = {.history = {.kind = tt_QOS_HISTORY_KEEP_ALL},
                                      .reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT}};
    static struct tt_Publisher pub;
    static struct tt_Subscriber sub;
    void* storage = NULL;
    if (publisher) {
        size_t size = tt_Publisher_storage_size(&UInt64Topic, &qos);
        storage = size != 0 ? calloc(1, size) : NULL;
        ret = tt_Node_create_publisher(tt_Context_default_node(&node), &pub, &UInt64Topic, "collision", &qos, NULL,
                                       storage, size);
    } else {
        size_t size = tt_Subscriber_storage_size(&UInt64Topic, &qos);
        storage = size != 0 ? calloc(1, size) : NULL;
        struct tt_SubscriberOptions sub_options = {.callback = (tt_SUBSCRIBER_CALLBACK)on_sample};
        ret = tt_Node_create_subscriber(tt_Context_default_node(&node), &sub, &UInt64Topic, "collision", &qos,
                                        &sub_options, storage, size);
    }
    if (ret != tt_RET_OK) {
        printf("RESULT label=%s role=%s endpoint_failed=%d\n", label, role, (int)ret);
        return 1;
    }

    uint64_t sent = 0;
    const uint64_t start = tt_get_ns();
    uint64_t next_publish = start;
    for (uint64_t now = start; now - start < duration_ns; now = tt_get_ns()) {
        ret = tt_Context_poll(&node, (int64_t)POLL_NS);
        if (ret != tt_RET_OK && ret != tt_RET_TIMEOUT && ret != tt_RET_INTERRUPTED) {
            printf("POLL_ERROR label=%s ret=%d\n", label, (int)ret);
            break;
        }
        if (publisher && now >= next_publish) {
            struct UInt64Data data = {.data = (own_tag << TAG_SHIFT) | (uint32_t)sent};
            if (tt_Publisher_publish(&pub, (struct tt_Data*)&data) == tt_RET_OK) {
                sent++;
            }
            next_publish += PUBLISH_EVERY_NS;
        }
    }

    printf("RESULT label=%s role=%s id=%u port=%u own=%llu foreign=%llu sent=%llu tx_shm=%llu tx_udp=%llu "
           "rx_shm=%llu rx_udp=%llu attach=",
           label, role, (unsigned)node.id, (unsigned)own_port, (unsigned long long)own_count,
           (unsigned long long)foreign_count, (unsigned long long)sent,
           (unsigned long long)node.tx_datagrams_by_transport[tt_TRANSPORT_SHM],
           (unsigned long long)node.tx_datagrams_by_transport[tt_TRANSPORT_UDP],
           (unsigned long long)node.rx_datagrams_by_transport[tt_TRANSPORT_SHM],
           (unsigned long long)node.rx_datagrams_by_transport[tt_TRANSPORT_UDP]);
    for (int reason = 0; reason < tt_SEGMENT_ATTACH_COUNT; reason++) {
        printf("%s%u", reason == 0 ? "" : ",", node.segment_attach[reason]);
    }
    printf("\n");
    fflush(stdout);
    tt_Context_destroy(&node);
    free(storage);
    return 0;
}
