/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// struct tt_InitOptions, bundle 1 (ROADMAP "Context options"): the net options, through tt_Context_create_with() and
// the mock HAL.
//   - every field, set to a value other than its default, changes what the context does (no field written and never
//     read): the destinations it sends to, the id it sends as, the interface it asks for, the segment it builds, what
//     its announce advertises;
//   - every check that replaced a static_assert or a silent clamp refuses a bad value with tt_RET_INVALID_ARGUMENT,
//     and accepts the boundary beside it (tests/mutants_init_options.sh removes each check and expects a failure);
//   - two contexts in one process with different options each keep their own - what the process-wide _tt_CONFIG could
//     not do;
//   - tt_Context_create() still reads _tt_CONFIG, once, into the context's own options, and nothing reads the global
//     after that.
// Built as rmw_tickle builds core (tt_DISCOVERY_OPTIONS, 64 KB datagrams, large samples, so the hybrid transport), so a
// peer link and the discovery range are accepted; tests/test_init_options_noflag.c checks that a build without it
// refuses them. The real HAL's use of addr, broadcast and port is tests/test_init_options_hal.c.

#define tt_DISCOVERY_OPTIONS 1
#define tt_MAX_BUFFER_LENGTH 65507
#define tt_LARGE_SAMPLES 1

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

#ifndef TICKLE_C
#define TICKLE_C "../src/tickle.c"
#endif
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; tests/mutants_init_options.sh passes a mutant

#define AUTO_ID 7 // what the mock's address auto-detection answers
#define LINK_ADDR 0x0a000007U
#define LINK_NETMASK 0xffffff00U
#define PEER_A 0x0a000021U
#define PEER_B 0x0a000022U

static struct tt_Context ctx;
static struct tt_Context other;

static void reset_mock(void) {
    test_mock_reset();
    test_mock_now = 5 * tt_SECOND;
    test_mock_node_id = AUTO_ID;
    test_mock_link_resolves = true; // every link resolves to 10.0.0.7/24, as an interface would
    test_mock_link_addr = LINK_ADDR;
    test_mock_link_netmask = LINK_NETMASK;
    test_mock_link_last_broadcast = NULL;
#if tt_CONTEXT_ID_CLAIM
    memset(test_mock_ids_held, 0, sizeof(test_mock_ids_held));
#endif
}

static tt_ret_t create_with(struct tt_Context* node, const struct tt_InitOptions* opts) {
    memset(node, 0, sizeof(*node));
    return tt_Context_create_with(node, opts);
}

static void destroy(struct tt_Context* node) {
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(node));
}

static struct tt_InitOptions defaults(void) {
    struct tt_InitOptions opts;
    tt_InitOptions_default(&opts);
    return opts;
}

// Two links on the options: the scalar broadcast's, and a second subnet.
static void two_links(struct tt_InitOptions* opts, uint8_t threshold) {
    opts->net.links[0] = (struct tt_LinkOptions) {.broadcast = "10.0.0.255", .unicast_threshold = threshold};
    opts->net.links[1] = (struct tt_LinkOptions) {.broadcast = "10.0.1.255", .unicast_threshold = threshold};
    opts->net.link_count = 2;
}

// ---- tt_InitOptions_default(): every zero resolved, and what tt_Context_create() has always used.

// Field by field: the struct has padding, so its bytes are not its value.
static bool same_net(const struct tt_NetOptions* a, const struct tt_NetOptions* b) {
    if (strcmp(a->addr, b->addr) != 0 || strcmp(a->broadcast, b->broadcast) != 0 || a->port != b->port ||
        a->context_id != b->context_id || a->link_count != b->link_count || a->discovery_range != b->discovery_range ||
        a->segment_slot_bytes != b->segment_slot_bytes || a->large_fragmentation != b->large_fragmentation ||
        a->ip_reassembly_max != b->ip_reassembly_max || a->reassembly_cap_off != b->reassembly_cap_off) {
        return false;
    }
    for (uint8_t i = 0; i < a->link_count; i++) {
        if (strcmp(a->links[i].broadcast, b->links[i].broadcast) != 0 ||
            a->links[i].unicast_threshold != b->links[i].unicast_threshold || a->links[i].peer != b->links[i].peer) {
            return false;
        }
    }
    return true;
}

