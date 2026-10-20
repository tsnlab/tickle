/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// DEADLINE: a gap between two samples longer than the period P must be reported as a missed deadline, on both the
// offered (publisher) and the requested (subscription) side. rmw's contract: a deadline is missed when the time between
// two samples exceeds the period; each missed period adds one to total_count.
//
// The defect this test was written for (QoS plan step 0, 2026-10-18; fixed by D4): the checks ran once per P from the
// entity's creation time C and asked only "is the last sample at least P old?". With the last sample before a gap at
// L = C + kP + phi and the next one at L + 1.5P, the gap was missed exactly when phi < P/2. Since D4 the check re-arms
// at the last activity + P and counts one per missed period, so the phase must not matter.
//
// Cases (one fresh publisher + subscription each, so the old timer phase is set by its creation time):
//   gap 1.5P at phi = 0.10, 0.25, 0.40, 0.60, 0.75, 0.90 P: exactly 1 on both sides (0 is the defect).
//   gap 0.5P at phi = 0.25, 0.75 P: exactly 0 on both sides - the falsifier: a check that counts without a gap.
//   gap 5.5P at phi = 0.25 P: exactly 5 on both sides - one per missed period, not one per silence.
//   control: gap 3P at phi = 0.25 and 0.75 P: 2, or 3 when the check due at L + 3P runs before the sample planned for
//   that same instant arrives - the gap ends on a period boundary, so either side of it is correct.
//   control: no gap (the cadence just continues) at phi = 0.25 P: 0 on both sides.
// Publishes are every P/4 before and after the gap, far inside the deadline.
//
// The counts are judged against the gaps that actually happened, not the ones planned (2026-10-20). Each sample's
// activity time - the rmw's own tt_get_ns() inside rmw_publish(), and the subscription's on arrival - is bracketed by
// two clocks read here: before rmw_publish() and after the take that returned it. An interval between two samples of
// G in [lo, hi] can count anything from ceil((lo - S)/P) - 1 to floor(hi/P) misses; summed over every interval from
// the last sample before the gap to the end of the case (plus the silence before the counts are read, which can only
// add), that is the realized range. S (CHECK_SLACK_NS, P/4) is how late a deadline check may run: a miss due at L + kP
// is counted only if its check runs before the sample that ends the gap arrives, so one due less than S before that
// sample may be lost to the poll thread's scheduling. The 3P cases lose theirs that way on an idle PC, which is why
// they always allowed 2..3. For the gap itself S is not assumed but measured (2026-10-21): under make check-gates at
// load ~25 a check ran later than S, after the sample that ended a 1.5P gap, and a correct 0 read as WRONG against an
// expected 1. So probes on the checks' own scheduler, due at L_hi + kP, stamp when the poll thread really got there
// (record_probe()), and the gap's lower bound is the number that ran before the sample ending it was published; a
// poll thread too late for the planned count makes the case VOID, not WRONG. The pre-D4 defect counts 0 where a probe
// proves 1, so it still FAILs whenever the poll thread was on time - every run on an idle PC. Until then a case was
// VOID whenever any of its ~20 publishes went out more than 5 ms late, which under a loaded PC (make check-gates, a
// shared CI runner) is most cases: one stayed VOID 3 times at 15-36 ms of lateness while every count it produced was
// right. Lateness alone is not a reason to discard a case; lateness that changes what the case tests is. P is 200 ms so
// that a scheduler late by tens of milliseconds - the publisher's sleep or the deadline check's - stays a small
// fraction of the half period every 1.5P case has on each side of its count.
//
// Reading rules, enforced below, not only stated here:
//   - VOID (not a result) if the realized range is not inside the planned one (a gap or a cadence interval that came
//     out long enough to change the count the case was meant to test, or counts read only after a check may have
//     run), if an interval before the gap was long enough to count a miss, or if the subscription did not take the
//     last sample before the gap or the first one after it - then the requested-side count would not be about the gap.
//     A VOID case is retried up to MAX_TRIES times while the run is inside RETRY_BUDGET_NS; still VOID is a failure
//     (exit 1), never a pass.
//   - PASS (exit 0): every case's count, on both sides, is inside its realized range, which is inside the planned one.
//   - FAIL (exit 1): anything else.
// The realized phase (L - C, in percent of P) is printed beside each verdict: it is what decides whether the
// pre-D4 defect could show in that case, and a loaded scheduler moves it.

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include <tickle/hal.h>    // tt_get_ns()
#include <tickle/tickle.h> // tt_DATA_ENCODE/_ENCODE_SIZE/_DECODE/_FREE

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/event.h"
#include "rmw/events_statuses/offered_deadline_missed.h"
#include "rmw/events_statuses/requested_deadline_missed.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/publisher_options.h"
#include "rmw/qos_profiles.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/subscription_options.h"
#include "rmw/time.h"
#include "rmw/types.h"
#include "rmw_tickle_c/rmw_tickle.h" // rmw_tickle_context_impl_t: the context the probes are scheduled on
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

