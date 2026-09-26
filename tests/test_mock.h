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

// Fake HAL + clock so tests can #include "../src/tickle.c" directly (to reach its static
// functions) and link without pulling in the real hal_linux.c - no socket, no real time.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/tickle.h>

// Defined in exactly one place per test binary (see test_common.h's DEFINE_STORAGE convention).
#ifdef TEST_MOCK_DEFINE_STORAGE
bool test_mock_link_resolves = false;
uint32_t test_mock_link_addr = 0;
uint32_t test_mock_link_netmask = 0;
int32_t test_mock_link_mtu = -1;
#else
extern bool test_mock_link_resolves;
extern uint32_t test_mock_link_addr;
extern uint32_t test_mock_link_netmask;
extern int32_t test_mock_link_mtu;
#endif
#ifdef TEST_MOCK_DEFINE_STORAGE
struct _tt_Config _tt_CONFIG = {
    .addr = _tt_NODE_ADDRESS,
    .port = _tt_NODE_PORT,
    .broadcast = _tt_NODE_BROADCAST,
    .node_id = tt_NODE_ID_INVALID, // unused by the mock HAL (test_mock_node_id drives it instead)
};

// Defaults: a quiet node - no incoming packets, sends "succeed", clock starts at 0.
uint64_t test_mock_now = 0;
int32_t test_mock_node_id = 1;
tt_ret_t test_mock_bind_return = tt_RET_OK;
int32_t test_mock_receive_return = -1;
bool test_mock_send_return_override = false;
int32_t test_mock_send_return = 0;
int test_mock_send_call_count = 0;
// tt_send_to() also bumps test_mock_send_call_count above (a test checking "was a response sent"
// shouldn't need to care which of the two actually carried it) - these track the unicast call
// specifically, for a test that cares whether the destination was right.
int test_mock_send_to_call_count = 0;
// tt_send_batch() calls - the system calls a batch costs on Linux - where test_mock_send_call_count still counts
// each datagram in it, through the per-datagram functions a batch is dispatched to.
int test_mock_send_batch_call_count = 0;
uint32_t test_mock_send_to_last_ip = 0;
// Every destination this run sent to, in order. The last_ip/last_port pair answers "where did that
// one go"; a per-link test needs "where did all of them go", because the whole point is that one
// buffer is addressed differently on different links and only the set shows that.
#define TEST_MOCK_MAX_SENDS 16
uint32_t test_mock_send_to_ips[TEST_MOCK_MAX_SENDS] = {0};
int test_mock_send_to_ip_count = 0;
uint16_t test_mock_send_to_last_port = 0;
// Copy of the most recent packet either send function was handed (truncated to the buffer size) -
// for a test that needs to decode what was actually sent, not just count sends. Always in the classic form
// (tt_Header + tt_SubmessageHeader), which every decoder in the tests reads: a datagram sent in the
// single-submessage form (struct tt_SingleHeader, tt_VERSION 10) is rewritten into it, 4 bytes longer, and
// test_mock_send_last_wire_len says how long it really was on the wire.
uint8_t test_mock_send_last_buf[tt_MAX_BUFFER_LENGTH + sizeof(struct tt_Header)];
size_t test_mock_send_last_len = 0;
size_t test_mock_send_last_wire_len = 0;
// Optional: called with every packet either send function is handed, untruncated - for a test that
// needs all of several datagrams, not just the last one. NULL (the default) = not called.
void (*test_mock_send_hook)(const void* buf, size_t len) = NULL;
int test_mock_wake_signal_call_count = 0;
// What tt_receive() was last asked to wait, and how often it was asked - for a test about how long
// tt_Node_poll() decides to wait. And whether a timed-out wait moves the clock by that much: off by
// default, because most tests rely on the clock standing still, and without it a positive-timeout
// poll that times out never sees time pass.
int64_t test_mock_receive_last_timeout = 0;
int test_mock_receive_call_count = 0;
bool test_mock_receive_advances_clock = false;
// A backstop for a test whose failure mode is a loop that never returns: past this many calls
// tt_receive() reports an interrupt, so the regression shows up as a failed assertion rather than a
// hung test binary. 0 (the default) = no limit.
int test_mock_receive_limit = 0;
#else
extern uint64_t test_mock_now;
extern int32_t test_mock_node_id;
extern tt_ret_t test_mock_bind_return;
extern int32_t test_mock_receive_return;
extern bool test_mock_send_return_override;
extern int32_t test_mock_send_return;
extern int test_mock_send_call_count;
extern int test_mock_send_to_call_count;
extern int test_mock_send_batch_call_count;
extern uint32_t test_mock_send_to_last_ip;
#define TEST_MOCK_MAX_SENDS 16
extern uint32_t test_mock_send_to_ips[TEST_MOCK_MAX_SENDS];
extern int test_mock_send_to_ip_count;
extern uint16_t test_mock_send_to_last_port;
extern uint8_t test_mock_send_last_buf[tt_MAX_BUFFER_LENGTH];
extern void (*test_mock_send_hook)(const void* buf, size_t len);
extern size_t test_mock_send_last_wire_len;
extern size_t test_mock_send_last_len;
extern int test_mock_wake_signal_call_count;
extern int64_t test_mock_receive_last_timeout;
extern int test_mock_receive_call_count;
extern bool test_mock_receive_advances_clock;
extern int test_mock_receive_limit;
#endif

