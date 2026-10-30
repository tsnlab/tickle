/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// One node of wifi_burst_loss_netns.sh (network profiles phase 3, Wi-Fi groundwork, 2026-10-22): a context with one
// link of the given profile, in one of three roles.
//
//   ping  publishes "wifi_ping" samples of -s BYTES at -r HZ for -d SECONDS once its Publisher has matched (it waits up
//         to MATCH_WAIT_S), subscribes "wifi_pong", and times each sample's round trip from its own send time to the
//         first pong naming it. Then it polls DRAIN_S more for late repairs.
//   echo  subscribes "wifi_ping", times each sample's one-way delay (both namespaces read the host's CLOCK_MONOTONIC),
//         and answers its first copy with a 16-byte pong of the same reliability from inside the callback. Polls for
//         -d SECONDS.
//   idle  one Subscriber of an unused topic, so its peers have an endpoint to see appear and depart; nothing else.
//         Polls for -d SECONDS. It carries no data, so its liveliness rests on its discovery summaries alone.
//
// -m be|rel: BEST_EFFORT or RELIABLE (the zeroed profile otherwise: KEEP_LAST 10, VOLATILE - rmw's default).
// -p ethernet|wifi: the link profile. RESULT ends with the discovery the context ran (2026-10-23): its miss threshold
// and what it sent of the WIFI profile's summary copies and announce repeats - the treatment a wifi arm names.
// The first -r samples (one second) are warm-up and not scored by either side.
//
// Every role watches discovery (tt_Context_set_discovery()) and prints one EVENT line per remote context's first
// appearance, departure and re-appearance, with CLOCK_MONOTONIC times, so the harness can tell a false peer death (a
// departure before that peer's own end_ns) from the farewell at its end. Each role prints one RESULT line: its
// create_ns and end_ns, what the link resolved to, the sample counts and latency quantiles of its role, the
// Publisher's own retransmission count, datagrams sent and received, and its CPU time. Built with -Dtt_RELIABLE_STATS
// it also prints RSTATS lines (reliable_stats_print.h).
//
// Usage: wifi_link_node ping|echo|idle -c CONTEXT_ID -b BROADCAST -d SECONDS [-m be|rel] [-s BYTES] [-r HZ]
//                       [-p ethernet|wifi] [-q STOP_FILE] [-l LATENCY_FILE]
// -q: echo and idle end as soon as STOP_FILE exists (the harness creates it once ping has ended), -d being only their
// bound. -l: the scored latencies (one-way for echo, round trip for ping), in microseconds, one per line, sorted.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h> // NOLINT(misc-include-cleaner) - getopt()/optarg: glibc-private headers

#include <sys/resource.h> // NOLINT(misc-include-cleaner) - getrusage(): struct rusage lives in a glibc-private header
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>
#ifdef tt_RELIABLE_STATS
#include "reliable_stats_print.h"
#endif

#define HEADER_BYTES 16U // send_ns (8), seq (4), reserved (4)
#define SEQ_OFFSET 8U
#define PONG_BYTES HEADER_BYTES
#define MAX_SEQ 65536U
#define MAX_EVENTS 64U
#define MAX_CONTEXTS 256U
#define NS_PER_S 1000000000ULL
#define NS_PER_US 1000ULL
#define NS_PER_MS 1000000ULL
#define US_PER_MS 1000.0
#define MATCH_WAIT_S 15ULL
#define DRAIN_S 3ULL
#define POLL_SLICE_NS (10ULL * NS_PER_MS)
#define MERGE_NS (200ULL * NS_PER_MS) // departures of one context's endpoints this close together are one death
#define DEFAULT_SIZE 100U
#define DEFAULT_RATE_HZ 100U
#define PERCENT 100U
#define P50 50U
#define P90 90U
#define P99 99U
#define MIN_ARGS 2

enum role { ROLE_PING, ROLE_ECHO, ROLE_IDLE };

