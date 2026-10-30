/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Receiving runs in place (DESIGN.md section 8, "Receiving runs in place"): core's half - the hint it gives the HAL
// after each run (tickle.c large_direct_aim()), what it does with a run the HAL read in place (direct_split), and the
// buffer a freed assembly leaves while a read may still be writing into it. The HAL's half, the read itself, is
// platform/linux/rx_direct_check.c against real sockets; here feed_aimed() does what it does - the framing into the
// receive buffer, the hinted bytes into the hinted place, the rest after them; that run handed over in place, anything
// else put back together - over the mock HAL, so every byte of every delivered sample is checked against its pattern.
// The receive buffer past the framing is filled with 0xEE for a run read in place: a core that read its payload from
// there instead of from the assembly delivers a torn sample.
//
// Covered: runs in order, each hinted from the last (framing to the byte, its place, its length) and taken without a
// copy, BEST_EFFORT and RELIABLE; a shorter run of the same sample from the same place; an aimed read that was another
// run, put back together and the hint backed off; a unit landed another way inside the hinted stretch withdraws the
// hint (else the next aimed read writes over it); no hint over a landed unit, after the last run, for a reverse-endian
// writer, or with TT_RX_DIRECT=0; an assembly freed while a read may be writing into it is parked until the read is
// over, and a run read in place for it is dropped, not copied from a buffer that went back; an assembly freed while a
// run read in place into it is being taken (a unit of the run jumps the watermark and the drain gives the assembly up)
// keeps its buffer until the run's last unit has been read from it. tests/mutants_rx_direct.py
// removes each condition and requires this binary to fail.
//
// Built as rmw_tickle builds core: a 65507-byte datagram, large samples.

#define tt_MAX_BUFFER_LENGTH 65507
#define tt_LARGE_SAMPLES 1

#include <byteswap.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: the hint and process_datagram_in()
#include "test_mock.h"

#define SENDER_ID 1
#define RECEIVER_ID 2
#define SENDER_IP 0x0a000001
#define PORT 8282
#define TOPIC_NAME "direct_topic"
#define ENDPOINT_NAME "direct_endpoint"

// 69 units on 11/12: 1,446 + 67 x 1,452 + a short tail.
#define LARGE_LEN 100000U
#define LARGE_UNITS 69U
#define RUN_N 10U
#define FRAMING 28U // the single header and FRAG_RUN_L's

// --- the receiver's large buffers, recorded so a release can be seen
// ---------------------------------------------------

#define LIVE_MAX 16
static uint8_t* live_buffers[LIVE_MAX];
static uint32_t live_sizes[LIVE_MAX];

static void* rx_acquire(void* user, uint32_t bytes) {
    (void)user;
    uint8_t* buffer = (uint8_t*)malloc(bytes);
    for (int i = 0; buffer != NULL && i < LIVE_MAX; i++) {
        if (live_buffers[i] == NULL) {
            live_buffers[i] = buffer;
            live_sizes[i] = bytes;
            break;
        }
    }
    return buffer;
}

// A buffer given back is poisoned and held, not freed at once: a core that still reads from it reads 0xDD - a torn
// unit the tests see - rather than memory that went back, which the benches' malloc'd buffers turn into a SIGSEGV.
#define RELEASED_MAX 64
static uint8_t* released_buffers[RELEASED_MAX];
static uint32_t released_count;

static void free_released(void) {
    for (uint32_t i = 0; i < released_count; i++) {
        free(released_buffers[i]);
        released_buffers[i] = NULL;
    }
    released_count = 0;
}

static void rx_release(void* user, void* buffer) {
    (void)user;
    for (int i = 0; i < LIVE_MAX; i++) {
        if (live_buffers[i] == buffer) {
            live_buffers[i] = NULL;
            memset(buffer, 0xDD, live_sizes[i]);
        }
    }
    if (released_count == RELEASED_MAX) {
        free_released();
    }
    released_buffers[released_count++] = (uint8_t*)buffer;
}

static bool is_live(const uint8_t* buffer) {
    for (int i = 0; i < LIVE_MAX; i++) {
        if (live_buffers[i] == buffer) {
            return true;
        }
    }
    return false;
}

static void* tx_acquire(void* user, uint32_t bytes) {
    (void)user;
    return malloc(bytes);
}

static void tx_release(void* user, void* buffer) {
    (void)user;
    free(buffer);
}

// --- the sample ------------------------------------------------------------------------------------------------------

static uint32_t sample_len;

static uint8_t pattern(uint32_t i) {
    return (uint8_t)((i * 7U) + 3U + (i >> 12U));
}

