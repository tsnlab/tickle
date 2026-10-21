/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// struct tt_InitOptions bundle 7 (ROADMAP "Context options"): the hybrid transport's estimator and control law, struct
// tt_HybridOptions.
//   - the defaults are the compiled tt_HYBRID_* settings (slow_start OFF where tt_HYBRID_SLOW_START is 0, path_clean_ns
//     the compiled one), a created context keeps them resolved, and a lifetime of its own scales path_clean_ns;
//   - every field, set to a value other than its default, changes a decision of the estimator or the control law: each
//     is compared with the same arm at the defaults, which must decide what the build's constants decide. The arms
//     run the estimator's own functions on a hand-built loss sequence, on a whitebox reader or writer holding the
//     options a created context resolved;
//   - the stale rule's exposure is -ln(stale_false_forget), and exactly tt_HYBRID_STALE_EXPOSURE at the build's rate;
//   - each creation check refuses a bad value with tt_RET_INVALID_ARGUMENT and accepts the boundary beside it
//     (tests/mutants_init_options_hybrid.sh removes each check and expects a failure here).
// Built as rmw_tickle builds core (64 KB datagrams, large samples), so the hybrid transport is in;
// tests/test_init_options_noflag.c checks that a build without it refuses the options.

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
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; tests/mutants_init_options_hybrid.sh passes a mutant

#if !tt_HYBRID_TRANSPORT
#error "this test is about a build with the hybrid transport"
#endif

#define CEILING 65507U
#define WRITER 9
#define PEER 5
#define R_UNITS 21690.0 // units a second: 1 MB at 30 Hz

static struct tt_Context ctx;  // created: what tt_Context_create_with() resolves and refuses
static struct tt_Context node; // whitebox: the estimator runs here, on ctx's options
static struct tt_Publisher pub;

static void reset_mock(void) {
    test_mock_reset();
    test_mock_now = 1000 * tt_SECOND;
    test_mock_link_resolves = true;
    test_mock_link_addr = 0x0a000007U;
    test_mock_link_netmask = 0xffffff00U;
}

static struct tt_InitOptions defaults(void) {
    struct tt_InitOptions opts;
    tt_InitOptions_default(&opts);
    return opts;
}

static tt_ret_t create_with(const struct tt_InitOptions* opts) {
    reset_mock();
    memset(&ctx, 0, sizeof(ctx));
    tt_ret_t ret = tt_Context_create_with(&ctx, opts);
    if (ret == tt_RET_OK) {
        EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&ctx));
    }
    return ret;
}

// Creates ctx with `hybrid` (which must be accepted), then a whitebox reader on its resolved options, started as
// creation starts one (reset_node_state(): the cap, tau, what it advertises).
static struct tt_HybridState* setup(const struct tt_HybridOptions* hybrid) {
    struct tt_InitOptions opts = defaults();
    opts.hybrid_transport = *hybrid;
    EXPECT_EQ_INT(tt_RET_OK, create_with(&opts));
    struct tt_InitOptions resolved = ctx.opts;
    reset_mock();
    memset(&node, 0, sizeof(node));
    node.opts = resolved;
    node_init_locks(&node);
    reset_node_state(&node);
    node.id = 2;
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = sizeof(node.tx_buffer);
    return &node.hybrid;
}

static const struct tt_HybridOptions* opts_of(void) {
    return &node.opts.hybrid_transport;
}

// ---- The defaults.

static bool same_options(const struct tt_HybridOptions* a, const struct tt_HybridOptions* b) {
    return a->collapse_ratio == b->collapse_ratio && a->drop == b->drop &&
           a->stale_false_forget == b->stale_false_forget && a->stale_p_min == b->stale_p_min &&
           a->probe_step == b->probe_step && a->scale_at == b->scale_at && a->scale_max == b->scale_max &&
           a->path_alpha == b->path_alpha && a->queue_lifetime_ns == b->queue_lifetime_ns &&
           a->path_clean_ns == b->path_clean_ns && a->window_frames == b->window_frames &&
           a->collapse_min_runs == b->collapse_min_runs && a->collapse_min_lost == b->collapse_min_lost &&
           a->window_max_runs == b->window_max_runs && a->none_seen_frames == b->none_seen_frames &&
           a->stale_events == b->stale_events && a->scale_min_events == b->scale_min_events && a->n_max == b->n_max &&
           a->slow_start == b->slow_start && a->probe_backoff_max == b->probe_backoff_max &&
           a->scale_ticks == b->scale_ticks;
}