struct fake_msg {
    uint32_t value;
};

static bool fake_convert(const void* source, void* dest) {
    ((struct fake_msg*)dest)->value = ((const struct fake_msg*)source)->value;
    return true;
}
static int32_t fake_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(uint32_t);
}
// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_DATA_ENCODE's own fixed signature
static int32_t fake_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    uint32_t value = ((struct fake_msg*)data)->value;
    for (int i = 0; i < 4; i++) {
        payload[i] = (uint8_t)(value >> (8 * i));
    }
    return (int32_t)sizeof(uint32_t);
}
static int32_t fake_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        value |= (uint32_t)payload[i] << (8 * i);
    }
    ((struct fake_msg*)data)->value = value;
    return (int32_t)sizeof(uint32_t);
}
static void fake_free(struct tt_Data* data) {
    (void)data;
}

static rosidl_typesupport_tickle_c_message_callbacks_t fake_callbacks = {
    .ros_type_name = "test_deadline_gap/msg/FakeMsg",
    .tickle_struct_size = sizeof(struct fake_msg),
    .ros_struct_size = sizeof(struct fake_msg),
    .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function)&fake_convert,
    .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function)&fake_convert,
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&fake_encode_size,
    .tickle_encode = (tt_DATA_ENCODE)&fake_encode,
    .tickle_decode = (tt_DATA_DECODE)&fake_decode,
    .tickle_free = (tt_DATA_FREE)&fake_free,
};
static rosidl_message_type_support_t fake_handle = {.data = &fake_callbacks,
                                                    .func = get_message_typesupport_handle_function};

#define NS_PER_MS 1000000ULL
#define NS_PER_S 1000000000ULL
#define PERCENT 100U // phases are given in percent of P
#define TOPIC_CHARS 64
#define PERIOD_MS 200ULL
#define PERIOD_NS (PERIOD_MS * NS_PER_MS)
#define CADENCE_NS (PERIOD_NS / 4) // publishes before and after the gap: far inside the deadline
#define WARMUP_PERIODS 1ULL        // whole periods of cadence before the period the gap starts in
#define AFTER_NS (PERIOD_NS)       // cadence after the gap, before the counts are read
#define MAX_TRIES 5
#define RETRY_BUDGET_NS (40ULL * NS_PER_S) // no retry starts after this much of the run: ctest's timeout is 60 s
#define LONG_GAP_PERIODS 5ULL              // the 5.5P case: whole periods missed
#define CHECK_SLACK_NS (PERIOD_NS / 4)     // how late a deadline check may run (header): half every 1.5P case's margin

#define PROBES (LONG_GAP_PERIODS + 1) // one per period the longest gap can miss, and one past it

enum gap_kind { GAP_NONE, GAP_0_5P, GAP_1_5P, GAP_3P, GAP_5_5P };