static void test_defaults_are_the_compiled_settings(void) {
    struct tt_InitOptions opts = defaults();
    EXPECT_TRUE(strcmp(opts.net.addr, _tt_CONTEXT_ADDRESS) == 0);
    EXPECT_TRUE(strcmp(opts.net.broadcast, _tt_CONTEXT_BROADCAST) == 0);
    EXPECT_EQ_U32(_tt_CONTEXT_PORT, opts.net.port);
    EXPECT_EQ_INT(tt_CONTEXT_ID_INVALID, opts.net.context_id);
    EXPECT_EQ_U32(0, opts.net.link_count);
    EXPECT_EQ_U32(tt_DISCOVERY_RANGE_SUBNET, opts.net.discovery_range);
    EXPECT_EQ_U32(0, opts.net.segment_slot_bytes);
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_AUTO, opts.net.large_fragmentation);
    EXPECT_EQ_U32(tt_IPV4_UDP_MAX_PAYLOAD, opts.net.ip_reassembly_max); // Linux's _tt_IP_REASSEMBLY_MAX
    EXPECT_EQ_U32(0, opts.net.reassembly_cap_off);

    // A zeroed struct and NULL read the same: the context's resolved options are the defaults.
    reset_mock();
    struct tt_InitOptions zero;
    memset(&zero, 0, sizeof(zero));
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, &zero));
    EXPECT_TRUE(same_net(&ctx.opts.net, &opts.net));
    destroy(&ctx);
    reset_mock();
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, NULL));
    EXPECT_TRUE(same_net(&ctx.opts.net, &opts.net));
    EXPECT_EQ_U32(1, ctx.link_count); // one link, built from the scalar settings
    EXPECT_EQ_U32(tt_UNICAST_PEER_THRESHOLD, ctx.links[0].unicast_threshold);
    destroy(&ctx);
}

// ---- Each field, set, changes behaviour.

// broadcast: the interface the context's link is resolved against.
static void test_broadcast_names_the_link_asked_for(void) {
    reset_mock();
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, NULL));
    EXPECT_TRUE(test_mock_link_last_broadcast != NULL && strcmp(test_mock_link_last_broadcast, "255.255.255.255") == 0);
    destroy(&ctx);

    reset_mock();
    struct tt_InitOptions opts = defaults();
    opts.net.broadcast = "10.0.0.255";
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, &opts));
    EXPECT_TRUE(test_mock_link_last_broadcast != NULL && strcmp(test_mock_link_last_broadcast, "10.0.0.255") == 0);
    EXPECT_TRUE(ctx.links[0].broadcast != NULL && strcmp(ctx.links[0].broadcast, "10.0.0.255") == 0);
    destroy(&ctx);
}

// addr: the default link's send address (the HAL's bind is tests/test_init_options_hal.c).
static void test_addr_is_the_default_links_address(void) {
    reset_mock();
    struct tt_InitOptions opts = defaults();
    opts.net.addr = "10.0.0.7";
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, &opts));
    EXPECT_TRUE(ctx.links[0].addr != NULL && strcmp(ctx.links[0].addr, "10.0.0.7") == 0);
    destroy(&ctx);
}

// context_id: the id the context sends as, instead of the auto-detected one.
static void test_context_id_is_the_source(void) {
    reset_mock();
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, NULL));
    EXPECT_EQ_U32(AUTO_ID, ctx.id);
    destroy(&ctx);
    reset_mock();
    struct tt_InitOptions opts = defaults();
    opts.net.context_id = 42;
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, &opts));
    EXPECT_EQ_U32(42, ctx.id);
    EXPECT_TRUE(build_and_send_update(&ctx, NULL, 0));
    node_flush(&ctx, 0, NULL);
    EXPECT_TRUE(test_mock_send_last_len > sizeof(struct tt_Header));
    EXPECT_EQ_U32(42, ((const struct tt_Header*)test_mock_send_last_buf)->source); // what went out
    destroy(&ctx);
}