static int32_t sized_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sample_len;
}

static int32_t sized_encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    (void)data;
    for (uint32_t i = 0; i < len; i++) {
        payload[i] = pattern(i);
    }
    return (int32_t)len;
}

static int delivered;
static int delivered_torn;

static int32_t checking_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)native;
    bool intact = len == ROUNDUP(sample_len);
    for (uint32_t i = 0; intact && i < sample_len; i++) {
        intact = payload[i] == pattern(i);
    }
    delivered_torn += intact ? 0 : 1;
    return 0;
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static void on_data(struct tt_Subscriber* s, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)s;
    (void)time;
    (void)seq_no;
    (void)data;
    delivered++;
}

// --- the two contexts ------------------------------------------------------------------------------------------------

static struct tt_Context sender;
static struct tt_Context receiver;
static struct tt_TypeSupport sender_topic;
static struct tt_TypeSupport receiver_topic;
static struct tt_Publisher pub;
static struct tt_Subscriber sub;
#define WIDE_WORDS (tt_RELIABLE_BITMAP_MAX_BITS / 64)
// The Subscriber's caller storage: its window, and its own reassembly slot (tt_Subscriber_storage_size()).
static uint64_t tracking[((size_t)tt_MAX_PEER_COUNT * WIDE_WORDS) + (tt_FRAG_SLOT_BYTES / sizeof(uint64_t))];
static uint64_t sub_be_storage[tt_FRAG_SLOT_BYTES / sizeof(uint64_t)]; // the BEST_EFFORT one's own slot

static void init_bare_node(struct tt_Context* node, uint8_t id) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = id;
    node->tx_tail = sizeof(struct tt_Header);
    node->tx_size = sizeof(node->tx_buffer);
}

// --- capture: the MTU fragments one publish sends, parsed into units -------------------------------------------------

#define UNITS_MAX 80
#define DATAGRAM_MAX 1500
static uint8_t datagrams[UNITS_MAX][DATAGRAM_MAX];
static uint32_t datagram_len[UNITS_MAX];
static uint32_t datagram_count;
static uint32_t unit_payload_at[UNITS_MAX];
static struct tt_DataHeader sample_header; // fragment 0's

static void capture_hook(const void* buf, size_t len) {
    if (datagram_count == UNITS_MAX || len > DATAGRAM_MAX) {
        test_failures++;
        return;
    }
    memcpy(datagrams[datagram_count], buf, len);
    datagram_len[datagram_count++] = (uint32_t)len;
}

// A RELIABLE Subscriber tracks `words` 64-bit words per writer (in `tracking`), and has no reorder buffer.
static void init_pair_window(bool reliable, uint16_t words) {
    test_mock_reset();
    free_released();
    for (int i = 0; i < LIVE_MAX; i++) {
        if (live_buffers[i] != NULL) {
            free(live_buffers[i]);
            live_buffers[i] = NULL;
        }
    }
    sample_len = LARGE_LEN;
    delivered = 0;
    delivered_torn = 0;

    init_bare_node(&sender, SENDER_ID);
    memset(&sender_topic, 0, sizeof(sender_topic));
    sender_topic.name = TOPIC_NAME;
    sender_topic.data_size = 8;
    sender_topic.data_encode_size = sized_encode_size;
    sender_topic.data_encode = sized_encode;
    EXPECT_EQ_INT(tt_RET_OK, tt_Node_create_publisher(tt_Context_default_node(&sender), &pub, &sender_topic,
                                                      ENDPOINT_NAME, &TEST_QOS_UNRELIABLE, NULL, NULL, 0));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_large_buffers(&sender, tx_acquire, tx_release, NULL));

    init_bare_node(&receiver, RECEIVER_ID);
    receiver.large.direct_allowed = true; // what tt_bind() sets unless TT_RX_DIRECT=0
    memset(&receiver_topic, 0, sizeof(receiver_topic));
    receiver_topic.name = TOPIC_NAME;
    receiver_topic.data_size = 8;
    receiver_topic.data_decode = checking_decode;
    receiver_topic.data_free = free_nothing;
    struct tt_QosProfile qos = reliable ? TEST_QOS_RELIABLE_READER : TEST_QOS_UNRELIABLE;
    if (reliable) {
        qos.reliability.tracking_words = words; // a best-effort one keeps the builtin window, as it always had
    }
    size_t storage_size = tt_Subscriber_storage_size(&receiver_topic, &qos);
    EXPECT_TRUE(storage_size <= sizeof(tracking));
    EXPECT_EQ_INT(tt_RET_OK,
                  tt_Node_create_subscriber(tt_Context_default_node(&receiver), &sub, &receiver_topic, ENDPOINT_NAME,
                                            &qos, &(struct tt_SubscriberOptions) {.callback = on_data},
                                            storage_size != 0 ? tracking : NULL, storage_size));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_set_large_buffers(&receiver, rx_acquire, rx_release, NULL));

    // A Publisher with no matched reader sends nothing (2026-10-15): known peers past the unicast threshold keep the
    // broadcast this capture is about.
    for (uint8_t i = 0; i <= tt_UNICAST_PEER_THRESHOLD; i++) {
        (void)upsert_peer(pub.peers, (uint8_t)(100U + i), 0x0a000100U + i, 8282);
    }
    datagram_count = 0;
    test_mock_send_hook = capture_hook;
    EXPECT_EQ_INT(tt_RET_OK, tt_Publisher_publish(&pub, (struct tt_Data*)&sample_len));
    test_mock_send_hook = NULL;
    EXPECT_EQ_U32(LARGE_UNITS, datagram_count);
    for (uint32_t i = 0; i < datagram_count; i++) {
        bool first = datagrams[i][3] == tt_SUBMESSAGE_TYPE_FRAG_FIRST_L;
        if (first) {
            memcpy(&sample_header, datagrams[i] + sizeof(struct tt_SingleHeader), sizeof(sample_header));
        }
        unit_payload_at[i] = (uint32_t)sizeof(struct tt_SingleHeader) +
                             (uint32_t)(first ? sizeof(struct tt_FragFirstLHeader) : sizeof(struct tt_FragContLHeader));
    }
}