// Call at the start of each test case so one test's overrides can't leak into the next.
static inline void test_mock_reset(void) {
    test_mock_now = 0;
    test_mock_node_id = 1;
    test_mock_bind_return = tt_RET_OK;
    test_mock_receive_return = -1;
    test_mock_send_return_override = false;
    test_mock_send_return = 0;
    test_mock_send_call_count = 0;
    test_mock_send_to_call_count = 0;
    test_mock_send_batch_call_count = 0;
    test_mock_send_to_last_ip = 0;
    test_mock_send_to_ip_count = 0;
    for (int i = 0; i < TEST_MOCK_MAX_SENDS; i++) {
        test_mock_send_to_ips[i] = 0;
    }
    test_mock_send_to_last_port = 0;
    test_mock_send_last_len = 0;
    test_mock_send_hook = NULL;
    test_mock_send_last_wire_len = 0;
    test_mock_wake_signal_call_count = 0;
    test_mock_receive_last_timeout = 0;
    test_mock_receive_call_count = 0;
    test_mock_receive_advances_clock = false;
    test_mock_receive_limit = 0;
}

// A datagram as sent, in the classic form a test's decoder reads: one in the single-submessage form
// (struct tt_SingleHeader, tt_VERSION 10) gets its tt_Header and tt_SubmessageHeader back, 4 bytes longer. `out`
// holds at least len + sizeof(struct tt_Header) bytes. Returns the classic length.
static inline size_t test_classic_form(const void* datagram, size_t len, uint8_t* out) {
    const uint8_t* in = (const uint8_t*)datagram;
    if (len < sizeof(struct tt_SingleHeader) || (in[0] != tt_SINGLE_MARKER_LE && in[0] != tt_SINGLE_MARKER_BE)) {
        memcpy(out, in, len);
        return len;
    }
    const struct tt_SingleHeader* single = (const struct tt_SingleHeader*)in;
    struct tt_Header header;
    uint16_t native = NATIVE_MAGIC_VALUE;
    uint8_t native_first = 0;
    memcpy(&native_first, &native, 1);
    bool is_native = single->marker == (uint8_t)(native_first | tt_SINGLE_MARKER_FLAG);
    header.magic_value = is_native ? NATIVE_MAGIC_VALUE : REVERSE_MAGIC_VALUE;
    header.version = single->version;
    header.source = single->source;
    uint16_t length = (uint16_t)len; // the submessage: its own header, plus everything after the single header
    struct tt_SubmessageHeader submessage = {single->type, tt_SUBMESSAGE_ID_ALL,
                                             is_native ? length : (uint16_t)((length >> 8) | (length << 8))};
    memcpy(out, &header, sizeof(header));
    memcpy(out + sizeof(header), &submessage, sizeof(submessage));
    memcpy(out + sizeof(header) + sizeof(submessage), in + sizeof(*single), len - sizeof(*single));
    return len + sizeof(struct tt_Header);
}

#ifdef TEST_MOCK_DEFINE_STORAGE
static void test_mock_capture_send(const void* buf, size_t len) {
    uint8_t classic[tt_MAX_BUFFER_LENGTH * 2 + sizeof(struct tt_Header)];
    size_t classic_len = len < tt_MAX_BUFFER_LENGTH * 2 ? test_classic_form(buf, len, classic) : len;
    test_mock_send_last_wire_len = len;
    test_mock_send_last_len =
        classic_len < sizeof(test_mock_send_last_buf) ? classic_len : sizeof(test_mock_send_last_buf);
    memcpy(test_mock_send_last_buf, len < tt_MAX_BUFFER_LENGTH * 2 ? classic : (const uint8_t*)buf,
           test_mock_send_last_len);
    if (test_mock_send_hook != NULL) {
        test_mock_send_hook(buf, len);
    }
}

// These replace the real platform HAL symbols (normally hal_linux.c) in a test binary.
uint64_t tt_get_ns(void) {
    return test_mock_now;
}

int32_t tt_get_node_id(void) {
    return test_mock_node_id;
}

// Link resolution, mocked as "nothing is configured" by default. Tests that care set
// test_mock_link_* first; everything else gets false, which is the same answer the real HAL gives
// for a limited broadcast, so an unconfigured test behaves exactly as an unconfigured node does.
bool tt_resolve_link(const char* broadcast, uint32_t* addr, uint32_t* netmask, uint32_t* bcast) {
    (void)broadcast;
    if (!test_mock_link_resolves) {
        return false;
    }
    *addr = test_mock_link_addr;
    *netmask = test_mock_link_netmask;
    *bcast = test_mock_link_addr | ~test_mock_link_netmask;
    return true;
}

