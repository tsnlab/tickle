/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The fuzzer's seed corpus (tests/fuzz_corpus/) is only worth anything while its packets still get
// past validate_packet_header(): a seed with a stale tt_VERSION or magic value is rejected before
// any parser runs, and the fuzzer would then be starting from nothing again without saying so.
// This checks every seed against the current header rules, and that the corpus is not empty. If it
// fails after a wire change, regenerate the seeds with `make fuzz-corpus`.

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: validate_packet_header() is static.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox

#ifndef FUZZ_CORPUS_DIR
#define FUZZ_CORPUS_DIR "../../tests/fuzz_corpus"
#endif

int main(void) {
    tt_current_log_level = TT_LOG_NONE;
    DIR* dir = opendir(FUZZ_CORPUS_DIR);
    EXPECT_TRUE(dir != NULL);
    if (dir == NULL) {
        return 1;
    }
    struct tt_Node node;
    memset(&node, 0, sizeof(node));
    node_init_locks(&node);
    node.id = 1; // the fuzz harness's own node id

    int seeds = 0;
    int parts = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".bin") == NULL) {
            continue;
        }
        char path[512];
        (void)snprintf(path, sizeof(path), "%s/%s", FUZZ_CORPUS_DIR, entry->d_name);
        static uint8_t buf[tt_MAX_BUFFER_LENGTH];
        FILE* f = fopen(path, "rb");
        EXPECT_TRUE(f != NULL);
        if (f == NULL) {
            continue;
        }
        size_t len = fread(buf, 1, sizeof(buf), f);
        (void)fclose(f);
        EXPECT_TRUE(len >= sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader));
        if (!validate_packet_header(&node, (struct tt_Header*)buf)) {
            printf("%s: rejected by validate_packet_header() - regenerate with `make fuzz-corpus`\n", entry->d_name);
            EXPECT_TRUE(false);
        }
        const struct tt_SubmessageHeader* sub = (const struct tt_SubmessageHeader*)(buf + sizeof(struct tt_Header));
        if (sub->type == tt_SUBMESSAGE_TYPE_UPDATE_PART) {
            parts++;
        }
        seeds++;
    }
    (void)closedir(dir);

    EXPECT_TRUE(seeds >= 4);
    EXPECT_TRUE(parts >= 2); // the parser the corpus exists for is actually seeded

    if (test_result() != 0) {
        return 1;
    }
    printf("test_fuzz_corpus: %d seeds, all pass the header check\n", seeds);
    return 0;
}