static void init_pair(bool reliable) {
    init_pair_window(reliable, WIDE_WORDS);
}

static uint32_t unit_length(uint32_t i) {
    return datagram_len[i] - unit_payload_at[i];
}

// --- runs ------------------------------------------------------------------------------------------------------------

static uint8_t run_bytes[70000];

static uint8_t native_marker(void) {
    uint16_t magic = NATIVE_MAGIC_VALUE;
    uint8_t marker = 0;
    memcpy(&marker, &magic, 1);
    return (uint8_t)(marker | tt_SINGLE_MARKER_FLAG);
}

// Units first .. first + n - 1 of the captured sample as one FRAG_RUN_L, as the writer sends it.
static uint32_t build_run(uint32_t first, uint32_t n) {
    struct tt_SingleHeader single = {native_marker(), tt_VERSION, SENDER_ID, tt_SUBMESSAGE_TYPE_FRAG_RUN_L};
    memcpy(run_bytes, &single, sizeof(single));
    uint32_t at = sizeof(single);
    struct tt_DataHeader data = sample_header;
    data.seq_no = sample_header.seq_no + first;
    struct tt_FragRunLHeader header = {data, (uint16_t)first, (uint16_t)datagram_count, (uint16_t)n, 0};
    memcpy(run_bytes + at, &header, sizeof(header));
    at += sizeof(header);
    for (uint32_t i = first; i < first + n; i++) {
        memcpy(run_bytes + at, datagrams[i] + unit_payload_at[i], unit_length(i));
        at += unit_length(i);
    }
    return at;
}

// The same run from a writer of the other byte order: the other marker, every multi-byte header field swapped.
static uint32_t build_run_reversed(uint32_t first, uint32_t n) {
    uint32_t len = build_run(first, n);
    run_bytes[0] = native_marker() == tt_SINGLE_MARKER_LE ? tt_SINGLE_MARKER_BE : tt_SINGLE_MARKER_LE;
    struct tt_FragRunLHeader header;
    memcpy(&header, run_bytes + sizeof(struct tt_SingleHeader), sizeof(header));
    header.data.endpoint_id = bswap_32(header.data.endpoint_id);
    header.data.seq_no = bswap_32(header.data.seq_no);
    header.data.timestamp = bswap_32(header.data.timestamp);
    header.data.entity_id = bswap_32(header.data.entity_id);
    header.frag_index = bswap_16(header.frag_index);
    header.frag_count = bswap_16(header.frag_count);
    header.run_count = bswap_16(header.run_count);
    memcpy(run_bytes + sizeof(struct tt_SingleHeader), &header, sizeof(header));
    return len;
}

static uint8_t rx[70000];

// A datagram read without a hint: the HAL's plain read.
static void feed(const uint8_t* bytes, uint32_t len) {
    memcpy(rx, bytes, len);
    (void)process_datagram_in(&receiver, rx, (int32_t)len, SENDER_IP, PORT, tt_TRANSPORT_UDP, 1);
}

