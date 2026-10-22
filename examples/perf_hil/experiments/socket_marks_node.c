/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// socket_marks_node - one side of socket_marks_netns.sh (network profiles items D and E): a RELIABLE UInt64 publisher
// whose context sets the traffic classes and SO_TXTIME from its arguments, or the subscriber that measures when each
// sample arrived against the time its publisher asked for.
//
//   pub <broadcast> <count> <interval_us> <offset_us> <txtime 0|1> <pD> <dD> <pR> <dR> <pC> <dC>
//       Publishes <count> samples, one per <interval_us>. Each sample's value is the CLOCK_TAI time it asked to leave
//       at: now + <offset_us>, given to tt_Publisher_publish_at() when <offset_us> > 0, else the time of the plain
//       tt_Publisher_publish(). pX / dX: SO_PRIORITY and DSCP of DATA, REPAIR, CONTROL. Ends, after the context is
//       destroyed (its goodbye is a CONTROL datagram too), with the HAL's own counts:
//       "PUB marks=<tx_marks> prio_cmsg=<0|1> data=<n> repair=<n> control=<n> timed=<n> sent=<n> retransmitted=<n>".
//   sub <broadcast> <seconds>
//       Subscribes for <seconds>; per sample, arrival (CLOCK_TAI, read in the callback) minus the value. Ends with
//       "SUB n=<samples> offset_us min=<> p50=<> p99=<> max=<>".

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/log.h>
#include <tickle/tickle.h>

#include "../../uint64/UInt64.h" // the generated codec by its path: CI's clang-tidy has no -I for it

#define MAX_SAMPLES 100000U
#define NS_PER_US 1000ULL
#define US_FLOAT 1e3
#define PUB_ARGC 13      // "pub" and its 11 arguments
#define SUB_ARGC 4       // "sub" and its 2 arguments
#define TXTIME_ARG 6     // where the txtime switch is
#define CLASS_ARG0 7     // where the classes' priority/DSCP pairs start
#define MATCH_WAIT_S 10U // the publisher waits this long for its subscriber
#define SETTLE_MS 200U   // then this long for the reader's first ACKNACKs
#define TAIL_S 15U       // the publisher's run ends at the latest this long after its last sample is due
#define POLL_MS 10U
#define PERCENTILE_99 99U
#define PERCENT 100U

static int64_t offsets_ns[MAX_SAMPLES];
static uint32_t received = 0;

static uint64_t tai_ns(void) {
    struct timespec now;
    // NOLINTNEXTLINE(misc-include-cleaner) - CLOCK_TAI: <time.h>'s Linux part
    clock_gettime(CLOCK_TAI, &now);
    return ((uint64_t)now.tv_sec * tt_SECOND) + (uint64_t)now.tv_nsec;
}

static void on_sample(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no, struct UInt64Data* data) {
    (void)sub;
    (void)timestamp;
    (void)seq_no;
    uint64_t arrival = tai_ns();
    if (received < MAX_SAMPLES) {
        offsets_ns[received] = (int64_t)(arrival - data->data);
    }
    received++;
}

static int compare(const void* left, const void* right) {
    int64_t lhs = *(const int64_t*)left;
    int64_t rhs = *(const int64_t*)right;
    return (lhs > rhs) - (lhs < rhs);
}

static struct tt_InitOptions options(const char* broadcast) {
    struct tt_InitOptions opts;
    tt_InitOptions_default(&opts);
    opts.net.broadcast = broadcast;
    return opts;
}

static uint32_t count = 0;
static uint32_t sent = 0;
static uint64_t interval_ns = 0;
static uint64_t offset_ns = 0;
static uint64_t match_deadline = 0;
static volatile int done = 0;

static void publish(struct tt_Context* node, uint64_t time, void* param) {
    struct tt_Publisher* pub = param;
    struct UInt64Data data;
    tt_ret_t ret = tt_RET_OK;
    if (offset_ns != 0) {
        data.data = tai_ns() + offset_ns;
        ret = tt_Publisher_publish_at(pub, (struct tt_Data*)&data, data.data);
    } else {
        data.data = tai_ns();
        ret = tt_Publisher_publish(pub, (struct tt_Data*)&data);
    }
    sent += ret == tt_RET_OK ? 1U : 0U;
    if (sent < count) {
        tt_Context_schedule(node, time + interval_ns, publish, pub);
    }
}

static void finish(struct tt_Context* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    done = 1;
}

static void start(struct tt_Context* node, uint64_t time, void* param) {
    struct tt_Publisher* pub = param;
    if (tt_Publisher_matched_count(pub) == 0 && time < match_deadline) {
        tt_Context_schedule(node, time + ((uint64_t)POLL_MS * tt_MILLISECOND), start, pub);
        return;
    }
    tt_Context_schedule(node, time + ((uint64_t)SETTLE_MS * tt_MILLISECOND), publish, pub);
}

