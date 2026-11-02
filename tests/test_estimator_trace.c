/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The estimator trace (-Dtt_ESTIMATOR_TRACE, X0d 2026-11-03; tickle.c's estimator_trace()): built here with the flag
// defined ahead of tickle.c, so this binary is the one build of the unit tests that has it. Readings, each enforced
// below:
//   T1 lines   the first poll prints one obj=wproxy line for the Subscriber's writer proxy and one obj=peer line for
//              the Publisher's reader entry, each with t_ns, ctx and the values set here - the retry interval the one
//              retry_interval_of() gives for them, and in the large build the repair timeout large_order_times() gives.
//   T2 limit   polls 50 ms and 99.999999 ms after it print nothing; the poll at 100 ms prints both lines again, with
//              the later t_ns. A trace printed every poll fails T2; one never printed again fails its second half.
//   T3 output  the lines go to the log's stream (tt_log_set_output()), so an experiment redirecting the log gets them.
// Run again in the large-sample build (LARGE_TEST_BINS), where the sending_* and repair_* fields exist.

#define tt_ESTIMATOR_TRACE 1

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/log.h>
#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: sets the estimators' fields
#include "test_mock.h"

struct sample {
    uint32_t value;
};

static int32_t sample_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(struct sample);
}

static int32_t sample_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < sizeof(struct sample)) {
        return -1;
    }
    memcpy(payload, data, sizeof(struct sample));
    return (int32_t)sizeof(struct sample);
}

static int32_t sample_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool native) {
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
    .name = "estimator_trace",
    .data_size = sizeof(struct sample),
    .data_encode_size = sample_encode_size,
    .data_encode = sample_encode,
    .data_decode = sample_decode,
    .data_free = sample_free,
};

static void on_sample(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)subscriber;
    (void)time;
    (void)seq_no;
    (void)data;
}

static struct tt_Context node;
static struct tt_Publisher pub;
static struct tt_Subscriber sub;
static uint64_t sub_storage[1 << 16]; // 512 KiB, 8-byte aligned

static int count_occurrences(const char* text, const char* needle) {
    int count = 0;
    size_t len = strlen(needle);
    for (const char* at = strstr(text, needle); at != NULL; at = strstr(at + len, needle)) {
        count++;
    }
    return count;
}

// One tt_Context_poll() at mock time `at`, the log captured: what it printed (the caller frees it).
static char* poll_at(uint64_t at) {
    char* text = NULL;
    size_t size = 0;
    FILE* log = open_memstream(&text, &size);
    EXPECT_TRUE(log != NULL);
    tt_log_set_output(log);
    test_mock_now = at;
    (void)tt_Context_poll(&node, 0);
    (void)fclose(log);
    tt_log_set_output(stderr);
    return text != NULL ? text : strdup("");
}

static void test_trace_prints_and_is_rate_limited(void) {
    test_mock_reset();
    test_mock_now = tt_SECOND;
    test_mock_link_resolves = true; // a link that owns the default broadcast, or the create refuses
    test_mock_link_addr = 0x0a000001U;
    test_mock_link_netmask = 0xffffff00U;
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create(&node));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&node), &pub, &topic, "estimator_trace",
                                                      &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
    // The storage a RELIABLE reader asks for in this build (the large-sample build's tracking window needs some).
    size_t sub_storage_size = tt_Subscriber_storage_size_for(&node, &topic, &TEST_QOS_RELIABLE_READER);
    EXPECT_TRUE(sub_storage_size <= sizeof(sub_storage));
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_subscriber(tt_Context_default_node(&node), &sub, &topic, "estimator_trace",
                                                       &TEST_QOS_RELIABLE_READER,
                                                       &(struct tt_SubscriberOptions) {.callback = on_sample},
                                                       sub_storage_size != 0 ? sub_storage : NULL, sub_storage_size));
    // A writer on context 2 as the Subscriber's proxy sees it, and a reader there as the Publisher's entry.
    struct tt_WriterProxy* proxy = &sub.writers[0];
    proxy->context_id = 2;
    proxy->entity_id = 0x1234;
    proxy->recovery_srtt_ns = 3000000;
    proxy->recovery_rttvar_ns = 500000;
    proxy->transit_srtt_ns = 1500000;
    proxy->transit_rttvar_ns = 250000;
    proxy->transit_min_ns = 1000000;
    struct tt_PeerAck* ack = &pub.peer_acks[0];
    ack->context_id = 2;
    ack->entity_id = 0x5678;
