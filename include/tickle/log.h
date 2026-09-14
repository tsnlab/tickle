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

// Log levels, from most to least verbose. Messages below the current level are dropped.
typedef enum { TT_LOG_DEBUG = 0, TT_LOG_INFO = 1, TT_LOG_WARNING = 2, TT_LOG_ERROR = 3, TT_LOG_NONE = 4 } tt_LogLevel;

// Set the minimum level that gets logged (default: TT_LOG_INFO).
void tt_log_set_level(tt_LogLevel level);
