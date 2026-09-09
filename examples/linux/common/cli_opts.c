/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#include "cli_opts.h"

#include <stdlib.h>
#include <string.h>

#include <tickle/log.h>

bool tt_example_parse_log_level(const char* str, tt_LogLevel* level) {
    if (strcmp(str, "debug") == 0) {
        *level = TT_LOG_DEBUG;
    } else if (strcmp(str, "info") == 0) {
        *level = TT_LOG_INFO;
    } else if (strcmp(str, "warning") == 0) {
        *level = TT_LOG_WARNING;
    } else if (strcmp(str, "error") == 0) {
        *level = TT_LOG_ERROR;
    } else if (strcmp(str, "none") == 0) {
        *level = TT_LOG_NONE;
    } else {
        return false;
    }
    return true;
}

enum tt_flag_match {
    TT_FLAG_NOT_MATCHED,
    TT_FLAG_MATCHED_OK,
    TT_FLAG_MATCHED_ERROR,
};

// The flags every example accepts regardless of `flags` - see cli_opts.h's own comment.
static enum tt_flag_match parse_general_flag(int argc, char** argv, int* i, struct tt_example_cli_options* opts) {
    if (strcmp(argv[*i], "-b") == 0 && *i + 1 < argc) {
        opts->broadcast = argv[++*i];
    } else if (strcmp(argv[*i], "-p") == 0 && *i + 1 < argc) {
        opts->port = atoi(argv[++*i]);
    } else if (strcmp(argv[*i], "-a") == 0 && *i + 1 < argc) {
        opts->bind_addr = argv[++*i];
    } else if (strcmp(argv[*i], "-I") == 0 && *i + 1 < argc) {
        opts->node_id = atoi(argv[++*i]);
    } else if (strcmp(argv[*i], "-n") == 0 && *i + 1 < argc) {
        opts->name = argv[++*i];
    } else if (strcmp(argv[*i], "-l") == 0 && *i + 1 < argc) {
        if (!tt_example_parse_log_level(argv[++*i], &opts->log_level)) {
            return TT_FLAG_MATCHED_ERROR;
        }
        opts->log_level_set = true;
    } else {
        return TT_FLAG_NOT_MATCHED;
    }
    return TT_FLAG_MATCHED_OK;
}

// The -c/-i/-d/-s flags, each only recognized if its TT_EXAMPLE_OPT_* bit is set in `flags`.
static enum tt_flag_match parse_conditional_flag(int argc, char** argv, int* i, struct tt_example_cli_options* opts,
                                                 uint32_t flags) {
    if ((flags & TT_EXAMPLE_OPT_COUNT) && strcmp(argv[*i], "-c") == 0 && *i + 1 < argc) {
        opts->count = (uint32_t)strtoul(argv[++*i], NULL, 10);
    } else if ((flags & TT_EXAMPLE_OPT_INTERVAL) && strcmp(argv[*i], "-i") == 0 && *i + 1 < argc) {
        opts->interval_s = strtod(argv[++*i], NULL);
    } else if ((flags & TT_EXAMPLE_OPT_DURATION) && strcmp(argv[*i], "-d") == 0 && *i + 1 < argc) {
        opts->duration_s = strtod(argv[++*i], NULL);
    } else if ((flags & TT_EXAMPLE_OPT_MESSAGE_SIZE) && strcmp(argv[*i], "-s") == 0 && *i + 1 < argc) {
        opts->message_size = (uint32_t)strtoul(argv[++*i], NULL, 10);
    } else if ((flags & TT_EXAMPLE_OPT_WARMUP_COOLDOWN) && strcmp(argv[*i], "-w") == 0 && *i + 1 < argc) {
        opts->warmup = strtod(argv[++*i], NULL);
    } else if ((flags & TT_EXAMPLE_OPT_WARMUP_COOLDOWN) && strcmp(argv[*i], "-W") == 0 && *i + 1 < argc) {
        opts->cooldown = strtod(argv[++*i], NULL);
    } else {
        return TT_FLAG_NOT_MATCHED;
    }
    return TT_FLAG_MATCHED_OK;
}

int tt_example_parse_args(int argc, char** argv, struct tt_example_cli_options* opts, uint32_t flags) {
    for (int i = 1; i < argc; i++) {
        enum tt_flag_match result = parse_general_flag(argc, argv, &i, opts);
        if (result == TT_FLAG_NOT_MATCHED) {
            result = parse_conditional_flag(argc, argv, &i, opts, flags);
        }
        if (result != TT_FLAG_MATCHED_OK) {
            return 1;
        }
    }
    return 0;
}
