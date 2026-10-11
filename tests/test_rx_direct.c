/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Receiving a large sample's fragments in place (DESIGN.md section 8, "Receiving in place"): core's half - the hint it
// gives the HAL after each fragment lands (large_direct_aim()), the check the HAL asks of every fragment it read in
// place (tt_rx_direct_expected()), a fragment found in place landing without a copy, a split datagram reaching the
// fragment path, and the hint withdrawn when its assembly is freed - its buffer kept while a read may be writing it.
// Each condition has a test its mutant in tests/mutants_rx_direct.py turns red. The HAL's half is
// platform/linux/rx_direct_check.sh, end to end against real sockets.
//
// Built as rmw_tickle builds core: a 65507-byte datagram and large samples.

#define tt_MAX_BUFFER_LENGTH 65507
#define tt_LARGE_SAMPLES 1

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: large_direct_aim() is static
#include "test_mock.h"

#define SOURCE 1
#define ENTITY 7
#define SAMPLE_SEQ 100U
#define COUNT 120U

static struct tt_Context node;
static struct tt_Header header;
static uint8_t payload[LARGE_CONT_PAYLOAD];
static uint32_t releases;

static void* test_acquire(void* user, uint32_t bytes) {
    (void)user;
    return malloc(bytes);
}

static void test_release(void* user, void* buffer) {
    (void)user;
    releases++;
    free(buffer);
}

static void setup(void) {
    test_mock_reset();
    for (uint32_t i = 0; i < tt_LARGE_ASSEMBLIES; i++) {
        free(node.large.assemblies[i].buffer);
    }
    memset(&node, 0, sizeof(node));
    node.large.acquire = test_acquire;
    node.large.release = test_release;
    header.source = SOURCE;
    header.magic_value = NATIVE_MAGIC_VALUE;
    releases = 0;
}

static struct tt_LargeAssembly* claim(void) {
    struct tt_LargeAssembly* assembly = large_assembly_claim(&node, NULL, NULL, SOURCE, ENTITY, SAMPLE_SEQ, COUNT);
    EXPECT_TRUE(assembly != NULL);
    return assembly;
}

static uint8_t* place_of(const struct tt_LargeAssembly* assembly, uint32_t index) {
    return assembly->buffer + tt_LARGE_CDR_OFFSET + large_offset(index);
}

// Fragment `index` (a continuation) landed from `from`, which is either a payload elsewhere or its own place.
static int land(struct tt_LargeAssembly* assembly, uint32_t index, const uint8_t* from) {
    uint32_t length = index == COUNT - 1U ? 100U : LARGE_CONT_PAYLOAD;
    struct large_fragment fragment = {
        &header, NULL, from, length, ENTITY, SAMPLE_SEQ + index, index, assembly->frag_count, 0, 0, false};
    return large_place(&node, assembly, &fragment);
}

// A fragment's framing as the wire carries it: the single form, then FRAG_CONT_L's header, native order.
static void framing(uint8_t* out, uint8_t source, uint32_t entity, uint32_t seq_no, uint16_t index, uint16_t count) {
    struct tt_SingleHeader single = {native_single_marker(), tt_VERSION, source, tt_SUBMESSAGE_TYPE_FRAG_CONT_L};
    struct tt_FragContLHeader cont = {entity, seq_no, index, count};
    memcpy(out, &single, sizeof(single));
    memcpy(out + sizeof(single), &cont, sizeof(cont));
}

// After a fragment lands, the hint names the run of unlanded fragments after it: where the first belongs, how many.
static void test_a_landing_aims_at_the_run_after_it(void) {
    setup();
    struct tt_LargeAssembly* assembly = claim();
    EXPECT_EQ_INT(1, land(assembly, 1, payload));
    EXPECT_TRUE(node.rx_direct_dest == place_of(assembly, 2));
    EXPECT_EQ_U64(LARGE_DIRECT_UNITS, node.rx_direct_units);
    EXPECT_EQ_U64(LARGE_DIRECT_HEADER, node.rx_direct_header);
    EXPECT_EQ_U64(LARGE_CONT_PAYLOAD, node.rx_direct_payload);
    // In order, landing in place: the run moves on by one and keeps its length.
    EXPECT_EQ_INT(1, land(assembly, 2, place_of(assembly, 2)));
    EXPECT_TRUE(node.rx_direct_dest == place_of(assembly, 3));
    EXPECT_EQ_U64(LARGE_DIRECT_UNITS, node.rx_direct_units);
}