static void test_the_defaults_are_the_builds(void) {
    struct tt_InitOptions opts = defaults();
    const struct tt_HybridOptions* h = &opts.hybrid_transport;
    EXPECT_TRUE(h->collapse_ratio == tt_HYBRID_COLLAPSE_RATIO);
    EXPECT_TRUE(h->drop == tt_HYBRID_DROP);
    EXPECT_TRUE(h->stale_false_forget == tt_HYBRID_STALE_FALSE_FORGET);
    EXPECT_TRUE(h->stale_p_min == tt_HYBRID_STALE_P_MIN);
    EXPECT_TRUE(h->probe_step == tt_HYBRID_PROBE_STEP);
    EXPECT_TRUE(h->scale_at == tt_HYBRID_SCALE_AT);
    EXPECT_TRUE(h->scale_max == tt_HYBRID_SCALE_MAX);
    EXPECT_TRUE(h->path_alpha == tt_HYBRID_PATH_ALPHA);
    EXPECT_EQ_U64(tt_HYBRID_QUEUE_LIFETIME_NS, h->queue_lifetime_ns);
    EXPECT_EQ_U64(tt_HYBRID_PATH_CLEAN_NS, h->path_clean_ns);
    EXPECT_EQ_U32(tt_HYBRID_WINDOW_FRAMES, h->window_frames);
    EXPECT_EQ_U32(tt_HYBRID_COLLAPSE_MIN_RUNS, h->collapse_min_runs);
    EXPECT_EQ_U32(tt_HYBRID_COLLAPSE_MIN_LOST, h->collapse_min_lost);
    EXPECT_EQ_U32(tt_HYBRID_WINDOW_MAX_RUNS, h->window_max_runs);
    EXPECT_EQ_U32(tt_HYBRID_NONE_SEEN_FRAMES, h->none_seen_frames);
    EXPECT_EQ_U32(tt_HYBRID_STALE_EVENTS, h->stale_events);
    EXPECT_EQ_U32(tt_HYBRID_SCALE_MIN_EVENTS, h->scale_min_events);
    EXPECT_EQ_U32(tt_HYBRID_N_MAX, h->n_max);
    EXPECT_EQ_U32(tt_HYBRID_SLOW_START != 0 ? tt_HYBRID_SLOW_START : tt_HYBRID_SLOW_START_OFF, h->slow_start);
    EXPECT_EQ_U32(tt_HYBRID_PROBE_BACKOFF_MAX, h->probe_backoff_max);
    EXPECT_EQ_U32(tt_HYBRID_SCALE_TICKS, h->scale_ticks);
    // A created context holds them resolved, from a zeroed group as from the defaults.
    struct tt_InitOptions zeroed = defaults();
    memset(&zeroed.hybrid_transport, 0, sizeof(zeroed.hybrid_transport));
    reset_mock();
    memset(&ctx, 0, sizeof(ctx));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_create_with(&ctx, &zeroed));
    EXPECT_TRUE(same_options(&ctx.opts.hybrid_transport, h));
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&ctx));
    // A lifetime of its own scales the MTU-only time with it; one set explicitly is kept.
    struct tt_HybridOptions own = {.queue_lifetime_ns = 10 * tt_SECOND};
    resolve_hybrid_options(&own);
    EXPECT_EQ_U64(20 * tt_SECOND, own.path_clean_ns);
    own = (struct tt_HybridOptions) {.queue_lifetime_ns = 10 * tt_SECOND, .path_clean_ns = 7 * tt_SECOND};
    resolve_hybrid_options(&own);
    EXPECT_EQ_U64(7 * tt_SECOND, own.path_clean_ns);
}

// ---- Each field moves a decision.