// The probes (2026-10-21): an entry on the deadline checks' own scheduler, due at the latest time the check for the
// k-th missed period of the gap can be due, stamps when the poll thread really ran it. The scheduler runs the
// earliest entry first, so by then that check had run - or an earlier check, running late, had already counted the
// k-th period (it counts every period up to when it runs). So a probe that ran before the sample ending the gap was
// even published proves the k-th miss counted, however late the poll thread was; one that ran after proves nothing.
static struct tt_Context* poll_context;
static _Atomic uint64_t probe_ran[PROBES];

static void record_probe(struct tt_Context* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    atomic_store((_Atomic uint64_t*)param, tt_get_ns());
}

struct gap_case {
    enum gap_kind kind;
    uint32_t phase_pct; // phi, in percent of P
};

struct outcome {
    bool void_run;
    const char* void_reason;
    uint64_t offered;   // total_count rise over the gap
    uint64_t requested; // total_count rise over the gap
    uint64_t low;       // the realized range: the counts the gaps that actually happened allow
    uint64_t high;
    uint64_t late_ns;  // worst publish lateness seen, reported only
    uint64_t phase_ns; // realized phi: L - C
};

// When a sample's activity happened, bracketed: before rmw_publish(), and after the take that returned it.
struct stamp {
    uint64_t lo;
    uint64_t hi;
};

// The deadline counts an interval between two activities can produce: misses are counted at L + kP while no new
// activity has come, so G strictly inside (kP, (k+1)P) gives k, and G == kP either k - 1 or k. A check may run up to
// CHECK_SLACK_NS after it is due, and one that runs after the next activity counts nothing, so only the misses due at
// least that long before the interval's end are certain.
static void interval_range(struct stamp from, struct stamp until, uint64_t* low, uint64_t* high) {
    uint64_t g_lo = until.lo > from.hi + CHECK_SLACK_NS ? until.lo - from.hi - CHECK_SLACK_NS : 0;
    uint64_t g_hi = until.hi - from.lo;
    *low = g_lo == 0 ? 0 : ((g_lo + PERIOD_NS - 1) / PERIOD_NS) - 1;
    *high = g_hi / PERIOD_NS;
}

static void sleep_until(uint64_t target) {
    for (;;) {
        uint64_t now = tt_get_ns();
        if (now >= target) {
            return;
        }
        uint64_t left = target - now;
        struct timespec ts = {.tv_sec = (time_t)(left / NS_PER_S), .tv_nsec = (long)(left % NS_PER_S)};
        (void)nanosleep(&ts, NULL);
    }
}

static uint64_t offered_total(const rmw_event_t* event) {
    rmw_offered_deadline_missed_status_t status;
    bool taken = false;
    assert(RMW_RET_OK == rmw_take_event(event, &status, &taken));
    assert(taken);
    return (uint64_t)status.total_count;
}
static uint64_t requested_total(const rmw_event_t* event) {
    rmw_requested_deadline_missed_status_t status;
    bool taken = false;
    assert(RMW_RET_OK == rmw_take_event(event, &status, &taken));
    assert(taken);
    return (uint64_t)status.total_count;
}

// Publishes `value` at `planned` (absolute tt_get_ns()), records how late it went out and when it was active (*active),
// and drains the subscription. Returns whether the subscription took exactly this value last.
static bool publish_at(rmw_publisher_t* pub, rmw_subscription_t* sub, uint64_t planned, uint32_t value,
                       uint64_t* late_ns, struct stamp* active) {
    sleep_until(planned);
    uint64_t sent = tt_get_ns();
    active->lo = sent;
    struct fake_msg message = {.value = value};
    assert(RMW_RET_OK == rmw_publish(pub, &message, NULL));
    if (sent - planned > *late_ns) {
        *late_ns = sent - planned;
    }
    bool got = false;
    bool taken = true;
    while (taken) {
        struct fake_msg incoming = {0};
        assert(RMW_RET_OK == rmw_take(sub, &incoming, &taken, NULL));
        if (taken) {
            got = incoming.value == value;
        }
    }
    active->hi = tt_get_ns();
    return got;
}