// The run stops before a fragment already landed: the HAL is never aimed at a place that holds a fragment.
static void test_the_run_stops_before_a_landed_fragment(void) {
    setup();
    struct tt_LargeAssembly* assembly = claim();
    EXPECT_EQ_INT(1, land(assembly, 10, payload));
    EXPECT_EQ_INT(1, land(assembly, 3, payload)); // out of order: the run after 3 ends at 10
    EXPECT_TRUE(node.rx_direct_dest == place_of(assembly, 4));
    EXPECT_EQ_U64(6, node.rx_direct_units);
    // And in order from there, the run keeps stopping before 10: 5..9.
    EXPECT_EQ_INT(1, land(assembly, 4, place_of(assembly, 4)));
    EXPECT_EQ_U64(5, node.rx_direct_units);
}

// Near the end the run is what is left; the last fragment landed leaves no hint.
static void test_the_run_ends_with_the_sample(void) {
    setup();
    struct tt_LargeAssembly* assembly = claim();
    EXPECT_EQ_INT(1, land(assembly, COUNT - 4U, payload));
    EXPECT_EQ_U64(3, node.rx_direct_units);
    EXPECT_EQ_INT(1, land(assembly, COUNT - 1U, payload));
    EXPECT_EQ_U64(0, node.rx_direct_units);
    EXPECT_TRUE(node.rx_direct_dest == NULL);
}

// A fragment found in its own place lands without a copy, and is counted; one elsewhere is copied there.
static void test_a_fragment_in_place_lands_without_a_copy(void) {
    setup();
    struct tt_LargeAssembly* assembly = claim();
    memset(place_of(assembly, 5), 0x3C, LARGE_CONT_PAYLOAD);
    EXPECT_EQ_INT(1, land(assembly, 5, place_of(assembly, 5)));
    EXPECT_EQ_U64(1, node.rx_direct_placed);
    EXPECT_EQ_INT(1, land(assembly, 6, payload));
    EXPECT_EQ_U64(1, node.rx_direct_placed);
    EXPECT_EQ_INT(0, memcmp(place_of(assembly, 6), payload, LARGE_CONT_PAYLOAD));
    EXPECT_EQ_INT(0x3C, place_of(assembly, 5)[LARGE_CONT_PAYLOAD - 1U]);
}

