/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The benches' reveal re-ask (tt_LinkOptions.repair_reveal, docs/DESIGN.md "Repair reveal", 2026-10-30), from the
// environment so that a harness's NODE_ENV picks it per tree: TT_REPAIR_REVEAL=off|revealed|all_missing, unset or
// empty = the profile's (0). Anything else stops the bench: an arm must not run a treatment it did not name.
// Each bench prints the context's resolved value as a "REVEAL:" line, so an arm reports the treatment it ran.
#ifndef REPAIR_REVEAL_ENV_H
#define REPAIR_REVEAL_ENV_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

static inline uint8_t repair_reveal_from_env(void) {
    const char* value = getenv("TT_REPAIR_REVEAL");
    if (value == NULL || value[0] == '\0') {
        return (uint8_t)tt_REPAIR_REVEAL_DEFAULT;
    }
    if (strcmp(value, "off") == 0) {
        return (uint8_t)tt_REPAIR_REVEAL_OFF;
    }
    if (strcmp(value, "revealed") == 0) {
        return (uint8_t)tt_REPAIR_REVEAL_REVEALED;
    }
    if (strcmp(value, "all_missing") == 0) {
        return (uint8_t)tt_REPAIR_REVEAL_ALL_MISSING;
    }
    fprintf(stderr, "TT_REPAIR_REVEAL=%s: expected off, revealed or all_missing\n", value);
    exit(2);
}

static inline void print_repair_reveal(const struct tt_Context* node) {
    printf("REVEAL: repair_reveal=%u pace_ns=%u env=%s\n", (unsigned)node->repair_reveal,
           (unsigned)node->repair_reveal_pace_ns,
           getenv("TT_REPAIR_REVEAL") != NULL ? getenv("TT_REPAIR_REVEAL") : "-");
}

#endif
