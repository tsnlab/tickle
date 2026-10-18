/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// rmw_qos_profile_check_compatible() (QoS plan D2): OK, WARNING or ERROR per rmw's documented rules, with the reason.
//
// The tables below are the expectation, written out per policy rather than computed, each policy varied over every
// value it takes with the others fixed at a compatible, known setting:
//   ERROR    what this rmw refuses to connect - core's rule for an announced pair (tickle.c qos_incompatible_policy());
//   WARNING  a kind that is SYSTEM_DEFAULT, UNKNOWN or BEST_AVAILABLE on a side that decides the outcome (rmw's rule;
//            BEST_AVAILABLE is decided at creation, so it is not known from the profile either), or a BEST_AVAILABLE
//            offered DEADLINE period or LIVELINESS lease against a finite request;
//   OK       everything else. Unspecified and infinite durations are the same (both resolve to infinite in core), and
//            a BEST_AVAILABLE requested duration takes the loosest offer, so it is always met.
// Then the reason: one entry per finding, each "ERROR: ...;" or "WARNING: ...;", every error before every warning, and
// a short buffer cut short and terminated rather than overrun. The control for the tables: the all-OK row (every
// policy known and compatible) gives OK with an empty reason, so a table of WARNINGs is not an artefact of a reason
// that always says something.
//
// Where this differs from rmw_dds_common's qos_profile_check_compatible() (the Fast DDS / Cyclone rmws' shared
// implementation; compared over these tables when it was written, 2026-10-19): it treats BEST_AVAILABLE as a known
// value (no warning), and an explicit infinite DEADLINE or lease against an unspecified one as an ERROR.

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "rmw/error_handling.h"
#include "rmw/qos_profiles.h"
#include "rmw/ret_types.h"
#include "rmw/time.h"
#include "rmw/types.h"

#define O RMW_QOS_COMPATIBILITY_OK
#define W RMW_QOS_COMPATIBILITY_WARNING
#define E RMW_QOS_COMPATIBILITY_ERROR
#define KINDS 5
#define REASON_SIZE 2048

static int checked;

// Every policy known and compatible: RELIABLE, VOLATILE, AUTOMATIC, unspecified durations.
static rmw_qos_profile_t known(void) {
    rmw_qos_profile_t qos = rmw_qos_profile_default;
    qos.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
    qos.durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
    qos.liveliness = RMW_QOS_POLICY_LIVELINESS_AUTOMATIC;
    qos.deadline = (rmw_time_t)RMW_DURATION_UNSPECIFIED;
    qos.liveliness_lease_duration = (rmw_time_t)RMW_DURATION_UNSPECIFIED;
    return qos;
}

static rmw_qos_compatibility_type_t check(const rmw_qos_profile_t* pub, const rmw_qos_profile_t* sub, char* reason) {
    rmw_qos_compatibility_type_t compatibility = (rmw_qos_compatibility_type_t)-1;
    assert(RMW_RET_OK == rmw_qos_profile_check_compatible(*pub, *sub, &compatibility, reason, REASON_SIZE));
    checked++;
    return compatibility;
}

// The reason agrees with the verdict: empty for OK; a WARNING entry and no ERROR for WARNING; an ERROR entry for
// ERROR; and each entry ends in ';'.
static void expect_verdict(rmw_qos_compatibility_type_t expected, const rmw_qos_profile_t* pub,
                           const rmw_qos_profile_t* sub, const char* what, int row, int col) {
    char reason[REASON_SIZE];
    rmw_qos_compatibility_type_t got = check(pub, sub, reason);
    if (got != expected) {
        printf("%s pub %d sub %d: expected %d, got %d (%s)\n", what, row, col, (int)expected, (int)got, reason);
    }
    assert(got == expected);
    switch (expected) {
    case RMW_QOS_COMPATIBILITY_OK:
        assert('\0' == reason[0]);
        break;
    case RMW_QOS_COMPATIBILITY_WARNING:
        assert(NULL == strstr(reason, "ERROR:") && 0 == strncmp(reason, "WARNING: ", 9));
        break;
    default:
        assert(0 == strncmp(reason, "ERROR: ", 7));
        break;
    }
    if ('\0' != reason[0]) {
        assert(';' == reason[strlen(reason) - 1]);
    }
}

static void test_reliability(void) {
    const rmw_qos_reliability_policy_t kinds[KINDS] = {
        RMW_QOS_POLICY_RELIABILITY_SYSTEM_DEFAULT, RMW_QOS_POLICY_RELIABILITY_RELIABLE,
        RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT, RMW_QOS_POLICY_RELIABILITY_UNKNOWN,
        RMW_QOS_POLICY_RELIABILITY_BEST_AVAILABLE};
    // publisher row, subscription column: SYSTEM_DEFAULT RELIABLE BEST_EFFORT UNKNOWN BEST_AVAILABLE
    const rmw_qos_compatibility_type_t expected[KINDS][KINDS] = {
        {W, W, O, W, W}, // SYSTEM_DEFAULT
        {O, O, O, O, O}, // RELIABLE meets every request
        {W, E, O, W, W}, // BEST_EFFORT
        {W, W, O, W, W}, // UNKNOWN
        {W, W, O, W, W}, // BEST_AVAILABLE
    };
    for (int pub_kind = 0; pub_kind < KINDS; pub_kind++) {
        for (int sub_kind = 0; sub_kind < KINDS; sub_kind++) {
            rmw_qos_profile_t pub = known();
            rmw_qos_profile_t sub = known();
            pub.reliability = kinds[pub_kind];
            sub.reliability = kinds[sub_kind];
            expect_verdict(expected[pub_kind][sub_kind], &pub, &sub, "reliability", pub_kind, sub_kind);
        }
    }
}