// From the last sample before the gap to the first after it; with no gap, the cadence just continues.
static uint64_t gap_length(enum gap_kind kind) {
    switch (kind) {
    case GAP_0_5P:
        return PERIOD_NS / 2;
    case GAP_1_5P:
        return (3 * PERIOD_NS) / 2;
    case GAP_3P:
        return 3 * PERIOD_NS;
    case GAP_5_5P:
        return (LONG_GAP_PERIODS * PERIOD_NS) + (PERIOD_NS / 2);
    case GAP_NONE:
        break;
    }
    return CADENCE_NS;
}

// The counts a case is planned to show on each side, inclusive (see this file's header).
static void expected_range(enum gap_kind kind, uint64_t* low, uint64_t* high) {
    switch (kind) {
    case GAP_1_5P:
        *low = 1;
        *high = 1;
        return;
    case GAP_3P:
        *low = 2;
        *high = 3;
        return;
    case GAP_5_5P:
        *low = 5;
        *high = 5;
        return;
    case GAP_0_5P:
    case GAP_NONE:
        break;
    }
    *low = 0;
    *high = 0;
}

static struct outcome run_case(rmw_node_t* talker, rmw_node_t* listener, struct gap_case gap_case, int index) {
    struct outcome out = {0};
    char topic[TOPIC_CHARS];
    (void)snprintf(topic, sizeof(topic), "/deadline_gap_%d", index);

    rmw_qos_profile_t qos = rmw_qos_profile_default;
    qos.depth = 10;
    qos.deadline.sec = 0;
    qos.deadline.nsec = PERIOD_NS;
    rmw_subscription_options_t sub_opts = rmw_get_default_subscription_options();
    rmw_publisher_options_t pub_opts = rmw_get_default_publisher_options();

    // Both deadline timers start at creation, a fraction of a millisecond apart: C is taken between them.
    rmw_subscription_t* sub = rmw_create_subscription(listener, &fake_handle, topic, &qos, &sub_opts);
    uint64_t created = tt_get_ns();
    rmw_publisher_t* pub = rmw_create_publisher(talker, &fake_handle, topic, &qos, &pub_opts);
    assert(NULL != sub && NULL != pub);
    rmw_event_t offered = rmw_get_zero_initialized_event();
    rmw_event_t requested = rmw_get_zero_initialized_event();
    assert(RMW_RET_OK == rmw_publisher_event_init(&offered, pub, RMW_EVENT_OFFERED_DEADLINE_MISSED));
    assert(RMW_RET_OK == rmw_subscription_event_init(&requested, sub, RMW_EVENT_REQUESTED_DEADLINE_MISSED));

    // Cadence from C up to and including L = C + WARMUP_PERIODS * P + phi, the last sample before the gap.
    uint64_t last = created + (WARMUP_PERIODS * PERIOD_NS) + ((PERIOD_NS * gap_case.phase_pct) / PERCENT);
    uint32_t value = 1;
    bool warmup_clean = true; // no interval before the gap long enough to count a miss
    struct stamp prev = {.lo = created, .hi = created};
    struct stamp now = prev;
    for (uint64_t at = created; at < last; at += CADENCE_NS) {
        (void)publish_at(pub, sub, at, value++, &out.late_ns, &now);
        uint64_t low = 0;
        uint64_t high = 0;
        interval_range(prev, now, &low, &high);
        warmup_clean = warmup_clean && high == 0;
        prev = now;
    }
    bool before_ok = publish_at(pub, sub, last, value++, &out.late_ns, &now);
    uint64_t low = 0;
    uint64_t high = 0;
    interval_range(prev, now, &low, &high);
    warmup_clean = warmup_clean && high == 0;
    struct stamp before_gap = now;
    out.phase_ns = before_gap.lo - created;
    for (uint64_t k = 1; k <= PROBES; k++) {
        atomic_store(&probe_ran[k - 1], 0);
        assert(tt_Context_schedule(poll_context, before_gap.hi + (k * PERIOD_NS), record_probe, &probe_ran[k - 1]));
    }
    uint64_t offered_before = offered_total(&offered);
    uint64_t requested_before = requested_total(&requested);
    // A miss counted before these reads is not in the case's count: as many as could have come due by now come off
    // the gap's lower bound.
    uint64_t read_before = tt_get_ns();
    uint64_t counted_early = (read_before - before_gap.lo) / PERIOD_NS;

