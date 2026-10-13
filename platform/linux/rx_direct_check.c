// Copyright (c) 2026 TSN Lab, Inc.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3, as published by the Free
// Software Foundation. A proprietary license is also available on request - see README.md.

// Reading a run in place (DESIGN.md section 8, "Receiving runs in place"): the HAL's half, against real sockets. Core's
// half - when a hint is made and what is done with a run read in place - is tests/test_rx_direct.c over the mock HAL.
// Here the hint is set by hand (struct tt_LargeState.direct_*, as large_direct_aim() sets it) over a buffer of this
// check's own, datagrams are sent to the context's data socket on loopback, and each is read with tt_try_receive():
//   1. control  - no hint: the datagram is read whole into the buffer, and no read is counted as aimed.
//   2. in place - the hinted run: the framing in the buffer, the payload in the hinted place, direct_split set to it.
//   2b. after  - a datagram queued behind a run read in place is not read by the drain (tt_try_receive() answers
//                 -1, no read of the socket) but by the next wait (tt_receive()), whole.
//   2c. no hint - a run read without a hint (not in place): the drain after it does not read the socket either; the
//                 next wait reads what came behind it, whole.
//   2d. GRO on  - receive offload on (as MTU-mode traffic leaves it): the hinted run is still read in place, and the
//                 drain after it does not read the socket.
//   2e. merged  - GRO on and a hint, and a merged read (a UDP_SEGMENT send over loopback, which a UDP_GRO socket takes
//                 whole) whose length and first framing are the hinted run's: not taken in place, handed out as its
//                 two datagrams, each whole, counted missed.
//   3. shorter  - a run of the same sample from the same place, fewer units: in place too; the hinted bytes past it
//                 untouched.
//   4-8. misses - the framing differs before the unit count, after it, a count of 0, more units than hinted (the
//                 rest read past the hinted length), a length that is not count x unit, a datagram no longer than the
//                 framing: each read whole into the buffer, exactly the bytes sent, direct_split not set, counted
//                 missed.
//   9. withdrawn - the hint withdrawn while the read is in the kernel (another thread freeing the assembly; here this
//                 check's own recvmsg() does it once the real one returns): the run is not handed over in place but
//                 read whole into the buffer, from the parked place.
//  10. switch   - TT_RX_DIRECT=0 in the environment: tt_bind() leaves direct_allowed off; unset, on.
// Exit 0 pass, 1 fail, 3 setup failed. Run by test.sh (test-linux) in a private namespace.

// Built as rmw_tickle is (platform/linux/Makefile passes the same), so a lint of this file alone sees the same build.
#ifndef tt_MAX_BUFFER_LENGTH
#define tt_MAX_BUFFER_LENGTH 65507
#endif
#ifndef tt_LARGE_SAMPLES
#define tt_LARGE_SAMPLES 1
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h> // setenv()
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <netinet/udp.h> // UDP_GRO, UDP_SEGMENT
#include <sys/socket.h>
#include <sys/syscall.h>
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/hal_linux.h>
#include <tickle/tickle.h>

enum check_exit { CHECK_PASS = 0, CHECK_FAIL = 1, CHECK_SETUP_FAILED = 3 };

#define CHECK_CONTEXT_ID 61 // any valid id; this context never meets another
#define UNIT 100U           // the hint's unit: small, so a datagram is easy to build
#define HINT_UNITS 3U
#define HINT_BYTES ((size_t)HINT_UNITS * UNIT)
#define BEHIND_EXTRA 7U    // a datagram queued behind the run (2b): a length no run has
#define FRAMING_FLIP 0x5AU // what turns 2b's datagram's last framing byte into another
#define PLACE_BYTES 1024U
#define SENTINEL 0xA5
#define FRAMING ((uint32_t)(sizeof(struct tt_SingleHeader) + sizeof(struct tt_FragRunLHeader)))
#define COUNT_AT ((uint32_t)(sizeof(struct tt_SingleHeader) + offsetof(struct tt_FragRunLHeader, run_count)))
#define SETTLE_US 1000
#define MAX_READS 16
#define CHECK_SOURCE 7U    // the framing's source byte: what read_it() tells this check's datagrams by
#define FRAMING_FILL 0x30U // the rest of the framing: any bytes, compared as bytes
#define PAYLOAD_STEP 13U   // a payload byte's pattern
#define BYTE_FLIP 0xFFU    // a framing byte made into another
#define SHORT_BY 8U        // a datagram this much shorter than the framing
#define WAIT_DIVISOR 10    // 2b's wait: a tenth of a second
#define LINE_BYTES 256U
static uint8_t place[PLACE_BYTES];
static uint8_t framing[FRAMING];
static uint8_t datagram[FRAMING + (8U * UNIT)];
static uint8_t buf[tt_MAX_BUFFER_LENGTH];