// links and link_count: a broadcast goes to every link's broadcast address on the context's port; one link goes
// through the HAL's own broadcast.
static void test_links_and_port_are_the_broadcast_destinations(void) {
    struct tx_destination out[TX_MAX_DESTINATIONS];
    reset_mock();
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, NULL));
    EXPECT_EQ_U32(1, broadcast_destinations(&ctx, out));
    EXPECT_EQ_U32(0, out[0].ip); // the HAL's broadcast
    destroy(&ctx);

    reset_mock();
    struct tt_InitOptions opts = defaults();
    two_links(&opts, 0);
    opts.net.port = 9100;
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, &opts));
    EXPECT_EQ_U32(2, ctx.link_count);
    EXPECT_EQ_U32(2, broadcast_destinations(&ctx, out));
    EXPECT_EQ_U32(LINK_ADDR | ~LINK_NETMASK, out[0].ip); // resolved by the (mock) OS
    EXPECT_EQ_U32(9100, out[0].port);
    EXPECT_EQ_U32(9100, out[1].port);
    EXPECT_EQ_U32(9100, ctx.opts.net.port);
    destroy(&ctx);
}

// unicast_threshold: per link, the count of known peers above which that link gets one broadcast instead.
static void test_unicast_threshold_decides_unicast_or_broadcast(void) {
    struct tt_Peer peers[2] = {{.context_id = 0x21, .ip = PEER_A, .port = 8282},
                               {.context_id = 0x22, .ip = PEER_B, .port = 8282}};
    struct tx_destination out[TX_MAX_DESTINATIONS];

    // Two peers on a link: unicast to both at the default threshold (2) ...
    reset_mock();
    struct tt_InitOptions opts = defaults();
    two_links(&opts, 0);
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, &opts));
    EXPECT_EQ_U32(tt_UNICAST_PEER_THRESHOLD, ctx.opts.net.links[0].unicast_threshold); // 0 resolved
    EXPECT_EQ_U32(2, tx_destinations(&ctx, peers, 2, out));
    EXPECT_EQ_U32(PEER_A, out[0].ip);
    destroy(&ctx);

    // ... one broadcast at threshold 1 ...
    reset_mock();
    two_links(&opts, 1);
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, &opts));
    EXPECT_EQ_U32(1, tx_destinations(&ctx, peers, 2, out));
    EXPECT_EQ_U32(LINK_ADDR | ~LINK_NETMASK, out[0].ip);
    destroy(&ctx);

    // ... and with tt_UNICAST_ALWAYS_BROADCAST even one peer is broadcast to.
    reset_mock();
    two_links(&opts, tt_UNICAST_ALWAYS_BROADCAST);
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, &opts));
    EXPECT_EQ_U32(0, ctx.links[0].unicast_threshold);
    EXPECT_EQ_U32(1, tx_destinations(&ctx, peers, 1, out));
    EXPECT_EQ_U32(LINK_ADDR | ~LINK_NETMASK, out[0].ip);
    destroy(&ctx);
}

// discovery_range: OFF sends nothing to the link; LOCALHOST drops a sender from elsewhere.
static void test_discovery_range_limits_the_link(void) {
    reset_mock();
    struct tt_InitOptions opts = defaults();
    opts.net.discovery_range = tt_DISCOVERY_RANGE_OFF;
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, &opts));
    int sent = test_mock_send_call_count;
    EXPECT_TRUE(build_and_send_update(&ctx, NULL, 0));
    node_flush(&ctx, 0, NULL);
    EXPECT_EQ_INT(sent, test_mock_send_call_count);
    destroy(&ctx);

    // The control: the default range sends it.
    reset_mock();
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, NULL));
    sent = test_mock_send_call_count;
    EXPECT_TRUE(build_and_send_update(&ctx, NULL, 0));
    node_flush(&ctx, 0, NULL);
    EXPECT_TRUE(test_mock_send_call_count > sent);
    destroy(&ctx);

    reset_mock();
    opts.net.discovery_range = tt_DISCOVERY_RANGE_LOCALHOST;
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, &opts));
    EXPECT_TRUE(!sender_in_range(&ctx, 0x08080808U));
    EXPECT_TRUE(sender_in_range(&ctx, 0x7f000001U));
    destroy(&ctx);
}

#if tt_SEGMENT_ENABLED
// segment_slot_bytes: the slot size of the segment the context builds.
static void test_segment_slot_bytes_is_the_ring_built(void) {
    const uint32_t configured = (uint32_t)tt_SEGMENT_SLOT_BYTES / 2U;
    reset_mock();
    struct tt_InitOptions opts = defaults();
    opts.net.segment_slot_bytes = configured;
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, &opts));
    ctx.hal.own_ip = LINK_ADDR;
    ctx.hal.own_port = 40000;
    create_own_segment(&ctx);
    EXPECT_TRUE(ctx.own_segment != NULL);
    if (ctx.own_segment != NULL) {
        EXPECT_EQ_U32(configured, ctx.own_segment->slot_bytes);
    }
    release_own_segment(&ctx);
    test_mock_segments_free();
    destroy(&ctx);
}
#endif

