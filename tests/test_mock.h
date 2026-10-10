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
    .addr = _tt_CONTEXT_ADDRESS,
    .port = _tt_CONTEXT_PORT,
    .broadcast = _tt_CONTEXT_BROADCAST,
    .context_id = tt_CONTEXT_ID_INVALID, // unused by the mock HAL (test_mock_node_id drives it instead)
};

// Defaults: a quiet node - no incoming packets, sends "succeed", clock starts at 0.
uint64_t test_mock_now = 0;
uint64_t test_mock_cpu_ns = 0;        // the polling thread's CPU clock (tt_thread_cpu_ns()), set as test_mock_now is
uint64_t test_mock_clock_step_ns = 0; // every clock read lets this much time pass: a spin on the clock ends
uint64_t test_mock_timer_resolution_ns = 1; // what tt_timer_resolution_ns() reports: the floor of measured lateness
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
// tt_Context_poll() decides to wait. And whether a timed-out wait moves the clock by that much: off by
// default, because most tests rely on the clock standing still, and without it a positive-timeout
// poll that times out never sees time pass.
int64_t test_mock_receive_last_timeout = 0;
int test_mock_receive_call_count = 0;
bool test_mock_receive_advances_clock = false;
uint64_t test_mock_receive_late_ns = 0; // ... and then this much more: how late the timer runs
// A backstop for a test whose failure mode is a loop that never returns: past this many calls
// tt_receive() reports an interrupt, so the regression shows up as a failed assertion rather than a
// hung test binary. 0 (the default) = no limit.
int test_mock_receive_limit = 0;
uint64_t test_mock_receive_data_advance_ns = 0;  // a wait that ends with a datagram lets this much time pass
int test_mock_try_receive_remaining = 0;         // tt_try_receive() hands back this many more datagrams ...
int32_t test_mock_try_receive_len = 0;           // ... of this length, whatever the buffer holds ...
uint64_t test_mock_try_receive_advance_ns = 0;   // ... each this much later than the one before
int test_mock_socket_reads_under_lock = 0;       // tt_try_receive() calls past the backlog - a read of the
                                                 // socket, on a real HAL - made holding the node's state lock
void (*test_mock_try_receive_hook)(void) = NULL; // called for each backlog datagram tt_try_receive() hands out
// tt_send_batch_nonblocking() (large-message stage 2): room left in the "send buffer", in datagrams - a batch beyond it
// is cut short there, as a full socket buffer cuts MSG_DONTWAIT short. -1 (the default): unlimited. And its calls.
int32_t test_mock_nonblocking_room = -1;
int test_mock_nonblocking_calls = 0;
#else
extern uint64_t test_mock_now;
extern uint64_t test_mock_cpu_ns;
extern uint64_t test_mock_clock_step_ns;
extern uint64_t test_mock_timer_resolution_ns;
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
extern uint64_t test_mock_receive_late_ns;
extern int test_mock_receive_limit;
extern uint64_t test_mock_receive_data_advance_ns;
extern int test_mock_try_receive_remaining;
extern int32_t test_mock_try_receive_len;
extern uint64_t test_mock_try_receive_advance_ns;
extern int test_mock_socket_reads_under_lock;
extern void (*test_mock_try_receive_hook)(void);
extern int32_t test_mock_nonblocking_room;
extern int test_mock_nonblocking_calls;
#endif