static void build_framing(void) {
    memset(framing, 0, sizeof(framing));
    framing[0] = (uint8_t)tt_SINGLE_MARKER_LE; // the HAL compares bytes, not their meaning
    framing[1] = tt_VERSION;
    framing[2] = CHECK_SOURCE;
    framing[3] = tt_SUBMESSAGE_TYPE_FRAG_RUN_L;
    for (uint32_t i = 4; i < FRAMING; i++) {
        framing[i] = (uint8_t)(FRAMING_FILL + i);
    }
    uint16_t count = HINT_UNITS;
    memcpy(framing + COUNT_AT, &count, sizeof(count));
}

static void hint(struct tt_Context* node) {
    memset(place, SENTINEL, sizeof(place));
    memcpy(node->large.direct_framing, framing, FRAMING);
    node->large.direct_framing_len = (uint8_t)FRAMING;
    node->large.direct_count_at = (uint8_t)COUNT_AT;
    node->large.direct_unit = (uint16_t)UNIT;
    node->large.direct_dest = place;
    node->large.direct_split = NULL;
    __atomic_store_n(&node->large.direct_bytes, HINT_UNITS * UNIT, __ATOMIC_SEQ_CST);
}

// A datagram: `head` (FRAMING bytes, or `len` if fewer) then payload bytes numbered from 1.
static uint32_t build(const uint8_t* head, uint32_t len) {
    memcpy(datagram, head, len < FRAMING ? len : FRAMING);
    for (uint32_t i = FRAMING; i < len; i++) {
        datagram[i] = (uint8_t)((i * PAYLOAD_STEP) + 1U);
    }
    return len;
}

static bool send_to(int sender, int socket_fd, uint32_t len) {
    struct sockaddr_in dest;
    socklen_t dest_len = sizeof(dest);
    if (getsockname(socket_fd, (struct sockaddr*)&dest, &dest_len) != 0) {
        return false;
    }
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool sent = sendto(sender, datagram, len, 0, (struct sockaddr*)&dest, sizeof(dest)) == (ssize_t)len;
    usleep(SETTLE_US);
    return sent;
}

// Reads until this check's datagram (len bytes) arrives; the context's own announces may come first.
static int32_t read_it(struct tt_Context* node, uint32_t len) {
    uint32_t ip = 0;
    uint16_t port = 0;
    for (int i = 0; i < MAX_READS; i++) {
        node->large.direct_split = NULL;
        memset(buf, 0, sizeof(buf)); // nothing of an earlier datagram can pass for this one
        int32_t got = tt_try_receive(node, buf, sizeof(buf), &ip, &port);
        if (got < 0) {
            continue; // sockets skipped after a run read in place (2b) answer -1 once, then are asked again
        }
        if ((uint32_t)got == len && buf[2] == framing[2]) {
            return got;
        }
    }
    return -1;
}

static int failures;
static bool setup_failed;
static struct tt_Context node;
static int sender = -1;
static uint32_t any_ip;
static uint16_t any_port;

// The HAL's recvmsg(), linked here ahead of the C library's: the system call, then - when asked - the hint withdrawn
// as another thread's large_direct_withdraw() does it, after the bytes went where the hint said.
static struct tt_Context* withdraw_after_read;

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) - libc's prototype names them __fd and so on
ssize_t recvmsg(int socket_fd, struct msghdr* msg, int flags) {
    ssize_t got = syscall(SYS_recvmsg, socket_fd, msg, flags);
    if (withdraw_after_read != NULL && got > 0) { // a read that took a datagram, not an empty socket
        __atomic_store_n(&withdraw_after_read->large.direct_bytes, 0U, __ATOMIC_SEQ_CST);
        withdraw_after_read = NULL;
    }
    return got;
}