// What the HAL asks of each fragment it read in place: the one the hint expects at that unit, nothing else.
static void test_only_the_expected_fragment_is_expected(void) {
    setup();
    struct tt_LargeAssembly* assembly = claim();
    EXPECT_EQ_INT(1, land(assembly, 1, payload)); // aimed at 2, 3, ...
    uint8_t h[LARGE_DIRECT_HEADER];
    const uint32_t full = LARGE_CONT_PAYLOAD;
    framing(h, SOURCE, ENTITY, SAMPLE_SEQ + 3U, 3, COUNT);
    EXPECT_TRUE(tt_rx_direct_expected(&node, 1, h, full));         // unit 1 is fragment 3
    EXPECT_TRUE(!(tt_rx_direct_expected(&node, 0, h, full)));      // not at unit 0
    EXPECT_TRUE(!(tt_rx_direct_expected(&node, 1, h, full - 1U))); // a short one that is not the last
    framing(h, SOURCE, ENTITY + 1U, SAMPLE_SEQ + 3U, 3, COUNT);
    EXPECT_TRUE(!(tt_rx_direct_expected(&node, 1, h, full))); // another writer's
    framing(h, SOURCE + 1U, ENTITY, SAMPLE_SEQ + 3U, 3, COUNT);
    EXPECT_TRUE(!(tt_rx_direct_expected(&node, 1, h, full))); // another context's
    framing(h, SOURCE, ENTITY, SAMPLE_SEQ + 4U, 3, COUNT);
    EXPECT_TRUE(!(tt_rx_direct_expected(&node, 1, h, full))); // another sample's (its seq_no)
    framing(h, SOURCE, ENTITY, SAMPLE_SEQ + 3U, 3, COUNT + 1U);
    EXPECT_TRUE(!(tt_rx_direct_expected(&node, 1, h, full))); // another count
    framing(h, SOURCE, ENTITY, SAMPLE_SEQ + 3U, 3, COUNT);
    h[3] = tt_SUBMESSAGE_TYPE_FRAG_FIRST_L;
    EXPECT_TRUE(!(tt_rx_direct_expected(&node, 1, h, full))); // another type
    framing(h, SOURCE, ENTITY, SAMPLE_SEQ + 3U, 3, COUNT);
    h[1] = (uint8_t)(tt_VERSION + 1U);
    EXPECT_TRUE(!(tt_rx_direct_expected(&node, 1, h, full))); // another version
    // The full form (a retransmission addressed to us) begins with 'K', which read as a single-form marker says the
    // other byte order: made so that its fields would match in that order, it is refused for its marker alone.
    framing(h, SOURCE, __builtin_bswap32(ENTITY), __builtin_bswap32(SAMPLE_SEQ + 3U), __builtin_bswap16(3),
            __builtin_bswap16(COUNT));
    h[0] = 'K';
    EXPECT_TRUE(!(tt_rx_direct_expected(&node, 1, h, full)));
    // The last fragment may be short, never empty or longer than a full one.
    EXPECT_EQ_INT(1, land(assembly, COUNT - 3U, payload)); // aimed at COUNT - 2, COUNT - 1
    framing(h, SOURCE, ENTITY, SAMPLE_SEQ + COUNT - 1U, (uint16_t)(COUNT - 1U), COUNT);
    EXPECT_TRUE(tt_rx_direct_expected(&node, 1, h, 100));
    EXPECT_TRUE(!(tt_rx_direct_expected(&node, 1, h, 0)));
    EXPECT_TRUE(!(tt_rx_direct_expected(&node, 1, h, full + 1U)));
}

// A fragment that has landed since the read is not expected again (a duplicate read in place would overwrite it).
static void test_a_landed_fragment_is_not_expected(void) {
    setup();
    struct tt_LargeAssembly* assembly = claim();
    EXPECT_EQ_INT(1, land(assembly, 1, payload));
    uint8_t h[LARGE_DIRECT_HEADER];
    framing(h, SOURCE, ENTITY, SAMPLE_SEQ + 4U, 4, COUNT);
    EXPECT_TRUE(tt_rx_direct_expected(&node, 2, h, LARGE_CONT_PAYLOAD));
    assembly->landed[0] |= 1ULL << 4U; // landed meanwhile, the hint not yet moved
    EXPECT_TRUE(!(tt_rx_direct_expected(&node, 2, h, LARGE_CONT_PAYLOAD)));
}

// An assembly freed and its entry claimed again for another sample is not the one the hint was for.
static void test_a_reclaimed_entry_is_not_the_hinted_one(void) {
    setup();
    struct tt_LargeAssembly* assembly = claim();
    EXPECT_EQ_INT(1, land(assembly, 1, payload));
    uint8_t h[LARGE_DIRECT_HEADER];
    framing(h, SOURCE, ENTITY, SAMPLE_SEQ + 2U, 2, COUNT);
    node.large.direct_claimed++; // as if the hint were for an earlier claim of the entry
    EXPECT_TRUE(!(tt_rx_direct_expected(&node, 0, h, LARGE_CONT_PAYLOAD)));
}

// Freeing the hinted assembly withdraws the hint and cancels what the HAL still holds read into it; if a read outside
// the lock may be writing into it, its buffer is kept for the poll thread to release after the read.
static void test_freeing_the_hinted_assembly_withdraws_and_parks(void) {
    setup();
    struct tt_LargeAssembly* assembly = claim();
    EXPECT_EQ_INT(1, land(assembly, 1, payload));
    large_assembly_free(&node, assembly);
    EXPECT_EQ_U64(0, node.rx_direct_units);
    EXPECT_TRUE(node.rx_direct_cancel);
    EXPECT_EQ_U64(1, releases); // no read in flight: released at once

    struct tt_LargeAssembly* again = claim();
    EXPECT_EQ_INT(1, land(again, 1, payload));
    node.rx_direct_cancel = false;
    large_direct_enter(&node); // a read outside the lock is running
    large_assembly_free(&node, again);
    EXPECT_EQ_U64(1, releases); // kept: the read may be writing into it
    EXPECT_TRUE(node.large.parked != NULL);
    large_direct_leave(&node);
    EXPECT_EQ_U64(2, releases); // released once the read is over
    EXPECT_TRUE(node.large.parked == NULL);
}