// What hal_linux.c direct_read() does with a hint: the framing into the receive buffer, the hinted bytes into the
// hinted place, the rest after them; the hinted run (or a shorter one of it) handed over in place - the receive buffer
// past the framing left as garbage - and anything else put back together. Returns whether it was in place.
static bool feed_aimed(const uint8_t* bytes, uint32_t len) {
    struct tt_LargeState* large = &receiver.large;
    const uint32_t hinted = large->direct_bytes;
    if (hinted == 0) {
        feed(bytes, len);
        return false;
    }
    const uint32_t framing = large->direct_framing_len;
    memset(rx, 0xEE, sizeof(rx));
    memcpy(rx, bytes, len < framing ? len : framing);
    if (len > framing) {
        memcpy(large->direct_dest, bytes + framing, len - framing < hinted ? len - framing : hinted);
    }
    if (len > framing + hinted) {
        memcpy(rx + framing + hinted, bytes + framing + hinted, len - framing - hinted);
    }
    const uint32_t at = large->direct_count_at;
    uint16_t count = 0;
    memcpy(&count, rx + at, sizeof(count));
    bool in_place = len > framing && memcmp(rx, large->direct_framing, at) == 0 &&
                    memcmp(rx + at + 2, large->direct_framing + at + 2, framing - at - 2) == 0 && count != 0 &&
                    (uint32_t)count * large->direct_unit <= hinted &&
                    len - framing == (uint32_t)count * large->direct_unit;
    large->direct_reads++;
    if (in_place) {
        large->direct_split = large->direct_dest;
    } else {
        large->direct_missed++;
        if (len > framing) {
            memcpy(rx + framing, large->direct_dest, len - framing < hinted ? len - framing : hinted);
        }
    }
    (void)process_datagram_in(&receiver, rx, (int32_t)len, SENDER_IP, PORT, tt_TRANSPORT_UDP, 1);
    return in_place;
}

static void feed_unit(uint32_t i) {
    feed(datagrams[i], datagram_len[i]);
}

static struct tt_LargeAssembly* the_assembly(void) {
    for (uint32_t i = 0; i < tt_LARGE_ASSEMBLIES; i++) {
        if (receiver.large.assemblies[i].buffer != NULL) {
            return &receiver.large.assemblies[i];
        }
    }
    return NULL;
}

// The hint names run `first` of `n` units: its place, its length and its framing to the byte.
static void expect_hint(uint32_t first, uint32_t n) {
    struct tt_LargeAssembly* assembly = the_assembly();
    EXPECT_TRUE(assembly != NULL);
    EXPECT_EQ_U32(n * LARGE_CONT_PAYLOAD, receiver.large.direct_bytes);
    if (assembly == NULL || receiver.large.direct_bytes == 0) {
        return;
    }
    EXPECT_TRUE(receiver.large.direct_dest == assembly->buffer + tt_LARGE_CDR_OFFSET + large_offset(first));
    EXPECT_EQ_U32(FRAMING, receiver.large.direct_framing_len);
    (void)build_run(first, n);
    EXPECT_TRUE(memcmp(receiver.large.direct_framing, run_bytes, FRAMING) == 0);
}

static void expect_one_intact(void) {
    EXPECT_EQ_INT(1, delivered);
    EXPECT_EQ_INT(0, delivered_torn);
}

// --- the tests -------------------------------------------------------------------------------------------------------

// Runs of 10 in order, the last one 8 (units 60-67: never the last unit), then the tail: every run after the first is
// hinted from the one before and taken in place, the last leaves no hint, and the sample is whole.
static void test_in_order(bool reliable) {
    init_pair(reliable);
    feed(run_bytes, build_run(0, RUN_N));
    expect_hint(RUN_N, RUN_N);
    uint32_t in_place = 0;
    for (uint32_t first = RUN_N; first < 60U; first += RUN_N) {
        in_place += feed_aimed(run_bytes, build_run(first, RUN_N)) ? 1U : 0U;
        expect_hint(first + RUN_N, first + RUN_N == 60U ? 8U : RUN_N);
        if (reliable) { // every unit of the run recorded: the watermark past it, so the writer is not asked again
            struct tt_WriterProxy* proxy = find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
            EXPECT_TRUE(proxy != NULL && proxy->ack_seq_no == sample_header.seq_no + first + RUN_N);
        }
    }
    in_place += feed_aimed(run_bytes, build_run(60, 8)) ? 1U : 0U;
    EXPECT_EQ_U32(0, receiver.large.direct_bytes); // the next is the last unit, which goes alone
    EXPECT_EQ_U32(6, in_place);
    EXPECT_EQ_U64(6, receiver.large.direct_runs);
    EXPECT_EQ_U64(0, receiver.large.direct_missed);
    feed_unit(LARGE_UNITS - 1U);
    expect_one_intact();
}

