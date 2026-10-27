/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// hybrid_run_rx.c - a run received for real: an IP-fragmented datagram through the kernel into a TickLE context
// (DESIGN.md section 8, "Hybrid transport", step 2). hybrid_run_rx.sh runs both modes in two network namespaces.
//
//   hybrid_run_rx recv <broadcast> <context id> <timeout s>
//       A context built as rmw_tickle builds core (65507-byte datagrams, large samples), with the real Linux HAL and
//       one best-effort Subscriber of "hybrid_run". Waits for one 20,000-byte and one 100,000-byte sample, each
//       checked byte for byte, and prints a RESULT line with its counters.
//   hybrid_run_rx send <receiver ip> runs|mtu
//       A plain UDP socket - not TickLE, which does not send runs yet - sending the two samples, hand-built: as runs
//       (14 units on 8/9: one FRAG_RUN of 13 units, about 18.9 KB, and the last unit as FRAG_CONT; 69 units on 11/12:
//       two FRAG_RUN_L of 45 and 23 units, about 65 and 33 KB, and the last unit as FRAG_CONT_L), or as MTU fragments
//       only (the control: the same samples with no datagram above 1,472 bytes, so no IP reassembly at all).
//
// Unit sizes are the classic-framing geometry both ends use at a 1472-byte control datagram: 1,447 then 1,454 bytes
// on 8/9, 1,446 then 1,452 on 11/12.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Built as rmw_tickle builds core (hybrid_run_rx.sh passes the same two settings to every translation unit).
#ifndef tt_MAX_BUFFER_LENGTH
#define tt_MAX_BUFFER_LENGTH 65507
#endif
#ifndef tt_LARGE_SAMPLES
#define tt_LARGE_SAMPLES 1
#endif

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/log.h>
#include <tickle/tickle.h>

#define TOPIC "hybrid_run"
#define ENDPOINT "e"
#define SMALL_LEN 20000U
#define LARGE_LEN 100000U
#define CONTROL 1472U
#define FRAMING 8U // the classic framing the unit sizes are cut by
#define SENDER_ID 1
#define ENTITY_ID 7U
#define MAX_LARGE_RUN 45U     // units in a run of a large sample: 45 fill a 65,372-byte datagram (DESIGN.md section 8)
#define POLL_MS 100           // one poll's wait while the samples are awaited
#define DATAGRAM_BYTES 65536U // the largest datagram this sends, rounded up
#define SEND_SPACING_US 2000U // between two datagrams: the receive buffer is not what is being tested
#define PATTERN_STEP 11U      // the sample's byte pattern
#define US_PER_S 1000000U
#define NS_PER_US 1000U

static uint8_t pattern(uint32_t i, uint32_t len) {
    return (uint8_t)((i * PATTERN_STEP) + (len >> 8U) + (i >> 10U));
}

// ------------------------------------------------------------------------------------------------ receive

static int small_ok;
static int large_ok;
static int torn;

static int32_t checking_decode(struct tt_Data* data, const uint8_t* payload, uint32_t len, bool native) {
    (void)data;
    (void)native;
    uint32_t expected = len == SMALL_LEN ? SMALL_LEN : LARGE_LEN;
    bool intact = len == SMALL_LEN || len == LARGE_LEN;
    for (uint32_t i = 0; intact && i < expected; i++) {
        intact = payload[i] == pattern(i, expected);
    }
    if (!intact) {
        torn++;
    } else if (len == SMALL_LEN) {
        small_ok++;
    } else {
        large_ok++;
    }
    return 0;
}

static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static void on_data(struct tt_Subscriber* sub, uint64_t time, uint16_t seq_no, struct tt_Data* data) {
    (void)sub;
    (void)time;
    (void)seq_no;
    (void)data;
}

static void* buffer_acquire(void* user, uint32_t bytes) {
    (void)user;
    return malloc(bytes);
}

