/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Held slots are passed over (DESIGN.md section 10, "Held slots"; segment_pass_held() in tickle.c): a slot whose record
// is still in use after it was read carries a held mark, writers pass it over at each lap, the reader moves past the
// indices they passed, and the release frees the slot for the first index at its position no writer has passed.
//
// Every failure here is silent somewhere: a writer that reuses a held slot changes bytes under their holder, a reader
// that does not move past a passed index stalls the ring for every writer, and a release that frees the slot for an
// index already passed leaves the reader waiting for a record nobody will write. So each test checks the held bytes,
// the order and count of what the reader read, and that the ring keeps moving afterwards.
//
// The control (test_without_a_hold_the_ring_is_byte_identical) runs the ring with nothing held against a copy of the
// ring as it was before held slots existed, and compares the whole region after every step.
// tests/mutants_segment_held.sh removes each piece and checks a test here fails.

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_mock.h"

#if !tt_SEGMENT_ENABLED || !tt_SAMPLE_LENDING
int main(void) {
    printf("test_segment_held: nothing to test (segment or lending compiled out)\n");
    return 0;
}
#else

#define SLOTS 8U
#define SLOT_BYTES 64U

// A ring in memory of our own, seeded as create_own_segment() seeds one: each slot free for its own index.
static struct tt_SegmentHeader* ring_make(uint32_t slots) {
    size_t bytes = segment_bytes(slots, SLOT_BYTES);
    size_t rounded = (bytes + 63U) & ~(size_t)63U;
    struct tt_SegmentHeader* header = (struct tt_SegmentHeader*)aligned_alloc(64, rounded);
    memset(header, 0, rounded);
    header->slots = slots;
    header->slot_bytes = SLOT_BYTES;
    for (uint32_t index = 0; index < slots; index++) {
        ((struct tt_SegmentSlot*)segment_slot(header, index))->sequence = index;
    }
    return header;
}

static uint32_t seq_of(struct tt_SegmentHeader* header, uint32_t index) {
    return __atomic_load_n(&((struct tt_SegmentSlot*)segment_slot(header, index))->sequence, __ATOMIC_ACQUIRE);
}

static bool ring_write(struct tt_SegmentHeader* header, uint32_t value) {
    uint8_t body[SLOT_BYTES];
    memset(body, (int)(value & 0xFFU), sizeof(body));
    memcpy(body, &value, sizeof(value));
    return segment_write(header, NULL, 0, body, (uint32_t)sizeof(body), 1, 2, 1);
}

// The drain's reading of the head, without a context: the record in place, or past a passed index, or nothing.
static bool ring_take(struct tt_SegmentHeader* header, uint32_t* value, uint32_t* index, uint8_t** record) {
    for (;;) {
        uint32_t len = 0;
        uint32_t ip = 0;
        uint16_t port = 0;
        uint16_t span = 0;
        if (segment_take(header, record, index, &len, &ip, &port, &span)) {
            memcpy(value, *record, sizeof(*value));
            return true;
        }
        uint32_t read_index = __atomic_load_n(&header->read_index, __ATOMIC_RELAXED);
        if (__atomic_load_n(&header->write_index, __ATOMIC_ACQUIRE) == read_index ||
            !segment_pass_head(header, read_index, seq_of(header, read_index))) {
            return false;
        }
    }
}

// segment_done()'s release of a record nobody kept.
static void ring_done(struct tt_SegmentHeader* header, uint32_t index) {
    __atomic_store_n(&((struct tt_SegmentSlot*)segment_slot(header, index))->sequence, index + header->slots,
                     __ATOMIC_RELEASE);
}

static bool record_is(const uint8_t* record, uint32_t value) {
    uint32_t got = 0;
    memcpy(&got, record, sizeof(got));
    if (got != value) {
        return false;
    }
    for (uint32_t k = sizeof(value); k < SLOT_BYTES; k++) {
        if (record[k] != (uint8_t)(value & 0xFFU)) {
            return false;
        }
    }
    return true;
}

// ---- The control: the ring as it was before held slots, copied here (main at 04c71310).

