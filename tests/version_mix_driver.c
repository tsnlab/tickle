/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// tests/version_mix.sh's driver: built against one tree's core (-DTICKLE_C, that tree's headers and mock HAL), it hands
// that core two discovery announces - OWN, written by the same tree's encoder (its fuzz corpus seed), and OTHER,
// written by another version's - each to a fresh context with a discovery table, and says what happened:
//   own    applied: its entities are in the table, and no version mismatch was counted - the control, without which
//          "refused" below could be a driver that applies nothing;
//   other  refused: the datagram rejected, nothing in the table, one version mismatch counted with OTHER's version.
// Exit 0 when both hold. The seeds come from node 2; the context here is node 1.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

#ifndef TICKLE_C
#define TICKLE_C "../src/tickle.c"
#endif
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox: process_packet() is static

static struct tt_Context node;
static struct tt_Discovery discovery;
static uint8_t buf[tt_MAX_BUFFER_LENGTH];

// Feeds the announce at `path` to a fresh context: how many entities it left in the table, and the version drops.
static uint32_t feed(const char* path, uint8_t* version, uint64_t* drops) {
    memset(&node, 0, sizeof(node));
    node_init_locks(&node);
    node.id = 1;
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = sizeof(node.tx_buffer);
    memset(&discovery, 0, sizeof(discovery));
    (void)tt_Context_set_discovery(&node, &discovery, NULL, NULL);
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot read %s\n", path);
        *version = 0;
        *drops = 0;
        return UINT32_MAX;
    }
    size_t len = fread(buf, 1, sizeof(buf), f);
    (void)fclose(f);
    *version = len > 1 ? buf[1] : 0; // both header forms carry the version in byte 1 for the single form ...
    if (len > 2 && (buf[0] == 'K' || buf[0] == 'T')) {
        *version = buf[2]; // ... and byte 2 for the classic one
    }
    (void)process_packet(&node, buf, 0, (uint32_t)len, 0x0a000002, 8282, tt_TRANSPORT_UDP);
    uint32_t entities = 0;
    for (int i = 0; i < tt_MAX_DISCOVERED_ENTITIES; i++) {
        entities += discovery.entities[i].context_id != tt_CONTEXT_ID_INVALID ? 1U : 0U;
    }
    *drops = node.version_mismatch_drops;
    return entities;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s OWN OTHER\n", argv[0]);
        return 2;
    }
    tt_current_log_level = TT_LOG_NONE;
    uint8_t own_version = 0;
    uint8_t other_version = 0;
    uint64_t own_drops = 0;
    uint64_t other_drops = 0;
    uint32_t own = feed(argv[1], &own_version, &own_drops);
    uint32_t other = feed(argv[2], &other_version, &other_drops);
    bool own_applied = own != UINT32_MAX && own > 0 && own_drops == 0 && own_version == tt_VERSION;
    bool other_refused = other == 0 && other_drops == 1 && other_version != tt_VERSION;
    printf("core v%u: own v%u announce %s (%u entities, %llu version drops); other v%u announce %s (%u entities, "
           "%llu version drops)\n",
           (unsigned)tt_VERSION, own_version, own_applied ? "applied" : "NOT APPLIED", own,
           (unsigned long long)own_drops, other_version, other_refused ? "refused" : "NOT REFUSED", other,
           (unsigned long long)other_drops);
    return own_applied && other_refused ? 0 : 1;
}