static void buffer_release(void* user, void* buffer) {
    (void)user;
    free(buffer);
}

static int receive(const char* broadcast, int context_id, int timeout_s) {
    _tt_CONFIG.broadcast = (char*)broadcast;
    _tt_CONFIG.context_id = context_id;
    tt_log_set_level(TT_LOG_WARNING);
    static struct tt_Context node;
    if (tt_Context_create(&node) != tt_RET_OK) {
        printf("RESULT: error=create\n");
        return 1;
    }
    static struct tt_TypeSupport topic;
    topic.name = TOPIC;
    topic.data_size = 8;
    topic.data_decode = checking_decode;
    topic.data_free = free_nothing;
    topic.max_encoded_size = LARGE_LEN; // the larger of the two samples it is sent
    // BEST_EFFORT, as the legacy create left it. KEEP_ALL: keep_last_depth 0, never skips in the segment drain, as the
    // legacy create; no reorder buffer and the builtin ACK window, so the profile needs no storage.
    static const struct tt_QosProfile qos = {
        .history = {.kind = tt_QOS_HISTORY_KEEP_ALL, .depth = 0, .bytes = 0},
        .reliability = {.kind = tt_QOS_RELIABILITY_BEST_EFFORT,
                        .tracking_words = 0,
                        .reorder_slots = tt_QOS_REORDER_NONE,
                        .max_blocking_ns = 0},
        .durability = {.kind = tt_QOS_DURABILITY_VOLATILE},
        .deadline = {.period_ns = tt_QOS_DURATION_INFINITE},
        .lifespan = {.duration_ns = tt_QOS_DURATION_INFINITE},
        .liveliness = {.kind = tt_QOS_LIVELINESS_AUTOMATIC, .lease_ns = tt_QOS_DURATION_INFINITE},
    };
    static const struct tt_SubscriberOptions options = {.callback = on_data,
                                                        .accept_callback = NULL,
                                                        .accept_callback_param = NULL};
    static struct tt_Subscriber sub;
    // Its own reassembly slot since 2026-10-27 (docs/DESIGN.md section 8, "Reassembly slots"); calloc is 8-aligned.
    size_t storage_size = tt_Subscriber_storage_size(&topic, &qos);
    void* storage = storage_size != 0 ? calloc(1, storage_size) : NULL;
    if ((storage_size != 0 && storage == NULL) ||
        tt_Node_create_subscriber(tt_Context_default_node(&node), &sub, &topic, ENDPOINT, &qos, &options, storage,
                                  storage_size) != tt_RET_OK ||
        tt_Context_set_large_buffers(&node, buffer_acquire, buffer_release, NULL) != tt_RET_OK) {
        printf("RESULT: error=subscriber\n");
        return 1;
    }
    printf("READY\n");
    (void)fflush(stdout);
    uint64_t end = tt_get_ns() + ((uint64_t)timeout_s * tt_SECOND);
    while ((small_ok == 0 || large_ok == 0) && tt_get_ns() < end) {
        (void)tt_Context_poll(&node, POLL_MS * (int64_t)tt_MILLISECOND);
    }
    printf("RESULT: small_ok=%d large_ok=%d torn=%d ip_reassembly_max=%u frag_dropped=%lu frag_reassembled=%lu "
           "large_dropped=%lu large_reassembled=%lu frag_large_skipped=%lu\n",
           small_ok, large_ok, torn, (unsigned)node.ip_reassembly_max, (unsigned long)node.frag_dropped,
           (unsigned long)node.frag_reassembled, (unsigned long)node.large.dropped,
           (unsigned long)node.large.reassembled, (unsigned long)node.frag_large_skipped);
    tt_Context_destroy(&node);
    return small_ok == 1 && large_ok == 1 && torn == 0 ? 0 : 1;
}

// ------------------------------------------------------------------------------------------------ send

static uint8_t sample[LARGE_LEN];
static uint8_t out[DATAGRAM_BYTES];