static struct tt_SegmentSlot* ref_claim(struct tt_SegmentHeader* header, uint32_t* claimed_out) {
    uint32_t claimed = __atomic_load_n(&header->write_index, __ATOMIC_RELAXED);
    struct tt_SegmentSlot* slot_header = NULL;
    for (;;) {
        uint32_t read_index = __atomic_load_n(&header->read_index, __ATOMIC_ACQUIRE);
        if (claimed - read_index >= header->slots) {
            return NULL;
        }
        slot_header = (struct tt_SegmentSlot*)segment_slot(header, claimed);
        if (__atomic_load_n(&slot_header->sequence, __ATOMIC_ACQUIRE) != claimed) {
            return NULL;
        }
        if (__atomic_compare_exchange_n(&header->write_index, &claimed, claimed + 1U, true, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED)) {
            break;
        }
    }
    *claimed_out = claimed;
    return slot_header;
}

static bool ref_write(struct tt_SegmentHeader* header, uint32_t value) {
    uint32_t claimed = 0;
    struct tt_SegmentSlot* slot_header = ref_claim(header, &claimed);
    if (slot_header == NULL) {
        return false;
    }
    uint8_t* payload = segment_slot_payload(slot_header);
    memset(payload, (int)(value & 0xFFU), SLOT_BYTES);
    memcpy(payload, &value, sizeof(value));
    segment_publish(slot_header, claimed, SLOT_BYTES, 1, 2, 1);
    return true;
}

static bool ref_take(struct tt_SegmentHeader* header, uint32_t* index) {
    uint32_t read_index = __atomic_load_n(&header->read_index, __ATOMIC_RELAXED);
    struct tt_SegmentSlot* slot_header = (struct tt_SegmentSlot*)segment_slot(header, read_index);
    if (__atomic_load_n(&slot_header->sequence, __ATOMIC_ACQUIRE) != read_index + 1U) {
        return false;
    }
    *index = read_index;
    __atomic_store_n(&header->read_index, read_index + 1U, __ATOMIC_RELEASE);
    return true;
}

// One step of the control on both rings: `burst` writes, or `burst` reads each released as read. Mismatches counted.
struct twin_run {
    struct tt_SegmentHeader* ring;
    struct tt_SegmentHeader* ref;
    uint32_t value;
    uint32_t refusals;
    uint32_t mismatches;
};
static void twin_writes(struct twin_run* run, uint32_t burst) {
    for (uint32_t k = 0; k < burst; k++, run->value++) {
        bool a = ring_write(run->ring, run->value);
        bool b = ref_write(run->ref, run->value);
        run->mismatches += a != b ? 1U : 0U;
        run->refusals += a ? 0U : 1U;
    }
}
static void twin_reads(struct twin_run* run, uint32_t burst) {
    for (uint32_t k = 0; k < burst; k++) {
        uint32_t got = 0;
        uint32_t index_a = 0;
        uint32_t index_b = 0;
        uint8_t* record = NULL;
        bool a = ring_take(run->ring, &got, &index_a, &record);
        bool b = ref_take(run->ref, &index_b);
        run->mismatches += (a != b || (a && index_a != index_b)) ? 1U : 0U;
        if (a) {
            ring_done(run->ring, index_a);
        }
        if (b) {
            ring_done(run->ref, index_b);
        }
    }
}

// With nothing held the ring does exactly what it did: the same refusals, the same indices and every byte of the
// region the same, after each step of a pseudo-random run of writes and reads that fills, empties and wraps it.
static void test_without_a_hold_the_ring_is_byte_identical(void) {
    struct twin_run run = {ring_make(SLOTS), ring_make(SLOTS), 0, 0, 0};
    struct tt_SegmentHeader* ring = run.ring;
    struct tt_SegmentHeader* ref = run.ref;
    const size_t bytes = segment_bytes(SLOTS, SLOT_BYTES);
    uint32_t state = 12345U;
    for (uint32_t step = 0; step < 20000; step++) {
        state = (state * 1103515245U) + 12345U;
        uint32_t burst = (state >> 16) % (SLOTS + 3U);
        if ((state >> 8) & 1U) {
            twin_writes(&run, burst);
        } else {
            twin_reads(&run, burst);
        }
        run.mismatches += memcmp(ring, ref, bytes) != 0 ? 1U : 0U;
    }
    const uint32_t mismatches = run.mismatches;
    const uint32_t refusals = run.refusals;
    EXPECT_EQ_U32(0, mismatches);
    EXPECT_TRUE(refusals > 1000U); // the run did fill the ring, many times
    EXPECT_TRUE(ring->write_index > 10U * SLOTS);
    free(ring);
    free(ref);
}

// ---- Held slots.