// large_fragmentation: the mode a Publisher that inherits sends with.
static void test_large_fragmentation_is_the_contexts_mode(void) {
    reset_mock();
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, NULL));
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_AUTO, ctx.large_fragmentation);
    destroy(&ctx);
    reset_mock();
    struct tt_InitOptions opts = defaults();
    opts.net.large_fragmentation = tt_LARGE_FRAGMENTATION_MTU;
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, &opts));
    EXPECT_EQ_U32(tt_LARGE_FRAGMENTATION_MTU, ctx.large_fragmentation);
    destroy(&ctx);
}

// ip_reassembly_max: what the announce advertises (capped by this build's receive buffer); NONE advertises 0.
static void test_ip_reassembly_max_is_advertised(void) {
    reset_mock();
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, NULL));
    EXPECT_EQ_U32(advertised_ip_reassembly_max(tt_IPV4_UDP_MAX_PAYLOAD), ctx.ip_reassembly_max);
    destroy(&ctx);
    reset_mock();
    struct tt_InitOptions opts = defaults();
    opts.net.ip_reassembly_max = 1000;
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, &opts));
    EXPECT_EQ_U32(1000, ctx.ip_reassembly_max);
    destroy(&ctx);
    reset_mock();
    opts.net.ip_reassembly_max = tt_IP_REASSEMBLY_NONE;
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, &opts));
    EXPECT_EQ_U32(0, ctx.ip_reassembly_max);
    destroy(&ctx);
}

// reassembly_cap_off: the reader advertises its ceiling uncapped.
static void test_reassembly_cap_off_switches_the_cap_off(void) {
    reset_mock();
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, NULL));
    EXPECT_TRUE(!ctx.hybrid.cap_off);
    destroy(&ctx);
    reset_mock();
    struct tt_InitOptions opts = defaults();
    opts.net.reassembly_cap_off = 1;
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, &opts));
    EXPECT_TRUE(ctx.hybrid.cap_off);
    destroy(&ctx);
}

// ---- Validation: each refused, and the boundary beside it accepted.

static void expect_refused(const struct tt_InitOptions* opts, const char* what) {
    reset_mock();
    tt_ret_t ret = create_with(&ctx, opts);
    if (ret != tt_RET_INVALID_ARGUMENT) {
        fprintf(stderr, "not refused: %s (returned %d)\n", what, (int)ret);
        test_failures++;
    }
    if (ret == tt_RET_OK) {
        destroy(&ctx);
    }
}

static void expect_accepted(const struct tt_InitOptions* opts, const char* what) {
    reset_mock();
    tt_ret_t ret = create_with(&ctx, opts);
    if (ret != tt_RET_OK) {
        fprintf(stderr, "refused: %s (returned %d)\n", what, (int)ret);
        test_failures++;
        return;
    }
    destroy(&ctx);
}

