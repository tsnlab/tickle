/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_tickle/PLAN.md's remaining-rmw-API-surface backlog - rmw_feature_supported() was previously
// missing as a symbol entirely (Milestone 15's own note). Unlike every other rmw_tickle_c entry
// point, this one takes no rmw_node_t/publisher/subscription at all (see rmw/features.h) - it's a
// pure, stateless query against this rmw's own fixed capabilities, so it needs no argument
// validation, node lookup, or locking.

#include <stdbool.h>

#include "rmw/features.h"

bool rmw_feature_supported(rmw_feature_t feature) {
    switch (feature) {
    // rmw_tickle_queued_message_t.publication_sequence_number/reception_sequence_number
    // (rmw_tickle_c/rmw_tickle.h) are both filled from real, monotonically-assigned wire/local
    // counters - tt_Publisher.seq_no on the wire (data_header->seq_no) and rmw_tickle_subscriber_t.
    // reception_sequence_number (subscriber_callback(), rmw_subscription.c) locally - not left at
    // 0 or otherwise unpopulated, so both message-info features are genuinely supported.
    case RMW_FEATURE_MESSAGE_INFO_PUBLICATION_SEQUENCE_NUMBER:
    case RMW_FEATURE_MESSAGE_INFO_RECEPTION_SEQUENCE_NUMBER:
        return true;
    // Neither dynamic-type discovery nor take_dynamic_message() exist anywhere in rmw_tickle -
    // rosidl_typesupport_tickle_c/_cpp only ever resolve a real, statically-generated typesupport
    // (rmw_typesupport.c's own rmw_tickle_get_message_callbacks()), never a runtime-introspected
    // dynamic one.
    case RMW_MIDDLEWARE_SUPPORTS_TYPE_DISCOVERY:
    case RMW_MIDDLEWARE_CAN_TAKE_DYNAMIC_MESSAGE:
    default:
        return false;
    }
}