struct options {
    enum role role;
    int context_id;
    const char* broadcast;
    uint64_t seconds;
    bool reliable;
    uint32_t size;
    uint32_t rate_hz;
    bool wifi;
    const char* stop_file;    // -q: echo and idle stop once this file exists (checked every poll slice)
    const char* latency_file; // -l: every scored latency, in microseconds, one per line
};

// The sample as the codec carries it: the first HEADER_BYTES of the payload.
struct header {
    uint64_t send_ns;
    uint32_t seq;
    uint32_t reserved;
};

struct event {
    uint64_t t_ns;
    uint8_t context_id;
    const char* what;
};

static struct tt_Context ctx;
static struct tt_Discovery discovery;
static struct tt_TypeSupport ping_topic;
static struct tt_TypeSupport pong_topic;
static struct tt_Publisher pub;
static struct tt_Subscriber sub;
static struct options opts;
static uint32_t ping_size = DEFAULT_SIZE;
static uint32_t warmup;

static uint8_t seen[MAX_SEQ];
static uint64_t latency_ns[MAX_SEQ];
static uint32_t scored;
static uint32_t duplicates;
static uint32_t below_warmup;
static uint32_t max_seq;
static uint32_t pongs_sent;
static uint32_t pongs_refused;

static uint64_t first_seen_ns[MAX_CONTEXTS];
static uint64_t departed_ns[MAX_CONTEXTS]; // the latest departure; 0: present (or never seen)
static struct event events[MAX_EVENTS];
static uint32_t event_count;
static uint32_t events_dropped;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // NOLINT(misc-include-cleaner) - CLOCK_MONOTONIC: a glibc-private header
    return ((uint64_t)ts.tv_sec * NS_PER_S) + (uint64_t)ts.tv_nsec;
}

static double cpu_ms(void) {
    struct rusage usage; // NOLINT(misc-include-cleaner) - see <sys/resource.h> above
    getrusage(RUSAGE_SELF, &usage);
    return ((double)usage.ru_utime.tv_sec * US_PER_MS) + ((double)usage.ru_utime.tv_usec / US_PER_MS) +
           ((double)usage.ru_stime.tv_sec * US_PER_MS) + ((double)usage.ru_stime.tv_usec / US_PER_MS);
}

// --- codec: the payload is the header, then zeros up to the topic's size ---------------------------------------------

static int32_t ping_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)ping_size;
}

static int32_t pong_encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)PONG_BYTES;
}

static int32_t encode(struct tt_Data* data, uint8_t* payload, uint32_t len) {
    memset(payload, 0, len);
    memcpy(payload, data, HEADER_BYTES);
    return (int32_t)len;
}

// Scores the sample as it is decoded: its one-way delay (echo) or round trip (ping), first copy only.
static int32_t decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)native;
    if (len < HEADER_BYTES) {
        return -1;
    }
    struct header* sample = (struct header*)data;
    memcpy(&sample->send_ns, payload, sizeof(sample->send_ns));
    memcpy(&sample->seq, payload + SEQ_OFFSET, sizeof(sample->seq));
    sample->reserved = 0;
    uint32_t seq = sample->seq;
    if (seq >= MAX_SEQ) {
        return (int32_t)len;
    }
    if (seen[seq] != 0) {
        duplicates++;
        sample->reserved = 1; // a copy: not answered again
        return (int32_t)len;
    }
    seen[seq] = 1;
    max_seq = seq > max_seq ? seq : max_seq;
    if (seq < warmup) {
        below_warmup++;
        return (int32_t)len;
    }
    latency_ns[scored++] = now_ns() - sample->send_ns;
    return (int32_t)len;
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static void on_ping(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)subscriber;
    (void)time;
    (void)seq_no;
    const struct header* sample = (const struct header*)data;
    if (sample->reserved != 0) {
        return;
    }
    struct header pong = {.send_ns = sample->send_ns, .seq = sample->seq, .reserved = 0};
    if (tt_Publisher_publish(&pub, (struct tt_Data*)&pong) == tt_RET_OK) {
        pongs_sent++;
    } else {
        pongs_refused++;
    }
}