static void test_durability(void) {
    const rmw_qos_durability_policy_t kinds[KINDS] = {
        RMW_QOS_POLICY_DURABILITY_SYSTEM_DEFAULT, RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL,
        RMW_QOS_POLICY_DURABILITY_VOLATILE, RMW_QOS_POLICY_DURABILITY_UNKNOWN,
        RMW_QOS_POLICY_DURABILITY_BEST_AVAILABLE};
    // SYSTEM_DEFAULT TRANSIENT_LOCAL VOLATILE UNKNOWN BEST_AVAILABLE
    const rmw_qos_compatibility_type_t expected[KINDS][KINDS] = {
        {W, W, O, W, W}, {O, O, O, O, O}, {W, E, O, W, W}, {W, W, O, W, W}, {W, W, O, W, W},
    };
    for (int pub_kind = 0; pub_kind < KINDS; pub_kind++) {
        for (int sub_kind = 0; sub_kind < KINDS; sub_kind++) {
            rmw_qos_profile_t pub = known();
            rmw_qos_profile_t sub = known();
            pub.durability = kinds[pub_kind];
            sub.durability = kinds[sub_kind];
            expect_verdict(expected[pub_kind][sub_kind], &pub, &sub, "durability", pub_kind, sub_kind);
        }
    }
}

static void test_liveliness(void) {
    const rmw_qos_liveliness_policy_t kinds[KINDS] = {
        RMW_QOS_POLICY_LIVELINESS_SYSTEM_DEFAULT, RMW_QOS_POLICY_LIVELINESS_AUTOMATIC,
        RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC, RMW_QOS_POLICY_LIVELINESS_UNKNOWN,
        RMW_QOS_POLICY_LIVELINESS_BEST_AVAILABLE};
    // SYSTEM_DEFAULT AUTOMATIC MANUAL_BY_TOPIC UNKNOWN BEST_AVAILABLE
    const rmw_qos_compatibility_type_t expected[KINDS][KINDS] = {
        {W, O, W, W, W}, // SYSTEM_DEFAULT
        {W, O, E, W, W}, // AUTOMATIC
        {O, O, O, O, O}, // MANUAL_BY_TOPIC meets every request
        {W, O, W, W, W}, // UNKNOWN
        {W, O, W, W, W}, // BEST_AVAILABLE
    };
    for (int pub_kind = 0; pub_kind < KINDS; pub_kind++) {
        for (int sub_kind = 0; sub_kind < KINDS; sub_kind++) {
            rmw_qos_profile_t pub = known();
            rmw_qos_profile_t sub = known();
            pub.liveliness = kinds[pub_kind];
            sub.liveliness = kinds[sub_kind];
            expect_verdict(expected[pub_kind][sub_kind], &pub, &sub, "liveliness", pub_kind, sub_kind);
        }
    }
}

// DEADLINE and the LIVELINESS lease share one rule: unspecified, infinite, 1 s, 2 s, BEST_AVAILABLE.
static const rmw_qos_compatibility_type_t duration_expected[KINDS][KINDS] = {
    {O, O, E, E, O}, // unspecified: infinite, meets no finite request
    {O, O, E, E, O}, // infinite: the same
    {O, O, O, O, O}, // 1 s meets 1 s and 2 s
    {O, O, E, O, O}, // 2 s does not meet 1 s
    {O, O, W, W, O}, // BEST_AVAILABLE: decided at creation
};

static void durations_of(rmw_time_t values[KINDS], rmw_time_t best_available) {
    values[0] = (rmw_time_t)RMW_DURATION_UNSPECIFIED;
    values[1] = (rmw_time_t)RMW_DURATION_INFINITE;
    values[2] = (rmw_time_t) {.sec = 1, .nsec = 0};
    values[3] = (rmw_time_t) {.sec = 2, .nsec = 0};
    values[4] = best_available;
}

static void test_deadline(void) {
    rmw_time_t values[KINDS];
    durations_of(values, (rmw_time_t)RMW_QOS_DEADLINE_BEST_AVAILABLE);
    for (int pub_kind = 0; pub_kind < KINDS; pub_kind++) {
        for (int sub_kind = 0; sub_kind < KINDS; sub_kind++) {
            rmw_qos_profile_t pub = known();
            rmw_qos_profile_t sub = known();
            pub.deadline = values[pub_kind];
            sub.deadline = values[sub_kind];
            expect_verdict(duration_expected[pub_kind][sub_kind], &pub, &sub, "deadline", pub_kind, sub_kind);
        }
    }
}

