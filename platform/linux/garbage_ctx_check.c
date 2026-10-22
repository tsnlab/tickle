// Copyright (c) 2025-2026 TSN Lab, Inc.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3, as published by the Free
// Software Foundation. A proprietary license is also available on request - see README.md.

// Contexts created in storage that was never zeroed, against the real Linux HAL (run by test.sh, test-linux).
//
// tt_Context is caller-owned and a caller need not zero it: the examples create theirs on the stack. reset_node_state()
// therefore has to leave every table either initialised or unread, and since 2026-10-22 it no longer writes the large
// ones in full (DESIGN.md, "What a context writes at creation"): tx_buffer and rx_buffer are written as datagrams are
// built and read, the scheduler heap and endpoints[] only up to their counts, the endpoint index behind an occupancy
// bitmap, an announce's fragment bitmap per source on first use, and a fragment slot when it is first claimed. This
// check is what says the stale bytes are really never read, by running everything that reads those tables from
// contexts whose storage holds a garbage pattern:
//
//   role a (one process, one address): A1 publishes four topics - best effort, reliable, and samples large enough to
//          be fragmented (where the build allows a sample that large) both reliable and best effort - and serves a
//          service. A2, a second context in the same process and so on the same host, subscribes to the two reliable
//          topics: the same-host path (the segment, where it is built in).
//   role b (another namespace): B1 subscribes to all four and calls the service. Its best-effort fragmented topic is
//          what the reassembly slots are for (a reliable reader puts fragments together in its reorder buffer).
//
// The build injects receive loss (TT_RX_DROP_PERCENT, hal_linux.c) so the reliable topics and the service are repaired,
// not merely delivered. Each process runs ROUNDS rounds. Round 1 creates the contexts in storage filled with the
// pattern; every later round destroys them and creates them again in the same storage without refilling it, which is
// the reset case: reset_node_state() over a context that has run, its tables full of the last round's state.
//
// The fill pattern is the second argument: a byte in hex ("a5"), "zero" (the control: zeroed storage, which must pass
// the same way), or "none" (malloc()'s storage left as it is - for valgrind's memcheck, which marks it undefined and
// reports a read of any byte nothing wrote, where a pattern would read as defined).
//
// PRE-REGISTERED READING, per round and per context, printed as one line and enforced below:
//   PASS  B1: every reliable sample (RELIABLE_COUNT) delivered once, in order, intact; at least BE_MIN_PCT% of each
//             best-effort topic's, none damaged; every call answered. A2: every reliable and fragmented
//             sample. A1: every sample published; retransmitted > 0 summed over the round (the loss was exercised -
//             a round in which nothing was resent did not test repair, and FAILS rather than passes).
//   The process exits 0 only when every round of every context passed, printing "garbage_ctx_check: PASS - all".
//
// Usage: garbage_ctx_check a|b <fill> <broadcast>   (env GC_ROUND_S: seconds a round lasts, default 14)
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "SetBool.h"

// Under MemorySanitizer (the Makefile's garbage_ctx_check_msan): a read of a byte nothing wrote is reported where it
// decides something, which is the question this check asks, more sharply than a fill pattern can. MSan learns what the
// kernel wrote through its interceptors, and it has none for epoll_pwait2(), which the HAL waits in - so this one is
// interposed here, makes the system call itself and marks the events the kernel returned as written.
#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
#include <sanitizer/msan_interface.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
int epoll_pwait2(int epfd, struct epoll_event* events, int maxevents, const struct timespec* timeout,
                 const sigset_t* sigmask) {
    long count = syscall(SYS_epoll_pwait2, epfd, events, maxevents, timeout, sigmask,
                         (size_t)(64 / 8)); // the kernel's sigset: 64 signals
    if (count > 0) {
        __msan_unpoison(events, (size_t)count * sizeof(*events));
    }
    return (int)count;
}
#endif
#endif