// `held` records kept (at ring positions spread over the ring), then three laps written and read one at a time: every
// record read in order, none refused, the held bytes untouched, each held slot passed once a lap; then every hold
// released and the ring runs on with every slot in use again.
static void wrap_with_held(uint32_t held) {
    struct tt_SegmentHeader* ring = ring_make(SLOTS);
    uint32_t value = 0;
    uint32_t kept_index[SLOTS];
    uint32_t kept_value[SLOTS];
    uint8_t* kept_record[SLOTS];
    uint32_t kept = 0;
    uint32_t refused = 0;
    uint32_t out_of_order = 0;
    uint32_t expect = 0;
    // The first lap: every (SLOTS / held)-th record kept, the rest released as read.
    for (uint32_t k = 0; k < SLOTS; k++) {
        EXPECT_TRUE(ring_write(ring, value++));
        uint32_t got = 0;
        uint32_t index = 0;
        uint8_t* record = NULL;
        EXPECT_TRUE(ring_take(ring, &got, &index, &record));
        out_of_order += got != expect++ ? 1U : 0U;
        if (((k * 3U) % SLOTS) < held) { // 3 is prime to SLOTS: exactly `held` records, spread over the ring
            segment_hold(ring, index);
            kept_index[kept] = index;
            kept_value[kept] = got;
            kept_record[kept] = record;
            kept++;
        } else {
            ring_done(ring, index);
        }
    }
    EXPECT_EQ_U32(held, kept);
    const uint32_t passed_before = ring->write_index;
    for (uint32_t k = 0; k < 3U * SLOTS; k++) {
        if (!ring_write(ring, value)) {
            refused++;
            continue;
        }
        value++;
        uint32_t got = 0;
        uint32_t index = 0;
        uint8_t* record = NULL;
        EXPECT_TRUE(ring_take(ring, &got, &index, &record));
        out_of_order += got != expect++ ? 1U : 0U;
        EXPECT_TRUE(record_is(record, got));
        for (uint32_t h = 0; h < kept; h++) {
            EXPECT_TRUE(index % SLOTS != kept_index[h] % SLOTS); // never a held slot
        }
        ring_done(ring, index);
    }
    EXPECT_EQ_U32(0, refused);
    EXPECT_EQ_U32(0, out_of_order);
    // write_index moved by the records and by one pass at every index of a held slot's position, nothing else.
    uint32_t passes = 0;
    for (uint32_t i = passed_before; i != ring->write_index; i++) {
        for (uint32_t h = 0; h < kept; h++) {
            passes += (i % SLOTS) == (kept_index[h] % SLOTS) ? 1U : 0U;
        }
    }
    EXPECT_EQ_U32((3U * SLOTS) + passes, ring->write_index - passed_before);
    EXPECT_TRUE(passes >= 3U * held - held);
    EXPECT_EQ_U32(ring->write_index, ring->read_index);
    for (uint32_t h = 0; h < kept; h++) {
        EXPECT_TRUE(record_is(kept_record[h], kept_value[h]));
        uint32_t sequence = seq_of(ring, kept_index[h]);
        EXPECT_EQ_U32(2U, (sequence - kept_index[h]) & (SLOTS - 1U)); // still marked
        segment_unhold(ring, kept_index[h]);
    }
    // Released: one more lap and a half is written ahead of the reader, which needs every slot free again.
    uint32_t written = 0;
    while (ring_write(ring, value + written)) {
        written++;
    }
    EXPECT_EQ_U32(SLOTS, written);
    for (uint32_t k = 0; k < written; k++) {
        uint32_t got = 0;
        uint32_t index = 0;
        uint8_t* record = NULL;
        EXPECT_TRUE(ring_take(ring, &got, &index, &record));
        EXPECT_EQ_U32(value + k, got);
        ring_done(ring, index);
    }
    free(ring);
}

static void test_wrap_with_one_to_all_but_one_held(void) {
    for (uint32_t held = 1; held < SLOTS; held++) {
        int before = test_failures;
        wrap_with_held(held);
        if (test_failures != before) {
            printf("  ... with %u held\n", (unsigned)held);
        }
    }
}