static void verdict(bool passed, const char* line) {
    printf("rx_direct_check: %s - %s\n", passed ? "PASS" : "FAIL", line);
    failures += passed ? 0 : 1;
}

// Sends `len` bytes of the datagram built last; a failure is the check's setup, not its subject.
static bool send_built(uint32_t len, const char* what) {
    if (!send_to(sender, node.hal.data_sock, len)) {
        printf("rx_direct_check: SETUP FAILED - send (%s)\n", what);
        setup_failed = true;
        return false;
    }
    return true;
}

static bool read_whole(int32_t got, uint32_t len) {
    return got == (int32_t)len && memcmp(buf, datagram, len) == 0;
}

// 1. control
static void check_control(void) {
    __atomic_store_n(&node.large.direct_bytes, 0U, __ATOMIC_SEQ_CST);
    uint64_t reads = node.large.direct_reads;
    uint32_t len = build(framing, FRAMING + HINT_BYTES);
    if (!send_built(len, "control")) {
        return;
    }
    int32_t got = read_it(&node, len);
    verdict(read_whole(got, len) && node.large.direct_reads == reads && node.large.direct_split == NULL,
            "control: no hint, read whole and not aimed");
}

// 2. in place
static void check_in_place(void) {
    hint(&node);
    uint64_t reads = node.large.direct_reads;
    uint32_t len = build(framing, FRAMING + HINT_BYTES);
    if (!send_built(len, "in place")) {
        return;
    }
    int32_t got = read_it(&node, len);
    bool placed = memcmp(place, datagram + FRAMING, HINT_BYTES) == 0;
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line), "in place: %d bytes, payload %s the hinted place, split %s", (int)got,
                   placed ? "in" : "NOT in", node.large.direct_split == place ? "set" : "NOT set");
    verdict(got == (int32_t)len && memcmp(buf, framing, FRAMING) == 0 && placed && node.large.direct_split == place &&
                node.large.direct_reads == reads + 1U,
            line);
}

// 2b. after
static void check_after(void) {
    hint(&node);
    uint32_t len = build(framing, FRAMING + HINT_BYTES);
    if (!send_built(len, "after")) {
        return;
    }
    uint32_t behind = FRAMING + UNIT + BEHIND_EXTRA;
    static uint8_t behind_bytes[FRAMING + UNIT + BEHIND_EXTRA];
    uint8_t other_head[FRAMING];
    memcpy(other_head, framing, FRAMING);
    other_head[FRAMING - 1U] ^= FRAMING_FLIP;
    build(other_head, behind);
    memcpy(behind_bytes, datagram, behind);
    if (!send_built(behind, "behind")) {
        return;
    }
    int32_t got = read_it(&node, len);
    bool first_in_place = got == (int32_t)len && node.large.direct_split == place;
    uint64_t reads_before = node.large.direct_reads;
    int32_t drained = tt_try_receive(&node, buf, sizeof(buf), &any_ip, &any_port);
    bool not_read = drained < 0 && node.large.direct_reads == reads_before;
    memset(buf, 0, sizeof(buf));
    int32_t waited = tt_receive(&node, buf, sizeof(buf), &any_ip, &any_port, (int64_t)tt_SECOND / WAIT_DIVISOR);
    bool by_the_wait = waited == (int32_t)behind && memcmp(buf, behind_bytes, behind) == 0;
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line),
                   "after a run in place (read %d, in place %d; the drain answered %d): the drain %s, the next wait "
                   "read %d of %u bytes %s",
                   (int)got, first_in_place ? 1 : 0, (int)drained, not_read ? "did not read the socket" : "READ it",
                   (int)waited, (unsigned)behind, by_the_wait ? "whole" : "NOT whole");
    verdict(first_in_place && not_read && by_the_wait, line);
}