#if tt_LARGE_SAMPLES
    proxy->sending_srtt_ns = 7000000;
    proxy->sending_rttvar_ns = 1000000;
    ack->repair_srtt_ns = 1048576; // the timeout: ((1048576 * 2) >> 10) + 1 = 2049 units of 1024 ns
    ack->repair_rttvar_ns = 4096;
#endif
    char expected[96];
    (void)snprintf(expected, sizeof(expected), " retry_interval_ns=%llu ",
                   (unsigned long long)retry_interval_of(reliable_opts(&node), reliable_retry_granularity(&node),
                                                         proxy->recovery_srtt_ns, proxy->recovery_rttvar_ns));

    const uint64_t start = 2 * tt_SECOND;
    char* first = poll_at(start);
    EXPECT_EQ_INT(2, count_occurrences(first, "ESTTRACE "));
    EXPECT_EQ_INT(1, count_occurrences(first, "obj=wproxy "));
    EXPECT_EQ_INT(1, count_occurrences(first, "obj=peer "));
    char stamp[48];
    (void)snprintf(stamp, sizeof(stamp), "ESTTRACE t_ns=%llu ctx=%u ", (unsigned long long)start, (unsigned)node.id);
    EXPECT_EQ_INT(2, count_occurrences(first, stamp));
    EXPECT_EQ_INT(1, count_occurrences(first, " writer_ctx=2 writer=00001234 recovery_srtt_ns=3000000 "
                                              "recovery_rttvar_ns=500000 "));
    EXPECT_EQ_INT(1, count_occurrences(first, expected));
    EXPECT_EQ_INT(1, count_occurrences(first, " transit_srtt_ns=1500000 transit_rttvar_ns=250000 "
                                              "transit_min_ns=1000000 "));
    EXPECT_EQ_INT(1, count_occurrences(first, " reader_ctx=2 reader=00005678 "));
#if tt_LARGE_SAMPLES
    EXPECT_EQ_INT(1, count_occurrences(first, " sending_srtt_ns=7000000 sending_rttvar_ns=1000000 "));
    EXPECT_EQ_INT(1, count_occurrences(first, " repair_srtt_ns=1048576 repair_rttvar_ns=4096 "
                                              "repair_timeout_ns=2098176 "));
#endif
#if tt_HYBRID_TRANSPORT
    // No announce from context 2: no p (code 0, read as 1) and n* 1.
    EXPECT_EQ_INT(1, count_occurrences(first, " hybrid_code=0x0000 hybrid_p=1 n_star=1"));
#endif
    // Every line ends with its newline: a line cut by the buffer would break a parser that reads whole lines.
    EXPECT_EQ_INT(2, count_occurrences(first, "\n"));
    free(first);

    char* soon = poll_at(start + (50 * tt_MILLISECOND));
    EXPECT_EQ_INT(0, count_occurrences(soon, "ESTTRACE "));
    free(soon);
    char* edge = poll_at(start + (100 * tt_MILLISECOND) - 1);
    EXPECT_EQ_INT(0, count_occurrences(edge, "ESTTRACE "));
    free(edge);
    char* next = poll_at(start + (100 * tt_MILLISECOND));
    EXPECT_EQ_INT(2, count_occurrences(next, "ESTTRACE "));
    (void)snprintf(stamp, sizeof(stamp), "ESTTRACE t_ns=%llu ", (unsigned long long)(start + (100 * tt_MILLISECOND)));
    EXPECT_EQ_INT(2, count_occurrences(next, stamp));
    free(next);

    (void)tt_Context_destroy(&node);
}

int main(void) {
    test_trace_prints_and_is_rate_limited();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_estimator_trace: all tests passed\n");
    return 0;
}