// Freeing another assembly leaves the hint alone.
static void test_freeing_another_assembly_keeps_the_hint(void) {
    setup();
    struct tt_LargeAssembly* other = large_assembly_claim(&node, NULL, NULL, SOURCE, ENTITY, 900, COUNT);
    struct tt_LargeAssembly* assembly = claim();
    EXPECT_EQ_INT(1, land(assembly, 1, payload));
    large_assembly_free(&node, other);
    EXPECT_EQ_U64(LARGE_DIRECT_UNITS, node.rx_direct_units);
    EXPECT_TRUE(!(node.rx_direct_cancel));
}

static int32_t decode_any(struct tt_Data* data, const uint8_t* bytes, uint32_t len, bool native) {
    (void)data;
    (void)bytes;
    (void)len;
    (void)native;
    return 0;
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static void on_data(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    (void)data;
}

// A datagram handed over as its framing alone, its payload read elsewhere (rx_split_payload), reaches the fragment
// path with that payload; the same framing without one is an empty fragment, refused.
static void test_a_split_datagram_carries_its_payload(void) {
    setup();
    struct tt_Subscriber sub;
    struct tt_Topic topic = {0};
    topic.name = "t";
    topic.data_size = 8;
    topic.data_decode = decode_any;
    topic.data_free = free_nothing;
    node_init_locks(&node);
    node.id = 2;
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = sizeof(node.tx_buffer);
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_subscriber(&node, &sub, &topic, "e", on_data));
    struct tt_LargeAssembly* assembly = claim();
    EXPECT_EQ_INT(1, land(assembly, 1, payload)); // aimed at 2
    uint8_t datagram[LARGE_DIRECT_HEADER];
    framing(datagram, SOURCE, ENTITY, SAMPLE_SEQ + 2U, 2, COUNT);
    memset(place_of(assembly, 2), 0x77, LARGE_CONT_PAYLOAD); // as the HAL's read left it
    uint64_t dropped = node.large.dropped;
    EXPECT_TRUE(process_packet(&node, datagram, 0, sizeof(datagram), 0x0a000001, 8282, tt_TRANSPORT_UDP));
    EXPECT_EQ_U64(dropped + 1U, node.large.dropped); // no payload: refused, nothing landed
    EXPECT_TRUE(!(large_has_landed(assembly, 2)));

    node.rx_split_payload = place_of(assembly, 2);
    node.rx_split_len = LARGE_CONT_PAYLOAD;
    // Best effort: a subscriber with no writer tracked, so the fragment goes to the best-effort assembly.
    EXPECT_TRUE(process_packet(&node, datagram, 0, sizeof(datagram), 0x0a000001, 8282, tt_TRANSPORT_UDP));
    node.rx_split_payload = NULL;
    EXPECT_TRUE(large_has_landed(assembly, 2));
    EXPECT_EQ_U64(1, node.rx_direct_placed); // found in place: landed without a copy
    EXPECT_EQ_INT(0x77, place_of(assembly, 2)[0]);
}

int main(void) {
    memset(payload, 0x5C, sizeof(payload));
    test_a_landing_aims_at_the_run_after_it();
    test_the_run_stops_before_a_landed_fragment();
    test_the_run_ends_with_the_sample();
    test_a_fragment_in_place_lands_without_a_copy();
    test_only_the_expected_fragment_is_expected();
    test_a_landed_fragment_is_not_expected();
    test_a_reclaimed_entry_is_not_the_hinted_one();
    test_freeing_the_hinted_assembly_withdraws_and_parks();
    test_freeing_another_assembly_keeps_the_hint();
    test_a_split_datagram_carries_its_payload();
    if (test_result() != 0) {
        return 1;
    }
    printf("test_rx_direct: all tests passed\n");
    return 0;
}