static void on_pong(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)subscriber;
    (void)time;
    (void)seq_no;
    (void)data;
}

// --- discovery: first appearance, departure, re-appearance of each remote context -----------------------------------

static void note_event(uint8_t context_id, const char* what, uint64_t t_ns) {
    if (event_count >= MAX_EVENTS) {
        events_dropped++;
        return;
    }
    events[event_count++] = (struct event) {.t_ns = t_ns, .context_id = context_id, .what = what};
}

static void on_discovery(struct tt_Context* node, uint8_t context_id, uint32_t endpoint_id, uint8_t kind, bool departed,
                         void* param) {
    (void)node;
    (void)endpoint_id;
    (void)kind;
    (void)param;
    uint64_t t_ns = now_ns();
    if (departed) {
        // One death takes every endpoint of the context with it: one event.
        if (departed_ns[context_id] == 0 || t_ns - departed_ns[context_id] > MERGE_NS) {
            note_event(context_id, "depart", t_ns);
        }
        departed_ns[context_id] = t_ns;
        return;
    }
    if (first_seen_ns[context_id] == 0) {
        first_seen_ns[context_id] = t_ns;
        note_event(context_id, "appear", t_ns);
    } else if (departed_ns[context_id] != 0) {
        note_event(context_id, "reappear", t_ns);
    }
    departed_ns[context_id] = 0;
}

// --- setup ----------------------------------------------------------------------------------------------------------

static void set_topic(struct tt_TypeSupport* topic, const char* name, int32_t (*size)(struct tt_Data*),
                      uint32_t max_size) {
    topic->name = name;
    topic->data_size = sizeof(struct header);
    topic->data_encode_size = size;
    topic->data_encode = encode;
    topic->data_decode = decode;
    topic->data_free = free_nothing;
    topic->max_encoded_size = max_size;
}

static struct tt_QosProfile profile(void) {
    struct tt_QosProfile qos;
    memset(&qos, 0, sizeof(qos)); // the computed default: RELIABLE, KEEP_LAST 10, VOLATILE
    qos.reliability.kind = opts.reliable ? tt_QOS_RELIABILITY_RELIABLE : tt_QOS_RELIABILITY_BEST_EFFORT;
    return qos;
}

static bool create_publisher(const struct tt_TypeSupport* topic) {
    struct tt_QosProfile qos = profile();
    struct tt_PublisherOptions options;
    memset(&options, 0, sizeof(options));
    size_t bytes = tt_Publisher_storage_size(topic, &qos);
    void* storage = bytes != 0 ? calloc(1, bytes) : NULL; // calloc: 8-byte aligned
    return (bytes == 0 || storage != NULL) &&
           tt_Node_create_publisher(tt_Context_default_node(&ctx), &pub, topic, "wifi_check", &qos, &options, storage,
                                    bytes) == tt_RET_OK;
}

static bool create_subscriber(const struct tt_TypeSupport* topic, tt_SUBSCRIBER_CALLBACK callback) {
    struct tt_QosProfile qos = profile();
    struct tt_SubscriberOptions options = {.callback = callback,
                                           .accept_callback = NULL,
                                           .accept_callback_param = NULL};
    size_t bytes = tt_Subscriber_storage_size(topic, &qos);
    void* storage = bytes != 0 ? calloc(1, bytes) : NULL;
    return (bytes == 0 || storage != NULL) &&
           tt_Node_create_subscriber(tt_Context_default_node(&ctx), &sub, topic, "wifi_check", &qos, &options, storage,
                                     bytes) == tt_RET_OK;
}

