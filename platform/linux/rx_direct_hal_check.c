// Copyright (c) 2026 TSN Lab, Inc.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3, as published by the Free
// Software Foundation. A proprietary license is also available on request - see README.md.

// The HAL's aimed read (hal_linux.c direct_read()) against crafted reads on loopback, in a private namespace
// (rx_direct_check.sh). The hint is set by hand, as core's large_direct_aim() would set it, on an assembly built by
// hand; the datagrams are sent as one UDP_SEGMENT run, so they arrive merged (UDP_GRO), as a large sample's do. Cases
// the end to end arms do not reach on their own:
//   aimed  - a run of the expected fragments: the first read (aimed at one, as the HAL starts) hands out fragment 2's
//            framing with its payload in place and the other two whole; the next (aimed at more) all three in place.
//   sizes  - a run of 500-byte datagrams, the first of them the expected (last, short) fragment: a run of another size
//            is not read as units - both datagrams come out whole and intact.
//   cancel - fragments read in place are not handed out once core has freed their assembly (rx_direct_cancel).
//   empty  - a zero-length datagram (a doorbell over UDP) with a hint out comes back as one.
// Built as rmw_tickle builds core. Exit 0 pass, 1 fail, 3 setup failed.
#ifndef tt_MAX_BUFFER_LENGTH
#define tt_MAX_BUFFER_LENGTH 65507
#endif
#ifndef tt_LARGE_SAMPLES
#define tt_LARGE_SAMPLES 1 // as the Makefile builds it; for a lint that compiles it without the Makefile's flags
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>   // NOLINT(misc-include-cleaner) - nanosleep()
#include <unistd.h> // NOLINT(misc-include-cleaner) - close()

#include <netinet/in.h>
#include <netinet/udp.h> // NOLINT(misc-include-cleaner) - SOL_UDP, UDP_SEGMENT
#include <sys/socket.h>  // NOLINT(misc-include-cleaner)
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/hal_linux.h>
#include <tickle/tickle.h>

#define CHECK_CONTEXT_ID 56
#define PEER_SOURCE 9 // the fragments' sender, as their framing names it
#define ENTITY 7U
#define SAMPLE_SEQ 1000U
#define COUNT 120U
#define HEADER 16U
#define PAYLOAD 1452U
#define UNIT (HEADER + PAYLOAD)
#define FIRST_PAYLOAD 1446U // fragment 0's
#define SHORT_SIZE 500U
#define REPORT_BYTES 256
#define SETTLE_NS 1000000L // after a send on loopback: delivery is immediate in practice; this removes the doubt
#define PATTERN_INDEX_MUL 31U
#define PATTERN_AT_MUL 7U
#define FILLER_MUL 13U

static struct tt_Context node;
static int sender = -1;
static int failures;
static uint8_t buf[tt_MAX_BUFFER_LENGTH];

static void report(bool passed, const char* name, const char* what) {
    printf("rx_direct_hal_check: %s %s - %s\n", passed ? "PASS" : "FAIL", name, what);
    if (!passed) {
        failures++;
    }
}

static uint8_t* place_of(uint32_t index) {
    return node.large.assemblies[0].buffer + tt_LARGE_CDR_OFFSET + FIRST_PAYLOAD + ((size_t)(index - 1U) * PAYLOAD);
}

static void aim(uint32_t first) {
    node.large.direct = &node.large.assemblies[0];
    node.large.direct_claimed = node.large.assemblies[0].claimed;
    node.large.direct_first = first;
    node.rx_direct_dest = place_of(first);
    node.rx_direct_header = HEADER;
    node.rx_direct_payload = PAYLOAD;
    node.rx_direct_units = COUNT - first;
    node.rx_direct_cancel = false;
}

static uint8_t pattern(uint32_t index, uint32_t offset) {
    return (uint8_t)((index * PATTERN_INDEX_MUL) + (offset * PATTERN_AT_MUL) + 1U);
}