// n_max: the most units of a run, as the writer bounds n and the reader its cap - which it then advertises as that many
// units, not as its ceiling in bytes.
static void test_n_max(void) {
    struct tt_HybridState* hybrid = setup(&(struct tt_HybridOptions) {0});
    EXPECT_EQ_U32(45, hybrid_units_within(&node, CEILING, false));
    EXPECT_EQ_U32(45, hybrid->cap);
    EXPECT_EQ_U32(node.ip_reassembly_max, hybrid->advertised);
    static struct tt_Context writer;
    memset(&writer, 0, sizeof(writer));
    node_init_locks(&writer);
    EXPECT_EQ_U32(45, hybrid_units_within(&writer, hybrid->advertised, false));
    hybrid = setup(&(struct tt_HybridOptions) {.n_max = 20});
    EXPECT_EQ_U32(20, hybrid_units_within(&node, CEILING, false));
    EXPECT_EQ_U32(20, hybrid->cap);
    EXPECT_EQ_U32(20, hybrid_units_within(&writer, hybrid->advertised, false)); // a writer at 45 reads 20
}

// window_frames: where the estimates halve.
static void test_window_frames(void) {
    setup(&(struct tt_HybridOptions) {0});
    struct tt_LossEstimate estimate = {0};
    loss_estimate_observe(&estimate, 999, 2, opts_of());
    loss_estimate_observe(&estimate, 1, 0, opts_of());
    EXPECT_EQ_U32(1000, estimate.frames);
    struct tt_RunWindow window = {0};
    run_window_observe(&window, 1000, false, opts_of());
    EXPECT_EQ_U32(1000, window.frames);
    setup(&(struct tt_HybridOptions) {.window_frames = 1000});
    estimate = (struct tt_LossEstimate) {0};
    loss_estimate_observe(&estimate, 999, 2, opts_of());
    loss_estimate_observe(&estimate, 1, 0, opts_of());
    EXPECT_EQ_U32(500, estimate.frames);
    EXPECT_EQ_U32(1, estimate.events);
    window = (struct tt_RunWindow) {0};
    run_window_observe(&window, 1000, false, opts_of());
    EXPECT_EQ_U32(500, window.frames);
}

// queue_lifetime_ns: tau at the start and as the runs' rate allows; and with it how long the writer's MTU-only time is.
static void test_queue_lifetime_ns(void) {
    for (int arm = 0; arm < 2; arm++) {
        uint64_t lifetime = arm == 0 ? 30 * tt_SECOND : 10 * tt_SECOND;
        struct tt_HybridState* hybrid =
            setup(&(struct tt_HybridOptions) {.queue_lifetime_ns = arm == 0 ? 0 : lifetime});
        EXPECT_EQ_U64(lifetime, hybrid->lifetime_ns);
        hybrid->rx_writers[0] = (struct tt_HybridRxWriter) {.entity_id = 1, .tick_runs = 1000, .source = 7};
        hybrid->lifetime_ns = 1;
        hybrid_measure_lifetime(hybrid, tt_SECOND, opts_of()->queue_lifetime_ns); // 1,000 runs a second: 65.5 s
        EXPECT_EQ_U64(lifetime, hybrid->lifetime_ns);
        EXPECT_EQ_U64(2 * lifetime, opts_of()->path_clean_ns);
    }
}

// path_clean_ns: a RELIABLE writer's reader whose fragment path failed 20 s ago gets MTU units for 60 s by default, a
// re-probe at 10 s.
static uint32_t writer_n(bool* one_run) {
    memset(&pub, 0, sizeof(pub));
    pub.peer_acks[0].context_id = PEER;
    pub.peer_watch[0].path_failed_ns = test_mock_now - (20 * tt_SECOND);
    node.hybrid.peer_loss_p[PEER] = HYBRID_LOSS_NONE_SEEN;
    *one_run = false;
    return hybrid_reliable_n(&node, &pub, PEER, 45, one_run);
}

static void test_path_clean_ns(void) {
    bool one_run = false;
    setup(&(struct tt_HybridOptions) {0});
    EXPECT_EQ_U32(1, writer_n(&one_run));
    EXPECT_TRUE(!one_run);
    setup(&(struct tt_HybridOptions) {.path_clean_ns = 10 * tt_SECOND});
    EXPECT_EQ_U32(45, writer_n(&one_run));
    EXPECT_TRUE(one_run);
}