#define ROUNDS 2
#define RELIABLE_COUNT 100U
#define BE_MIN_PCT 50U
#define PCT 100U
#define CALL_COUNT 40U
// A request or its answer is lost one time in ten at each end (TT_RX_DROP_PERCENT): with the default 3 retries a call
// ends unanswered about once in 700, which over 80 calls failed this check one run in ten. With 20 a call is answered.
#define CALL_RETRIES 20U
#define PUBLISH_PERIOD_NS (10ULL * tt_MILLISECOND)
#define CALL_START_NS (1500ULL * tt_MILLISECOND)
#define MATCH_WAIT_NS (6ULL * tt_SECOND)
#define POLL_NS (50LL * (int64_t)tt_MILLISECOND)
#define ROUND_GAP_S 2U
#define ROUND_S_DEFAULT 14U
#define SMALL_BYTES 16U
#define DEPTH 128U // >= RELIABLE_COUNT: nothing published is ever overwritten before it is acknowledged
// Fragmented where the build allows a sample this large (rmw_tickle's build: 65507-byte datagrams, MTU fragments).
#define FRAG_WANT 20000U
#define FRAG_BYTES (FRAG_WANT + 64U <= tt_MAX_SAMPLE_LENGTH ? FRAG_WANT : 1024U)
#define FRAGMENTED (FRAG_BYTES == FRAG_WANT)

// --- the sample: a sequence number, then a pattern that depends on it, so a stale or misplaced byte is seen ---------

struct gc_sample {
    uint32_t seq;
    uint32_t bytes; // the encoded size
    bool intact;    // set by decode
};

// Any mix of the byte's offset and the sample's seq will do, as long as a byte from another sample or another offset
// is unlikely to match.
#define PATTERN_OFFSET_MUL 13U
#define PATTERN_SEQ_MUL 7U
#define PATTERN_OFFSET_SHIFT 9U
static uint8_t pattern(uint32_t i, uint32_t seq) {
    return (uint8_t)((i * PATTERN_OFFSET_MUL) + (seq * PATTERN_SEQ_MUL) + (i >> PATTERN_OFFSET_SHIFT));
}

static int32_t gc_encode_size(struct tt_Data* data) {
    return (int32_t)((const struct gc_sample*)data)->bytes;
}

static int32_t gc_encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    const struct gc_sample* sample = (const struct gc_sample*)data;
    if (len < sample->bytes || sample->bytes < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(payload, &sample->seq, sizeof(uint32_t));
    for (uint32_t i = sizeof(uint32_t); i < sample->bytes; i++) {
        payload[i] = pattern(i, sample->seq);
    }
    return (int32_t)sample->bytes;
}

static int32_t gc_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native_endian) {
    struct gc_sample* sample = (struct gc_sample*)data;
    (void)is_native_endian; // both ends are this machine
    if (len < sizeof(uint32_t)) {
        return -1;
    }
    memcpy(&sample->seq, payload, sizeof(uint32_t));
    sample->bytes = len;
    sample->intact = true;
    for (uint32_t i = sizeof(uint32_t); i < len; i++) {
        if (payload[i] != pattern(i, sample->seq)) {
            sample->intact = false;
            break;
        }
    }
    return 0;
}

static void gc_free(struct tt_Data* data) {
    (void)data;
}

static struct tt_TypeSupport small_type = {
    .name = "gc/Small",
    .data_size = sizeof(struct gc_sample),
    .data_encode_size = gc_encode_size,
    .data_encode = gc_encode,
    .data_decode = gc_decode,
    .data_free = gc_free,
    .max_encoded_size = SMALL_BYTES,
};

static struct tt_TypeSupport frag_type = {
    .name = "gc/Frag",
    .data_size = sizeof(struct gc_sample),
    .data_encode_size = gc_encode_size,
    .data_encode = gc_encode,
    .data_decode = gc_decode,
    .data_free = gc_free,
    .max_encoded_size = FRAG_BYTES,
};

