/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// test_discovery_siblings.c's cases on the scan core's default table keeps: at 16 entries tt_DISCOVERY_INDEXED is
// off, and every lookup is the linear one.
#define tt_MAX_DISCOVERED_ENTITIES 16
#include "test_discovery_siblings.c" // NOLINT(bugprone-suspicious-include) -- the same cases, built on the scan