// The reveal re-ask (tt_LinkOptions.repair_reveal, docs/DESIGN.md "Repair reveal", 2026-10-30) from the environment, so
// that a harness's NODE_ENV picks it per tree: TT_REPAIR_REVEAL=off|revealed|all_missing, unset or empty = the
// profile's (0). Anything else stops the bench: an arm must not run a treatment it did not name. The context's resolved
// value is printed as a "REVEAL:" line, so an arm reports the treatment it ran.
static uint8_t repair_reveal_from_env(void) {
    const char* value = getenv("TT_REPAIR_REVEAL");
    if (value == NULL || value[0] == '\0') {
        return (uint8_t)tt_REPAIR_REVEAL_DEFAULT;
    }
    if (strcmp(value, "off") == 0) {
        return (uint8_t)tt_REPAIR_REVEAL_OFF;
    }
    if (strcmp(value, "revealed") == 0) {
        return (uint8_t)tt_REPAIR_REVEAL_REVEALED;
    }
    if (strcmp(value, "all_missing") == 0) {
        return (uint8_t)tt_REPAIR_REVEAL_ALL_MISSING;
    }
    fprintf(stderr, "TT_REPAIR_REVEAL=%s: expected off, revealed or all_missing\n", value);
    exit(2);
}

static void print_repair_reveal(const struct tt_Context* node) {
    const char* value = getenv("TT_REPAIR_REVEAL");
    printf("REVEAL: repair_reveal=%u pace_ns=%u env=%s\n", (unsigned)node->repair_reveal,
           (unsigned)node->repair_reveal_pace_ns, value != NULL ? value : "-");
}

static bool create_context(void) {
    struct tt_InitOptions init;
    memset(&init, 0, sizeof(init));
    init.net.broadcast = opts.broadcast;
    init.net.context_id = opts.context_id;
    init.net.links[0].broadcast = opts.broadcast;
    init.net.links[0].profile = opts.wifi ? tt_LINK_PROFILE_WIFI : tt_LINK_PROFILE_ETHERNET;
    init.net.links[0].repair_reveal = repair_reveal_from_env(); // TT_REPAIR_REVEAL
    init.net.link_count = 1;
    tt_ret_t created = tt_Context_create_with(&ctx, &init);
    if (created != tt_RET_OK) {
        fprintf(stderr, "tt_Context_create_with: %d\n", (int)created);
        return false;
    }
    print_repair_reveal(&ctx);
    return tt_Context_set_discovery(&ctx, &discovery, on_discovery, NULL) == tt_RET_OK;
}

static bool stop_asked(void) {
    return opts.stop_file != NULL && access(opts.stop_file, F_OK) == 0;
}

static void poll_until(uint64_t until) {
    for (uint64_t now = now_ns(); now < until; now = now_ns()) {
        uint64_t left = until - now;
        (void)tt_Context_poll(&ctx, (int64_t)(left < POLL_SLICE_NS ? left : POLL_SLICE_NS));
    }
}

// Polls until `until`, or until the stop file appears.
static void poll_or_stop(uint64_t until) {
    for (uint64_t now = now_ns(); now < until && !stop_asked(); now = now_ns()) {
        poll_until(now + POLL_SLICE_NS);
    }
}

// --- roles ----------------------------------------------------------------------------------------------------------

struct ping_run {
    uint64_t join_ns; // create to matched; 0: never matched
    uint32_t published;
    uint32_t publish_failed;
};

static void ping_publish(struct ping_run* run, uint64_t create_ns) {
    uint64_t match_deadline = now_ns() + (MATCH_WAIT_S * NS_PER_S);
    while (tt_Publisher_matched_count(&pub) == 0 && now_ns() < match_deadline) {
        poll_until(now_ns() + POLL_SLICE_NS);
    }
    if (tt_Publisher_matched_count(&pub) == 0) {
        return;
    }
    run->join_ns = now_ns() - create_ns;
    uint64_t period = NS_PER_S / opts.rate_hz;
    uint64_t end = now_ns() + (opts.seconds * NS_PER_S);
    struct header sample = {.send_ns = 0, .seq = 0, .reserved = 0};
    for (uint64_t next = now_ns(); next < end && run->published < MAX_SEQ; next += period) {
        sample.seq = run->published;
        sample.send_ns = now_ns();
        if (tt_Publisher_publish(&pub, (struct tt_Data*)&sample) == tt_RET_OK) {
            run->published++;
        } else {
            run->publish_failed++;
        }
        poll_until(next + period);
    }
    poll_until(now_ns() + (DRAIN_S * NS_PER_S));
}