// Call at the start of each test case so one test's overrides can't leak into the next.
static inline void test_mock_reset(void) {
    test_mock_now = 0;
    test_mock_cpu_ns = 0;
    test_mock_clock_step_ns = 0;
    test_mock_timer_resolution_ns = 1;
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
    test_mock_receive_late_ns = 0;
    test_mock_receive_limit = 0;
    test_mock_receive_data_advance_ns = 0;
    test_mock_try_receive_remaining = 0;
    test_mock_try_receive_len = 0;
    test_mock_try_receive_advance_ns = 0;
    test_mock_try_receive_hook = NULL;
    test_mock_socket_reads_under_lock = 0;
    test_mock_nonblocking_room = -1;
    test_mock_nonblocking_calls = 0;
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
    if (len == 0) {
        // A doorbell. There is no header to normalise, and test_classic_form() would read one out of
        // a zero-length buffer to try.
        test_mock_send_last_wire_len = 0;
        test_mock_send_last_len = 0;
        if (test_mock_send_hook != NULL) {
            test_mock_send_hook(buf, len);
        }
        return;
    }
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
    test_mock_now += test_mock_clock_step_ns;
    return test_mock_now;
}

uint64_t tt_timer_resolution_ns(void) {
    return test_mock_timer_resolution_ns;
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

tt_ret_t tt_bind(struct tt_Context* node) {
    (void)node;
    return test_mock_bind_return;
}

void tt_close(struct tt_Context* node) {
    (void)node;
}

tt_ret_t tt_wake_signal(struct tt_Context* node) {
    (void)node;
    test_mock_wake_signal_call_count++;
    // Nothing actually blocks in this mock's tt_receive() (it's a canned return value, not a
    // real wait) - a test exercising tt_Context_interrupt()'s effect on tt_Context_poll() drives that
    // directly by setting test_mock_receive_return = -3 instead.
    return tt_RET_OK;
}

#if tt_CONTEXT_ID_CLAIM
// (g8) The mock's host registry: the ids held in this process, which every context here shares, as contexts on one
// host share /dev/shm. A test sets test_mock_ids_held[] to stand for other processes.
bool test_mock_ids_held[tt_MAX_CONTEXT_IDS];

static bool test_mock_avoided(const uint8_t* avoid, uint32_t id) {
    return avoid != NULL && ((avoid[id / 8] >> (id % 8)) & 1U) != 0;
}

// The same choice hal_linux.c's registry makes.
uint8_t tt_claim_context_id(struct tt_Context* node, uint8_t preferred, const uint8_t* avoid, uint32_t salt) {
    uint8_t id = tt_CONTEXT_ID_INVALID;
    if (preferred > 0 && preferred < 255 && !test_mock_ids_held[preferred] && !test_mock_avoided(avoid, preferred)) {
        id = preferred;
    } else {
        uint32_t free_count = 0;
        for (uint32_t i = 254; i > 0; i--) {
            free_count += !test_mock_ids_held[i] && !test_mock_avoided(avoid, i) ? 1U : 0U;
        }
        uint32_t skip = free_count > 0 && salt != 0 ? salt % free_count : 0;
        for (uint32_t i = 254; i > 0 && free_count > 0; i--) {
            if (!test_mock_ids_held[i] && !test_mock_avoided(avoid, i) && skip-- == 0) {
                id = (uint8_t)i;
                break;
            }
        }
    }
    if (id != tt_CONTEXT_ID_INVALID) {
        if (node->hal.claimed_id != tt_CONTEXT_ID_INVALID) {
            test_mock_ids_held[node->hal.claimed_id] = false;
        }
        test_mock_ids_held[id] = true;
        node->hal.claimed_id = id;
    }
    return id;
}

bool tt_is_own_address(const struct tt_Context* node, uint32_t ip, uint16_t port) {
    return ip == node->hal.own_ip && port == node->hal.own_port;
}

void tt_own_address(const struct tt_Context* node, uint32_t* ip, uint16_t* port) {
    *ip = node->hal.own_ip;
    *port = node->hal.own_port;
}
#endif

int32_t tt_send(struct tt_Context* node, const void* buf, size_t len) {
    (void)node;
#if tt_DISCOVERY_OPTIONS
    if (_tt_CONFIG.discovery_range == tt_DISCOVERY_RANGE_OFF) {
        return (int32_t)len; // as hal_linux.c: discovery off sends nothing
    }
#endif
#if tt_CONTEXT_ID_CLAIM
    if (node->id_muted) {
        node->id_muted_drops++; // as hal_linux.c: a context without an id of its own sends nothing, and fails
        return -1;
    }
#endif

    test_mock_send_call_count++;
    test_mock_capture_send(buf, len);

    if (test_mock_send_return_override) {
        return test_mock_send_return;
    }

    return (int32_t)len;
}

int32_t tt_send_to(struct tt_Context* node, const void* buf, size_t len, uint32_t ip, uint16_t port) {
    (void)node;
    (void)buf;
#if tt_DISCOVERY_OPTIONS
    if (_tt_CONFIG.discovery_range == tt_DISCOVERY_RANGE_OFF) {
        return (int32_t)len;
    }
#endif
#if tt_CONTEXT_ID_CLAIM
    if (node->id_muted) {
        node->id_muted_drops++; // as hal_linux.c: a context without an id of its own sends nothing, and fails
        return -1;
    }
#endif

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

int32_t tt_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout) {
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
        test_mock_now += (uint64_t)timeout + test_mock_receive_late_ns; // the whole wait, and the timer's lateness
    }
    if (test_mock_receive_return >= 0) {
        test_mock_now += test_mock_receive_data_advance_ns;
    }
    return test_mock_receive_return;
}