// A run shorter than hinted, from the same place, is still the one: taken in place, and the next hint is its length.
static void test_shorter_run(void) {
    init_pair(true);
    feed(run_bytes, build_run(0, RUN_N));
    EXPECT_TRUE(feed_aimed(run_bytes, build_run(RUN_N, 4)));
    expect_hint(14, 4);
    EXPECT_TRUE(feed_aimed(run_bytes, build_run(14, 4)));
    feed(run_bytes, build_run(18, 25));
    feed(run_bytes, build_run(43, 25));
    feed_unit(LARGE_UNITS - 1U);
    expect_one_intact();
    EXPECT_EQ_U64(2, receiver.large.direct_runs);
}

// An aimed read that was another run: put back together, processed as any run, and no hint for the next 2 runs.
static void test_miss_and_back_off(void) {
    init_pair(true);
    feed(run_bytes, build_run(0, RUN_N));
    expect_hint(RUN_N, RUN_N);
    EXPECT_TRUE(!feed_aimed(run_bytes, build_run(20, RUN_N))); // its payload went to units 10-19's place first
    EXPECT_EQ_U64(1, receiver.large.direct_missed);
    EXPECT_EQ_U32(0, receiver.large.direct_bytes); // backed off: no hint after the run that missed
    feed(run_bytes, build_run(RUN_N, RUN_N));
    EXPECT_EQ_U32(0, receiver.large.direct_bytes); // nor after the next
    feed(run_bytes, build_run(30, RUN_N));
    expect_hint(40, RUN_N);                                   // the third aims again
    EXPECT_TRUE(feed_aimed(run_bytes, build_run(40, RUN_N))); // in place: the back-off starts over
    expect_hint(50, RUN_N);
    EXPECT_TRUE(!feed_aimed(run_bytes, build_run(58, RUN_N))); // missed again: 2 runs without a hint, not 4
    feed(run_bytes, build_run(50, 4));
    EXPECT_EQ_U32(0, receiver.large.direct_bytes);
    feed(run_bytes, build_run(54, 2));
    expect_hint(56, 2);
    feed(run_bytes, build_run(56, 2));
    feed_unit(LARGE_UNITS - 1U);
    expect_one_intact();
}

// A run whose units had all landed already places nothing: no hint, and the one there was is withdrawn. A run that
// completes the sample (the tail came first) frees the assembly: no hint into it.
static void test_no_hint_without_a_landing(void) {
    init_pair(true);
    feed(run_bytes, build_run(0, RUN_N));
    expect_hint(RUN_N, RUN_N);
    feed(run_bytes, build_run(0, RUN_N));
    EXPECT_EQ_U32(0, receiver.large.direct_bytes);
    feed_unit(25); // an MTU unit lands (and is not a run): the dup run after it still places nothing
    feed(run_bytes, build_run(0, RUN_N));
    EXPECT_EQ_U32(0, receiver.large.direct_bytes);

    init_pair(true);
    feed(run_bytes, build_run(0, 30));
    feed_unit(LARGE_UNITS - 1U);
    feed(run_bytes, build_run(30, 30));
    expect_hint(60, 8);
    EXPECT_TRUE(feed_aimed(run_bytes, build_run(60, 8)));
    expect_one_intact();
    EXPECT_EQ_U32(0, receiver.large.direct_bytes);
}

// A run read in place that core never took (the datagram refused before its run: here, a version core does not speak)
// leaves nothing behind: the next run is read from its own datagram, not from the stale place.
static void test_untaken_split_is_cleared(void) {
    init_pair(true);
    feed(run_bytes, build_run(0, RUN_N));
    uint32_t len = build_run(RUN_N, RUN_N);
    run_bytes[1] = (uint8_t)(tt_VERSION + 1U);
    EXPECT_TRUE(!feed_aimed(run_bytes, len));                 // the framing differs: not in place
    receiver.large.direct_split = receiver.large.direct_dest; // as if it had been
    run_bytes[1] = tt_VERSION;
    memcpy(rx, run_bytes, len);
    rx[1] = (uint8_t)(tt_VERSION + 1U);
    (void)process_datagram_in(&receiver, rx, (int32_t)len, SENDER_IP, PORT, tt_TRANSPORT_UDP, 1);
    EXPECT_TRUE(receiver.large.direct_split == NULL);
}

