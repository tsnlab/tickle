/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The one field the wire identity tests (test_wire_identity_*.c) mask when they compare a capture with the parent's
// golden (2026-11-05): the 2 bytes of an announce's tt_AnnounceHeader.marks. The parent wrote them as zero; this tree
// fills them with the announce's tag and the echo of the tag it answers (DESIGN.md section 3), so no announce is
// byte-identical to the parent's. Every other byte is still compared as it is. Self-contained: only the public wire
// structs.

#ifndef TEST_WIRE_MASK_H
#define TEST_WIRE_MASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <tickle/tickle.h>

// Zeroes the marks of every announce in one datagram, as sent. An announce can be the second submessage of a datagram,
// behind a batched summary, so every submessage is walked.
static void mask_announce_marks_in(uint8_t* datagram, uint32_t length) {
    bool single = length > 3U && (datagram[0] == tt_SINGLE_MARKER_LE || datagram[0] == tt_SINGLE_MARKER_BE);
    if (single) {
        uint32_t body = (uint32_t)sizeof(struct tt_SingleHeader);
        if (length >= body + sizeof(struct tt_DataHeader) + sizeof(struct tt_AnnounceHeader) &&
            datagram[3] == tt_SUBMESSAGE_TYPE_DATA) {
            struct tt_DataHeader data;
            memcpy(&data, datagram + body, sizeof(data));
            if (data.endpoint_id == tt_DISCOVERY_ENDPOINT_ID && data.entity_id == tt_DISCOVERY_ENTITY_ID) {
                memset(datagram + body + sizeof(struct tt_DataHeader) + offsetof(struct tt_AnnounceHeader, marks), 0,
                       sizeof(uint16_t));
            }
        }
        return;
    }
    uint32_t pos = (uint32_t)sizeof(struct tt_Header);
    while (pos + sizeof(struct tt_SubmessageHeader) <= length) {
        struct tt_SubmessageHeader sub_header;
        memcpy(&sub_header, datagram + pos, sizeof(sub_header));
        uint32_t body = pos + (uint32_t)sizeof(sub_header);
        uint32_t header = 0U;
        if (sub_header.type == tt_SUBMESSAGE_TYPE_DATA) {
            header = (uint32_t)sizeof(struct tt_DataHeader);
        } else if (sub_header.type == tt_SUBMESSAGE_TYPE_FRAG_FIRST) {
            header = (uint32_t)sizeof(struct tt_FragFirstHeader);
        }
        if (header != 0U && body + header + sizeof(struct tt_AnnounceHeader) <= length) {
            struct tt_DataHeader data;
            memcpy(&data, datagram + body, sizeof(data));
            if (data.endpoint_id == tt_DISCOVERY_ENDPOINT_ID && data.entity_id == tt_DISCOVERY_ENTITY_ID) {
                memset(datagram + body + header + offsetof(struct tt_AnnounceHeader, marks), 0, sizeof(uint16_t));
            }
        }
        if (sub_header.length == 0U) {
            break;
        }
        pos += sub_header.length;
    }
}

#endif
