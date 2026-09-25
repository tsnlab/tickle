/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Writes the fuzzer's seed corpus (tests/fuzz_corpus/) from the real encoder: `make fuzz-corpus`.
//
// Why seeds at all: `make fuzz` used to start from nothing, and in CI's 45 seconds libFuzzer
// rarely builds a packet with a valid header, a known submessage type and self-consistent fields -
// so a parser that needs all three (UPDATE_PART, 2026-09-24) was barely reached. Mutating from
// valid packets reaches it immediately.
//
// Why generated rather than hand-written bytes: the seeds must match the current wire format and
// tt_VERSION exactly, or validate_packet_header() rejects them before any parser runs and they are
// silently worthless. test_fuzz_corpus.c checks that the checked-in files still pass that gate;
// when it fails, rerun this.
//
// Seeds come from a sender with node id 2, because the fuzz harness's own node is 1 and a packet
// from yourself is handled differently.

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: build_and_send_update() is static.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox

#define SENDER_ID 2
#define MAX_ENDPOINTS 24
#define MAX_DATAGRAMS 8

static uint8_t datagrams[MAX_DATAGRAMS][tt_MAX_BUFFER_LENGTH * 2];
static size_t datagram_len[MAX_DATAGRAMS];
static int datagram_count;

static void capture(const void* buf, size_t len) {
    if (datagram_count < MAX_DATAGRAMS && len <= sizeof(datagrams[0])) {
        memcpy(datagrams[datagram_count], buf, len);
        datagram_len[datagram_count++] = len;
    }
}

static int32_t decode_nothing(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)payload;
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

static struct tt_Node sender;
static struct tt_Topic topics[MAX_ENDPOINTS];
static struct tt_Subscriber subs[MAX_ENDPOINTS];
static char names[MAX_ENDPOINTS][48];

// Announces `count` Subscribers - the first named "fuzz" like the harness's own endpoint, so the
// fuzzer also starts from an announce that matches something - and captures what goes out.
static int announce(int count) {
    memset(&sender, 0, sizeof(sender));
    node_init_locks(&sender);
    sender.id = SENDER_ID;
    sender.tx_tail = sizeof(struct tt_Header);
    sender.tx_size = tt_MAX_BUFFER_LENGTH * 2;
    sender.last_modified = 1000;
    for (int i = 0; i < count; i++) {
        if (i == 0) {
            (void)snprintf(names[i], sizeof(names[i]), "fuzz");
        } else {
            (void)snprintf(names[i], sizeof(names[i]), "/my_robot_node/some_topic_name_%04d", i);
        }
        memset(&topics[i], 0, sizeof(topics[i]));
        topics[i].name = names[i];
        topics[i].data_size = 4;
        topics[i].data_decode = decode_nothing;
        topics[i].data_free = free_nothing;
        if (tt_Node_create_subscriber(&sender, &subs[i], &topics[i], "fuzz_sub", on_data) != tt_RET_OK) {
            return -1;
        }
    }
    datagram_count = 0;
    test_mock_send_hook = capture;
    if (!build_and_send_update(&sender, NULL, 0)) {
        return -1;
    }
    node_flush(&sender, 0, NULL);
    return datagram_count;
}

// The packet cut `cut` bytes short, with its (only) submessage's length field shortened to match -
// so it passes the packet-level length check and reaches the submessage parser with an entity list
// that runs out before its count says it should. Cutting without fixing the length would only ever
// exercise that first check.
static size_t truncate_consistently(uint8_t* buf, size_t len, size_t cut) {
    struct tt_SubmessageHeader* sub = (struct tt_SubmessageHeader*)(buf + sizeof(struct tt_Header));
    sub->length = (uint16_t)(sub->length - cut);
    return len - cut;
}

static int write_seed(const char* dir, const char* name, const uint8_t* buf, size_t len) {
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE* f = fopen(path, "wb");
    if (f == NULL || fwrite(buf, 1, len, f) != len) {
        fprintf(stderr, "gen_fuzz_corpus: cannot write %s: %s\n", path, strerror(errno));
        if (f != NULL) {
            (void)fclose(f);
        }
        return 1;
    }
    (void)fclose(f);
    printf("  %s (%zu bytes)\n", name, len);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s OUT_DIR\n", argv[0]);
        return 2;
    }
    const char* dir = argv[1];
    tt_current_log_level = TT_LOG_NONE;
    int fail = 0;

    // A whole announce in one UPDATE, and the same cut off mid-entity.
    if (announce(3) != 1) {
        fprintf(stderr, "gen_fuzz_corpus: a 3-endpoint announce should be one datagram\n");
        return 1;
    }
    fail |= write_seed(dir, "update_single.bin", datagrams[0], datagram_len[0]);
    fail |= write_seed(dir, "update_single_truncated.bin", datagrams[0],
                       truncate_consistently(datagrams[0], datagram_len[0], 8));

    // An announce too large for one datagram: every part, and the first cut off mid-entity.
    int parts = announce(MAX_ENDPOINTS);
    if (parts < 2) {
        fprintf(stderr, "gen_fuzz_corpus: a %d-endpoint announce should need parts, got %d\n", MAX_ENDPOINTS, parts);
        return 1;
    }
    for (int i = 0; i < parts; i++) {
        char name[64];
        (void)snprintf(name, sizeof(name), "update_part_%d_of_%d.bin", i, parts);
        fail |= write_seed(dir, name, datagrams[i], datagram_len[i]);
    }
    fail |= write_seed(dir, "update_part_truncated.bin", datagrams[0],
                       truncate_consistently(datagrams[0], datagram_len[0], 8));

    return fail;
}