// A unit that lands another way in the hinted assembly - a repair - withdraws the hint: the next aimed read would
// otherwise write whatever it is over that unit's place. Here it is a run of another stretch, which would clobber
// unit 12; nothing lands unit 12 again, so a clobbered one is delivered torn.
static void test_repair_withdraws_the_hint(void) {
    init_pair(true);
    feed(run_bytes, build_run(0, RUN_N));
    expect_hint(RUN_N, RUN_N);
    feed_unit(12);
    EXPECT_EQ_U32(0, receiver.large.direct_bytes);
    (void)feed_aimed(run_bytes, build_run(30, RUN_N));
    feed(run_bytes, build_run(RUN_N, 2));
    feed(run_bytes, build_run(13, 17));
    feed(run_bytes, build_run(40, 28));
    feed_unit(LARGE_UNITS - 1U);
    expect_one_intact();
}

// No hint over a stretch where a unit has landed already.
static void test_no_hint_over_a_landed_unit(void) {
    init_pair(true);
    feed(run_bytes, build_run(0, 5));
    feed_unit(12);
    feed(run_bytes, build_run(5, 5)); // the next five, 10-14, include 12
    EXPECT_EQ_U32(0, receiver.large.direct_bytes);
    feed(run_bytes, build_run(15, 5)); // 20-24: none landed
    expect_hint(20, 5);
}

// A reverse-endian writer's run lands, and is not hinted: the framing is compared byte for byte in native order.
static void test_reverse_endian_not_hinted(void) {
    init_pair(true);
    feed(run_bytes, build_run_reversed(0, RUN_N));
    EXPECT_TRUE(the_assembly() != NULL && the_assembly()->received == RUN_N);
    EXPECT_EQ_U32(0, receiver.large.direct_bytes);
}

static void test_turned_off(void) {
    init_pair(true);
    receiver.large.direct_allowed = false;
    feed(run_bytes, build_run(0, RUN_N));
    EXPECT_EQ_U32(0, receiver.large.direct_bytes);
}

// The hinted assembly freed while the polling thread may be inside a read (direct_busy): its buffer is not given back
// until the read is over - the next datagram core processes - and the hint is gone at once. Freed outside a read, it
// goes back at once (the control).
static void test_freed_during_a_read_is_parked(void) {
    init_pair(true);
    feed(run_bytes, build_run(0, RUN_N));
    struct tt_LargeAssembly* assembly = the_assembly();
    uint8_t* buffer = assembly != NULL ? assembly->buffer : NULL;
    receiver.large.direct_busy = 1;
    large_assembly_abandon(&receiver, assembly);
    EXPECT_EQ_U32(0, receiver.large.direct_bytes);
    EXPECT_TRUE(is_live(buffer));
    EXPECT_TRUE(receiver.large.direct_parked == buffer);
    receiver.large.direct_busy = 0;
    feed_unit(0); // any datagram: the read is over
    EXPECT_TRUE(!is_live(buffer));
    EXPECT_TRUE(receiver.large.direct_parked == NULL);

    init_pair(true);
    feed(run_bytes, build_run(0, RUN_N));
    assembly = the_assembly();
    buffer = assembly != NULL ? assembly->buffer : NULL;
    large_assembly_abandon(&receiver, assembly);
    EXPECT_TRUE(!is_live(buffer));
    EXPECT_TRUE(receiver.large.direct_parked == NULL);
}

// A run read in place, then its assembly freed before core took it: dropped and counted, nothing landed, and the
// buffer goes back only after.
static void test_run_for_a_freed_assembly_is_dropped(void) {
    init_pair(true);
    feed(run_bytes, build_run(0, RUN_N));
    struct tt_LargeAssembly* assembly = the_assembly();
    uint8_t* buffer = assembly != NULL ? assembly->buffer : NULL;
    uint32_t len = build_run(RUN_N, RUN_N);
    memcpy(rx, run_bytes, FRAMING);
    memset(rx + FRAMING, 0xEE, len - FRAMING);
    memcpy(receiver.large.direct_dest, run_bytes + FRAMING, len - FRAMING);
    receiver.large.direct_split = receiver.large.direct_dest; // the HAL read it in place ...
    receiver.large.direct_busy = 1;
    large_assembly_abandon(&receiver, assembly); // ... and another thread freed the assembly before core took it
    receiver.large.direct_busy = 0;
    (void)process_datagram_in(&receiver, rx, (int32_t)len, SENDER_IP, PORT, tt_TRANSPORT_UDP, 1);
    EXPECT_EQ_U64(1, receiver.large.direct_cancelled);
    EXPECT_EQ_U64(0, receiver.large.direct_runs);
    EXPECT_TRUE(receiver.large.direct_split == NULL);
    EXPECT_TRUE(!is_live(buffer));
    EXPECT_TRUE(the_assembly() == NULL); // nothing claimed for it: the run was not taken from the receive buffer
}

