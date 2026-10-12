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

#include <arpa/inet.h>
#include <netinet/in.h>
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
#define PLACE_BYTES 1024U
#define SENTINEL 0xA5
#define FRAMING ((uint32_t)(sizeof(struct tt_SingleHeader) + sizeof(struct tt_FragRunLHeader)))
#define COUNT_AT ((uint32_t)(sizeof(struct tt_SingleHeader) + offsetof(struct tt_FragRunLHeader, run_count)))
#define SETTLE_US 1000
#define MAX_READS 16

static uint8_t place[PLACE_BYTES];
static uint8_t framing[FRAMING];
static uint8_t datagram[FRAMING + (8U * UNIT)];
static uint8_t buf[tt_MAX_BUFFER_LENGTH];

static void build_framing(void) {
    memset(framing, 0, sizeof(framing));
    framing[0] = (uint8_t)tt_SINGLE_MARKER_LE; // the HAL compares bytes, not their meaning
    framing[1] = tt_VERSION;
    framing[2] = 7;
    framing[3] = tt_SUBMESSAGE_TYPE_FRAG_RUN_L;
    for (uint32_t i = 4; i < FRAMING; i++) {
        framing[i] = (uint8_t)(0x30U + i);
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
        datagram[i] = (uint8_t)(i * 13U + 1U);
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
            return got;
        }
        if ((uint32_t)got == len && buf[2] == framing[2]) {
            return got;
        }
    }
    return -1;
}

static int failures;

// The HAL's recvmsg(), linked here ahead of the C library's: the system call, then - when asked - the hint withdrawn
// as another thread's large_direct_withdraw() does it, after the bytes went where the hint said.
static struct tt_Context* withdraw_after_read;

ssize_t recvmsg(int socket_fd, struct msghdr* msg, int flags) {
    ssize_t got = syscall(SYS_recvmsg, socket_fd, msg, flags);
    if (withdraw_after_read != NULL && got > 0) { // a read that took a datagram, not an empty socket
        __atomic_store_n(&withdraw_after_read->large.direct_bytes, 0U, __ATOMIC_SEQ_CST);
        withdraw_after_read = NULL;
    }
    return got;
}

static void expect(bool ok, const char* what) {
    if (!ok) {
        printf("rx_direct_check: FAIL - %s\n", what);
        failures++;
    }
}

// One datagram that is not the hinted run: read whole, exactly as sent, not in place, counted missed.
static void check_miss(struct tt_Context* node, int sender, const uint8_t* head, uint32_t len, const char* what) {
    hint(node);
    uint64_t missed = node->large.direct_missed;
    build(head, len);
    if (!send_to(sender, node->hal.data_sock, len)) {
        printf("rx_direct_check: SETUP FAILED - send (%s)\n", what);
        failures++;
        return;
    }
    int32_t got = read_it(node, len);
    bool whole = got == (int32_t)len && memcmp(buf, datagram, len) == 0;
    bool ok = whole && node->large.direct_split == NULL && node->large.direct_missed == missed + 1U;
    printf("rx_direct_check: %s - miss, %s: read %d of %u bytes %s, split %s, missed +%lu\n", ok ? "PASS" : "FAIL",
           what, (int)got, (unsigned)len, whole ? "whole" : "NOT whole", node->large.direct_split ? "SET" : "not set",
           (unsigned long)(node->large.direct_missed - missed));
    failures += ok ? 0 : 1;
}

