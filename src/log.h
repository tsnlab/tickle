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

#include <stdarg.h>
#include <stdio.h>

#include <tickle/log.h> // tt_LogLevel, tt_log_set_level (public API)

// Log configuration
extern tt_LogLevel tt_current_log_level;
extern FILE* tt_log_output;

// Initialize logging system
void tt_log_init(tt_LogLevel level, FILE* output);

// Set log output
void tt_log_set_output(FILE* output);

// Log functions
void tt_log_debug(const char* format, ...);
void tt_log_info(const char* format, ...);
void tt_log_warning(const char* format, ...);
void tt_log_error(const char* format, ...);

// Internal logging function
void tt_log_internal(tt_LogLevel level, const char* level_str, const char* format, va_list args);

// Convenience macros. Check the level here, before calling the tt_log_* function, so a
// suppressed call costs one comparison instead of a function call (with its format-string
// argument expressions already evaluated) that only then discovers it has nothing to do -
// these run on the packet-processing hot path, some many times per packet.
#define TT_LOG_DEBUG(...)                           \
    do {                                            \
        if (tt_current_log_level <= TT_LOG_DEBUG) { \
            tt_log_debug(__VA_ARGS__);              \
        }                                           \
    } while (0)
#define TT_LOG_INFO(...)                           \
    do {                                           \
        if (tt_current_log_level <= TT_LOG_INFO) { \
            tt_log_info(__VA_ARGS__);              \
        }                                          \
    } while (0)
#define TT_LOG_WARNING(...)                           \
    do {                                              \
        if (tt_current_log_level <= TT_LOG_WARNING) { \
            tt_log_warning(__VA_ARGS__);              \
        }                                             \
    } while (0)
#define TT_LOG_ERROR(...)                           \
    do {                                            \
        if (tt_current_log_level <= TT_LOG_ERROR) { \
            tt_log_error(__VA_ARGS__);              \
        }                                           \
    } while (0)