struct geometry {
    uint32_t first; // unit 0's payload
    uint32_t cont;  // every other full unit's
};

static uint32_t unit_offset(const struct geometry* geo, uint32_t i) {
    return i == 0 ? 0 : geo->first + ((i - 1) * geo->cont);
}

static uint32_t unit_length(const struct geometry* geo, uint32_t i, uint32_t len) {
    uint32_t offset = unit_offset(geo, i);
    uint32_t full = i == 0 ? geo->first : geo->cont;
    return len - offset < full ? len - offset : full;
}

// The single-submessage header, in this host's byte order as a TickLE sender writes it: 'k' little-endian, 't' big.
static uint32_t single_header(uint8_t type) {
    const uint16_t probe = 1;
    uint8_t low = 0;
    memcpy(&low, &probe, 1);
    struct tt_SingleHeader single = {low == 1 ? (uint8_t)tt_SINGLE_MARKER_LE : (uint8_t)tt_SINGLE_MARKER_BE, tt_VERSION,
                                     SENDER_ID, type};
    memcpy(out, &single, sizeof(single));
    return (uint32_t)sizeof(single);
}

static int sock_fd;
static struct sockaddr_in dest;
static int sent_above_mtu;

static int send_out(uint32_t len) {
    if (sendto(sock_fd, out, len, 0, (const struct sockaddr*)&dest, sizeof(dest)) != (ssize_t)len) {
        perror("sendto");
        return -1;
    }
    sent_above_mtu += len > CONTROL ? 1 : 0;
    usleep(SEND_SPACING_US);
    return 0;
}

static struct tt_DataHeader data_header(uint32_t seq_no) {
    struct timespec ts;
    // NOLINTNEXTLINE(misc-include-cleaner) - CLOCK_REALTIME, as hal_linux.c's tt_get_wall_ns()
    (void)clock_gettime(CLOCK_REALTIME, &ts);
    struct tt_DataHeader data = {tt_hash_id(TOPIC, ENDPOINT), seq_no,
                                 (uint32_t)(((uint64_t)ts.tv_sec * US_PER_S) + ((uint64_t)ts.tv_nsec / NS_PER_US)),
                                 ENTITY_ID};
    return data;
}

// One MTU fragment of a sample: FRAG_FIRST/_CONT, or the _L pair.
static int send_unit(bool large, const struct geometry* geo, uint32_t len, uint32_t seq_base, uint32_t count,
                     uint32_t i) {
    uint32_t pos;
    if (i == 0) {
        pos = single_header(large ? tt_SUBMESSAGE_TYPE_FRAG_FIRST_L : tt_SUBMESSAGE_TYPE_FRAG_FIRST);
        if (large) {
            struct tt_FragFirstLHeader hdr = {data_header(seq_base), (uint16_t)count};
            memcpy(out + pos, &hdr, sizeof(hdr));
            pos += sizeof(hdr);
        } else {
            struct tt_FragFirstHeader hdr = {data_header(seq_base), (uint8_t)count};
            memcpy(out + pos, &hdr, sizeof(hdr));
            pos += sizeof(hdr);
        }
    } else {
        pos = single_header(large ? tt_SUBMESSAGE_TYPE_FRAG_CONT_L : tt_SUBMESSAGE_TYPE_FRAG_CONT);
        if (large) {
            struct tt_FragContLHeader hdr = {ENTITY_ID, seq_base + i, (uint16_t)i, (uint16_t)count};
            memcpy(out + pos, &hdr, sizeof(hdr));
            pos += sizeof(hdr);
        } else {
            struct tt_FragContHeader hdr = {ENTITY_ID, seq_base + i, (uint8_t)i, (uint8_t)count};
            memcpy(out + pos, &hdr, sizeof(hdr));
            pos += sizeof(hdr);
        }
    }
    uint32_t bytes = unit_length(geo, i, len);
    memcpy(out + pos, sample + unit_offset(geo, i), bytes);
    return send_out(pos + bytes);
}