static int compare_u64(const void* first, const void* second) {
    uint64_t lhs = *(const uint64_t*)first;
    uint64_t rhs = *(const uint64_t*)second;
    return (lhs > rhs) - (lhs < rhs);
}

static double quantile_us(uint32_t percent) {
    if (scored == 0) {
        return 0.0;
    }
    uint32_t index = (uint32_t)(((uint64_t)scored * percent) / PERCENT);
    index = index >= scored ? scored - 1U : index;
    return (double)latency_ns[index] / (double)NS_PER_US;
}

static void write_latencies(void) {
    if (opts.latency_file == NULL) {
        return;
    }
    FILE* file = fopen(opts.latency_file, "we");
    if (file == NULL) {
        fprintf(stderr, "cannot write %s\n", opts.latency_file);
        return;
    }
    for (uint32_t i = 0; i < scored; i++) {
        fprintf(file, "%llu\n", (unsigned long long)(latency_ns[i] / NS_PER_US));
    }
    (void)fclose(file);
}

static void print_events(void) {
    for (uint32_t i = 0; i < event_count; i++) {
        printf("EVENT role_ctx=%d kind=%s peer=%u t_ns=%llu\n", opts.context_id, events[i].what,
               (unsigned)events[i].context_id, (unsigned long long)events[i].t_ns);
    }
}

static void print_result(const char* role, uint64_t create_ns, const struct ping_run* run, double cpu) {
    const struct tt_LinkOptions* link = &ctx.opts.net.links[0];
    printf("RESULT role=%s ctx=%d reliable=%d size=%u rate_hz=%u profile=%s link_mtu=%u unicast_threshold=%u "
           "create_ns=%llu end_ns=%llu join_ms=%.1f published=%u publish_failed=%u warmup=%u scored=%u duplicates=%u "
           "below_warmup=%u max_seq=%u p50_us=%.1f p90_us=%.1f p99_us=%.1f max_us=%.1f pongs_sent=%u pongs_refused=%u "
           "retransmitted=%u tx_datagrams=%llu rx_datagrams=%llu cpu_ms=%.1f events=%u events_dropped=%u "
           "miss_threshold=%u summary_copies=%llu announce_repeats=%llu\n",
           role, opts.context_id, opts.reliable ? 1 : 0, opts.role == ROLE_PING ? ping_size : PONG_BYTES, opts.rate_hz,
           opts.wifi ? "wifi" : "ethernet", (unsigned)link->mtu, (unsigned)ctx.links[0].unicast_threshold,
           (unsigned long long)create_ns, (unsigned long long)now_ns(),
           run != NULL ? (double)run->join_ns / (double)NS_PER_MS : 0.0, run != NULL ? run->published : 0U,
           run != NULL ? run->publish_failed : 0U, warmup, scored, duplicates, below_warmup, max_seq, quantile_us(P50),
           quantile_us(P90), quantile_us(P99), scored > 0 ? (double)latency_ns[scored - 1U] / (double)NS_PER_US : 0.0,
           pongs_sent, pongs_refused, opts.role == ROLE_IDLE ? 0U : pub.retransmitted,
           (unsigned long long)ctx.tx_datagrams, (unsigned long long)ctx.rx_datagrams, cpu, event_count, events_dropped,
           (unsigned)ctx.opts.discovery.miss_threshold, (unsigned long long)ctx.summary_copies_sent,
           (unsigned long long)ctx.announce_repeats_sent);
}

static const char* role_name(void) {
    switch (opts.role) {
    case ROLE_PING:
        return "ping";
    case ROLE_ECHO:
        return "echo";
    case ROLE_IDLE:
    default:
        return "idle";
    }
}