// The collapse test: a window of 26 runs lost of 1,000 at n = 8 over a 0.1% path is one at the defaults
// (tests/test_hybrid_send.c, test_the_collapse_test_is_the_designs); not with more runs, more losses or a larger ratio
// asked for.
static void test_the_collapse_fields(void) {
    struct tt_LossEstimate mtu = {30, 30000, 0, 0};
    struct tt_RunWindow excess = {1000, 26, 8000};
    setup(&(struct tt_HybridOptions) {0});
    EXPECT_TRUE(hybrid_collapsed(&excess, &mtu, opts_of()));
    setup(&(struct tt_HybridOptions) {.collapse_min_runs = 1001, .window_max_runs = 2000});
    EXPECT_TRUE(!hybrid_collapsed(&excess, &mtu, opts_of()));
    setup(&(struct tt_HybridOptions) {.collapse_min_lost = 27, .collapse_min_runs = 27});
    EXPECT_TRUE(!hybrid_collapsed(&excess, &mtu, opts_of()));
    setup(&(struct tt_HybridOptions) {.collapse_ratio = 3.0});
    EXPECT_TRUE(!hybrid_collapsed(&excess, &mtu, opts_of()));
    // collapse_min_runs bounds a clean verdict too.
    struct tt_RunWindow consistent = {1000, 8, 8000};
    setup(&(struct tt_HybridOptions) {0});
    EXPECT_TRUE(hybrid_clean(&consistent, &mtu, opts_of()));
    setup(&(struct tt_HybridOptions) {.collapse_min_runs = 1001, .window_max_runs = 2000});
    EXPECT_TRUE(!hybrid_clean(&consistent, &mtu, opts_of()));
}

// window_max_runs: a window neither collapsed nor clean gathers more, up to it.
static void test_window_max_runs(void) {
    for (int arm = 0; arm < 2; arm++) {
        struct tt_HybridState* hybrid = setup(&(struct tt_HybridOptions) {.window_max_runs = arm == 0 ? 0 : 500});
        hybrid->mtu_loss = (struct tt_LossEstimate) {30, 30000, 0, 0};
        hybrid->window = (struct tt_RunWindow) {1000, 20, 8000};
        hybrid->window_n = 8;
        hybrid->cap = 8;
        hybrid_control(hybrid, test_mock_now, R_UNITS, 45, opts_of());
        EXPECT_EQ_U32(arm == 0 ? 1000 : 0, hybrid->window.sent); // kept (1,000 < 1,024), or decided
        EXPECT_EQ_U32(8, hybrid->cap);
    }
}

// drop and probe_backoff_max: a collapse drops the cap to drop x the ceiling; a failed probe doubles the next probe's
// wait, up to 2^probe_backoff_max.
static void collapse_at_8(struct tt_HybridState* hybrid) {
    hybrid->mtu_loss = (struct tt_LossEstimate) {30, 30000, 0, 0};
    hybrid->window = (struct tt_RunWindow) {1000, 540, 8000};
    hybrid->window_n = 8;
    hybrid->probe_since_ns = 1; // a probe in progress: it failed
    hybrid_collapse(hybrid, test_mock_now, R_UNITS, opts_of());
}

static void test_drop_and_probe_backoff_max(void) {
    struct tt_HybridState* hybrid = setup(&(struct tt_HybridOptions) {0});
    collapse_at_8(hybrid);
    uint32_t n_fail = hybrid->n_fail;
    EXPECT_EQ_U32(hybrid_drop(n_fail, tt_HYBRID_DROP), hybrid->cap);
    for (int i = 0; i < 5; i++) {
        collapse_at_8(hybrid);
    }
    EXPECT_EQ_U32(tt_HYBRID_PROBE_BACKOFF_MAX, hybrid->backoff);
    hybrid = setup(&(struct tt_HybridOptions) {.drop = 0.3, .probe_backoff_max = 1});
    collapse_at_8(hybrid);
    EXPECT_EQ_U32(n_fail, hybrid->n_fail);
    EXPECT_EQ_U32(hybrid_drop(n_fail, 0.3), hybrid->cap);
    EXPECT_TRUE(hybrid_drop(n_fail, 0.3) != hybrid_drop(n_fail, tt_HYBRID_DROP));
    for (int i = 0; i < 5; i++) {
        collapse_at_8(hybrid);
    }
    EXPECT_EQ_U32(1, hybrid->backoff);
    EXPECT_EQ_U64(test_mock_now + ((2U * hybrid->lifetime_ns) << 1U), hybrid->probe_at_ns);
}