int main(void) {
    _tt_CONFIG.broadcast = "127.255.255.255";
    _tt_CONFIG.context_id = CHECK_CONTEXT_ID;
    build_framing();
    unsetenv("TT_RX_DIRECT");
    static struct tt_Context node;
    if (tt_Context_create(&node) != tt_RET_OK) {
        printf("rx_direct_check: SETUP FAILED - cannot create context\n");
        return CHECK_SETUP_FAILED;
    }
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    if (sender < 0) {
        printf("rx_direct_check: SETUP FAILED - no sender socket\n");
        return CHECK_SETUP_FAILED;
    }
    usleep(SETTLE_US);
    uint32_t ip = 0;
    uint16_t port = 0;
    while (tt_try_receive(&node, buf, sizeof(buf), &ip, &port) >= 0) {
    }

    // 1. control
    __atomic_store_n(&node.large.direct_bytes, 0U, __ATOMIC_SEQ_CST);
    uint64_t reads = node.large.direct_reads;
    uint32_t len = build(framing, FRAMING + (HINT_UNITS * UNIT));
    if (!send_to(sender, node.hal.data_sock, len)) {
        printf("rx_direct_check: SETUP FAILED - send (control)\n");
        return CHECK_SETUP_FAILED;
    }
    int32_t got = read_it(&node, len);
    expect(got == (int32_t)len && memcmp(buf, datagram, len) == 0, "control: the datagram not read whole");
    expect(node.large.direct_reads == reads && node.large.direct_split == NULL, "control: a read counted as aimed");

    // 2. in place
    hint(&node);
    reads = node.large.direct_reads;
    len = build(framing, FRAMING + (HINT_UNITS * UNIT));
    if (!send_to(sender, node.hal.data_sock, len)) {
        printf("rx_direct_check: SETUP FAILED - send (in place)\n");
        return CHECK_SETUP_FAILED;
    }
    got = read_it(&node, len);
    bool in_place = got == (int32_t)len && memcmp(buf, framing, FRAMING) == 0 &&
                    memcmp(place, datagram + FRAMING, HINT_UNITS * UNIT) == 0 && node.large.direct_split == place &&
                    node.large.direct_reads == reads + 1U;
    printf("rx_direct_check: %s - in place: %d bytes, payload %s the hinted place, split %s\n",
           in_place ? "PASS" : "FAIL", (int)got,
           memcmp(place, datagram + FRAMING, HINT_UNITS * UNIT) == 0 ? "in" : "NOT in",
           node.large.direct_split == place ? "set" : "NOT set");
    failures += in_place ? 0 : 1;

    // 3. shorter
    hint(&node);
    uint8_t shorter[FRAMING];
    memcpy(shorter, framing, FRAMING);
    uint16_t two = 2;
    memcpy(shorter + COUNT_AT, &two, sizeof(two));
    len = build(shorter, FRAMING + (2U * UNIT));
    if (!send_to(sender, node.hal.data_sock, len)) {
        printf("rx_direct_check: SETUP FAILED - send (shorter)\n");
        return CHECK_SETUP_FAILED;
    }
    got = read_it(&node, len);
    bool rest_untouched = true;
    for (uint32_t i = 2U * UNIT; i < HINT_UNITS * UNIT; i++) {
        rest_untouched = rest_untouched && place[i] == SENTINEL;
    }
    bool shorter_ok = got == (int32_t)len && memcmp(place, datagram + FRAMING, 2U * UNIT) == 0 && rest_untouched &&
                      node.large.direct_split == place;
    printf("rx_direct_check: %s - shorter run in place, the hinted bytes past it %s\n", shorter_ok ? "PASS" : "FAIL",
           rest_untouched ? "untouched" : "WRITTEN");
    failures += shorter_ok ? 0 : 1;

    // 4-8. misses
    uint8_t other[FRAMING];
    memcpy(other, framing, FRAMING);
    other[COUNT_AT - 1U] ^= 0xFFU; // before the count: frag_count's high byte
    check_miss(&node, sender, other, FRAMING + (HINT_UNITS * UNIT), "framing differs before the count");
    memcpy(other, framing, FRAMING);
    other[FRAMING - 1U] ^= 0xFFU; // after it
    check_miss(&node, sender, other, FRAMING + (HINT_UNITS * UNIT), "framing differs after the count");
    memcpy(other, framing, FRAMING);
    uint16_t count = 0;
    memcpy(other + COUNT_AT, &count, sizeof(count));
    check_miss(&node, sender, other, FRAMING, "a count of 0 (the framing alone)");
    count = HINT_UNITS + 2U;
    memcpy(other + COUNT_AT, &count, sizeof(count));
    check_miss(&node, sender, other, FRAMING + ((HINT_UNITS + 2U) * UNIT), "more units than hinted");
    check_miss(&node, sender, framing, FRAMING + (HINT_UNITS * UNIT) - 1U, "a length not count x unit");
    check_miss(&node, sender, framing, FRAMING - 8U, "shorter than the framing");

    // 8b. a caller's buffer shorter than the framing and the hinted bytes: not aimed - read into it, cut as recvfrom()
    // cuts, the hinted place untouched.
    hint(&node);
    uint64_t aimed = node.large.direct_reads;
    len = build(framing, FRAMING + (HINT_UNITS * UNIT));
    if (!send_to(sender, node.hal.data_sock, len)) {
        printf("rx_direct_check: SETUP FAILED - send (short buffer)\n");
        return CHECK_SETUP_FAILED;
    }
    static uint8_t small[FRAMING + UNIT];
    got = -1;
    for (int i = 0; i < MAX_READS && got != (int32_t)sizeof(small); i++) {
        got = tt_try_receive(&node, small, sizeof(small), &ip, &port);
    }
    bool short_ok = got == (int32_t)sizeof(small) && memcmp(small, datagram, sizeof(small)) == 0 &&
                    node.large.direct_reads == aimed && place[0] == SENTINEL;
    printf("rx_direct_check: %s - a buffer too short for the hint: %d bytes read into it, %s\n",
           short_ok ? "PASS" : "FAIL", (int)got, node.large.direct_reads == aimed ? "not aimed" : "AIMED");
    failures += short_ok ? 0 : 1;

    // 9. withdrawn
    hint(&node);
    uint64_t missed = node.large.direct_missed;
    len = build(framing, FRAMING + (HINT_UNITS * UNIT));
    if (!send_to(sender, node.hal.data_sock, len)) {
        printf("rx_direct_check: SETUP FAILED - send (withdrawn)\n");
        return CHECK_SETUP_FAILED;
    }
    withdraw_after_read = &node;
    got = read_it(&node, len);
    withdraw_after_read = NULL;
    bool withdrawn_ok = got == (int32_t)len && memcmp(buf, datagram, len) == 0 && node.large.direct_split == NULL &&
                        node.large.direct_missed == missed + 1U;
    printf("rx_direct_check: %s - withdrawn during the read: read %d of %u bytes %s, split %s, missed +%lu\n",
           withdrawn_ok ? "PASS" : "FAIL", (int)got, (unsigned)len,
           got == (int32_t)len && memcmp(buf, datagram, len) == 0 ? "whole" : "NOT whole",
           node.large.direct_split != NULL ? "SET" : "not set", (unsigned long)(node.large.direct_missed - missed));
    failures += withdrawn_ok ? 0 : 1;

    tt_Context_destroy(&node);

    // 10. switch
    expect(node.large.direct_allowed, "switch: unset, the read in place is not on");
    setenv("TT_RX_DIRECT", "0", 1);
    static struct tt_Context off;
    if (tt_Context_create(&off) != tt_RET_OK) {
        printf("rx_direct_check: SETUP FAILED - cannot create the second context\n");
        return CHECK_SETUP_FAILED;
    }
    expect(!off.large.direct_allowed, "switch: TT_RX_DIRECT=0 left it on");
    tt_Context_destroy(&off);
    (void)close(sender);
    if (failures != 0) {
        printf("rx_direct_check: FAIL - %d check(s)\n", failures);
        return CHECK_FAIL;
    }
    printf("rx_direct_check: PASS - all\n");
    return CHECK_PASS;
}