    uint64_t resume = last + gap_length(gap_case.kind);
    bool after_ok = publish_at(pub, sub, resume, value++, &out.late_ns, &now);
    bool after_cadence_ok = true; // each sample's own drain took it, so its stamp brackets its arrival
    interval_range(before_gap, now, &out.low, &out.high);
    // The gap's certain misses are the probes that ran before its end was published, not the ones due CHECK_SLACK_NS
    // before it: under make check-gates (load ~25) a check ran more than that late, after the sample, and a 1.5P case
    // counted a correct 0 against an expected 1 (2026-10-21).
    out.low = 0;
    for (uint64_t k = 1; k <= PROBES; k++) {
        uint64_t ran = atomic_load(&probe_ran[k - 1]);
        out.low += ran != 0 && ran < now.lo ? 1U : 0U;
    }
    out.low = out.low > counted_early ? out.low - counted_early : 0;
    prev = now;
    for (uint64_t at = resume + CADENCE_NS; at <= resume + AFTER_NS; at += CADENCE_NS) {
        after_cadence_ok = publish_at(pub, sub, at, value++, &out.late_ns, &now) && after_cadence_ok;
        interval_range(prev, now, &low, &high);
        out.low += low;
        out.high += high;
        prev = now;
    }
    out.offered = offered_total(&offered) - offered_before;
    out.requested = requested_total(&requested) - requested_before;
    // The silence from the last sample to these reads: a check may or may not have run in it.
    out.high += (tt_get_ns() - prev.lo) / PERIOD_NS;

    uint64_t planned_low = 0;
    uint64_t planned_high = 0;
    expected_range(gap_case.kind, &planned_low, &planned_high);
    if (out.low < planned_low || out.high > planned_high) {
        out.void_run = true;
        out.void_reason = "the realized gaps allow a count outside the planned one";
    } else if (!warmup_clean) {
        out.void_run = true;
        out.void_reason = "an interval before the gap was long enough to count a miss";
    } else if (!before_ok || !after_ok || !after_cadence_ok) {
        out.void_run = true;
        out.void_reason = "the subscription did not take a sample from the gap on as it was published";
    }

    for (uint64_t k = 1; k <= PROBES; k++) {
        (void)tt_Context_unschedule(poll_context, record_probe, &probe_ran[k - 1]); // none may run into the next case
    }
    assert(RMW_RET_OK == rmw_event_fini(&offered));
    assert(RMW_RET_OK == rmw_event_fini(&requested));
    assert(RMW_RET_OK == rmw_destroy_publisher(talker, pub));
    assert(RMW_RET_OK == rmw_destroy_subscription(listener, sub));
    return out;
}

static const char* kind_name(enum gap_kind kind) {
    switch (kind) {
    case GAP_0_5P:
        return "0.5P";
    case GAP_1_5P:
        return "1.5P";
    case GAP_3P:
        return "3P  ";
    case GAP_5_5P:
        return "5.5P";
    case GAP_NONE:
        break;
    }
    return "none";
}

// A case run until it is not VOID, at most MAX_TRIES times, and no retry once the run is RETRY_BUDGET_NS old.
static struct outcome run_with_retries(rmw_node_t* talker, rmw_node_t* listener, struct gap_case gap_case, int index,
                                       uint64_t started, int* tries) {
    struct outcome out;
    *tries = 0;
    do {
        out = run_case(talker, listener, gap_case, (index * MAX_TRIES) + *tries);
        (*tries)++;
        if (out.void_run) {
            printf("  gap %s phi %2u%%: VOID (%s; realized %llu..%llu, offered +%llu requested +%llu, worst lateness "
                   "%.2f ms)\n",
                   kind_name(gap_case.kind), gap_case.phase_pct, out.void_reason, (unsigned long long)out.low,
                   (unsigned long long)out.high, (unsigned long long)out.offered, (unsigned long long)out.requested,
                   (double)out.late_ns / (double)NS_PER_MS);
        }
    } while (out.void_run && *tries < MAX_TRIES && tt_get_ns() - started < RETRY_BUDGET_NS);
    return out;
}