// What came behind a run: sent, then neither read by the drain (tt_try_receive() answers -1: the socket is skipped)
// nor lost - the next wait reads it whole. Returns whether all three held; *drained and *waited for the line.
static bool behind_waits(int32_t* drained, int32_t* waited) {
    uint32_t behind = FRAMING + UNIT + BEHIND_EXTRA;
    static uint8_t behind_bytes[FRAMING + UNIT + BEHIND_EXTRA];
    uint8_t other_head[FRAMING];
    memcpy(other_head, framing, FRAMING);
    other_head[FRAMING - 1U] ^= FRAMING_FLIP;
    build(other_head, behind);
    memcpy(behind_bytes, datagram, behind);
    if (!send_built(behind, "behind")) {
        return false;
    }
    uint64_t reads_before = node.large.direct_reads;
    *drained = tt_try_receive(&node, buf, sizeof(buf), &any_ip, &any_port);
    bool not_read = *drained < 0 && node.large.direct_reads == reads_before;
    memset(buf, 0, sizeof(buf));
    *waited = tt_receive(&node, buf, sizeof(buf), &any_ip, &any_port, (int64_t)tt_SECOND / WAIT_DIVISOR);
    return not_read && *waited == (int32_t)behind && memcmp(buf, behind_bytes, behind) == 0;
}

// 2c. no hint
static void check_after_unaimed(void) {
    __atomic_store_n(&node.large.direct_bytes, 0U, __ATOMIC_SEQ_CST);
    uint32_t len = build(framing, FRAMING + HINT_BYTES);
    if (!send_built(len, "unaimed")) {
        return;
    }
    int32_t got = read_it(&node, len);
    int32_t drained = 0;
    int32_t waited = 0;
    bool behind = read_whole(got, len) && behind_waits(&drained, &waited);
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line),
                   "after a run read without a hint (read %d): the drain answered %d without reading, the wait read "
                   "%d: %s",
                   (int)got, (int)drained, (int)waited, behind ? "as it should" : "NOT as it should");
    verdict(behind && node.large.direct_split == NULL, line);
}

// Receive offload on, as gro_turn_on() leaves it; false when the kernel refuses (the case is then not run).
static bool gro_on(bool on) {
    int value = on ? 1 : 0;
    bool set = setsockopt(node.hal.sock, SOL_UDP, UDP_GRO, &value, sizeof(value)) == 0 &&
               setsockopt(node.hal.data_sock, SOL_UDP, UDP_GRO, &value, sizeof(value)) == 0;
    node.hal.gro_state = (uint8_t)(on && set ? TT_GRO_ON : TT_GRO_PLAIN);
    node.hal.gro_quiet = 0;
    return set;
}

// 2d. GRO on
static void check_gro_in_place(void) {
    if (!gro_on(true)) {
        printf("rx_direct_check: SKIP - GRO on: the kernel refuses UDP_GRO\n");
        (void)gro_on(false);
        return;
    }
    hint(&node);
    uint64_t runs = node.large.direct_reads;
    uint32_t len = build(framing, FRAMING + HINT_BYTES);
    if (!send_built(len, "GRO in place")) {
        (void)gro_on(false);
        return;
    }
    int32_t got = read_it(&node, len);
    bool placed = got == (int32_t)len && memcmp(buf, framing, FRAMING) == 0 &&
                  memcmp(place, datagram + FRAMING, HINT_BYTES) == 0 && node.large.direct_split == place &&
                  node.large.direct_reads == runs + 1U;
    __atomic_store_n(&node.large.direct_bytes, 0U, __ATOMIC_SEQ_CST);
    int32_t drained = 0;
    int32_t waited = 0;
    bool behind = placed && behind_waits(&drained, &waited);
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line),
                   "GRO on: the hinted run read %d, %s; the drain after it answered %d, the wait %d", (int)got,
                   placed ? "in place" : "NOT in place", (int)drained, (int)waited);
    verdict(placed && behind, line);
    (void)gro_on(false);
}