// Every slot held: a writer is refused - after passing each held slot once, never spinning - and the reader moves
// past what it passed. Releasing one slot lets exactly one record in.
static void test_all_slots_held_refuses_and_does_not_spin(void) {
    struct tt_SegmentHeader* ring = ring_make(SLOTS);
    uint32_t indices[SLOTS];
    for (uint32_t k = 0; k < SLOTS; k++) {
        EXPECT_TRUE(ring_write(ring, k));
        uint32_t got = 0;
        uint8_t* record = NULL;
        EXPECT_TRUE(ring_take(ring, &got, &indices[k], &record));
        segment_hold(ring, indices[k]);
    }
    EXPECT_TRUE(!ring_write(ring, 100));
    EXPECT_EQ_U32(2U * SLOTS, ring->write_index); // each held slot passed once, then full by the indices
    uint32_t got = 0;
    uint32_t index = 0;
    uint8_t* record = NULL;
    EXPECT_TRUE(!ring_take(ring, &got, &index, &record)); // nothing to read; the passes are taken on the way
    EXPECT_EQ_U32(2U * SLOTS, ring->read_index);
    EXPECT_TRUE(!ring_write(ring, 101)); // still all held: passed again, refused again
    EXPECT_EQ_U32(3U * SLOTS, ring->write_index);
    EXPECT_TRUE(!ring_take(ring, &got, &index, &record));
    segment_unhold(ring, indices[3]);
    EXPECT_TRUE(ring_write(ring, 102));
    EXPECT_TRUE(!ring_write(ring, 103));
    EXPECT_TRUE(ring_take(ring, &got, &index, &record));
    EXPECT_EQ_U32(102, got);
    EXPECT_EQ_U32(3U, index % SLOTS);
    ring_done(ring, index);
    for (uint32_t k = 0; k < SLOTS; k++) {
        if (k != 3) {
            uint32_t held_value = 0;
            memcpy(&held_value, segment_slot_payload((struct tt_SegmentSlot*)segment_slot(ring, indices[k])),
                   sizeof(held_value));
            EXPECT_EQ_U32(k, held_value);
        }
    }
    free(ring);
}

// A slot passed over and released before the reader came to the index it was passed at reads index + slots there,
// and the reader moves past it rather than waiting for a record nobody will write.
static void test_a_release_between_the_pass_and_the_reader(void) {
    struct tt_SegmentHeader* ring = ring_make(SLOTS);
    uint32_t got = 0;
    uint32_t index = 0;
    uint8_t* record = NULL;
    EXPECT_TRUE(ring_write(ring, 0));
    EXPECT_TRUE(ring_take(ring, &got, &index, &record));
    segment_hold(ring, index); // slot 0 held at index 0
    for (uint32_t k = 1; k < SLOTS; k++) {
        EXPECT_TRUE(ring_write(ring, k)); // 1..7, unread
    }
    EXPECT_TRUE(!ring_write(ring, SLOTS)); // index 8 passed (slot 0 held), then full by the indices
    EXPECT_EQ_U32(SLOTS + 2U, seq_of(ring, 0));
    EXPECT_EQ_U32(SLOTS + 1U, ring->write_index);
    segment_unhold(ring, 0);
    EXPECT_EQ_U32(2U * SLOTS, seq_of(ring, 0)); // free for index 16: index 8 stays passed
    for (uint32_t k = 1; k < SLOTS; k++) {
        EXPECT_TRUE(ring_take(ring, &got, &index, &record));
        EXPECT_EQ_U32(k, got);
        ring_done(ring, index);
    }
    EXPECT_TRUE(ring_write(ring, 50)); // index 9
    EXPECT_TRUE(ring_take(ring, &got, &index, &record));
    EXPECT_EQ_U32(50, got);
    EXPECT_EQ_U32(SLOTS + 1U, index);
    ring_done(ring, index);
    for (uint32_t k = 0; k < 2U * SLOTS; k++) { // and on round, slot 0 in use again
        EXPECT_TRUE(ring_write(ring, 60 + k));
        EXPECT_TRUE(ring_take(ring, &got, &index, &record));
        EXPECT_EQ_U32(60 + k, got);
        ring_done(ring, index);
    }
    free(ring);
}