// --- endpoints and what they saw -------------------------------------------------------------------------------------

struct gc_sub {
    struct tt_Subscriber sub; // first: the callback's pointer is this struct's
    const char* label;
    uint32_t bytes;    // what every sample must be
    uint32_t received; // delivered
    uint32_t next;     // the seq a reliable subscriber must see next
    uint32_t out_of_order;
    uint32_t damaged;
    void* storage;
};

struct gc_pub {
    struct tt_Publisher pub;
    const char* label;
    uint32_t bytes;
    uint32_t published;
    uint32_t wanted_matches;
    void* storage;
};

static void on_sample(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)time;
    (void)seq_no;
    struct gc_sub* sub = (struct gc_sub*)subscriber;
    const struct gc_sample* sample = (const struct gc_sample*)data;
    sub->received++;
    if (!sample->intact || sample->bytes != sub->bytes) {
        sub->damaged++;
    }
    if (sample->seq != sub->next) {
        sub->out_of_order++;
    }
    sub->next = sample->seq + 1U;
}

static uint32_t calls_made;
static uint32_t calls_answered;
static uint32_t calls_failed;
static bool call_outstanding;
static uint32_t served;

static int8_t on_request(struct tt_Server* server, struct SetBoolRequest* request, struct SetBoolResponse* response,
                         tt_RequestId request_id) {
    (void)server;
    (void)request_id;
    response->success = request->data;
    response->message = "gc";
    served++;
    return 0;
}

static void on_response(struct tt_Client* client, int8_t return_code, struct SetBoolResponse* response) {
    (void)client;
    call_outstanding = false;
    if (return_code == 0 && response != NULL && response->success == ((calls_made & 1U) != 0U)) {
        calls_answered++;
    } else {
        calls_failed++;
        printf("garbage_ctx_check: call %u failed: return_code %d, response %s\n", calls_made, (int)return_code,
               response == NULL ? "none" : "wrong");
    }
}

// --- the contexts ----------------------------------------------------------------------------------------------------

struct gc_context {
    const char* label;
    struct tt_Context* node; // the storage under test
    pthread_t thread; // NOLINT(misc-include-cleaner) - <pthread.h> is included; the tool maps pthread_t elsewhere
    uint64_t deadline_ns;
};

static uint8_t fill_byte;
static bool fill_none; // leave malloc()'s storage as it is
static bool fill_zero;
static const char* broadcast;

// The context's storage: allocated once, filled once (round 1), reused as it stands by every later round.
static struct tt_Context* storage_for(void) {
    struct tt_Context* node = (struct tt_Context*)malloc(sizeof(struct tt_Context));
    if (node == NULL) {
        return NULL;
    }
    if (fill_zero) {
        memset((void*)node, 0, sizeof(*node));
    } else if (!fill_none) {
        memset((void*)node, fill_byte, sizeof(*node));
    }
    return node;
}

static bool create(struct gc_context* ctx) {
    struct tt_InitOptions opts;
    tt_InitOptions_default(&opts);
    opts.net.broadcast = broadcast;
#if tt_FRAG_ENABLED
    // MTU fragments, so the reassembly slots are used whatever the cost model would choose.
    opts.net.large_fragmentation = tt_LARGE_FRAGMENTATION_MTU;
#endif
    tt_ret_t ret = tt_Context_create_with(ctx->node, &opts);
    if (ret != tt_RET_OK) {
        printf("garbage_ctx_check: %s: tt_Context_create_with() returned %d\n", ctx->label, (int)ret);
        return false;
    }
    return true;
}