// 2e. merged
static void check_gro_merged(void) {
    if (!gro_on(true)) {
        printf("rx_direct_check: SKIP - merged: the kernel refuses UDP_GRO\n");
        (void)gro_on(false);
        return;
    }
    hint(&node);
    uint64_t missed = node.large.direct_missed;
    uint32_t len = build(framing, FRAMING + HINT_BYTES); // the hinted run's length and framing, sent as two datagrams
    uint16_t segment = (uint16_t)((len + 1U) / 2U);
    struct sockaddr_in dest;
    socklen_t dest_len = sizeof(dest);
    bool sent = getsockname(node.hal.data_sock, (struct sockaddr*)&dest, &dest_len) == 0;
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int gso = socket(AF_INET, SOCK_DGRAM, 0);
    int size = segment;
    sent = sent && gso >= 0 && setsockopt(gso, SOL_UDP, UDP_SEGMENT, &size, sizeof(size)) == 0 &&
           sendto(gso, datagram, len, 0, (struct sockaddr*)&dest, sizeof(dest)) == (ssize_t)len;
    if (gso >= 0) {
        (void)close(gso);
    }
    usleep(SETTLE_US);
    if (!sent) {
        printf("rx_direct_check: SKIP - merged: no UDP_SEGMENT send on this kernel\n");
        (void)gro_on(false);
        return;
    }
    int32_t first = read_it(&node, segment);
    bool first_whole = first == (int32_t)segment && memcmp(buf, datagram, segment) == 0;
    bool not_split = node.large.direct_split == NULL;
    __atomic_store_n(&node.large.direct_bytes, 0U, __ATOMIC_SEQ_CST);
    // Not cleared first: the second datagram is handed out where the merged read left it, in this buffer.
    int32_t second = tt_try_receive(&node, buf, sizeof(buf), &any_ip, &any_port);
    const uint8_t* second_at = buf + node.rx_offset;
    bool second_whole = second == (int32_t)(len - segment) && memcmp(second_at, datagram + segment, len - segment) == 0;
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line),
                   "merged read the hinted run's length: first %d of %u bytes %s, split %s, second %d %s, missed +%lu",
                   (int)first, (unsigned)segment, first_whole ? "whole" : "NOT whole", not_split ? "not set" : "SET",
                   (int)second, second_whole ? "whole" : "NOT whole",
                   (unsigned long)(node.large.direct_missed - missed));
    verdict(first_whole && not_split && second_whole && node.large.direct_missed == missed + 1U, line);
    (void)gro_on(false);
}

// 3. shorter
static void check_shorter(void) {
    hint(&node);
    uint8_t shorter[FRAMING];
    memcpy(shorter, framing, FRAMING);
    uint16_t two = 2;
    memcpy(shorter + COUNT_AT, &two, sizeof(two));
    uint32_t len = build(shorter, FRAMING + (2U * UNIT));
    if (!send_built(len, "shorter")) {
        return;
    }
    int32_t got = read_it(&node, len);
    bool rest_untouched = true;
    for (uint32_t i = 2U * UNIT; i < HINT_UNITS * UNIT; i++) {
        rest_untouched = rest_untouched && place[i] == SENTINEL;
    }
    verdict(got == (int32_t)len && memcmp(place, datagram + FRAMING, (size_t)2U * UNIT) == 0 && rest_untouched &&
                node.large.direct_split == place,
            rest_untouched ? "shorter run in place, the hinted bytes past it untouched"
                           : "shorter run: the hinted bytes past it WRITTEN");
}

// One datagram that is not the hinted run: read whole, exactly as sent, not in place, counted missed.
static void check_miss(const uint8_t* head, uint32_t len, const char* what) {
    hint(&node);
    uint64_t missed = node.large.direct_missed;
    build(head, len);
    if (!send_built(len, what)) {
        return;
    }
    int32_t got = read_it(&node, len);
    bool whole = read_whole(got, len);
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line), "miss, %s: read %d of %u bytes %s, split %s, missed +%lu", what, (int)got,
                   (unsigned)len, whole ? "whole" : "NOT whole", node.large.direct_split ? "SET" : "not set",
                   (unsigned long)(node.large.direct_missed - missed));
    verdict(whole && node.large.direct_split == NULL && node.large.direct_missed == missed + 1U, line);
}

// 4-8. misses
static void check_misses(void) {
    uint8_t other[FRAMING];
    memcpy(other, framing, FRAMING);
    other[COUNT_AT - 1U] ^= BYTE_FLIP; // before the count: frag_count's high byte
    check_miss(other, FRAMING + HINT_BYTES, "framing differs before the count");
    memcpy(other, framing, FRAMING);
    other[FRAMING - 1U] ^= BYTE_FLIP; // after it
    check_miss(other, FRAMING + HINT_BYTES, "framing differs after the count");
    memcpy(other, framing, FRAMING);
    uint16_t count = 0;
    memcpy(other + COUNT_AT, &count, sizeof(count));
    check_miss(other, FRAMING, "a count of 0 (the framing alone)");
    count = HINT_UNITS + 2U;
    memcpy(other + COUNT_AT, &count, sizeof(count));
    check_miss(other, FRAMING + ((HINT_UNITS + 2U) * UNIT), "more units than hinted");
    check_miss(framing, FRAMING + HINT_BYTES - 1U, "a length not count x unit");
    check_miss(framing, FRAMING - SHORT_BY, "shorter than the framing");
}