static void test_validation(void) {
    struct tt_InitOptions opts = defaults();
    opts.net.addr = "10.0.0";
    expect_refused(&opts, "net.addr of three octets");
    opts.net.addr = "10.0.0.256";
    expect_refused(&opts, "net.addr with an octet above 255");
    opts.net.addr = "10.0.0.7";
    expect_accepted(&opts, "net.addr 10.0.0.7");

    opts = defaults();
    opts.net.broadcast = "broadcast";
    expect_refused(&opts, "net.broadcast not an address");
    opts.net.broadcast = "10.0.0.255/24";
    expect_refused(&opts, "net.broadcast with a prefix");

    opts = defaults();
    for (uint8_t i = 0; i < tt_MAX_LINK_COUNT; i++) {
        opts.net.links[i] = (struct tt_LinkOptions) {.broadcast = "10.0.0.255"};
    }
    opts.net.link_count = tt_MAX_LINK_COUNT;
    expect_accepted(&opts, "net.link_count = tt_MAX_LINK_COUNT");
    opts.net.link_count = tt_MAX_LINK_COUNT + 1; // every link it can hold valid: only the count is wrong
    expect_refused(&opts, "net.link_count above tt_MAX_LINK_COUNT");

    opts = defaults();
    two_links(&opts, 0);
    opts.net.links[1].broadcast = NULL;
    expect_refused(&opts, "net.links[1].broadcast NULL");
    opts.net.links[1].broadcast = "10.0.1";
    expect_refused(&opts, "net.links[1].broadcast not an address");
    opts.net.links[1].broadcast = "10.0.1.255";
    opts.net.links[1].addr = "ten";
    expect_refused(&opts, "net.links[1].addr not an address");
    opts.net.links[1].addr = NULL;
    opts.net.links[1].unicast_threshold = tt_MAX_PEER_COUNT;
    expect_refused(&opts, "net.links[1].unicast_threshold = tt_MAX_PEER_COUNT");
    opts.net.links[1].unicast_threshold = tt_MAX_PEER_COUNT - 1;
    expect_accepted(&opts, "net.links[1].unicast_threshold = tt_MAX_PEER_COUNT - 1");
    // A peer link may name a subnet; an interface link may not.
    opts.net.links[1] = (struct tt_LinkOptions) {.broadcast = "10.9.9.0/24", .peer = true};
    expect_accepted(&opts, "a peer link with a prefix");
    opts.net.links[1].peer = false;
    expect_refused(&opts, "an interface link with a prefix");

    opts = defaults();
    opts.net.discovery_range = tt_DISCOVERY_RANGE_OFF + 1;
    expect_refused(&opts, "net.discovery_range unknown");

#if tt_SEGMENT_ENABLED
    opts = defaults();
    opts.net.segment_slot_bytes = 1;
    expect_refused(&opts, "net.segment_slot_bytes below a header");
    opts.net.segment_slot_bytes = (uint32_t)FRAG_WHOLE_DATA_LIMIT;
    expect_accepted(&opts, "net.segment_slot_bytes = one datagram");
#endif

    opts = defaults();
    opts.net.large_fragmentation = tt_LARGE_FRAGMENTATION_IP + 1;
    expect_refused(&opts, "net.large_fragmentation unknown");
    opts.net.large_fragmentation = tt_LARGE_FRAGMENTATION_IP;
    expect_accepted(&opts, "net.large_fragmentation IP");

    opts = defaults();
    opts.net.ip_reassembly_max = tt_IPV4_UDP_MAX_PAYLOAD + 1;
    expect_refused(&opts, "net.ip_reassembly_max above one datagram");
    opts.net.ip_reassembly_max = tt_IPV4_UDP_MAX_PAYLOAD;
    expect_accepted(&opts, "net.ip_reassembly_max = one datagram");

    opts = defaults();
    opts.net.reassembly_cap_off = 2;
    expect_refused(&opts, "net.reassembly_cap_off 2");

    opts = defaults();
    opts.capacity.reserved = 1;
    expect_refused(&opts, "a placeholder group's reserved field");
}

// ---- Two contexts, two sets of options.

static void test_two_contexts_keep_their_own_options(void) {
    reset_mock();
    struct tt_InitOptions a = defaults();
    two_links(&a, 1);
    a.net.port = 9100;
    a.net.context_id = 11;
    a.net.ip_reassembly_max = 1000;
    struct tt_InitOptions b = defaults();
    b.net.context_id = 12;
    b.net.broadcast = "10.0.0.255";
    EXPECT_EQ_INT(tt_RET_OK, create_with(&ctx, &a));
    EXPECT_EQ_INT(tt_RET_OK, create_with(&other, &b));
    // The caller's structs may go: the contexts copied them.
    memset(&a, 0xEE, sizeof(a));
    memset(&b, 0xEE, sizeof(b));

    EXPECT_EQ_U32(11, ctx.id);
    EXPECT_EQ_U32(12, other.id);
    EXPECT_EQ_U32(2, ctx.link_count);
    EXPECT_EQ_U32(1, other.link_count);
    EXPECT_EQ_U32(1000, ctx.ip_reassembly_max);
    EXPECT_EQ_U32(advertised_ip_reassembly_max(tt_IPV4_UDP_MAX_PAYLOAD), other.ip_reassembly_max);

    struct tx_destination out[TX_MAX_DESTINATIONS];
    EXPECT_EQ_U32(2, broadcast_destinations(&ctx, out));
    EXPECT_EQ_U32(9100, out[1].port);
    EXPECT_EQ_U32(1, broadcast_destinations(&other, out));
    EXPECT_EQ_U32(0, out[0].ip);
    EXPECT_EQ_U32(_tt_CONTEXT_PORT, other.opts.net.port);
    EXPECT_TRUE(strcmp(other.opts.net.broadcast, "10.0.0.255") == 0);
    EXPECT_TRUE(strcmp(ctx.opts.net.broadcast, _tt_CONTEXT_BROADCAST) == 0);

    // Two peers: one broadcast for the context at threshold 1, unicast for the one at the default.
    struct tt_Peer peers[2] = {{.context_id = 0x21, .ip = PEER_A, .port = 8282},
                               {.context_id = 0x22, .ip = PEER_B, .port = 8282}};
    EXPECT_EQ_U32(1, tx_destinations(&ctx, peers, 2, out));
    EXPECT_EQ_U32(2, tx_destinations(&other, peers, 2, out));

    // Each sends as itself.
    EXPECT_TRUE(build_and_send_update(&ctx, NULL, 0));
    node_flush(&ctx, 0, NULL);
    EXPECT_EQ_U32(11, ((const struct tt_Header*)test_mock_send_last_buf)->source);
    EXPECT_TRUE(build_and_send_update(&other, NULL, 0));
    node_flush(&other, 0, NULL);
    EXPECT_EQ_U32(12, ((const struct tt_Header*)test_mock_send_last_buf)->source);
    destroy(&ctx);
    destroy(&other);
}