static int run_pub(int argc, char** argv) {
    if (argc != PUB_ARGC) {
        fprintf(stderr, "pub needs %d arguments\n", PUB_ARGC - 2);
        return 2;
    }
    struct tt_InitOptions opts = options(argv[2]);
    count = (uint32_t)strtoul(argv[3], NULL, 10);
    interval_ns = strtoull(argv[4], NULL, 10) * NS_PER_US;
    offset_ns = strtoull(argv[5], NULL, 10) * NS_PER_US;
    opts.socket.txtime = atoi(argv[TXTIME_ARG]) != 0 ? tt_SWITCH_ON : tt_SWITCH_OFF;
    for (int cls = 0; cls < (int)tt_TX_CLASS_COUNT; cls++) {
        opts.socket.tx_class[cls].priority = (uint32_t)strtoul(argv[CLASS_ARG0 + (2 * cls)], NULL, 0);
        opts.socket.tx_class[cls].dscp = (uint8_t)strtoul(argv[CLASS_ARG0 + (2 * cls) + 1], NULL, 0);
    }
    static struct tt_Context node;
    if (tt_Context_create_with(&node, &opts) != tt_RET_OK) {
        fprintf(stderr, "PUB cannot create the context\n");
        return 1;
    }
    static struct tt_Publisher pub;
    size_t storage_size = tt_Publisher_storage_size(&UInt64Topic, NULL);
    void* storage = calloc(1, storage_size);
    if (storage == NULL || tt_Node_create_publisher(tt_Context_default_node(&node), &pub, &UInt64Topic, "marks", NULL,
                                                    NULL, storage, storage_size) != tt_RET_OK) {
        fprintf(stderr, "PUB cannot create the publisher\n");
        return 1;
    }
    match_deadline = tt_get_ns() + ((uint64_t)MATCH_WAIT_S * tt_SECOND);
    tt_Context_schedule(&node, tt_get_ns(), start, &pub);
    uint64_t limit = match_deadline + ((uint64_t)TAIL_S * tt_SECOND) + ((uint64_t)count * interval_ns);
    bool finishing = false;
    while (!done && tt_get_ns() < limit) {
        if (sent >= count && !finishing) {
            tt_Context_schedule(&node, tt_get_ns() + tt_SECOND, finish, NULL); // repairs and ACKNACKs end
            finishing = true;
        }
        (void)tt_Context_poll(&node, (int64_t)POLL_MS * (int64_t)tt_MILLISECOND);
    }
    tt_Context_destroy(&node); // first: its goodbye is a CONTROL datagram, counted too (the HAL keeps its counts)
    printf("PUB marks=%u prio_cmsg=%d data=%llu repair=%llu control=%llu timed=%llu sent=%u retransmitted=%u\n",
           (unsigned)node.hal.tx_marks, node.hal.tx_priority_cmsg ? 1 : 0,
           (unsigned long long)node.hal.tx_class_datagrams[tt_TX_CLASS_DATA],
           (unsigned long long)node.hal.tx_class_datagrams[tt_TX_CLASS_REPAIR],
           (unsigned long long)node.hal.tx_class_datagrams[tt_TX_CLASS_CONTROL],
           (unsigned long long)node.hal.tx_timed_datagrams, sent, (unsigned)pub.retransmitted);
    free(storage);
    return 0;
}

static int run_sub(int argc, char** argv) {
    if (argc != SUB_ARGC) {
        fprintf(stderr, "sub needs %d arguments\n", SUB_ARGC - 2);
        return 2;
    }
    struct tt_InitOptions opts = options(argv[2]);
    uint64_t seconds = strtoull(argv[3], NULL, 10);
    static struct tt_Context node;
    if (tt_Context_create_with(&node, &opts) != tt_RET_OK) {
        fprintf(stderr, "SUB cannot create the context\n");
        return 1;
    }
    static struct tt_Subscriber sub;
    size_t storage_size = tt_Subscriber_storage_size(&UInt64Topic, NULL);
    void* storage = calloc(1, storage_size);
    struct tt_SubscriberOptions sub_options = {.callback = (tt_SUBSCRIBER_CALLBACK)on_sample};
    if (storage == NULL || tt_Node_create_subscriber(tt_Context_default_node(&node), &sub, &UInt64Topic, "marks", NULL,
                                                     &sub_options, storage, storage_size) != tt_RET_OK) {
        fprintf(stderr, "SUB cannot create the subscriber\n");
        return 1;
    }
    uint64_t end = tt_get_ns() + (seconds * tt_SECOND);
    while (tt_get_ns() < end) {
        (void)tt_Context_poll(&node, (int64_t)POLL_MS * (int64_t)tt_MILLISECOND);
    }
    uint32_t samples = received < MAX_SAMPLES ? received : MAX_SAMPLES;
    if (samples == 0) {
        printf("SUB n=0\n");
    } else {
        qsort(offsets_ns, samples, sizeof(offsets_ns[0]), compare);
        const uint32_t p50 = samples / 2;
        const uint32_t p99 = (samples * PERCENTILE_99) / PERCENT;
        printf("SUB n=%u offset_us min=%.1f p50=%.1f p99=%.1f max=%.1f\n", samples, (double)offsets_ns[0] / US_FLOAT,
               (double)offsets_ns[p50] / US_FLOAT, (double)offsets_ns[p99] / US_FLOAT,
               (double)offsets_ns[samples - 1] / US_FLOAT);
    }
    tt_Context_destroy(&node);
    free(storage);
    return 0;
}

int main(int argc, char** argv) {
    tt_log_set_level(TT_LOG_WARNING);
    if (argc >= 2 && strcmp(argv[1], "pub") == 0) {
        return run_pub(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "sub") == 0) {
        return run_sub(argc, argv);
    }
    fprintf(stderr, "usage: %s pub|sub ... (see the file's header)\n", argv[0]);
    return 2;
}