// 8b. a caller's buffer shorter than the framing and the hinted bytes: not aimed - read into it, cut as recvfrom()
// cuts, the hinted place untouched.
static void check_short_buffer(void) {
    hint(&node);
    uint64_t aimed = node.large.direct_reads;
    uint32_t len = build(framing, FRAMING + HINT_BYTES);
    if (!send_built(len, "short buffer")) {
        return;
    }
    static uint8_t small[FRAMING + UNIT];
    int32_t got = -1;
    for (int i = 0; i < MAX_READS && got != (int32_t)sizeof(small); i++) {
        got = tt_try_receive(&node, small, sizeof(small), &any_ip, &any_port);
    }
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line), "a buffer too short for the hint: %d bytes read into it, %s", (int)got,
                   node.large.direct_reads == aimed ? "not aimed" : "AIMED");
    verdict(got == (int32_t)sizeof(small) && memcmp(small, datagram, sizeof(small)) == 0 &&
                node.large.direct_reads == aimed && place[0] == SENTINEL,
            line);
}

// 9. withdrawn
static void check_withdrawn(void) {
    hint(&node);
    uint64_t missed = node.large.direct_missed;
    uint32_t len = build(framing, FRAMING + HINT_BYTES);
    if (!send_built(len, "withdrawn")) {
        return;
    }
    withdraw_after_read = &node;
    int32_t got = read_it(&node, len);
    withdraw_after_read = NULL;
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line), "withdrawn during the read: read %d of %u bytes %s, split %s, missed +%lu",
                   (int)got, (unsigned)len, read_whole(got, len) ? "whole" : "NOT whole",
                   node.large.direct_split != NULL ? "SET" : "not set",
                   (unsigned long)(node.large.direct_missed - missed));
    verdict(read_whole(got, len) && node.large.direct_split == NULL && node.large.direct_missed == missed + 1U, line);
}

// 10. switch
static void check_switch(void) {
    verdict(node.large.direct_allowed, "switch: unset, the read in place is on");
    setenv("TT_RX_DIRECT", "0", 1);
    static struct tt_Context off;
    if (tt_Context_create(&off) != tt_RET_OK) {
        printf("rx_direct_check: SETUP FAILED - cannot create the second context\n");
        setup_failed = true;
        return;
    }
    verdict(!off.large.direct_allowed, "switch: TT_RX_DIRECT=0 turns it off");
    tt_Context_destroy(&off);
}

int main(void) {
    _tt_CONFIG.broadcast = "127.255.255.255";
    _tt_CONFIG.context_id = CHECK_CONTEXT_ID;
    build_framing();
    unsetenv("TT_RX_DIRECT");
    if (tt_Context_create(&node) != tt_RET_OK) {
        printf("rx_direct_check: SETUP FAILED - cannot create context\n");
        return CHECK_SETUP_FAILED;
    }
    sender = socket(AF_INET, SOCK_DGRAM, 0);
    if (sender < 0) {
        printf("rx_direct_check: SETUP FAILED - no sender socket\n");
        return CHECK_SETUP_FAILED;
    }
    usleep(SETTLE_US);
    while (tt_try_receive(&node, buf, sizeof(buf), &any_ip, &any_port) >= 0) {
    }
    check_control();
    check_in_place();
    check_after();
    check_after_unaimed();
    check_gro_in_place();
    check_gro_merged();
    check_shorter();
    check_misses();
    check_short_buffer();
    check_withdrawn();
    tt_Context_destroy(&node);
    check_switch();
    (void)close(sender);
    if (setup_failed) {
        return CHECK_SETUP_FAILED;
    }
    if (failures != 0) {
        printf("rx_direct_check: FAIL - %d check(s)\n", failures);
        return CHECK_FAIL;
    }
    printf("rx_direct_check: PASS - all\n");
    return CHECK_PASS;
}
