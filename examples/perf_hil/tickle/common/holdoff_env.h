/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The reorder hold-off's A/B arm from the environment (ab/reorder-holdoff, stage S3), so that a harness's NODE_ENV
// picks it per run with one binary: TT_HOLDOFF_POLICY=
//   B | D | E      auto with that candidate (tickle.h tt_HOLDOFF_POLICY_*; E is the branch's default)
//   auto           auto with the default candidate; unset or empty: what the context's links resolve (ETHERNET and
//                  WIFI auto)
//   f<N>us, f<N>ms a fixed hold of N microseconds or milliseconds (the plan's arms: f250us f1ms f4ms f12ms)
//   off            no hold: every gap NACKed at once, the behaviour before the hold-off
// Anything else stops the bench: an arm must not run a treatment it did not name. Set through core's private seam
// (tt_holdoff_test_policy, tt_holdoff_test_value) before the context is created. The context's resolved hold-off is
// printed as a "HOLDOFF:" line, so an arm reports the treatment it ran. Removed with the seam before the hold-off
// lands.

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/tickle.h>

// A bench of this tree built against a tree without the hold-off (an A/B's main arm) has no HOLDOFF: line and no
// TT_HOLDOFF_POLICY; one that sets it there stops, as for an unknown value.
#ifdef tt_REORDER_HOLDOFF_OFF
// N of "f<N><unit>", times the unit's ns; 0 when the text is not that.
static inline uint64_t holdoff_env_fixed(const char* value) {
    if (value[0] != 'f' || value[1] < '0' || value[1] > '9') {
        return 0;
    }
    uint64_t number = 0;
    const char* cursor = value + 1;
    while (*cursor >= '0' && *cursor <= '9' && number < tt_REORDER_HOLDOFF_MAX) {
        number = (number * 10U) + (uint64_t)(*cursor - '0');
        cursor++;
    }
    uint64_t unit = 0;
    if (strcmp(cursor, "us") == 0) {
        unit = tt_MICROSECOND;
    } else if (strcmp(cursor, "ms") == 0) {
        unit = tt_MILLISECOND;
    }
    uint64_t hold = number * unit;
    return hold <= tt_REORDER_HOLDOFF_MAX ? hold : 0U;
}

static inline void holdoff_from_env(void) {
    const char* value = getenv("TT_HOLDOFF_POLICY");
    if (value == NULL || value[0] == '\0') {
        return;
    }
#if TT_HOLDOFF_TEST_SEAM
    uint64_t fixed = holdoff_env_fixed(value);
    if (strcmp(value, "B") == 0) {
        tt_holdoff_test_policy = (uint8_t)tt_HOLDOFF_POLICY_B;
    } else if (strcmp(value, "D") == 0) {
        tt_holdoff_test_policy = (uint8_t)tt_HOLDOFF_POLICY_D;
    } else if (strcmp(value, "E") == 0 || strcmp(value, "auto") == 0) {
        tt_holdoff_test_policy = (uint8_t)tt_HOLDOFF_POLICY_E;
    } else if (strcmp(value, "off") == 0) {
        tt_holdoff_test_value = tt_REORDER_HOLDOFF_OFF;
    } else if (fixed != 0) {
        tt_holdoff_test_value = fixed;
    } else {
        fprintf(stderr, "TT_HOLDOFF_POLICY=%s: expected B, D, E, auto, off or f<N>us / f<N>ms\n", value);
        exit(2);
    }
    // B, D, E and auto run auto whatever the links resolve: the seam's value is 0 (no override) there, so a link of a
    // profile whose default is off would leave the candidate unused - the HOLDOFF: line shows it if so.
#else
    fprintf(stderr, "TT_HOLDOFF_POLICY=%s: this build has no TT_HOLDOFF_TEST_SEAM\n", value);
    exit(2);
#endif
}

static inline void print_holdoff(const struct tt_Context* node) {
    static const char* const modes[] = {"off", "fixed", "auto"};
    static const char* const policies[] = {"-", "B", "D", "E"};
    const char* value = getenv("TT_HOLDOFF_POLICY");
    unsigned mode = node->holdoff_mode <= tt_HOLDOFF_MODE_AUTO ? node->holdoff_mode : 0U;
    unsigned policy = node->holdoff_policy <= tt_HOLDOFF_POLICY_E ? node->holdoff_policy : 0U;
    printf("HOLDOFF: mode=%s policy=%s value_ns=%u env=%s\n", modes[mode], policies[policy], (unsigned)node->holdoff_ns,
           value != NULL && value[0] != '\0' ? value : "-");
}
#else
static inline void holdoff_from_env(void) {
    const char* value = getenv("TT_HOLDOFF_POLICY");
    if (value != NULL && value[0] != '\0') {
        fprintf(stderr, "TT_HOLDOFF_POLICY=%s: this tree has no reorder hold-off\n", value);
        exit(2);
    }
}

static inline void print_holdoff(const struct tt_Context* node) {
    (void)node;
}
#endif