int32_t tt_link_mtu(uint32_t addr) {
    (void)addr;
    return test_mock_link_mtu;
}

tt_ret_t tt_bind(struct tt_Node* node) {
    (void)node;
    return test_mock_bind_return;
}

void tt_close(struct tt_Node* node) {
    (void)node;
}

tt_ret_t tt_wake_signal(struct tt_Node* node) {
    (void)node;
    test_mock_wake_signal_call_count++;
    // Nothing actually blocks in this mock's tt_receive() (it's a canned return value, not a
    // real wait) - a test exercising tt_Node_interrupt()'s effect on tt_Node_poll() drives that
    // directly by setting test_mock_receive_return = -3 instead.
    return tt_RET_OK;
}

int32_t tt_send(struct tt_Node* node, const void* buf, size_t len) {
    (void)node;

    test_mock_send_call_count++;
    test_mock_capture_send(buf, len);

    if (test_mock_send_return_override) {
        return test_mock_send_return;
    }

    return (int32_t)len;
}

int32_t tt_send_to(struct tt_Node* node, const void* buf, size_t len, uint32_t ip, uint16_t port) {
    (void)node;
    (void)buf;

    test_mock_send_call_count++;
    test_mock_send_to_call_count++;
    test_mock_capture_send(buf, len);
    test_mock_send_to_last_ip = ip;
    test_mock_send_to_last_port = port;
    if (test_mock_send_to_ip_count < TEST_MOCK_MAX_SENDS) {
        test_mock_send_to_ips[test_mock_send_to_ip_count++] = ip;
    }

    if (test_mock_send_return_override) {
        return test_mock_send_return;
    }

    return (int32_t)len;
}

int32_t tt_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout) {
    (void)node;
    (void)buf;
    (void)len;

    *ip = 0;
    *port = 0;

    test_mock_receive_last_timeout = timeout;
    test_mock_receive_call_count++;
    if (test_mock_receive_limit > 0 && test_mock_receive_call_count > test_mock_receive_limit) {
        return -3;
    }
    if (test_mock_receive_advances_clock && test_mock_receive_return == -1 && timeout > 0) {
        test_mock_now += (uint64_t)timeout; // the whole wait elapsed with nothing arriving
    }
    return test_mock_receive_return;
}

int32_t tt_send_iov(struct tt_Node* node, const void* hdr, size_t hdr_len, const void* body, size_t body_len,
                    uint32_t ip, uint16_t port) {
    (void)node;

    // Captured as the one datagram the two pieces make, like the other send functions' buffers. Static:
    // a fragment's pieces are small, but a zero-copy body may be a whole datagram of 64 KB.
    static uint8_t joined[65536];
    if (hdr_len + body_len <= sizeof(joined)) {
        memcpy(joined, hdr, hdr_len);
        memcpy(joined + hdr_len, body, body_len);
        test_mock_capture_send(joined, hdr_len + body_len);
    }

    test_mock_send_call_count++;
    if (ip != 0) {
        test_mock_send_to_call_count++;
        test_mock_send_to_last_ip = ip;
        test_mock_send_to_last_port = port;
        if (test_mock_send_to_ip_count < TEST_MOCK_MAX_SENDS) {
            test_mock_send_to_ips[test_mock_send_to_ip_count++] = ip;
        }
    }

    if (test_mock_send_return_override) {
        return test_mock_send_return;
    }
    return (int32_t)(hdr_len + body_len);
}

// One call, dispatched to the per-datagram mocks above so that every counter and capture sees each datagram
// exactly as it would have seen it sent alone - a test that counts tt_send() against tt_send_to() still can.
int32_t tt_send_batch(struct tt_Node* node, const struct tt_OutDatagram* datagrams, uint32_t count) {
    test_mock_send_batch_call_count++;
    for (uint32_t i = 0; i < count; i++) {
        const struct tt_OutDatagram* datagram = &datagrams[i];
        int32_t result;
        if (datagram->body_len != 0) {
            result = tt_send_iov(node, datagram->head, datagram->head_len, datagram->body, datagram->body_len,
                                 datagram->ip, datagram->port);
        } else if (datagram->ip == 0) {
            result = tt_send(node, datagram->head, datagram->head_len);
        } else {
            result = tt_send_to(node, datagram->head, datagram->head_len, datagram->ip, datagram->port);
        }
        if (result < 0) {
            return result;
        }
    }
    return (int32_t)count;
}

int32_t tt_try_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    (void)node;
    (void)buf;
    (void)len;

    *ip = 0;
    *port = 0;

    // The mock feeds at most the one datagram test_mock_receive_return describes, via tt_receive()
    // above - tt_Node_poll()'s drain loop then immediately sees "nothing more waiting" here and
    // stops, so no whitebox test needs to model a multi-packet kernel backlog.
    return -1;
}
#endif
