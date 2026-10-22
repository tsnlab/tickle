/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// ROLE=segment: two TickLE contexts in tasks of ONE FreeRTOS image, exchanging samples through the shared-memory
// segment the HAL provides (DESIGN.md section 10, "The segment on FreeRTOS") instead of through lwIP. Discovery and
// matching stay UDP. Context A (id 11) publishes a RELIABLE topic and a BEST_EFFORT one; context B (id 12) subscribes
// to both. Each context is polled by its own task; a control task publishes and decides.
//
// SEGMENT_TEST_ARM (test.sh builds each arm as its own image; 1-3 with -Dtt_SEGMENT_ENABLED=1):
//   1 shm     - the segment, its memory from the HAL's default allocator (the FreeRTOS heap)
//   2 static  - the segment, its memory from a static region given to tt_freertos_set_segment_memory()
//   3 refused - the segment compiled in, but the allocator refuses: no segment exists
//   4 off     - the module compiled out (-Dtt_SEGMENT_ENABLED=0), the FreeRTOS default
//
// The reading, pre-registered and enforced below (decide()), not only described here. Counts are deltas over the
// measured window: N samples per topic, published once both directions are attached (arms 1-2) or once matched
// (arms 3-4), after a warm-up.
//   every arm: B delivers each RELIABLE window sample exactly once, and no BEST_EFFORT sample twice or unpublished.
//   BEST_EFFORT exactly once in arms 1-2, where the ring cannot lose one; in arms 3-4 at least 95% of N, since over
//   lwIP it may miss samples (README) - its loopback lost 2 of 200 once (2026-10-21) while RELIABLE recovered its own.
//   arms 1-2 (the claim):
//     A tx over the segment >= 2N, and B rx over the segment >= 2N: every sample went through the ring;
//     A unicast over UDP (tx UDP minus broadcasts) == 0: no sample, ACKNACK, heartbeat or doorbell went through lwIP;
//     A broadcasts < N/10: broadcasts (announces, summaries) are too few to have carried the samples either;
//     B woken by the HAL's doorbell > 0: the reader slept and its writer rang it - not a timeout that happened to help;
//     both contexts built a segment.
//   arm 2 also: after both contexts are destroyed, every region the HAL took from the static allocator was given back.
//   arms 3-4 (the fallback, and the control that the criteria above can fail):
//     A and B moved nothing over the segment, built none, and A's unicast over UDP >= 2N - the samples went through
//     lwIP, which is what the counters above must see when the segment is absent.
// FAIL on any one. A run that ends without reaching the verdict exits through test.sh's timeout, never as a PASS.

#include <FreeRTOS.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <task.h>

#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "UInt64.h"
#include "board/uart.h"
#include "net_init.h"

#ifndef SEGMENT_TEST_ARM // unnamed: the arm the build's own tt_SEGMENT_ENABLED makes (FreeRTOS default: off)
#if tt_SEGMENT_ENABLED
#define SEGMENT_TEST_ARM 1
#else
#define SEGMENT_TEST_ARM 4
#endif
#endif
#define ARM_SHM 1
#define ARM_STATIC 2
#define ARM_REFUSED 3
#define ARM_OFF 4
#if (SEGMENT_TEST_ARM == ARM_OFF) == (tt_SEGMENT_ENABLED != 0)
#error "SEGMENT_TEST_ARM 4 is the build with tt_SEGMENT_ENABLED=0, and only that build"
#endif
#define SEGMENT_ARM (SEGMENT_TEST_ARM == ARM_SHM || SEGMENT_TEST_ARM == ARM_STATIC)