// A writer that marked an index passed and stopped before moving write_index: the reader, or the next writer, moves it
// for it - neither waits on it.
static void test_a_pass_left_half_done_is_finished_by_others(void) {
    struct tt_SegmentHeader* ring = ring_make(SLOTS);
    uint32_t got = 0;
    uint32_t index = 0;
    uint8_t* record = NULL;
    for (uint32_t k = 0; k < SLOTS; k++) {
        EXPECT_TRUE(ring_write(ring, k));
        EXPECT_TRUE(ring_take(ring, &got, &index, &record));
        if (k == 0) {
            segment_hold(ring, index);
        } else {
            ring_done(ring, index);
        }
    }
    // The half-done pass: slot 0 marked passed at index 8, write_index still 8.
    ((struct tt_SegmentSlot*)segment_slot(ring, 0))->sequence = SLOTS + 2U;
    EXPECT_TRUE(!ring_take(ring, &got, &index, &record)); // write_index == read_index: nothing outstanding yet
    EXPECT_TRUE(segment_pass_head(ring, SLOTS, seq_of(ring, SLOTS))); // the reader asked directly finishes it
    EXPECT_EQ_U32(SLOTS + 1U, ring->write_index);
    EXPECT_EQ_U32(SLOTS + 1U, ring->read_index);
    // The writer's side: a second slot held and marked passed at its next index, write_index left behind.
    EXPECT_TRUE(ring_write(ring, 9)); // index 9
    EXPECT_TRUE(ring_take(ring, &got, &index, &record));
    segment_hold(ring, index); // slot 1 held at 9
    for (uint32_t k = 10; k < 2U * SLOTS; k++) {
        EXPECT_TRUE(ring_write(ring, k));
        EXPECT_TRUE(ring_take(ring, &got, &index, &record));
        ring_done(ring, index);
    }
    EXPECT_EQ_U32(2U * SLOTS, ring->write_index);
    // A writer marked index 16 passed (slot 0 still held) and stopped there, write_index left at 16.
    ((struct tt_SegmentSlot*)segment_slot(ring, 2U * SLOTS))->sequence = (2U * SLOTS) + 2U;
    EXPECT_TRUE(ring_write(ring, 18)); // moves write_index past 16, passes 17 (slot 1 held) and claims 18
    EXPECT_EQ_U32((2U * SLOTS) + 3U, ring->write_index);
    EXPECT_EQ_U32((2U * SLOTS) + 3U, seq_of(ring, 1)); // slot 1 passed at 17
    EXPECT_TRUE(ring_take(ring, &got, &index, &record));
    EXPECT_EQ_U32(18, got);
    EXPECT_EQ_U32((2U * SLOTS) + 2U, index);
    free(ring);
}

// A ring of fewer than 4 slots cannot tell a mark from a published or free slot, makes none, and keeps the old rule:
// a held slot is left published and the ring is full when a writer comes round to it.
static void test_a_small_ring_keeps_the_old_rule(void) {
    struct tt_SegmentHeader* ring = ring_make(2);
    uint32_t got = 0;
    uint32_t index = 0;
    uint8_t* record = NULL;
    EXPECT_TRUE(ring_write(ring, 0));
    EXPECT_TRUE(ring_take(ring, &got, &index, &record));
    segment_hold(ring, index);
    EXPECT_EQ_U32(1U, seq_of(ring, 0)); // left published
    EXPECT_TRUE(ring_write(ring, 1));
    EXPECT_TRUE(ring_take(ring, &got, &index, &record));
    ring_done(ring, index);
    EXPECT_TRUE(!ring_write(ring, 2)); // index 2 is slot 0: refused, not passed
    EXPECT_EQ_U32(2U, ring->write_index);
    segment_unhold(ring, 0);
    EXPECT_EQ_U32(2U, seq_of(ring, 0));
    EXPECT_TRUE(ring_write(ring, 2));
    free(ring);
}

// ---- Several writers, one reader that keeps some records and releases them from another thread.

#define STRESS_WRITERS 2U
#define STRESS_RECORDS 100000U
#define STRESS_KEEP_MAX 3U

struct stress_kept {
    uint32_t index;
    uint32_t value;
    uint8_t* record;
};

static struct {
    struct tt_SegmentHeader* ring;
    pthread_mutex_t lock; // the kept list, shared by the reader and the releaser
    struct stress_kept kept[STRESS_KEEP_MAX];
    uint32_t kept_count;
    uint32_t done_writers;
    uint32_t corrupt;
    uint32_t released;
    bool reader_done;
} stress;

static void* stress_writer(void* arg) {
    uint32_t writer = *(const uint32_t*)arg;
    for (uint32_t n = 0; n < STRESS_RECORDS;) {
        if (ring_write(stress.ring, (writer << 24) | n)) {
            n++;
        } else {
            sched_yield();
        }
    }
    __atomic_add_fetch(&stress.done_writers, 1U, __ATOMIC_ACQ_REL);
    return NULL;
}