// ---- The legacy path: tt_Context_create() reads _tt_CONFIG once, and the context keeps what it read.

static void test_legacy_create_reads_the_global_once(void) {
    reset_mock();
    _tt_CONFIG.context_id = 33;
    _tt_CONFIG.port = 9200;
    _tt_CONFIG.ip_reassembly_max = 0; // the global's zero is "none", as it always was
    _tt_CONFIG.links[0] = (struct _tt_Link) {.broadcast = "10.0.0.255", .unicast_threshold = 0};
    _tt_CONFIG.links[1] = (struct _tt_Link) {.broadcast = "10.0.1.255", .unicast_threshold = 2};
    _tt_CONFIG.link_count = 2;
    memset(&ctx, 0, sizeof(ctx));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create(&ctx));
    EXPECT_EQ_U32(33, ctx.id);
    EXPECT_EQ_U32(9200, ctx.opts.net.port);
    EXPECT_EQ_U32(0, ctx.ip_reassembly_max);
    EXPECT_EQ_U32(0, ctx.links[0].unicast_threshold); // the global's zero threshold kept its meaning
    EXPECT_EQ_U32(2, ctx.links[1].unicast_threshold);
    // Written after creation, the global changes nothing.
    _tt_CONFIG.port = 9300;
    _tt_CONFIG.link_count = 0;
    struct tx_destination out[TX_MAX_DESTINATIONS];
    EXPECT_EQ_U32(2, broadcast_destinations(&ctx, out));
    EXPECT_EQ_U32(9200, out[0].port);
    destroy(&ctx);

    // A port the global holds and a UDP port cannot be is refused rather than truncated.
    reset_mock();
    _tt_CONFIG.port = 70000;
    memset(&ctx, 0, sizeof(ctx));
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, tt_Context_create(&ctx));

    _tt_CONFIG.context_id = tt_CONTEXT_ID_INVALID;
    _tt_CONFIG.port = _tt_CONTEXT_PORT;
    _tt_CONFIG.ip_reassembly_max = _tt_IP_REASSEMBLY_MAX;
    memset(_tt_CONFIG.links, 0, sizeof(_tt_CONFIG.links));
    _tt_CONFIG.link_count = 0;
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    test_defaults_are_the_compiled_settings();
    test_broadcast_names_the_link_asked_for();
    test_addr_is_the_default_links_address();
    test_context_id_is_the_source();
    test_links_and_port_are_the_broadcast_destinations();
    test_unicast_threshold_decides_unicast_or_broadcast();
    test_discovery_range_limits_the_link();
#if tt_SEGMENT_ENABLED
    test_segment_slot_bytes_is_the_ring_built();
#endif
    test_large_fragmentation_is_the_contexts_mode();
    test_ip_reassembly_max_is_advertised();
    test_reassembly_cap_off_switches_the_cap_off();
    test_validation();
    test_two_contexts_keep_their_own_options();
    test_legacy_create_reads_the_global_once();

    if (test_result() != 0) {
        return 1;
    }
    printf("test_init_options: all tests passed\n");
    return 0;
}