// Fragment `index`'s datagram, `size` bytes: its framing, then pattern bytes.
static void fragment(uint8_t* out, uint32_t index, uint32_t size) {
    // Little-endian, as this host: the marker that says so, and the fields in its order.
    struct tt_SingleHeader single = {tt_SINGLE_MARKER_LE, tt_VERSION, PEER_SOURCE, tt_SUBMESSAGE_TYPE_FRAG_CONT_L};
    struct tt_FragContLHeader cont = {ENTITY, SAMPLE_SEQ + index, (uint16_t)index, (uint16_t)COUNT};
    memcpy(out, &single, sizeof(single));
    memcpy(out + sizeof(single), &cont, sizeof(cont));
    for (uint32_t at = HEADER; at < size; at++) {
        out[at] = pattern(index, at);
    }
}

// One UDP_SEGMENT send of `bytes` cut every `segment` to the context's data socket.
static bool send_run(const uint8_t* bytes, uint32_t len, uint32_t segment) {
    struct sockaddr_in dest;
    socklen_t dest_len = sizeof dest;
    if (getsockname(node.hal.data_sock, (struct sockaddr*)&dest, &dest_len) != 0) {
        return false;
    }
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int size = (int)segment;
    if (setsockopt(sender, SOL_UDP, UDP_SEGMENT, &size, sizeof size) != 0) {
        return false;
    }
    bool sent = sendto(sender, bytes, len, 0, (struct sockaddr*)&dest, sizeof dest) == (ssize_t)len;
    struct timespec settle = {0, SETTLE_NS};
    nanosleep(&settle, NULL);
    return sent;
}

static int32_t take(void) {
    uint32_t ip = 0;
    uint16_t port = 0;
    return tt_try_receive(&node, buf, sizeof buf, &ip, &port);
}

// The datagram just taken is fragment `index` read in place: framing handed out, payload where it belongs, intact.
static bool in_place(int32_t got, uint32_t index) {
    if (got != (int32_t)HEADER || node.rx_split_payload != place_of(index) || node.rx_split_len != PAYLOAD) {
        return false;
    }
    uint8_t want[UNIT];
    fragment(want, index, UNIT);
    return memcmp(buf + node.rx_offset, want, HEADER) == 0 && memcmp(place_of(index), want + HEADER, PAYLOAD) == 0;
}

// The datagram just taken is `want` whole, in the receive buffer.
static bool whole(int32_t got, const uint8_t* want, uint32_t size) {
    return got == (int32_t)size && node.rx_split_payload == NULL && memcmp(buf + node.rx_offset, want, size) == 0;
}

static void check_aimed(void) {
    uint8_t run[3 * UNIT];
    for (uint32_t k = 0; k < 3; k++) {
        fragment(run + ((size_t)k * UNIT), 2 + k, UNIT);
    }
    char what[REPORT_BYTES];
    for (int round = 1; round <= 2; round++) {
        aim(2);
        if (!send_run(run, sizeof run, UNIT)) {
            report(false, "aimed", "cannot send the run");
            return;
        }
        uint64_t split = node.rx_direct_split;
        bool passed = true;
        for (uint32_t k = 0; k < 3; k++) {
            int32_t got = take();
            bool expect_in_place = round == 2 || k == 0;
            passed = passed && (expect_in_place ? in_place(got, 2 + k) : whole(got, run + ((size_t)k * UNIT), UNIT));
        }
        passed = passed && take() == -1;
        snprintf(what, sizeof what, "round %d: %llu of 3 in place (must be %d), every byte where it belongs", round,
                 (unsigned long long)(node.rx_direct_split - split), round == 2 ? 3 : 1);
        report(passed && node.rx_direct_split - split == (round == 2 ? 3U : 1U), "aimed", what);
    }
}