#define CONTEXT_A_ID 11
#define CONTEXT_B_ID 12
#define WINDOW_SAMPLES 200U          // N, per topic
#define WARMUP_FLAG (1ULL << 40)     // warm-up samples' values; the window's are 0..N-1
#define BEST_EFFORT_UDP_LOSS_DIV 20U // arms 3-4: BEST_EFFORT may lose up to N/20 over lwIP
#define WARMUP_SAMPLES_UDP 20U       // arms 3-4: warm-up samples per topic (1-2 warm up until attached)
#define MATCH_WAIT_MS 10000U         // both publishers matched, from the first announce
#define ATTACH_WAIT_MS 5000U         // arms 1-2: both directions attached, publishing warm-up samples
#define WARMUP_BURST 20U             // arms 1-2: warm-up samples per topic per burst, one a tick
// Arms 1-2: the pause after each warm-up burst - two idle HEARTBEAT periods, so that A's writer asks B for an answer
// and B's ACKNACK, the only thing B sends A, takes B's direction (both_attached()).
#define WARMUP_IDLE_MS ((uint32_t)(2U * tt_IDLE_HEARTBEAT_PERIOD / tt_MILLISECOND))
#define DELIVERY_WAIT_MS 10000U               // the window's N + N delivered
#define SETTLE_MS 300U                        // after delivery, so that late acknowledgements land in the window
#define POLL_SLICE_NS (20LL * tt_MILLISECOND) // a poll task's longest wait, so that it sees `stop`
#define CONTROL_STACK_WORDS 2048
// A poll task's stack: twice the examples' 1,024 words. Its peak, reported below, was 830-1,015 words in every arm
// (2026-10-20), and 1,391 with the segment before seam_send_batch() stopped copying a batch onto the stack - so the
// margin is what this is for.
#define POLL_STACK_WORDS 2048

#define VIRT_TEST_FINISHER ((volatile uint32_t*)0x00100000UL)
#define FINISHER_PASS 0x5555U
#define FINISHER_FAIL 0x3333U

static struct tt_Context ctx_a;
static struct tt_Context ctx_b;
static struct tt_Publisher pub_reliable;
static struct tt_Publisher pub_best_effort;
static struct tt_Subscriber sub_reliable;
static struct tt_Subscriber sub_best_effort;
#define PUB_STORAGE_BYTES 4096U
#define SUB_STORAGE_BYTES 8192U
static uint64_t pub_reliable_storage[PUB_STORAGE_BYTES / sizeof(uint64_t)];
static uint64_t pub_best_effort_storage[PUB_STORAGE_BYTES / sizeof(uint64_t)];
static uint64_t sub_reliable_storage[SUB_STORAGE_BYTES / sizeof(uint64_t)];
static uint64_t sub_best_effort_storage[SUB_STORAGE_BYTES / sizeof(uint64_t)];

static bool failed;

static void check(bool passed, const char* what) {
    printf("segment: %s %s\n", passed ? "ok  " : "FAIL", what);
    if (!passed) {
        failed = true;
    }
}