static bool make_pub(struct tt_Context* node, struct gc_pub* pub, const struct tt_TypeSupport* type, const char* topic,
                     bool reliable) {
    struct tt_QosProfile qos = reliable ? tt_QOS_PROFILE_DEFAULT : tt_QOS_PROFILE_SENSOR_DATA;
    qos.history.depth = DEPTH;
    size_t size = tt_Publisher_storage_size_for(node, type, &qos);
    pub->storage = calloc(1, size);
    if (pub->storage == NULL) {
        return false;
    }
    tt_ret_t ret =
        tt_Node_create_publisher(tt_Context_default_node(node), &pub->pub, type, topic, &qos, NULL, pub->storage, size);
    if (ret != tt_RET_OK) {
        printf("garbage_ctx_check: publisher %s: %d\n", topic, (int)ret);
        return false;
    }
    return true;
}

static bool make_sub(struct tt_Context* node, struct gc_sub* sub, const struct tt_TypeSupport* type, const char* topic,
                     bool reliable) {
    struct tt_QosProfile qos = reliable ? tt_QOS_PROFILE_DEFAULT : tt_QOS_PROFILE_SENSOR_DATA;
    qos.history.depth = DEPTH;
    size_t size = tt_Subscriber_storage_size_for(node, type, &qos);
    sub->storage = calloc(1, size);
    if (sub->storage == NULL) {
        return false;
    }
    struct tt_SubscriberOptions options = {.callback = on_sample};
    tt_ret_t ret = tt_Node_create_subscriber(tt_Context_default_node(node), &sub->sub, type, topic, &qos, &options,
                                             sub->storage, size);
    if (ret != tt_RET_OK) {
        printf("garbage_ctx_check: subscriber %s: %d\n", topic, (int)ret);
        return false;
    }
    return true;
}

// --- role a ----------------------------------------------------------------------------------------------------------

#define PUBS 4
static struct gc_pub pubs[PUBS];
static uint64_t pump_started_ns;
static bool pump_going;
static uint32_t pump_tick;

static void pump(struct tt_Context* node, uint64_t time, void* param) {
    (void)param;
    if (!pump_going) {
        bool matched = true;
        for (int i = 0; i < PUBS; i++) {
            matched = matched && tt_Publisher_matched_count(&pubs[i].pub) >= pubs[i].wanted_matches;
        }
        if (!matched && time - pump_started_ns < MATCH_WAIT_NS) {
            (void)tt_Context_schedule(node, time + PUBLISH_PERIOD_NS, pump, NULL);
            return;
        }
        pump_going = true;
    }
    // Each topic first in turn: the HAL's injected loss drops only the first datagram a wait reads (tt_receive()), so
    // a fixed order would lose only the first topic's.
    bool more = false;
    pump_tick++;
    for (uint32_t k = 0; k < PUBS; k++) {
        struct gc_pub* pub = &pubs[(pump_tick + k) % PUBS];
        if (pub->published >= RELIABLE_COUNT) {
            continue;
        }
        struct gc_sample sample = {.seq = pub->published, .bytes = pub->bytes};
        if (tt_Publisher_publish(&pub->pub, (struct tt_Data*)&sample) == tt_RET_OK) {
            pub->published++;
        }
        more = more || pub->published < RELIABLE_COUNT;
    }
    if (more) {
        (void)tt_Context_schedule(node, time + PUBLISH_PERIOD_NS, pump, NULL);
    }
}

// --- role b ----------------------------------------------------------------------------------------------------------

static struct tt_Client client;
static uint64_t calls_from_ns;

static void caller(struct tt_Context* node, uint64_t time, void* param) {
    (void)param;
    if (calls_made >= CALL_COUNT) {
        return;
    }
    if (time >= calls_from_ns && !call_outstanding) {
        struct SetBoolRequest request = {.data = ((calls_made + 1U) & 1U) != 0U};
        if (tt_Client_call(&client, (struct tt_Request*)&request) == tt_RET_OK) {
            calls_made++;
            call_outstanding = true;
        }
    }
    (void)tt_Context_schedule(node, time + PUBLISH_PERIOD_NS, caller, NULL);
}