// Units first .. first + units - 1 as one run.
static int send_run(bool large, const struct geometry* geo, uint32_t seq_base, uint32_t count, uint32_t first,
                    uint32_t units) {
    uint32_t pos = single_header(large ? tt_SUBMESSAGE_TYPE_FRAG_RUN_L : tt_SUBMESSAGE_TYPE_FRAG_RUN);
    if (large) {
        struct tt_FragRunLHeader hdr = {data_header(seq_base + first), (uint16_t)first, (uint16_t)count,
                                        (uint16_t)units, 0};
        memcpy(out + pos, &hdr, sizeof(hdr));
        pos += sizeof(hdr);
    } else {
        struct tt_FragRunHeader hdr = {data_header(seq_base + first), (uint8_t)first, (uint8_t)count, (uint8_t)units,
                                       0};
        memcpy(out + pos, &hdr, sizeof(hdr));
        pos += sizeof(hdr);
    }
    uint32_t bytes = unit_offset(geo, first + units) - unit_offset(geo, first);
    memcpy(out + pos, sample + unit_offset(geo, first), bytes);
    return send_out(pos + bytes);
}

static int send_sample(bool large, uint32_t len, uint32_t seq_base, bool runs, uint32_t* next_seq) {
    struct geometry geo = large ? (struct geometry) {CONTROL - FRAMING - (uint32_t)sizeof(struct tt_FragFirstLHeader),
                                                     CONTROL - FRAMING - (uint32_t)sizeof(struct tt_FragContLHeader)}
                                : (struct geometry) {CONTROL - FRAMING - (uint32_t)sizeof(struct tt_FragFirstHeader),
                                                     CONTROL - FRAMING - (uint32_t)sizeof(struct tt_FragContHeader)};
    for (uint32_t i = 0; i < len; i++) {
        sample[i] = pattern(i, len);
    }
    uint32_t count = 1 + ((len - geo.first + geo.cont - 1) / geo.cont);
    *next_seq = seq_base + count;
    uint32_t i = 0;
    if (runs) {
        uint32_t max_run = large ? MAX_LARGE_RUN : count - 1U;
        while (i < count - 1) {
            uint32_t units = count - 1 - i < max_run ? count - 1 - i : max_run;
            if (send_run(large, &geo, seq_base, count, i, units) != 0) {
                return -1;
            }
            i += units;
        }
    }
    for (; i < count; i++) {
        if (send_unit(large, &geo, len, seq_base, count, i) != 0) {
            return -1;
        }
    }
    printf("sent %s sample of %u bytes: %u units, seq_no %u..%u\n", large ? "large" : "small", len, count, seq_base,
           seq_base + count - 1);
    return 0;
}

static int send_samples(const char* receiver_ip, bool runs) {
    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return 1;
    }
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)_tt_CONFIG.port);
    if (inet_pton(AF_INET, receiver_ip, &dest.sin_addr) != 1) {
        return 1;
    }
    uint32_t next = 1;
    if (send_sample(false, SMALL_LEN, 1, runs, &next) != 0 || send_sample(true, LARGE_LEN, next, runs, &next) != 0) {
        return 1;
    }
    printf("SENT: mode=%s datagrams_above_1472=%d\n", runs ? "runs" : "mtu", sent_above_mtu);
    close(sock_fd);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 5 && strcmp(argv[1], "recv") == 0) {
        return receive(argv[2], atoi(argv[3]), atoi(argv[4]));
    }
    if (argc == 4 && strcmp(argv[1], "send") == 0) {
        return send_samples(argv[2], strcmp(argv[3], "runs") == 0);
    }
    fprintf(stderr, "usage: %s recv <broadcast> <context id> <timeout s> | send <receiver ip> runs|mtu\n", argv[0]);
    return 2;
}
