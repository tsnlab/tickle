/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#pragma once

// Shared command-line option parsing for the example binaries (client/server, publisher/
// subscriber, ping/pong, perf_client/perf_server - see README.md's "Command-line options"). Every
// example accepts the same -b/-p/-a/-I/-n/-l interface-configuration flags plus some subset of
// -c/-i/-d/-s depending on whether it's a sender, a receiver, or perf_client (which is both). Each
// example still owns its own defaults and print_usage() text (those differ per example); this
// only shares the parsing loop and log-level lookup that used to be copy-pasted byte-for-byte into
// all eight of them.

#include <stdbool.h>
#include <stdint.h>

#include <tickle/log.h>

enum tt_example_opt_flags {
    TT_EXAMPLE_OPT_COUNT = 1u << 0,        // -c: senders only
    TT_EXAMPLE_OPT_INTERVAL = 1u << 1,     // -i: senders only (perf_client too)
    TT_EXAMPLE_OPT_DURATION = 1u << 2,     // -d: receivers only (perf_client too)
    TT_EXAMPLE_OPT_MESSAGE_SIZE = 1u << 3, // -s: perf_client only
};

struct tt_example_cli_options {
    char* broadcast;
    int port;        // 0 = keep the compiled-in default
    char* bind_addr; // NULL = keep the compiled-in default
    int node_id;     // 0 = keep the default (auto-detect via tt_get_node_id())
    char* name;      // topic/service name to rendezvous on
    tt_LogLevel log_level;
    bool log_level_set;

    // Only meaningful if the matching TT_EXAMPLE_OPT_* bit was passed to
    // tt_example_parse_args() - otherwise left at whatever the caller defaulted it to.
    uint32_t count;        // -c, 0 = unlimited
    double interval_s;     // -i
    double duration_s;     // -d, 0 = run until Ctrl+C
    uint32_t message_size; // -s
};

bool tt_example_parse_log_level(const char* str, tt_LogLevel* level);

// Parses argv into opts, which the caller must have already filled with its own defaults (they
// differ per example - see each example's own parse_args()). Only recognizes -c/-i/-d/-s if the
// matching TT_EXAMPLE_OPT_* bit is set in `flags`; anything else unrecognized (including a
// disabled one of those four) fails the same way an unknown flag does. Returns 0 on success,
// non-zero if argv held an unrecognized/incomplete/rejected option - the caller is expected to
// print its own usage and exit in that case.
int tt_example_parse_args(int argc, char** argv, struct tt_example_cli_options* opts, uint32_t flags);