// probe_step: a probe above a ceiling of 10 goes to 13 by default (1.25), to 20 at 2.
static void test_probe_step(void) {
    for (int arm = 0; arm < 2; arm++) {
        struct tt_HybridState* hybrid = setup(&(struct tt_HybridOptions) {.probe_step = arm == 0 ? 0.0 : 2.0});
        hybrid->n_fail = 10;
        hybrid->cap = 9;
        hybrid->probe_at_ns = 0;
        hybrid_grow(hybrid, test_mock_now, 45, opts_of());
        EXPECT_EQ_U32(arm == 0 ? 13 : 20, hybrid->cap);
    }
}

// slow_start: the cap a reader starts at.
static void test_slow_start(void) {
    EXPECT_EQ_U32(45, setup(&(struct tt_HybridOptions) {0})->cap);
    EXPECT_EQ_U32(45, setup(&(struct tt_HybridOptions) {.slow_start = tt_HYBRID_SLOW_START_OFF})->cap);
    EXPECT_EQ_U32(4, setup(&(struct tt_HybridOptions) {.slow_start = 4})->cap);
}

// none_seen_frames: "none seen" after that many clean frames.
static void test_none_seen_frames(void) {
    struct tt_LossEstimate clean = {0, 500, 0, 0};
    setup(&(struct tt_HybridOptions) {0});
    EXPECT_EQ_U32(HYBRID_LOSS_NONE_SEEN, hybrid_rx_loss_code(&clean, opts_of()));
    setup(&(struct tt_HybridOptions) {.none_seen_frames = 501});
    EXPECT_TRUE(hybrid_rx_loss_code(&clean, opts_of()) != HYBRID_LOSS_NONE_SEEN);
}

// The stale rule's estimate: `events` events at F frames, then `clean` clean ones.
static bool forgets(uint32_t frames, uint32_t events, uint32_t clean) {
    struct tt_LossEstimate estimate = {0};
    loss_estimate_observe(&estimate, frames - events, 0, opts_of());
    loss_estimate_observe(&estimate, events, events, opts_of());
    loss_estimate_observe(&estimate, clean, 0, opts_of());
    return loss_estimate_forget_stale(&estimate, opts_of());
}

// stale_events, stale_false_forget, stale_p_min: three events at F = 200 are forgotten after 4,109 clean frames at the
// defaults (test_the_stale_rule_is_the_designs), never when the rule takes two at most; at a false-forget rate of 1e-2
// after 729 (200 (100^(1/3) - 1) = 728.8), one frame fewer not; one event at F = 50 after the floor -ln(1e-2) / 1e-3 =
// 4,605.2 frames there (the exposure derived), after 9.21 / 1e-2 (921, a hair above it in double: 922) at a p_min of
// 1e-2, and at neither by default.
static void test_the_stale_fields(void) {
    setup(&(struct tt_HybridOptions) {0});
    EXPECT_TRUE(forgets(200, 3, 4109));
    EXPECT_TRUE(!forgets(200, 3, 729));
    EXPECT_TRUE(!forgets(50, 1, 4606));
    EXPECT_TRUE(!forgets(50, 1, 922));
    setup(&(struct tt_HybridOptions) {.stale_events = 2});
    EXPECT_TRUE(!forgets(200, 3, 4109));
    setup(&(struct tt_HybridOptions) {.stale_false_forget = 1e-2});
    EXPECT_TRUE(forgets(200, 3, 729));
    EXPECT_TRUE(!forgets(200, 3, 728));
    EXPECT_TRUE(forgets(50, 1, 4606));
    EXPECT_TRUE(!forgets(50, 1, 4605));
    setup(&(struct tt_HybridOptions) {.stale_p_min = 1e-2});
    EXPECT_TRUE(forgets(50, 1, 922));
    EXPECT_TRUE(!forgets(50, 1, 920));
}

// The exposure: config.h's own 9.21 at the build's rate, -ln of any other - the series to double precision.
static bool near(double a, double b) {
    double d = a - b;
    return d < 1e-12 && d > -1e-12;
}