static void* stress_releaser(void* arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&stress.lock);
        if (stress.kept_count > 0) {
            struct stress_kept oldest = stress.kept[0];
            memmove(&stress.kept[0], &stress.kept[1], (stress.kept_count - 1U) * sizeof(stress.kept[0]));
            stress.kept_count--;
            stress.corrupt += record_is(oldest.record, oldest.value) ? 0U : 1U;
            segment_unhold(stress.ring, oldest.index);
            stress.released++;
        }
        bool finished = stress.reader_done && stress.kept_count == 0;
        pthread_mutex_unlock(&stress.lock);
        if (finished) {
            return NULL;
        }
        sched_yield();
    }
}

static void test_writers_reader_and_releaser_together(void) {
    memset(&stress, 0, sizeof(stress));
    stress.ring = ring_make(SLOTS);
    pthread_mutex_init(&stress.lock, NULL);
    pthread_t writers[STRESS_WRITERS];
    static uint32_t writer_ids[STRESS_WRITERS];
    for (uint32_t w = 0; w < STRESS_WRITERS; w++) {
        writer_ids[w] = w;
    }
    pthread_t releaser;
    for (uint32_t w = 0; w < STRESS_WRITERS; w++) {
        EXPECT_EQ_INT(0, pthread_create(&writers[w], NULL, stress_writer, &writer_ids[w]));
    }
    EXPECT_EQ_INT(0, pthread_create(&releaser, NULL, stress_releaser, NULL));
    uint32_t next[STRESS_WRITERS] = {0};
    uint32_t out_of_order = 0;
    uint32_t read = 0;
    uint32_t kept_total = 0;
    uint32_t reader_corrupt = 0;
    uint32_t state = 777U;
    while (read < STRESS_WRITERS * STRESS_RECORDS) {
        uint32_t got = 0;
        uint32_t index = 0;
        uint8_t* record = NULL;
        if (!ring_take(stress.ring, &got, &index, &record)) {
            sched_yield();
            continue;
        }
        read++;
        uint32_t writer = got >> 24;
        if (writer >= STRESS_WRITERS || (got & 0xFFFFFFU) != next[writer]) {
            out_of_order++;
        } else {
            next[writer]++;
        }
        reader_corrupt += record_is(record, got) ? 0U : 1U; // the releaser counts its own, under the lock
        state = (state * 1103515245U) + 12345U;
        bool keep = false;
        if (((state >> 16) % 7U) == 0) {
            pthread_mutex_lock(&stress.lock);
            if (stress.kept_count < STRESS_KEEP_MAX) {
                stress.kept[stress.kept_count++] = (struct stress_kept) {index, got, record};
                // Marked under the lock the releaser takes: it cannot release a slot before it is marked held.
                segment_hold(stress.ring, index);
                keep = true;
                kept_total++;
            }
            pthread_mutex_unlock(&stress.lock);
        }
        if (!keep) {
            ring_done(stress.ring, index);
        }
    }
    pthread_mutex_lock(&stress.lock);
    stress.reader_done = true;
    pthread_mutex_unlock(&stress.lock);
    for (uint32_t w = 0; w < STRESS_WRITERS; w++) {
        pthread_join(writers[w], NULL);
    }
    pthread_join(releaser, NULL);
    EXPECT_EQ_U32(0, out_of_order);
    EXPECT_EQ_U32(0, stress.corrupt);
    EXPECT_EQ_U32(0, reader_corrupt);
    for (uint32_t w = 0; w < STRESS_WRITERS; w++) {
        EXPECT_EQ_U32(STRESS_RECORDS, next[w]);
    }
    EXPECT_TRUE(kept_total > 1000U); // the run did hold slots, many times
    EXPECT_EQ_U32(kept_total, stress.released);
    // Everything released and read: the ring takes a full lap again.
    uint32_t written = 0;
    while (ring_write(stress.ring, written)) {
        written++;
    }
    EXPECT_EQ_U32(SLOTS, written);
    pthread_mutex_destroy(&stress.lock);
    free(stress.ring);
}

int main(void) {
    test_without_a_hold_the_ring_is_byte_identical();
    test_wrap_with_one_to_all_but_one_held();
    test_all_slots_held_refuses_and_does_not_spin();
    test_a_release_between_the_pass_and_the_reader();
    test_a_pass_left_half_done_is_finished_by_others();
    test_a_small_ring_keeps_the_old_rule();
    test_writers_reader_and_releaser_together();
    printf("test_segment_held: %s\n", test_failures == 0 ? "all tests passed" : "FAILED");
    return test_result();
}
#endif