// --- a run at once (large_run_batched(), large_accept_run())
// ----------------------------------------------------------

static struct tt_Subscriber sub_be;

static void feed_rest_from(uint32_t first) {
    while (first < LARGE_UNITS - 1U) {
        uint32_t n = LARGE_UNITS - 1U - first < 40U ? LARGE_UNITS - 1U - first : 40U;
        feed(run_bytes, build_run(first, n));
        first += n;
    }
    feed_unit(LARGE_UNITS - 1U);
}

// A run sent again after part of it was recorded (units 10-14 as MTU units, the watermark at 15): taken a unit at a
// time - 10-14 duplicates, 15-19 recorded - not as a span from 10, which update_reliable_ack() would call late and
// record nothing of, leaving 15-19 landed and never acknowledged.
static void test_batch_run_across_the_watermark(void) {
    init_pair(true);
    feed(run_bytes, build_run(0, RUN_N));
    for (uint32_t i = RUN_N; i < 15U; i++) {
        feed_unit(i);
    }
    struct tt_WriterProxy* proxy = find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
    EXPECT_TRUE(proxy != NULL && proxy->ack_seq_no == sample_header.seq_no + 15U);
    uint64_t duplicates = receiver.large.duplicate;
    feed(run_bytes, build_run(RUN_N, RUN_N));
    EXPECT_EQ_U64(duplicates + 5U, receiver.large.duplicate);
    EXPECT_TRUE(proxy != NULL && proxy->ack_seq_no == sample_header.seq_no + 20U);
    feed_rest_from(20);
    expect_one_intact();
}

// A run reaching past the window (64 bits here): a unit at a time, as before - the units past it jump the watermark
// and the sample below it is given up - not as a span, which would leave them landed and unrecorded.
static void test_batch_run_past_the_window(void) {
    init_pair_window(true, 1);
    feed(run_bytes, build_run(0, 2));
    feed(run_bytes, build_run(58, RUN_N)); // offsets 56-65 from the watermark at 2
    struct tt_WriterProxy* proxy = find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id);
    EXPECT_TRUE(proxy != NULL && seq_delta(proxy->ack_seq_no, sample_header.seq_no + 58U) > 0);
    EXPECT_EQ_U64(1, receiver.large.abandoned);
}

// The same, the run read in place: its first units land in the assembly it was read into, the next one jumps the
// watermark and the drain gives that assembly up - and the units after it are still read from there, now for the
// assembly claimed in its place. Its buffer must not go back before the run is taken: a buffer given back is poisoned
// here, so a unit copied from it lands as 0xDD (on main, 2026-10-31: the C7 slow reader's SIGSEGV in large_place(),
// the source of the memcpy unmapped).
static void test_run_in_place_outlives_its_assembly(void) {
    init_pair_window(true, 1);
    feed(run_bytes, build_run(0, 2));
    feed(run_bytes, build_run(58, 5)); // offsets 56-60 from the watermark at 2: a span, units 2-57 missing
    expect_hint(63, 5);
    struct tt_LargeAssembly* first = the_assembly();
    uint8_t* first_buffer = first != NULL ? first->buffer : NULL;
    EXPECT_TRUE(feed_aimed(run_bytes, build_run(63, 5))); // offsets 61-65: unit 66 is past the window
    EXPECT_EQ_U64(1, receiver.large.abandoned);
    struct tt_LargeAssembly* second = the_assembly();
    EXPECT_TRUE(second != NULL && second->buffer != first_buffer);
    EXPECT_TRUE(!is_live(first_buffer)); // given back once the run was taken
    if (second == NULL) {
        return;
    }
    EXPECT_TRUE(large_has_landed(second, 67));
    EXPECT_TRUE(memcmp(second->buffer + tt_LARGE_CDR_OFFSET + large_offset(67), datagrams[67] + unit_payload_at[67],
                       LARGE_CONT_PAYLOAD) == 0);
}