struct tally {
    bool all_hold;
    bool any_void;
};

// Reads one case's outcome into the tally, and prints it.
static void judge(struct gap_case gap_case, struct outcome out, int tries, struct tally* tally) {
    if (out.void_run) {
        tally->any_void = true;
        return;
    }
    // run_case() VOIDs a realized range that is not inside the planned one, so this is the planned count or tighter.
    uint64_t low = out.low;
    uint64_t high = out.high;
    bool holds = out.offered >= low && out.offered <= high && out.requested >= low && out.requested <= high;
    tally->all_hold = tally->all_hold && holds;
    printf("  gap %s phi %2u%%: offered +%llu requested +%llu  expected %llu..%llu  %s (realized phi %llu%%, worst "
           "lateness %.2f ms, tries %d)\n",
           kind_name(gap_case.kind), gap_case.phase_pct, (unsigned long long)out.offered,
           (unsigned long long)out.requested, (unsigned long long)low, (unsigned long long)high, holds ? "ok" : "WRONG",
           (unsigned long long)(((out.phase_ns * PERCENT) / PERIOD_NS) % PERCENT),
           (double)out.late_ns / (double)NS_PER_MS, tries);
}

int main(void) {
    fake_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    rmw_init_options_t options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(&options, allocator));
    options.enclave = rcutils_strdup("/", allocator);
    assert(NULL != options.enclave);
    rmw_context_t context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(&options, &context));
    poll_context = &((rmw_tickle_context_impl_t*)context.impl)->tickle_context;
    rmw_node_t* talker = rmw_create_node(&context, "deadline_gap_talker", "/");
    rmw_node_t* listener = rmw_create_node(&context, "deadline_gap_listener", "/");
    assert(NULL != talker && NULL != listener);

    static const struct gap_case cases[] = {
        {GAP_1_5P, 10}, {GAP_1_5P, 25}, {GAP_1_5P, 40}, {GAP_1_5P, 60}, {GAP_1_5P, 75}, {GAP_1_5P, 90},
        {GAP_0_5P, 25}, {GAP_0_5P, 75}, {GAP_5_5P, 25}, {GAP_3P, 25},   {GAP_3P, 75},   {GAP_NONE, 25},
    };
    const int count = (int)(sizeof(cases) / sizeof(cases[0]));

    struct tally tally = {.all_hold = true, .any_void = false};
    printf("deadline gap: P = %llu ms, cadence %llu ms, gap 1.5P = %llu ms\n", PERIOD_MS, CADENCE_NS / NS_PER_MS,
           (3 * PERIOD_MS) / 2);
    uint64_t started = tt_get_ns();
    for (int i = 0; i < count; i++) {
        int tries = 0;
        struct outcome out = run_with_retries(talker, listener, cases[i], i, started, &tries);
        judge(cases[i], out, tries, &tally);
    }

    assert(RMW_RET_OK == rmw_destroy_node(talker));
    assert(RMW_RET_OK == rmw_destroy_node(listener));
    assert(RMW_RET_OK == rmw_shutdown(&context));
    assert(RMW_RET_OK == rmw_context_fini(&context));
    assert(RMW_RET_OK == rmw_init_options_fini(&options));

    if (tally.any_void) {
        printf(
            "deadline gap: FAIL - a case stayed VOID after its tries (at most %d, none started after %llu s); nothing "
            "was decided\n",
            MAX_TRIES, RETRY_BUDGET_NS / NS_PER_S);
        return 1;
    }
    if (!tally.all_hold) {
        printf("deadline gap: FAIL - a count is outside its expected range\n");
        return 1;
    }
    printf("deadline gap: PASS - every gap counted one per missed period on both sides, at every phase\n");
    return 0;
}
