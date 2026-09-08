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

int tt_example_parse_args(int argc, char** argv, struct tt_example_cli_options* opts, uint32_t flags) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            opts->broadcast = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            opts->port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            opts->bind_addr = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            opts->name = argv[++i];
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            if (!tt_example_parse_log_level(argv[++i], &opts->log_level)) {
                return 1;
            }
            opts->log_level_set = true;
        } else if ((flags & TT_EXAMPLE_OPT_COUNT) && strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            opts->count = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if ((flags & TT_EXAMPLE_OPT_INTERVAL) && strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            opts->interval_s = strtod(argv[++i], NULL);
        } else if ((flags & TT_EXAMPLE_OPT_DURATION) && strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            opts->duration_s = strtod(argv[++i], NULL);
        } else if ((flags & TT_EXAMPLE_OPT_MESSAGE_SIZE) && strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            opts->message_size = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else {
            return 1;
        }
    }
    return 0;
}