static int run_role(void) {
    uint64_t create_ns = now_ns();
    if (!create_context()) {
        return 1;
    }
    double cpu_before = cpu_ms();
    bool created = true;
    struct ping_run run = {.join_ns = 0, .published = 0, .publish_failed = 0};
    switch (opts.role) {
    case ROLE_PING:
        created = create_publisher(&ping_topic) && create_subscriber(&pong_topic, on_pong);
        if (created) {
            ping_publish(&run, create_ns);
        }
        break;
    case ROLE_ECHO:
        created = create_publisher(&pong_topic) && create_subscriber(&ping_topic, on_ping);
        if (created) {
            poll_or_stop(create_ns + (opts.seconds * NS_PER_S));
        }
        break;
    case ROLE_IDLE:
    default:
        created = create_subscriber(&pong_topic, on_pong);
        if (created) {
            poll_or_stop(create_ns + (opts.seconds * NS_PER_S));
        }
        break;
    }
    if (!created) {
        fprintf(stderr, "cannot create the endpoints\n");
        return 1;
    }
    double cpu = cpu_ms() - cpu_before;
    qsort(latency_ns, scored, sizeof(uint64_t), compare_u64);
    print_events();
    write_latencies();
    print_result(role_name(), create_ns, opts.role == ROLE_PING ? &run : NULL, cpu);
#ifdef tt_RELIABLE_STATS
    print_reliable_stats(opts.role == ROLE_PING ? "ping" : "echo");
#endif
    fflush(stdout);
    (void)tt_Context_destroy(&ctx);
    return 0;
}

static bool parse_role(const char* name) {
    if (strcmp(name, "ping") == 0) {
        opts.role = ROLE_PING;
    } else if (strcmp(name, "echo") == 0) {
        opts.role = ROLE_ECHO;
    } else if (strcmp(name, "idle") == 0) {
        opts.role = ROLE_IDLE;
    } else {
        return false;
    }
    return true;
}

static bool parse_options(int argc, char** argv) {
    if (argc < MIN_ARGS || !parse_role(argv[1])) {
        return false;
    }
    opts.context_id = -1;
    opts.size = DEFAULT_SIZE;
    opts.rate_hz = DEFAULT_RATE_HZ;
    int option = 0;
    optind = 2; // NOLINT(misc-include-cleaner) - see <unistd.h> above
    while ((option = getopt(argc, argv, "c:b:d:m:s:r:p:q:l:")) != -1) { // NOLINT(misc-include-cleaner)
        switch (option) {
        case 'c':
            opts.context_id = atoi(optarg) /* NOLINT(misc-include-cleaner) */;
            break;
        case 'b':
            opts.broadcast = optarg;
            break;
        case 'd':
            opts.seconds = strtoull(optarg, NULL, 10);
            break;
        case 'm':
            opts.reliable = strcmp(optarg, "rel") == 0;
            break;
        case 's':
            opts.size = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'r':
            opts.rate_hz = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'p':
            opts.wifi = strcmp(optarg, "wifi") == 0;
            break;
        case 'q':
            opts.stop_file = optarg;
            break;
        case 'l':
            opts.latency_file = optarg;
            break;
        default:
            return false;
        }
    }
    return opts.context_id > 0 && opts.broadcast != NULL && opts.seconds > 0 && opts.rate_hz > 0 &&
           opts.size >= HEADER_BYTES && opts.size <= tt_MAX_SAMPLE_LENGTH;
}

int main(int argc, char** argv) {
    if (!parse_options(argc, argv)) {
        fprintf(stderr,
                "usage: %s ping|echo|idle -c CONTEXT_ID -b BROADCAST -d SECONDS [-m be|rel] [-s BYTES] [-r HZ] "
                "[-p ethernet|wifi] [-q STOP_FILE] [-l LATENCY_FILE]\n",
                argv[0]);
        return 2;
    }
    ping_size = opts.size;
    warmup = opts.rate_hz; // one second of samples
    set_topic(&ping_topic, "wifi_ping", ping_encode_size, ping_size);
    set_topic(&pong_topic, opts.role == ROLE_IDLE ? "wifi_idle" : "wifi_pong", pong_encode_size, PONG_BYTES);
    return run_role();
}