// --- polling ---------------------------------------------------------------------------------------------------------

static void* poll_until(void* param) {
    struct gc_context* ctx = (struct gc_context*)param;
    while (tt_get_ns() < ctx->deadline_ns) {
        tt_ret_t ret = tt_Context_poll(ctx->node, POLL_NS);
        if (ret != tt_RET_OK && ret != tt_RET_TIMEOUT) {
            printf("garbage_ctx_check: %s: tt_Context_poll() returned %d\n", ctx->label, (int)ret);
            break;
        }
    }
    return NULL;
}

static bool sub_complete(const struct gc_sub* sub, const char* where, int round) {
    bool passed = sub->received == RELIABLE_COUNT && sub->out_of_order == 0 && sub->damaged == 0;
    printf("garbage_ctx_check: round %d %s %s received=%u/%u out_of_order=%u damaged=%u %s\n", round, where, sub->label,
           sub->received, RELIABLE_COUNT, sub->out_of_order, sub->damaged, passed ? "ok" : "FAIL");
    return passed;
}

static int run_a(int round, struct tt_Context* a1_node, struct tt_Context* a2_node, uint64_t round_ns) {
    struct gc_context ctx_a1 = {.label = "A1", .node = a1_node};
    struct gc_context ctx_a2 = {.label = "A2", .node = a2_node};
    memset(pubs, 0, sizeof(pubs));
    static struct gc_sub a2_subs[2];
    memset(a2_subs, 0, sizeof(a2_subs));
    static struct tt_Server server; // static: rmw_tickle's build makes it larger than a thread's stack
    memset(&server, 0, sizeof(server));
    served = 0;
    pump_going = false;
    pump_tick = 0;
    if (!create(&ctx_a1) || !create(&ctx_a2)) {
        return 1;
    }
    pubs[0] = (struct gc_pub) {.label = "be", .bytes = SMALL_BYTES, .wanted_matches = 1};
    pubs[1] = (struct gc_pub) {.label = "rel", .bytes = SMALL_BYTES, .wanted_matches = 2};
    pubs[2] = (struct gc_pub) {.label = "frag", .bytes = FRAG_BYTES, .wanted_matches = 2};
    pubs[3] = (struct gc_pub) {.label = "frag_be", .bytes = FRAG_BYTES, .wanted_matches = 1};
    a2_subs[0] = (struct gc_sub) {.label = "rel", .bytes = SMALL_BYTES};
    a2_subs[1] = (struct gc_sub) {.label = "frag", .bytes = FRAG_BYTES};
    struct tt_ServerOptions server_options = {.callback = (tt_SERVER_CALLBACK)on_request};
    if (!make_pub(ctx_a1.node, &pubs[0], &small_type, "gc_be", false) ||
        !make_pub(ctx_a1.node, &pubs[1], &small_type, "gc_rel", true) ||
        !make_pub(ctx_a1.node, &pubs[2], &frag_type, "gc_frag", true) ||
        !make_pub(ctx_a1.node, &pubs[3], &frag_type, "gc_frag_be", false) ||
        tt_Node_create_server(tt_Context_default_node(ctx_a1.node), &server, &SetBoolService, "gc_svc", NULL,
                              &server_options, NULL, 0) != tt_RET_OK ||
        !make_sub(ctx_a2.node, &a2_subs[0], &small_type, "gc_rel", true) ||
        !make_sub(ctx_a2.node, &a2_subs[1], &frag_type, "gc_frag", true)) {
        return 1;
    }
    uint64_t now = tt_get_ns();
    pump_started_ns = now;
    (void)tt_Context_schedule(ctx_a1.node, now, pump, NULL);
    ctx_a1.deadline_ns = now + round_ns;
    ctx_a2.deadline_ns = now + round_ns;
    pthread_create(&ctx_a1.thread, NULL, poll_until, &ctx_a1);
    pthread_create(&ctx_a2.thread, NULL, poll_until, &ctx_a2);
    pthread_join(ctx_a1.thread, NULL);
    pthread_join(ctx_a2.thread, NULL);

    bool passed = true;
    uint32_t retransmitted = 0;
    for (int i = 0; i < PUBS; i++) {
        retransmitted += pubs[i].pub.retransmitted;
        printf("garbage_ctx_check: round %d A1 %s published=%u/%u matched=%u retransmitted=%u\n", round, pubs[i].label,
               pubs[i].published, RELIABLE_COUNT, (unsigned)tt_Publisher_matched_count(&pubs[i].pub),
               pubs[i].pub.retransmitted);
        passed = passed && pubs[i].published == RELIABLE_COUNT;
    }
    uint64_t shm = ctx_a1.node->tx_datagrams_by_transport[tt_TRANSPORT_SHM];
    unsigned long long reassembled = 0;
#if tt_FRAG_ENABLED
    reassembled = (unsigned long long)ctx_a2.node->frag_reassembled;
#endif
    printf("garbage_ctx_check: round %d A1 served=%u tx_shm=%llu frag_reassembled(A2)=%llu fragmented=%d\n", round,
           served, (unsigned long long)shm, reassembled, FRAGMENTED ? 1 : 0);
    if (retransmitted == 0) {
        printf("garbage_ctx_check: round %d A1: nothing was retransmitted - the loss was not exercised: FAIL\n", round);
        passed = false;
    }
#if tt_SEGMENT_ENABLED
    if (shm == 0) {
        printf("garbage_ctx_check: round %d A1: nothing went over shared memory to A2: FAIL\n", round);
        passed = false;
    }
#endif
    passed = sub_complete(&a2_subs[0], "A2", round) && passed;
    passed = sub_complete(&a2_subs[1], "A2", round) && passed;
    tt_Context_destroy(ctx_a1.node);
    tt_Context_destroy(ctx_a2.node);
    for (int i = 0; i < PUBS; i++) {
        free(pubs[i].storage);
    }
    free(a2_subs[0].storage);
    free(a2_subs[1].storage);
    return passed ? 0 : 1;
}