static void test_the_exposure_is_derived(void) {
    struct tt_HybridOptions h = {0};
    resolve_hybrid_options(&h);
    EXPECT_TRUE(hybrid_stale_exposure(&h) == tt_HYBRID_STALE_EXPOSURE);
    h.stale_false_forget = 1e-2;
    EXPECT_TRUE(near(4.605170185988091, hybrid_stale_exposure(&h)));
    EXPECT_TRUE(near(-9.210340371976184, hybrid_ln(1e-4)));
    EXPECT_TRUE(near(-0.6931471805599453, hybrid_ln(0.5)));
    EXPECT_TRUE(near(-0.10536051565782628, hybrid_ln(0.9)));
    EXPECT_TRUE(near(-34.538776394910684, hybrid_ln(1e-15)));
}

// scale_at, scale_ticks, scale_max, scale_min_events: R halved since the anchor scales a ceiling of 10 to 20 on the
// third tick by default; not when the factor asked for is 2.5; on the first when one tick is asked for; R quartered
// scales it to 40 (at most 4x), to 20 at a scale_max of 2; p, at 5 events, moves it only when 5 are enough.
static struct tt_HybridState* scale_reader(const struct tt_HybridOptions* h) {
    struct tt_HybridState* hybrid = setup(h);
    hybrid->n_fail = 10;
    hybrid->cap = 9;
    hybrid->anchor_r = R_UNITS;
    hybrid->anchor_p = 0.0;
    return hybrid;
}

static void ticks(struct tt_HybridState* hybrid, int count, double units_per_s) {
    for (int i = 0; i < count; i++) {
        hybrid_scale(hybrid, test_mock_now, units_per_s, 45, opts_of());
    }
}

static void test_the_scale_fields(void) {
    struct tt_HybridState* hybrid = scale_reader(&(struct tt_HybridOptions) {0});
    ticks(hybrid, 2, R_UNITS / 2.0);
    EXPECT_EQ_U32(10, hybrid->n_fail);
    ticks(hybrid, 1, R_UNITS / 2.0);
    EXPECT_EQ_U32(20, hybrid->n_fail);
    hybrid = scale_reader(&(struct tt_HybridOptions) {.scale_at = 2.5});
    ticks(hybrid, 3, R_UNITS / 2.0);
    EXPECT_EQ_U32(10, hybrid->n_fail);
    hybrid = scale_reader(&(struct tt_HybridOptions) {.scale_ticks = 1});
    ticks(hybrid, 1, R_UNITS / 2.0);
    EXPECT_EQ_U32(20, hybrid->n_fail);
    hybrid = scale_reader(&(struct tt_HybridOptions) {0});
    ticks(hybrid, 3, R_UNITS / 4.0);
    EXPECT_EQ_U32(40, hybrid->n_fail);
    hybrid = scale_reader(&(struct tt_HybridOptions) {.scale_max = 2.0});
    ticks(hybrid, 3, R_UNITS / 4.0);
    EXPECT_EQ_U32(20, hybrid->n_fail);
    for (int arm = 0; arm < 2; arm++) {
        hybrid = scale_reader(&(struct tt_HybridOptions) {.scale_min_events = arm == 0 ? 0 : 5});
        hybrid->mtu_loss = (struct tt_LossEstimate) {5, 5000, 0, 0}; // p 1e-3
        hybrid->anchor_p = 2e-3;                                     // R p halved by p alone
        ticks(hybrid, 1, R_UNITS);
        EXPECT_EQ_INT(arm == 0 ? 0 : 1, hybrid->scale_streak);
    }
}

// path_alpha: a writer's sample all of whose one run was lost with its tail through, on a path whose MTU units lost
// nothing in 100,000 frames: frame loss alone does that with a chance of 3.3e-4 - no verdict at 1e-6, a failed path
// (one unit, re-probed later) at 1e-3.
static void test_path_alpha(void) {
    for (int arm = 0; arm < 2; arm++) {
        struct tt_HybridState* hybrid = setup(&(struct tt_HybridOptions) {.path_alpha = arm == 0 ? 0.0 : 1e-3});
        hybrid->mtu_loss = (struct tt_LossEstimate) {0, 100000, 0, 0};
        struct tt_HybridRxWriter* writer = &hybrid->rx_writers[0];
        *writer = (struct tt_HybridRxWriter) {.entity_id = 1, .count = 12, .body_lost = 11, .source = WRITER};
        writer->layout_n = 11;
        writer->path_odds = 1.0;
        hybrid_rx_judge_path(&node, writer);
        EXPECT_TRUE((hybrid->path_until_ns != 0) == (arm == 1));
        EXPECT_EQ_U32(arm == 0 ? 45 : 1, hybrid->cap);
    }
}