static void check_sizes(void) {
    uint8_t run[2 * SHORT_SIZE];
    fragment(run, COUNT - 1U, SHORT_SIZE); // the expected last fragment, short
    for (uint32_t at = 0; at < SHORT_SIZE; at++) {
        run[SHORT_SIZE + at] = (uint8_t)(at * FILLER_MUL);
    }
    aim(COUNT - 1U);
    bool sent = send_run(run, sizeof run, SHORT_SIZE);
    int32_t first = take();
    bool first_ok = whole(first, run, SHORT_SIZE);
    int32_t second = take();
    bool second_ok = whole(second, run + SHORT_SIZE, SHORT_SIZE);
    char what[REPORT_BYTES];
    snprintf(what, sizeof what, "a run of two %u-byte datagrams: %d and %d bytes, %s and %s (must be whole and intact)",
             SHORT_SIZE, first, second, first_ok ? "intact" : "NOT intact", second_ok ? "intact" : "NOT intact");
    report(sent && first_ok && second_ok && take() == -1, "sizes", what);
}

static void check_cancel(void) {
    uint8_t run[3 * UNIT];
    for (uint32_t k = 0; k < 3; k++) {
        fragment(run + ((size_t)k * UNIT), 2 + k, UNIT);
    }
    aim(2);
    bool sent = send_run(run, sizeof run, UNIT);
    int32_t got = take();
    bool first = in_place(got, 2);
    node.rx_direct_cancel = true; // core freed the assembly while the HAL held the other two
    int32_t after = take();
    char what[REPORT_BYTES];
    snprintf(what, sizeof what,
             "first in place: %s; after the cancel: %d (must be -1: nothing read into it handed out)",
             first ? "yes" : "no", after);
    report(sent && first && after == -1, "cancel", what);
}

static void check_empty(void) {
    aim(2);
    bool sent = send_run(buf, 0, UNIT);
    int32_t got = take();
    char what[REPORT_BYTES];
    snprintf(what, sizeof what, "a zero-length datagram with a hint out: %d (must be 0)", got);
    report(sent && got == 0 && take() == -1, "empty", what);
}

int main(void) {
    setenv("TT_RX_DIRECT", "1", 1); // off by default (hal_linux.c direct_setup())
    _tt_CONFIG.broadcast = "127.255.255.255";
    _tt_CONFIG.context_id = CHECK_CONTEXT_ID;
    if (tt_Context_create(&node) != tt_RET_OK) {
        printf("rx_direct_hal_check: SETUP FAILED - cannot create context\n");
        return 3;
    }
    sender = socket(AF_INET, SOCK_DGRAM, 0);
    struct tt_LargeAssembly* assembly = &node.large.assemblies[0];
    assembly->buffer = (uint8_t*)malloc(tt_LARGE_CDR_OFFSET + (COUNT * PAYLOAD));
    assembly->capacity = tt_LARGE_CDR_OFFSET + (COUNT * PAYLOAD);
    assembly->frag_count = COUNT;
    assembly->seq_no = SAMPLE_SEQ;
    assembly->entity_id = ENTITY;
    assembly->source = PEER_SOURCE;
    assembly->claimed = 5;
    if (sender < 0 || assembly->buffer == NULL || !node.hal.gro_allowed) {
        printf("rx_direct_hal_check: SETUP FAILED - sender %d, buffer %p, UDP_GRO %s\n", sender,
               (void*)assembly->buffer, node.hal.gro_allowed ? "allowed" : "refused");
        return 3;
    }
    aim(2);
    (void)take(); // with a hint out the first read turns UDP_GRO on, so the runs below arrive merged
    check_aimed();
    check_sizes();
    check_cancel();
    check_empty();
    printf("rx_direct_hal_check: RESULT failures=%d direct_reads=%llu direct_split=%llu direct_gathered=%llu\n",
           failures, (unsigned long long)node.rx_direct_reads, (unsigned long long)node.rx_direct_split,
           (unsigned long long)node.rx_direct_gathered);
    free(assembly->buffer);
    assembly->buffer = NULL;
    node.large.direct = NULL;
    node.rx_direct_units = 0;
    (void)close(sender);
    tt_Context_destroy(&node);
    return failures == 0 ? 0 : 1;
}
