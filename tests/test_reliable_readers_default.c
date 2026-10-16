/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// test_reliable_readers.c at the default tt_MAX_ACK_ENTRIES (16): of 20 RELIABLE Subscribers the 17th and later are
// refused at the writer (logged "Ack table full"), the behaviour kept; the first 16 are matched and acknowledged.
#define TEST_DEFAULT_LIMIT 1
#include "test_reliable_readers.c" // NOLINT(bugprone-suspicious-include) -- the same cases at another limit