int32_t tt_send_iov(struct tt_Context* node, const void* hdr, size_t hdr_len, const void* body, size_t body_len,
                    uint32_t ip, uint16_t port) {
    (void)node;
#if tt_DISCOVERY_OPTIONS
    if (_tt_CONFIG.discovery_range == tt_DISCOVERY_RANGE_OFF) {
        return (int32_t)(hdr_len + body_len);
    }
#endif
#if tt_CONTEXT_ID_CLAIM
    if (node->id_muted) {
        node->id_muted_drops++; // as hal_linux.c: a context without an id of its own sends nothing, and fails
        return -1;
    }
#endif

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
int32_t tt_send_batch(struct tt_Context* node, const struct tt_OutDatagram* datagrams, uint32_t count) {
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

// As tt_send_batch(), cut short at test_mock_nonblocking_room datagrams when that is not -1 - a full send buffer.
int32_t tt_send_batch_nonblocking(struct tt_Context* node, const struct tt_OutDatagram* datagrams, uint32_t count) {
    test_mock_nonblocking_calls++;
    uint32_t fits = count;
    if (test_mock_nonblocking_room >= 0 && (uint32_t)test_mock_nonblocking_room < fits) {
        fits = (uint32_t)test_mock_nonblocking_room;
    }
    if (fits == 0) {
        return 0;
    }
    int32_t sent = tt_send_batch(node, datagrams, fits);
    if (sent < 0) {
        return sent;
    }
    if (test_mock_nonblocking_room >= 0) {
        test_mock_nonblocking_room -= (int32_t)fits;
    }
    return (int32_t)fits;
}

// The mock's backlog counts as held: tt_try_receive() hands it out with nothing to wait for.
//
// **THIS DOES NOT MEAN WHAT THE LINUX HAL'S tt_rx_buffered() MEANS, and core logic that branches on
// it cannot be tested here.** Here it is kernel-side availability - datagrams the mock will hand out.
// In hal_linux.c it is `rx_count - rx_next`: datagrams ALREADY PULLED into our own batch by a
// previous recvmmsg(), with rx_count assigned in exactly one place, after a receive that read
// something. The two are near-opposites on the case that matters: a process that has not received
// yet sees a non-zero count here and zero there.
//
// That is not hypothetical. On 2026-09-30 a liveliness fix deferred judgement while
// tt_rx_buffered() > 0, to stop a descheduled node declaring peers dead whose datagrams were already
// waiting. It passed its tests here and was a no-op in production, because on a starved wake-up the
// batch is empty and the kernel holds everything. Four mutants died against a condition that cannot
// be true in the thing being fixed. The replacement (unobserved_ns in check_liveliness()) asks how
// late the scheduler entry is instead, which needs no HAL query and so cannot be faked by a mock.
//
// So: if a change in core reads this function to decide something, the test that covers it belongs
// somewhere the real HAL runs - or the decision belongs on a quantity the mock cannot misrepresent.
uint32_t tt_rx_buffered(const struct tt_Context* node) {
    (void)node;
    return test_mock_try_receive_remaining > 0 ? (uint32_t)test_mock_try_receive_remaining : 0U;
}

// No cheaper way to know than reading, so "may be" - the behaviour before tt_rx_maybe_ready() existed.
bool tt_rx_maybe_ready(struct tt_Context* node) {
    (void)node;
    return true;
}

int32_t tt_try_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    (void)node;
    (void)buf;
    (void)len;

    *ip = 0;
    *port = 0;

    // By default the mock feeds at most the one datagram test_mock_receive_return describes, via tt_receive()
    // above - tt_Context_poll()'s drain loop then immediately sees "nothing more waiting" here and stops. A test
    // that needs a backlog sets test_mock_try_receive_remaining: that many more, the buffer left as it is.
    if (test_mock_try_receive_remaining > 0) {
        test_mock_try_receive_remaining--;
        test_mock_now += test_mock_try_receive_advance_ns;
        if (test_mock_try_receive_hook != NULL) {
            test_mock_try_receive_hook();
        }
        return test_mock_try_receive_len;
    }
    if (__atomic_load_n(&node->state_owner, __ATOMIC_RELAXED) == tt_thread_self()) {
        test_mock_socket_reads_under_lock++;
    }
    return -1;
}

#if tt_SEGMENT_ENABLED
// The segment's platform half, mocked (SHM_PLAN.md stage 1). Regions live in ordinary heap memory
// and are found by name, which is all core asks of them: the ring, the naming and the header checks
// are core's, so a test driving two contexts through this mock exercises the same code a real
// mapping would - only the pages differ.
//
// Deliberately NOT a no-op returning NULL. A mock that could never attach would make every shm test
// pass by falling back to UDP, which is the failure these tests exist to detect.
#define TEST_MOCK_MAX_SEGMENTS 8

struct test_mock_segment {
    char path[tt_SEGMENT_PATH_LENGTH];
    void* region;
    size_t bytes;
    bool present;  // unlinked segments stay mapped for whoever holds them, as a real one would
    bool detached; // cleared on create/attach: a second detach with no attach between is the defect
};

struct test_mock_segment test_mock_segments[TEST_MOCK_MAX_SEGMENTS];
int test_mock_segment_creates = 0;
int test_mock_segment_attaches = 0;
// Every call to tt_segment_attach(), including the ones that find nothing. The successful ones are
// counted above; this one exists because the cost of a *failed* attach is what a peer on another
// host pays, and it was being paid once per datagram. A count that only rose on success could not
// see that at all.
int test_mock_segment_attach_calls = 0;
int test_mock_segment_double_detaches = 0;
// Detaches of a region the mock never handed out: on Linux, an munmap() of whatever the pointer held - an entry read
// before it was set up (segment_peer()) is how one happens.
int test_mock_segment_stray_detaches = 0;
// Unlinks, so that "the segment was released" can be asserted on the file being taken away and not
// only on a pointer going NULL. Lazy release has to do both, and a release that forgot the unlink
// would leave a file in /dev/shm for good while every pointer assertion still passed.
int test_mock_segment_unlinks = 0;

static struct test_mock_segment* test_mock_find_segment(const char* path) {
    for (int i = 0; i < TEST_MOCK_MAX_SEGMENTS; i++) {
        if (test_mock_segments[i].region != NULL && test_mock_segments[i].present &&
            strcmp(test_mock_segments[i].path, path) == 0) {
            return &test_mock_segments[i];
        }
    }
    return NULL;
}

void* tt_segment_create(const char* path, size_t bytes) {
    struct test_mock_segment* existing = test_mock_find_segment(path);
    if (existing != NULL) {
        existing->present = false; // replaced, as a real create unlinks first
    }
    for (int i = 0; i < TEST_MOCK_MAX_SEGMENTS; i++) {
        if (test_mock_segments[i].region == NULL) {
            test_mock_segments[i].region = calloc(1, bytes);
            if (test_mock_segments[i].region == NULL) {
                return NULL;
            }
            snprintf(test_mock_segments[i].path, sizeof(test_mock_segments[i].path), "%s", path);
            test_mock_segments[i].bytes = bytes;
            test_mock_segments[i].present = true;
            test_mock_segments[i].detached = false;
            test_mock_segment_creates++;
            return test_mock_segments[i].region;
        }
    }
    return NULL;
}

void* tt_segment_attach(const char* path, size_t bytes, uint8_t* why) {
    test_mock_segment_attach_calls++;
    struct test_mock_segment* found = test_mock_find_segment(path);
    if (found == NULL) {
        *why = (uint8_t)tt_SEGMENT_ABSENT;
        return NULL;
    }
    if (found->bytes < bytes) {
        *why = (uint8_t)tt_SEGMENT_BAD_HEADER;
        return NULL;
    }
    *why = (uint8_t)tt_SEGMENT_ATTACHED;
    found->detached = false;
    test_mock_segment_attaches++;
    return found->region; // one region, two users - which is the point
}

// The region outlives its attachers here, as a real mapping's file does - but a *second* detach of
// the same pointer without a re-attach is recorded, because that is the one thing a no-op cannot
// otherwise reproduce. A real munmap makes the page inaccessible, so a double detach followed by any
// read is a segfault; on 2026-09-29 that crashed every node in the integration suite at teardown
// while every unit test here stayed green.
void tt_segment_detach(void* mapping, size_t bytes) {
    (void)bytes;
    for (int i = 0; i < TEST_MOCK_MAX_SEGMENTS; i++) {
        if (test_mock_segments[i].region == mapping) {
            if (test_mock_segments[i].detached) {
                test_mock_segment_double_detaches++;
            }
            test_mock_segments[i].detached = true;
            return;
        }
    }
    test_mock_segment_stray_detaches++;
}

// No doorbell FIFO in the mock: peers ring over (mock) UDP, as with a platform that has none.
int32_t tt_segment_bell_create(struct tt_Context* node, const char* path) {
    (void)node;
    (void)path;
    return -1;
}
void tt_segment_bell_destroy(struct tt_Context* node, const char* path) {
    (void)node;
    (void)path;
}
int32_t tt_segment_bell_open(const char* path) {
    (void)path;
    return -1;
}
void tt_segment_bell_ring(int32_t bell) {
    (void)bell;
}
void tt_segment_bell_close(int32_t bell) {
    (void)bell;
}
uint64_t tt_thread_cpu_ns(void) {
    return test_mock_cpu_ns;
}

void tt_segment_unlink(const char* path) {
    test_mock_segment_unlinks++;
    struct test_mock_segment* found = test_mock_find_segment(path);
    if (found != NULL) {
        found->present = false;
    }
}

// Frees every region. Called by a test that creates segments; test_mock_reset() does not, because a
// context may still hold a mapping when it runs.
static void test_mock_segments_free(void) {
    for (int i = 0; i < TEST_MOCK_MAX_SEGMENTS; i++) {
        free(test_mock_segments[i].region);
        test_mock_segments[i].region = NULL;
        test_mock_segments[i].present = false;
        test_mock_segments[i].detached = false;
    }
    test_mock_segment_creates = 0;
    test_mock_segment_attaches = 0;
    test_mock_segment_attach_calls = 0;
    test_mock_segment_double_detaches = 0;
    test_mock_segment_stray_detaches = 0;
    test_mock_segment_unlinks = 0;
}
#endif

#endif