// A best-effort Subscriber beside the RELIABLE one: every run a unit at a time, so both get the sample.
static void test_batch_not_with_a_best_effort_subscriber(void) {
    init_pair(true);
    EXPECT_EQ_INT(tt_RET_OK,
                  tt_Node_create_subscriber(tt_Context_default_node(&receiver), &sub_be, &receiver_topic, ENDPOINT_NAME,
                                            &TEST_QOS_UNRELIABLE, &(struct tt_SubscriberOptions) {.callback = on_data},
                                            sub_be_storage, sizeof(sub_be_storage)));
    feed(run_bytes, build_run(0, RUN_N));
    feed_rest_from(RUN_N);
    EXPECT_EQ_INT(2, delivered);
    EXPECT_EQ_INT(0, delivered_torn);

    // A best-effort Subscriber alone, the sample's first run lost: a later run claims its assembly, as a unit does.
    init_pair(false);
    feed(run_bytes, build_run(RUN_N, RUN_N));
    EXPECT_TRUE(the_assembly() != NULL && the_assembly()->received == RUN_N);

    // The same Subscriber already tracking the writer (a whole sample before), the next sample's first run lost: the
    // run is routed to it, and claims the assembly.
    init_pair(false);
    feed(run_bytes, build_run(0, RUN_N));
    feed_rest_from(RUN_N);
    EXPECT_EQ_INT(1, delivered);
    EXPECT_TRUE(find_writer_proxy(&sub, SENDER_ID, pub.endpoint.entity_id) != NULL);
    sample_header.seq_no += LARGE_UNITS;
    feed(run_bytes, build_run(RUN_N, RUN_N));
    EXPECT_TRUE(the_assembly() != NULL && the_assembly()->received == RUN_N);
}

// A best-effort assembly of the sample still held (its Subscriber went after the sample's first run): the runs after
// go to it a unit at a time as before, so it completes and is freed with the RELIABLE one.
static void test_batch_not_with_a_best_effort_assembly(void) {
    init_pair(true);
    EXPECT_EQ_INT(tt_RET_OK,
                  tt_Node_create_subscriber(tt_Context_default_node(&receiver), &sub_be, &receiver_topic, ENDPOINT_NAME,
                                            &TEST_QOS_UNRELIABLE, &(struct tt_SubscriberOptions) {.callback = on_data},
                                            sub_be_storage, sizeof(sub_be_storage)));
    feed(run_bytes, build_run(0, RUN_N));
    EXPECT_EQ_INT(tt_RET_OK, tt_Subscriber_destroy(&sub_be));
    feed_rest_from(RUN_N);
    expect_one_intact();
    EXPECT_TRUE(the_assembly() == NULL);
}

// The first run of a sample after the last one's, not unit 0's: no assembly yet, so a unit at a time, which claims it.
// And a run whose count contradicts its sample's assembly: a unit at a time, each dropped.
static void test_batch_needs_the_assembly(void) {
    init_pair(true);
    feed(run_bytes, build_run(0, RUN_N));
    feed_rest_from(RUN_N);
    EXPECT_EQ_INT(1, delivered);
    EXPECT_TRUE(the_assembly() == NULL);
    // the same sample's units again as if a second sample (its seq_nos moved on), a run before its unit 0
    sample_header.seq_no += LARGE_UNITS;
    feed(run_bytes, build_run(RUN_N, RUN_N));
    EXPECT_TRUE(the_assembly() != NULL && the_assembly()->received == RUN_N);
    uint64_t dropped = receiver.large.dropped;
    uint32_t len = build_run(20, RUN_N);
    struct tt_FragRunLHeader header;
    memcpy(&header, run_bytes + sizeof(struct tt_SingleHeader), sizeof(header));
    header.frag_count = (uint16_t)(LARGE_UNITS + 1U);
    memcpy(run_bytes + sizeof(struct tt_SingleHeader), &header, sizeof(header));
    feed(run_bytes, len);
    EXPECT_EQ_U64(dropped + RUN_N, receiver.large.dropped);
}

int main(void) {
    tt_current_log_level = TT_LOG_NONE;
    test_in_order(true);
    test_in_order(false);
    test_shorter_run();
    test_miss_and_back_off();
    test_repair_withdraws_the_hint();
    test_no_hint_over_a_landed_unit();
    test_no_hint_without_a_landing();
    test_untaken_split_is_cleared();
    test_reverse_endian_not_hinted();
    test_turned_off();
    test_freed_during_a_read_is_parked();
    test_run_for_a_freed_assembly_is_dropped();
    test_batch_run_across_the_watermark();
    test_batch_run_past_the_window();
    test_run_in_place_outlives_its_assembly();
    test_batch_not_with_a_best_effort_subscriber();
    test_batch_not_with_a_best_effort_assembly();
    test_batch_needs_the_assembly();
    free_released();
    if (test_result() != 0) {
        return 1;
    }
    printf("test_rx_direct: all tests passed\n");
    return 0;
}