static int run_b(int round, struct tt_Context* b1_node, uint64_t round_ns) {
    struct gc_context ctx_b1 = {.label = "B1", .node = b1_node};
    static struct gc_sub subs[4];
    memset(subs, 0, sizeof(subs));
    memset(&client, 0, sizeof(client));
    calls_made = 0;
    calls_answered = 0;
    calls_failed = 0;
    call_outstanding = false;
    if (!create(&ctx_b1)) {
        return 1;
    }
    subs[0] = (struct gc_sub) {.label = "be", .bytes = SMALL_BYTES};
    subs[1] = (struct gc_sub) {.label = "rel", .bytes = SMALL_BYTES};
    subs[2] = (struct gc_sub) {.label = "frag", .bytes = FRAG_BYTES};
    subs[3] = (struct gc_sub) {.label = "frag_be", .bytes = FRAG_BYTES};
    struct tt_ClientOptions client_options = {.callback = (tt_CLIENT_CALLBACK)on_response,
                                              .call_retry_count = CALL_RETRIES};
    if (!make_sub(ctx_b1.node, &subs[0], &small_type, "gc_be", false) ||
        !make_sub(ctx_b1.node, &subs[1], &small_type, "gc_rel", true) ||
        !make_sub(ctx_b1.node, &subs[2], &frag_type, "gc_frag", true) ||
        !make_sub(ctx_b1.node, &subs[3], &frag_type, "gc_frag_be", false) ||
        tt_Node_create_client(tt_Context_default_node(ctx_b1.node), &client, &SetBoolService, "gc_svc", NULL,
                              &client_options, NULL, 0) != tt_RET_OK) {
        return 1;
    }
    uint64_t now = tt_get_ns();
    calls_from_ns = now + CALL_START_NS;
    (void)tt_Context_schedule(ctx_b1.node, now, caller, NULL);
    ctx_b1.deadline_ns = now + round_ns;
    poll_until(&ctx_b1);

    bool passed = true;
    for (int i = 0; i < 4; i += 3) { // the two best-effort topics: most samples, none damaged
        bool be_passed = subs[i].received * PCT >= RELIABLE_COUNT * BE_MIN_PCT && subs[i].damaged == 0;
        printf("garbage_ctx_check: round %d B1 %s received=%u/%u (need %u%%) damaged=%u %s\n", round, subs[i].label,
               subs[i].received, RELIABLE_COUNT, BE_MIN_PCT, subs[i].damaged, be_passed ? "ok" : "FAIL");
        passed = passed && be_passed;
    }
    passed = sub_complete(&subs[1], "B1", round) && passed;
    passed = sub_complete(&subs[2], "B1", round) && passed;
    bool calls_passed = calls_made == CALL_COUNT && calls_answered == CALL_COUNT && calls_failed == 0;
    printf("garbage_ctx_check: round %d B1 calls made=%u answered=%u failed=%u (need %u) %s\n", round, calls_made,
           calls_answered, calls_failed, CALL_COUNT, calls_passed ? "ok" : "FAIL");
    passed = passed && calls_passed;
#if tt_FRAG_ENABLED
    printf("garbage_ctx_check: round %d B1 frag_reassembled=%llu frag_abandoned=%llu\n", round,
           (unsigned long long)ctx_b1.node->frag_reassembled, (unsigned long long)ctx_b1.node->frag_abandoned);
#endif
    tt_Context_destroy(ctx_b1.node);
    for (int i = 0; i < 4; i++) {
        free(subs[i].storage);
    }
    return passed ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc != 4 || (strcmp(argv[1], "a") != 0 && strcmp(argv[1], "b") != 0)) {
        fprintf(stderr, "usage: %s a|b <hex byte|zero|none> <broadcast>\n", argv[0]);
        return 2;
    }
    bool role_a = strcmp(argv[1], "a") == 0;
    fill_none = strcmp(argv[2], "none") == 0;
    fill_zero = strcmp(argv[2], "zero") == 0;
    if (!fill_none && !fill_zero) {
        fill_byte = (uint8_t)strtoul(argv[2], NULL, 16);
    }
    broadcast = argv[3];
    const char* round_env = getenv("GC_ROUND_S");
    uint64_t round_s = round_env != NULL ? strtoull(round_env, NULL, 10) : ROUND_S_DEFAULT;
    printf("garbage_ctx_check: role %s fill %s, sizeof(struct tt_Context) %zu, sample %u bytes (fragmented %d), "
           "rounds %d of %llu s\n",
           argv[1], argv[2], sizeof(struct tt_Context), FRAG_BYTES, FRAGMENTED ? 1 : 0, ROUNDS,
           (unsigned long long)round_s);

    struct tt_Context* first = storage_for();
    struct tt_Context* second = role_a ? storage_for() : NULL;
    if (first == NULL || (role_a && second == NULL)) {
        printf("garbage_ctx_check: out of memory\n");
        return 1;
    }
    int failures = 0;
    for (int round = 1; round <= ROUNDS; round++) {
        // Later rounds: the same storage, not refilled - a context reset over the state the last round left.
        int failed =
            role_a ? run_a(round, first, second, round_s * tt_SECOND) : run_b(round, first, round_s * tt_SECOND);
        printf("garbage_ctx_check: round %d %s\n", round, failed == 0 ? "PASS" : "FAIL");
        failures += failed;
        if (round < ROUNDS) {
            sleep(ROUND_GAP_S);
        }
    }
    free(first);
    free(second);
    printf("garbage_ctx_check: %s\n", failures == 0 ? "PASS - all" : "FAIL");
    fflush(stdout);
    return failures == 0 ? 0 : 1;
}