static void finish(void) {
    printf("segment: %s\n", failed ? "FAIL" : "PASS");
    *VIRT_TEST_FINISHER = failed ? (FINISHER_FAIL | (1U << 16)) : FINISHER_PASS;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// One topic's deliveries in B: each window value once, counted apart from warm-up samples and from strays.
struct delivery {
    uint8_t seen[WINDOW_SAMPLES];
    uint32_t window;     // distinct window values delivered
    uint32_t duplicates; // a window value delivered twice
    uint32_t strays;     // neither a window value nor a warm-up one
    uint32_t warmup;
};
static struct delivery got_reliable;
static struct delivery got_best_effort;

static void note_delivery(struct delivery* got, uint64_t value) {
    if ((value & WARMUP_FLAG) != 0) {
        __atomic_add_fetch(&got->warmup, 1U, __ATOMIC_SEQ_CST);
    } else if (value >= WINDOW_SAMPLES) {
        __atomic_add_fetch(&got->strays, 1U, __ATOMIC_SEQ_CST);
    } else if (got->seen[value] != 0) {
        __atomic_add_fetch(&got->duplicates, 1U, __ATOMIC_SEQ_CST);
    } else {
        got->seen[value] = 1;
        __atomic_add_fetch(&got->window, 1U, __ATOMIC_SEQ_CST);
    }
}

static void on_reliable(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct UInt64Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    note_delivery(&got_reliable, data->data);
}

static void on_best_effort(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct UInt64Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    note_delivery(&got_best_effort, data->data);
}

#if SEGMENT_TEST_ARM == ARM_STATIC
// The application's own segment memory (hal_freertos.h): one fixed region per context, from a static array, with
// every hand-out and hand-back counted so the arm can show the HAL returns what it took.
#define STATIC_REGIONS 2U
#define STATIC_REGION_BYTES (tt_SEGMENT_BYTES + 1024U)
static uint8_t static_memory[STATIC_REGIONS][STATIC_REGION_BYTES] __attribute__((aligned(tt_SEGMENT_CACHE_LINE)));
static bool static_taken[STATIC_REGIONS];
static uint32_t static_allocs;
static uint32_t static_releases;

static void* static_alloc(size_t bytes) {
    if (bytes > STATIC_REGION_BYTES) {
        return NULL;
    }
    for (uint32_t i = 0; i < STATIC_REGIONS; i++) {
        if (!static_taken[i]) {
            static_taken[i] = true;
            static_allocs++;
            return static_memory[i];
        }
    }
    return NULL;
}

static void static_release(void* region, size_t bytes) {
    (void)bytes;
    for (uint32_t i = 0; i < STATIC_REGIONS; i++) {
        if (region == static_memory[i] && static_taken[i]) {
            static_taken[i] = false;
            static_releases++;
        }
    }
}
#elif SEGMENT_TEST_ARM == ARM_REFUSED
static void* refuse_alloc(size_t bytes) {
    (void)bytes;
    return NULL;
}

static void refuse_release(void* region, size_t bytes) {
    (void)region;
    (void)bytes;
}
#endif

// What a context's counters say about the window.
struct snapshot {
    uint64_t tx_udp;
    uint64_t tx_shm;
    uint64_t rx_udp;
    uint64_t rx_shm;
    uint64_t broadcast_to_udp;
    uint64_t segments_created;
    uint32_t bell_wakes;
};

static void take(const struct tt_Context* ctx, struct snapshot* out) {
    out->tx_udp = ctx->tx_datagrams_by_transport[tt_TRANSPORT_UDP];
    out->tx_shm = ctx->tx_datagrams_by_transport[tt_TRANSPORT_SHM];
    out->rx_udp = ctx->rx_datagrams_by_transport[tt_TRANSPORT_UDP];
    out->rx_shm = ctx->rx_datagrams_by_transport[tt_TRANSPORT_SHM];
    out->broadcast_to_udp = ctx->segment_broadcast_to_udp;
    out->segments_created = ctx->segments_created;
#if tt_SEGMENT_ENABLED
    out->bell_wakes = ctx->hal.bell_wakes;
#else
    out->bell_wakes = 0;
#endif
}

static volatile bool stop;
static volatile uint32_t poll_tasks_done;

static void poll_task(void* param) {
    struct tt_Context* ctx = param;
    while (!stop) {
        (void)tt_Context_poll(ctx, POLL_SLICE_NS);
    }
    printf("segment: poll task of context %u: stack peak %lu of %u words\n", ctx->id,
           (unsigned long)(POLL_STACK_WORDS - uxTaskGetStackHighWaterMark(NULL)), (unsigned)POLL_STACK_WORDS);
    __atomic_add_fetch(&poll_tasks_done, 1U, __ATOMIC_SEQ_CST);
    vTaskSuspend(NULL);
}

static bool create_context(struct tt_Context* ctx, int32_t id) {
    _tt_CONFIG.context_id = id; // read by tt_Context_create(): the two are created one after the other, here
    tt_ret_t ret = tt_Context_create(ctx);
    if (ret != tt_RET_OK) {
        printf("segment: tt_Context_create(%ld) failed: %d\n", (long)id, ret);
        return false;
    }
    printf("segment: context id=%u at %lu.%lu.%lu.%lu:%u\n", ctx->id, (unsigned long)(ctx->hal.own_ip >> 24),
           (unsigned long)((ctx->hal.own_ip >> 16) & 0xffU), (unsigned long)((ctx->hal.own_ip >> 8) & 0xffU),
           (unsigned long)(ctx->hal.own_ip & 0xffU), ctx->hal.own_port);
    return true;
}

static bool create_endpoints(void) {
    struct tt_Node* node_a = tt_Context_default_node(&ctx_a);
    struct tt_Node* node_b = tt_Context_default_node(&ctx_b);
    const struct tt_QosProfile* best_effort = &tt_QOS_PROFILE_SENSOR_DATA;
    struct tt_SubscriberOptions reliable_options = {.callback = (tt_SUBSCRIBER_CALLBACK)on_reliable};
    struct tt_SubscriberOptions best_effort_options = {.callback = (tt_SUBSCRIBER_CALLBACK)on_best_effort};
    tt_ret_t ret[4];
    ret[0] = tt_Node_create_publisher(node_a, &pub_reliable, &UInt64Topic, "segment_reliable", NULL, NULL,
                                      pub_reliable_storage, sizeof(pub_reliable_storage));
    ret[1] = tt_Node_create_publisher(node_a, &pub_best_effort, &UInt64Topic, "segment_best_effort", best_effort, NULL,
                                      pub_best_effort_storage, sizeof(pub_best_effort_storage));
    ret[2] = tt_Node_create_subscriber(node_b, &sub_reliable, &UInt64Topic, "segment_reliable", NULL, &reliable_options,
                                       sub_reliable_storage, sizeof(sub_reliable_storage));
    ret[3] = tt_Node_create_subscriber(node_b, &sub_best_effort, &UInt64Topic, "segment_best_effort", best_effort,
                                       &best_effort_options, sub_best_effort_storage, sizeof(sub_best_effort_storage));
    for (int i = 0; i < 4; i++) {
        if (ret[i] != tt_RET_OK) {
            printf("segment: endpoint %d failed: %d\n", i, ret[i]);
            return false;
        }
    }
    return true;
}

static bool publish(struct tt_Publisher* pub, uint64_t value) {
    struct UInt64Data data = {.data = value};
    return tt_Publisher_publish(pub, (struct tt_Data*)&data) == tt_RET_OK;
}

// Arms 1-2: whether each context has the other's segment attached, read from its peer table - the state the window's
// claim is about. A first send to a peer whose segment is not built yet goes over UDP and is not asked again for a
// while (segment.attach_retry_ns), so the window starts only once both directions are on the ring.
//
// A context attaches a peer's segment on its first send to that peer, never at discovery. A sends B every sample; B
// sends A only ACKNACKs, and a stream of samples a tick apart asks for none (each publish pushes the idle HEARTBEAT
// back, config.h). So B's direction cannot be waited for under a steady stream. On this PC an ACKNACK about a second
// into the stream took it; on CI's runner none came (57f46f16, 2026-10-21: B sent A 4 unicasts in the whole run, 2 over
// UDP before A's segment existed and 2 over the ring after the warm-up, none in its 5 s), so the check failed while the
// window itself went over the ring. The warm-up is therefore bursts, each followed by a pause long enough for A's idle
// HEARTBEAT and B's answer - which is what attaches B when its entry for A is cleared before the warm-up (measured,
// 3 of 3 attached 240-260 ms in, against 980 ms by the leftover ACKNACK with the pause at 0).
static bool attached(const struct tt_Context* ctx, uint8_t peer) {
#if tt_SEGMENT_ENABLED
    return ctx->segment_peer_live[peer] != 0 && ctx->segment_peers[peer].mapping != NULL;
#else
    (void)ctx;
    (void)peer;
    return false;
#endif
}

static bool both_attached(void) {
    return attached(&ctx_a, CONTEXT_B_ID) && attached(&ctx_b, CONTEXT_A_ID);
}

// Which direction is missing, and what its entry holds, when the warm-up ends without both: so a failure names the
// side and the state (never set up, cached "absent", due to be asked again) instead of only saying that one is missing.
static void print_attach(const char* who, const struct tt_Context* ctx, uint8_t peer) {
#if tt_SEGMENT_ENABLED
    const struct tt_SegmentPeer* entry = &ctx->segment_peers[peer];
    const bool live = ctx->segment_peer_live[peer] != 0;
    printf("segment: %s: set up %d, mapped %d, cached absent %d, due to be asked again %d\n", who, live,
           live && entry->mapping != NULL, live && entry->missing, live && entry->recheck_due);
#else
    (void)who;
    (void)ctx;
    (void)peer;
#endif
}

static void print_delta(const char* who, const struct snapshot* before, const struct snapshot* after) {
    printf("segment: %s tx_udp=%lu tx_shm=%lu rx_udp=%lu rx_shm=%lu broadcast=%lu segments=%lu bell_wakes=%lu\n", who,
           (unsigned long)(after->tx_udp - before->tx_udp), (unsigned long)(after->tx_shm - before->tx_shm),
           (unsigned long)(after->rx_udp - before->rx_udp), (unsigned long)(after->rx_shm - before->rx_shm),
           (unsigned long)(after->broadcast_to_udp - before->broadcast_to_udp), (unsigned long)after->segments_created,
           (unsigned long)(after->bell_wakes - before->bell_wakes));
}

static void decide(const struct snapshot* a_before, const struct snapshot* a_after, const struct snapshot* b_before,
                   const struct snapshot* b_after) {
    const uint64_t both_topics = 2ULL * WINDOW_SAMPLES;
    const uint64_t a_tx_shm = a_after->tx_shm - a_before->tx_shm;
    const uint64_t b_rx_shm = b_after->rx_shm - b_before->rx_shm;
    const uint64_t a_broadcast = a_after->broadcast_to_udp - a_before->broadcast_to_udp;
    const uint64_t a_unicast_udp = (a_after->tx_udp - a_before->tx_udp) - a_broadcast;
    printf("segment: arm %d, N=%u per topic; RELIABLE %lu/%u (dup %lu, stray %lu), BEST_EFFORT %lu/%u (dup %lu, stray "
           "%lu)\n",
           SEGMENT_TEST_ARM, WINDOW_SAMPLES, (unsigned long)got_reliable.window, WINDOW_SAMPLES,
           (unsigned long)got_reliable.duplicates, (unsigned long)got_reliable.strays,
           (unsigned long)got_best_effort.window, WINDOW_SAMPLES, (unsigned long)got_best_effort.duplicates,
           (unsigned long)got_best_effort.strays);
    print_delta("A", a_before, a_after);
    print_delta("B", b_before, b_after);
    printf("segment: A unicast over UDP %lu\n", (unsigned long)a_unicast_udp);

    check(got_reliable.window == WINDOW_SAMPLES && got_reliable.duplicates == 0 && got_reliable.strays == 0,
          "RELIABLE: every window sample delivered exactly once");
    check(got_best_effort.duplicates == 0 && got_best_effort.strays == 0, "BEST_EFFORT: no sample twice, none stray");
#if SEGMENT_ARM
    check(got_best_effort.window == WINDOW_SAMPLES, "BEST_EFFORT: every window sample delivered (the ring)");
#else
    check(got_best_effort.window >= WINDOW_SAMPLES - (WINDOW_SAMPLES / BEST_EFFORT_UDP_LOSS_DIV),
          "BEST_EFFORT: at least 95% of the window delivered (over lwIP)");
#endif
#if SEGMENT_ARM
    check(a_tx_shm >= both_topics, "A sent >= 2N datagrams over the segment");
    check(b_rx_shm >= both_topics, "B received >= 2N datagrams over the segment");
    check(a_unicast_udp == 0, "A sent no unicast datagram over UDP in the window");
    check(a_broadcast < WINDOW_SAMPLES / 10U, "A's broadcasts in the window are too few to carry the samples");
    check(b_after->bell_wakes - b_before->bell_wakes > 0, "B was woken by the HAL doorbell");
    check(a_after->segments_created >= 1 && b_after->segments_created >= 1, "both contexts built a segment");
#else
    check(a_tx_shm == 0 && b_rx_shm == 0, "nothing moved over the segment");
    check(a_after->segments_created == 0 && b_after->segments_created == 0, "no segment was built");
    check(a_unicast_udp >= both_topics,
          "A's samples went over UDP (unicast >= 2N): the counters see lwIP when it carries them");
#endif
}

static void control_task(void* param) {
    (void)param;
    net_init();
#if SEGMENT_TEST_ARM == ARM_STATIC
    tt_freertos_set_segment_memory(static_alloc, static_release);
#elif SEGMENT_TEST_ARM == ARM_REFUSED
    tt_freertos_set_segment_memory(refuse_alloc, refuse_release);
#endif
    if (!create_context(&ctx_a, CONTEXT_A_ID) || !create_context(&ctx_b, CONTEXT_B_ID) || !create_endpoints()) {
        failed = true;
        finish();
    }
    xTaskCreate(poll_task, "poll_a", POLL_STACK_WORDS, &ctx_a, tskIDLE_PRIORITY + 1, NULL);
    xTaskCreate(poll_task, "poll_b", POLL_STACK_WORDS, &ctx_b, tskIDLE_PRIORITY + 1, NULL);

    uint32_t waited = 0;
    while ((tt_Publisher_matched_count(&pub_reliable) == 0 || tt_Publisher_matched_count(&pub_best_effort) == 0) &&
           waited < MATCH_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    printf("segment: matched after %lu ms\n", (unsigned long)waited);
    check(waited < MATCH_WAIT_MS, "both publishers matched B over UDP discovery");

    // Warm-up: a sample a tick on each topic - for a fixed 20 (arms 3-4), or in bursts of WARMUP_BURST, each followed
    // by WARMUP_IDLE_MS of quiet, until both directions are attached (arms 1-2; both_attached() says why the quiet).
    uint64_t warm = 0;
    waited = 0;
    do {
        (void)publish(&pub_reliable, WARMUP_FLAG | warm);
        (void)publish(&pub_best_effort, WARMUP_FLAG | warm);
        warm++;
        vTaskDelay(pdMS_TO_TICKS(1));
        waited++;
#if SEGMENT_ARM
        for (uint32_t idle = 0; warm % WARMUP_BURST == 0 && idle < WARMUP_IDLE_MS && !both_attached(); idle += 10) {
            vTaskDelay(pdMS_TO_TICKS(10));
            waited += 10;
        }
#endif
    } while (SEGMENT_ARM ? (!both_attached() && waited < ATTACH_WAIT_MS) : warm < WARMUP_SAMPLES_UDP);
#if SEGMENT_ARM
    printf("segment: both directions attached after %lu warm-up samples, %lu ms\n", (unsigned long)warm,
           (unsigned long)waited);
    print_attach("A to B", &ctx_a, CONTEXT_B_ID);
    print_attach("B to A", &ctx_b, CONTEXT_A_ID);
    check(both_attached(), "both contexts attached the other's segment");
#endif
    vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));

    struct snapshot a_before;
    struct snapshot b_before;
    take(&ctx_a, &a_before);
    take(&ctx_b, &b_before);
    uint32_t refused = 0;
    for (uint64_t value = 0; value < WINDOW_SAMPLES; value++) {
        refused += publish(&pub_reliable, value) ? 0U : 1U;
        refused += publish(&pub_best_effort, value) ? 0U : 1U;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    check(refused == 0, "every window publish accepted");
    waited = 0;
    while ((got_reliable.window < WINDOW_SAMPLES || got_best_effort.window < WINDOW_SAMPLES) &&
           waited < DELIVERY_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    printf("segment: window delivered %lu ms after its last publish\n", (unsigned long)waited);
    vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
    struct snapshot a_after;
    struct snapshot b_after;
    take(&ctx_a, &a_after);
    take(&ctx_b, &b_after);
    decide(&a_before, &a_after, &b_before, &b_after);

    // Both contexts destroyed, as an application ends them: release_segments() detaches and unlinks through the HAL.
    stop = true;
    while (__atomic_load_n(&poll_tasks_done, __ATOMIC_SEQ_CST) < 2U) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    check(tt_Context_destroy(&ctx_b) == tt_RET_OK && tt_Context_destroy(&ctx_a) == tt_RET_OK,
          "both contexts destroyed");
#if SEGMENT_TEST_ARM == ARM_STATIC
    printf("segment: static regions handed out %lu, given back %lu\n", (unsigned long)static_allocs,
           (unsigned long)static_releases);
    check(static_allocs >= 2 && static_releases == static_allocs, "every static region the HAL took was given back");
#endif
    finish();
}

int main(void) {
    uart_init();
    printf("\ntickle/freertos: segment role boot OK (arm %d)\n", SEGMENT_TEST_ARM);
    xTaskCreate(control_task, "control", CONTROL_STACK_WORDS, NULL, tskIDLE_PRIORITY + 2, NULL);
    vTaskStartScheduler();
    for (;;) {
    }
}