static void test_lease(void) {
    rmw_time_t values[KINDS];
    durations_of(values, (rmw_time_t)RMW_QOS_LIVELINESS_LEASE_DURATION_BEST_AVAILABLE);
    for (int pub_kind = 0; pub_kind < KINDS; pub_kind++) {
        for (int sub_kind = 0; sub_kind < KINDS; sub_kind++) {
            rmw_qos_profile_t pub = known();
            rmw_qos_profile_t sub = known();
            pub.liveliness_lease_duration = values[pub_kind];
            sub.liveliness_lease_duration = values[sub_kind];
            expect_verdict(duration_expected[pub_kind][sub_kind], &pub, &sub, "lease", pub_kind, sub_kind);
        }
    }
}

static int count(const char* text, const char* what) {
    int found = 0;
    for (const char* at = strstr(text, what); NULL != at; at = strstr(at + 1, what)) {
        found++;
    }
    return found;
}

static void test_reason(void) {
    char reason[REASON_SIZE];
    rmw_qos_profile_t pub = known();
    rmw_qos_profile_t sub = known();
    // The control: all known and compatible.
    assert(RMW_QOS_COMPATIBILITY_OK == check(&pub, &sub, reason) && '\0' == reason[0]);

    // Every policy an error: five entries, in the order the policies are checked.
    pub.reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
    sub.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
    sub.durability = RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL;
    pub.deadline = (rmw_time_t) {.sec = 2, .nsec = 0};
    sub.deadline = (rmw_time_t) {.sec = 1, .nsec = 0};
    sub.liveliness = RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC;
    pub.liveliness_lease_duration = (rmw_time_t) {.sec = 2, .nsec = 0};
    sub.liveliness_lease_duration = (rmw_time_t) {.sec = 1, .nsec = 0};
    assert(RMW_QOS_COMPATIBILITY_ERROR == check(&pub, &sub, reason));
    printf("every policy: %s\n", reason);
    assert(5 == count(reason, "ERROR: ") && 0 == count(reason, "WARNING: ") && 5 == count(reason, ";"));
    const char* reliability = strstr(reason, "reliable");
    const char* durability = strstr(reason, "transient local");
    const char* deadline = strstr(reason, "deadline");
    const char* liveliness = strstr(reason, "manual by topic");
    const char* lease = strstr(reason, "lease");
    assert(reliability && durability && deadline && liveliness && lease);
    assert(reliability < durability && durability < deadline && deadline < liveliness && liveliness < lease);

    // Errors before warnings, although the warning's policy is checked first.
    pub = known();
    sub = known();
    pub.reliability = RMW_QOS_POLICY_RELIABILITY_SYSTEM_DEFAULT; // a warning, reliability
    pub.durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;         // an error, durability
    sub.durability = RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL;
    pub.deadline = (rmw_time_t)RMW_QOS_DEADLINE_BEST_AVAILABLE; // a warning, deadline
    sub.deadline = (rmw_time_t) {.sec = 1, .nsec = 0};
    assert(RMW_QOS_COMPATIBILITY_ERROR == check(&pub, &sub, reason));
    printf("errors and warnings: %s\n", reason);
    assert(1 == count(reason, "ERROR: ") && 2 == count(reason, "WARNING: "));
    assert(0 == strncmp(reason, "ERROR: ", 7) && strstr(reason, "ERROR: ") < strstr(reason, "WARNING: "));

    // rmw_qos_profile_default itself: liveliness SYSTEM_DEFAULT on both sides, a warning (as rmw_dds_common says).
    pub = rmw_qos_profile_default;
    sub = rmw_qos_profile_default;
    rmw_qos_compatibility_type_t defaults = check(&pub, &sub, reason);
    printf("rmw_qos_profile_default x2: %d %s\n", (int)defaults, reason);
    assert((RMW_QOS_POLICY_LIVELINESS_SYSTEM_DEFAULT == pub.liveliness ? W : O) == defaults);

    // A short buffer: cut, terminated, never overrun; the verdict unchanged. NULL with size 0 is allowed.
    char small[24];
    memset(small, 'x', sizeof(small));
    rmw_qos_compatibility_type_t verdict = O;
    pub = known();
    sub = known();
    pub.reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
    sub.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
    assert(RMW_RET_OK == rmw_qos_profile_check_compatible(pub, sub, &verdict, small, 16));
    assert(E == verdict && 15 == strlen(small) && 'x' == small[16]);
    assert(RMW_RET_OK == rmw_qos_profile_check_compatible(pub, sub, &verdict, NULL, 0) && E == verdict);
    // The argument checks.
    assert(RMW_RET_INVALID_ARGUMENT == rmw_qos_profile_check_compatible(pub, sub, NULL, small, sizeof(small)));
    rmw_reset_error();
    assert(RMW_RET_INVALID_ARGUMENT == rmw_qos_profile_check_compatible(pub, sub, &verdict, NULL, 8));
    rmw_reset_error();
}

int main(void) {
    test_reliability();
    test_durability();
    test_liveliness();
    test_deadline();
    test_lease();
    test_reason();
    printf("test_qos_compatible: %d checks, PASS\n", checked);
    return 0;
}
