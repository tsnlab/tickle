/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The one contract tt_Context.local_scratch's entry in check_context_reset.py's allowlist rests on:
// a durable backlog's fragmented sample, gathered into that buffer, is never handed to a caller
// unless every fragment was there.
//
// Its own binary, because the function under test needs TWO flags that no other test binary has
// together - cached_sample() lives inside `#if tt_LOCAL_DELIVERY` (default 0) and its gather loop
// inside `#if tt_FRAG_ENABLED`, which is compiled in by raising tt_MAX_SAMPLE_LENGTH above
// tt_MAX_BUFFER_LENGTH. The first version of this test sat in test_reliable_pubsub.c, where
// fragmentation is off: it compiled, it "passed", and it never ran a line. Its symbol was not even
// in the binary. That is what a test wrapped in a flag its binary does not set is worth, and it
// took a mutant that would not die to notice.
#define tt_LOCAL_DELIVERY 1
#define tt_MAX_SAMPLE_LENGTH 16000

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: cached_sample() is static, same approach as the other whitebox tests here.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

// Stages one fragment of a sample in the cache: `index`'s header, `bytes` of payload, at `offset`.
// Returns the submessage length it wrote, so the caller can advance.
static uint16_t stage_fragment(struct tt_ReliableCache* cache, uint16_t depth, uint32_t seq_no, uint32_t index,
                               uint8_t frag_count, uint32_t offset, uint32_t bytes, uint8_t fill) {
    uint32_t headers = (uint32_t)(sizeof(struct tt_SubmessageHeader) +
                                  (index == 0 ? sizeof(struct tt_FragFirstHeader) : sizeof(struct tt_FragContHeader)));
    uint8_t* record = cache->arena + offset;
    struct tt_SubmessageHeader* head = (struct tt_SubmessageHeader*)record;
    memset(record, 0, headers + bytes);
    head->type = index == 0 ? tt_SUBMESSAGE_TYPE_FRAG_FIRST : tt_SUBMESSAGE_TYPE_FRAG_CONT;
    head->length = (uint16_t)(headers + bytes);
    if (index == 0) {
        struct tt_FragFirstHeader* first = (struct tt_FragFirstHeader*)(head + 1);
        first->frag_count = frag_count;
        first->data.timestamp = 1234;
    }
    memset(record + headers, fill, bytes);
    struct tt_ReliableCacheIndex* slot = &cache->index[(seq_no - 1) % depth];
    slot->seq_no = seq_no;
    slot->offset = offset;
    slot->len = head->length;
    slot->timestamp = 0;
    return head->length;
}

// The one contract tt_Context.local_scratch's allowlist entry rests on, stated as a thing that can
// fail - and until 2026-09-30 that entry named the WRONG MECHANISM. It said the gap logic guarantees
// an incomplete sample is never delivered. The gap logic has nothing to do with it.
//
// cached_sample() sets *payload = NULL at the top and reaches *payload = out only after copying all
// frag_count fragments contiguously; every one of its early returns is before that assignment, and
// its caller checks for NULL. So a missing fragment leaves the scratch buffer PARTIALLY WRITTEN and
// never read - and that, not the gap logic, is why nobody ever sees uninitialised bytes from it.
//
// An allowlist entry naming the wrong mechanism is worse than one saying "believed": the wrong
// mechanism reads as checked. Someone maintaining the gap logic would have thought they were
// maintaining this invariant and would not have been, and someone changing these early returns would
// not have known they were.
static void test_a_fragmented_sample_with_a_hole_hands_back_no_payload(void) {
    test_mock_reset();
    const uint32_t depth = 4;
    const uint32_t part_bytes = 8;

    // Control first: all three fragments present, so the gather completes and hands back the buffer.
    // Without this arm every assertion below would also hold for a cached_sample() that had stopped
    // returning payloads at all, which is the failure that would make the whole check vacuous.
    {
        TEST_RELIABLE_CACHE(cache, 4);
        uint32_t offset = 0;
        for (uint32_t i = 0; i < 3; i++) {
            offset += stage_fragment(&cache, (uint16_t)depth, 1 + i, i, 3, offset, part_bytes, (uint8_t)(0xA0 + i));
        }
        uint8_t out[tt_MAX_SAMPLE_LENGTH];
        memset(out, 0xEE, sizeof(out));
        const uint8_t* payload = (const uint8_t*)&out[0]; // deliberately non-NULL going in
        uint32_t length = 0;
        uint64_t sent_us = 0;
        uint32_t taken = cached_sample(&cache, (uint16_t)depth, 1, 0, out, &payload, &length, &sent_us);
        EXPECT_EQ_U32(3, taken);
        EXPECT_TRUE(payload == out);
        EXPECT_EQ_U32(3 * part_bytes, length);
        EXPECT_EQ_U32(0xA0, (uint32_t)out[0]);                      // fragment 0's bytes
        EXPECT_EQ_U32(0xA2, (uint32_t)out[(size_t)2 * part_bytes]); // fragment 2's, contiguous behind them
    }

    // The hole: a FRAG_FIRST claiming three fragments with only the first in the cache.
    {
        TEST_RELIABLE_CACHE(cache, 4);
        (void)stage_fragment(&cache, (uint16_t)depth, 1, 0, 3, 0, part_bytes, 0xA0);
        uint8_t out[tt_MAX_SAMPLE_LENGTH];
        memset(out, 0xEE, sizeof(out));
        const uint8_t* payload = (const uint8_t*)&out[0]; // non-NULL going in, so NULL means it was set
        uint32_t length = 0xffffffffU;
        uint64_t sent_us = 0;
        uint32_t taken = cached_sample(&cache, (uint16_t)depth, 1, 0, out, &payload, &length, &sent_us);
        // No payload, whatever else happened. This is the whole contract: the caller's `if (payload
        // != NULL)` is what keeps a partly-gathered buffer from ever being read.
        EXPECT_TRUE(payload == NULL);
        EXPECT_EQ_U32(3, taken); // and it says how many fragments the sample claimed, so the caller can step past
        // The buffer IS partly written - fragment 0 was copied before the hole was found - and that
        // is fine precisely because payload is NULL. Asserted so nobody "fixes" it into a clean
        // buffer and quietly moves the guarantee somewhere it is not.
        EXPECT_EQ_U32(0xA0, (uint32_t)out[0]);
    }
}

int main(void) {
    test_a_fragmented_sample_with_a_hole_hands_back_no_payload();

    printf("test_durable_frag_gather: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