// ---- The checks made at creation, each against the boundary it allows.

static void expect(tt_ret_t want, const struct tt_HybridOptions* hybrid, const char* what) {
    struct tt_InitOptions opts = defaults();
    opts.hybrid_transport = *hybrid;
    tt_ret_t got = create_with(&opts);
    if (got != want) {
        printf("FAIL: %s: created %d, expected %d\n", what, (int)got, (int)want);
    }
    EXPECT_EQ_INT(want, got);
}

#define REFUSED(what, ...) expect(tt_RET_INVALID_ARGUMENT, &(struct tt_HybridOptions) {__VA_ARGS__}, what)
#define ACCEPTED(what, ...) expect(tt_RET_OK, &(struct tt_HybridOptions) {__VA_ARGS__}, what)

static void test_the_checks(void) {
    ACCEPTED("the defaults", 0);
    REFUSED("n_max above the build's", .n_max = tt_HYBRID_N_MAX + 1);
    ACCEPTED("n_max the build's", .n_max = tt_HYBRID_N_MAX);
    ACCEPTED("n_max 1", .n_max = 1);
    REFUSED("window_frames at none_seen_frames", .window_frames = 500, .none_seen_frames = 500);
    ACCEPTED("window_frames just above none_seen_frames", .window_frames = 501, .none_seen_frames = 500);
    REFUSED("collapse_ratio below 1", .collapse_ratio = 0.99);
    ACCEPTED("collapse_ratio 1", .collapse_ratio = 1.0);
    REFUSED("collapse_min_lost above collapse_min_runs", .collapse_min_runs = 16, .collapse_min_lost = 17);
    ACCEPTED("collapse_min_lost at collapse_min_runs", .collapse_min_runs = 16, .collapse_min_lost = 16);
    REFUSED("window_max_runs below collapse_min_runs", .collapse_min_runs = 16, .window_max_runs = 15);
    ACCEPTED("window_max_runs at collapse_min_runs", .collapse_min_runs = 16, .window_max_runs = 16);
    REFUSED("drop 1", .drop = 1.0);
    REFUSED("drop negative", .drop = -0.5);
    ACCEPTED("drop just below 1", .drop = 0.999);
    REFUSED("stale_false_forget 1", .stale_false_forget = 1.0);
    ACCEPTED("stale_false_forget 0.5", .stale_false_forget = 0.5);
    REFUSED("stale_p_min 1", .stale_p_min = 1.0);
    ACCEPTED("stale_p_min 0.5", .stale_p_min = 0.5);
    REFUSED("path_alpha 1", .path_alpha = 1.0);
    ACCEPTED("path_alpha 0.5", .path_alpha = 0.5);
    REFUSED("probe_step 1", .probe_step = 1.0);
    ACCEPTED("probe_step just above 1", .probe_step = 1.01);
    REFUSED("scale_at 1", .scale_at = 1.0);
    ACCEPTED("scale_at just above 1", .scale_at = 1.01);
    REFUSED("scale_max below 1", .scale_max = 0.5);
    ACCEPTED("scale_max 1", .scale_max = 1.0);
    REFUSED("queue_lifetime_ns above an hour", .queue_lifetime_ns = (3600ULL * tt_SECOND) + 1U);
    ACCEPTED("queue_lifetime_ns an hour", .queue_lifetime_ns = 3600ULL * tt_SECOND);
    REFUSED("probe_backoff_max 17", .probe_backoff_max = 17);
    ACCEPTED("probe_backoff_max 16", .probe_backoff_max = 16);
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    test_the_defaults_are_the_builds();
    test_n_max();
    test_window_frames();
    test_queue_lifetime_ns();
    test_path_clean_ns();
    test_the_collapse_fields();
    test_window_max_runs();
    test_drop_and_probe_backoff_max();
    test_probe_step();
    test_slow_start();
    test_none_seen_frames();
    test_the_stale_fields();
    test_the_exposure_is_derived();
    test_the_scale_fields();
    test_path_alpha();
    test_the_checks();
    if (test_result() != 0) {
        return 1;
    }
    printf("test_init_options_hybrid: all tests passed\n");
    return 0;
}
