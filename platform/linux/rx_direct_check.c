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
//                 next wait reads what came behind it, whole. In 2b-2d the skipped read is counted (idle_skips=).
//   2f. not a run - the control of that skip: after a datagram that is not a run the drain reads the socket and
//                 returns what came behind it.
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
//  11. nonblock - both sockets O_NONBLOCK (so a wait does not check a datagram's checksum before the read does);
//                 TT_RX_NONBLOCK=0 leaves both blocking. And a blocking send still blocks rather than failing: with
//                 the data socket's send buffer filled to refusal (MSG_DONTWAIT), tt_send_to() waits for room and
//                 sends - it returns the length, not -1 with EAGAIN (SKIP where loopback never lets the buffer fill).
//  12. deadline - the wait's deadline timer, with the poll loop's absolute deadline set by hand (wait_until): a wait
//                 that ends for data leaves the timer armed; the next, with a LATER deadline, sets nothing - the timer
//                 fires at the old time, is re-armed, and the wait times out at its own deadline, not before; a wait
//                 with an EARLIER deadline re-arms at once. Outside a poll (wait_until 0) the timer is not used.
//  12b. deadline after no data - the timer only after 8 waits in a row ended for data: after a fire the next wait
//                 takes its timeout (no set); a ping-pong never uses it; 8 datagrams put it back; a stale fire seen by
//                 a plain wait is read once and the wait goes on; TT_RX_LAZY_ADAPT=0 (the flag) turns the rule off.
//  12c. fire and data - a fire in a wait that ends for a datagram starts the run again, with the timer in use and
//                 without; the control, a plain wait for a datagram with no fire, counts one.
//  13. again  - a blocking send the buffer refused is made again, from what the sender kept for it: this check's own
//                 sendto() and sendmsg() refuse the next call (EAGAIN) and tt_send(), tt_send_to() and tt_send_iov()
//                 each return the length after exactly two calls, the datagram arriving whole at its address; the
//                 control, refused with EPERM instead, returns -1 with EPERM after one call (nothing waits for room);
//                 and a send nothing refuses makes one call.
// Exit 0 pass, 1 fail, 3 setup failed. Run by test.sh (test-linux) in a private namespace.

// Built as rmw_tickle is (platform/linux/Makefile passes the same), so a lint of this file alone sees the same build.
#ifndef tt_MAX_BUFFER_LENGTH
#define tt_MAX_BUFFER_LENGTH 65507
#endif
#ifndef tt_LARGE_SAMPLES
#define tt_LARGE_SAMPLES 1
#endif

#include <errno.h>
#include <fcntl.h>   // fcntl(F_GETFL): case 11
#include <pthread.h> // 12c: a datagram sent while the wait runs
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
#define NONBLOCK_SEND_BYTES 60000U // a datagram that takes most of a small send buffer
#define NONBLOCK_FILL_TRIES 64
#define NONBLOCK_SNDBUF 4096 // a send buffer small enough to fill
#define NS_PER_US 1000
#define DEADLINE_FIRST_NS 200000000ULL  // 12: the first wait's deadline, 200 ms
#define DEADLINE_SECOND_NS 300000000ULL // the second's, later
#define DEADLINE_SHORT_NS 20000000ULL   // an earlier one, 20 ms
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

// The HAL's sendto() and sendmsg(), linked here ahead of the C library's (13): the system call, unless the next calls
// are to be refused - then -1 with refuse_errno, as a full send buffer (EAGAIN) or a refusal of another kind. Counted.
static int refuse_sends;
static int refuse_errno;
static int send_calls;

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) - libc's prototype names them __fd and so on
ssize_t sendto(int socket_fd, const void* data, size_t len, int flags, const struct sockaddr* to, socklen_t to_len) {
    send_calls++;
    if (refuse_sends > 0) {
        refuse_sends--;
        errno = refuse_errno;
        return -1;
    }
    return syscall(SYS_sendto, socket_fd, data, len, flags, to, to_len);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) - as above
ssize_t sendmsg(int socket_fd, const struct msghdr* msg, int flags) {
    send_calls++;
    if (refuse_sends > 0) {
        refuse_sends--;
        errno = refuse_errno;
        return -1;
    }
    return syscall(SYS_sendmsg, socket_fd, msg, flags);
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
    uint64_t skips_before = node.hal.rx_idle_skips;
    *drained = tt_try_receive(&node, buf, sizeof(buf), &any_ip, &any_port);
    // Skipped, and counted as skipped (the rx line's idle_skips=, what a rig run reads to see the cut engage).
    bool not_read = *drained < 0 && node.large.direct_reads == reads_before && node.hal.rx_idle_skips > skips_before;
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

// 2f. not a run (the control of 2b-2d's skip): a datagram that is not a run leaves its socket asked - the drain after
// it returns what came behind it, with no empty answer between.
static void check_not_a_run(void) {
    __atomic_store_n(&node.large.direct_bytes, 0U, __ATOMIC_SEQ_CST);
    uint8_t head[FRAMING];
    memcpy(head, framing, FRAMING);
    // Not the single form: neither a run nor a large fragment (which would turn receive offload on for the cases after)
    head[0] ^= BYTE_FLIP;
    uint32_t len = build(head, FRAMING + HINT_BYTES);
    if (!send_built(len, "not a run")) {
        return;
    }
    int32_t got = read_it(&node, len);
    uint32_t behind = FRAMING + UNIT + BEHIND_EXTRA;
    head[FRAMING - 1U] ^= FRAMING_FLIP;
    build(head, behind);
    if (!send_built(behind, "behind not a run")) {
        return;
    }
    uint64_t skips_before = node.hal.rx_idle_skips;
    bool drained_it = false;
    bool empty_between = false;
    for (int i = 0; i < MAX_READS && !drained_it && !empty_between; i++) {
        int32_t drained = tt_try_receive(&node, buf, sizeof(buf), &any_ip, &any_port);
        empty_between = drained < 0;
        drained_it = drained == (int32_t)behind && buf[2] == framing[2];
    }
    // Drained to its end, as a poll would: that resets rx_idle and its recheck clock, which the cases after start from.
    while (tt_try_receive(&node, buf, sizeof(buf), &any_ip, &any_port) >= 0) {
    }
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line),
                   "not a run (read %d): the drain after it %s what came behind, %s, skips counted %lu", (int)got,
                   drained_it ? "returned" : "did NOT return", empty_between ? "an empty answer first" : "at once",
                   (unsigned long)(node.hal.rx_idle_skips - skips_before));
    // The skip count is printed, not judged: a socket found empty in the same drain is skipped too (rx_idle), and which
    // socket the drain asks first alternates. What is judged is that the data socket was read.
    verdict(got == (int32_t)len && drained_it, line);
}

// Receive offload on, as gro_turn_on() leaves it; false when the kernel refuses (the case is then not run).
static bool gro_on(bool enable) {
    int value = enable ? 1 : 0;
    bool set = setsockopt(node.hal.sock, SOL_UDP, UDP_GRO, &value, sizeof(value)) == 0 &&
               setsockopt(node.hal.data_sock, SOL_UDP, UDP_GRO, &value, sizeof(value)) == 0;
    node.hal.gro_state = (uint8_t)(enable && set ? TT_GRO_ON : TT_GRO_PLAIN);
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

// 11. nonblock
static bool socket_nonblocking(int socket_fd) {
    int flags = fcntl(socket_fd, F_GETFL);
    return flags >= 0 && (flags & O_NONBLOCK) != 0;
}

static void check_nonblocking(void) {
    verdict(socket_nonblocking(node.hal.sock) && socket_nonblocking(node.hal.data_sock),
            "nonblock: unset, both sockets are O_NONBLOCK");
    // A small send buffer, sent into with MSG_DONTWAIT until it refuses, then one blocking send.
    int small = NONBLOCK_SNDBUF;
    // NOLINTNEXTLINE(misc-include-cleaner) - SOL_SOCKET/SO_SNDBUF: <sys/socket.h>, via glibc-private headers
    (void)setsockopt(node.hal.data_sock, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    struct sockaddr_in sink;
    socklen_t sink_len = sizeof(sink);
    (void)getsockname(sender, (struct sockaddr*)&sink, &sink_len);
    sink.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    static uint8_t big[NONBLOCK_SEND_BYTES];
    memset(big, 1, sizeof(big));
    int refused = 0;
    for (int i = 0; i < NONBLOCK_FILL_TRIES && refused == 0; i++) {
        if (sendto(node.hal.data_sock, big, sizeof(big), MSG_DONTWAIT, (struct sockaddr*)&sink, sizeof(sink)) < 0) {
            refused = errno;
        }
    }
    int32_t sent = tt_send_to(&node, big, sizeof(big), INADDR_LOOPBACK, ntohs(sink.sin_port));
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line), "nonblock: a blocking send after the buffer refused (%s) returned %d of %u",
                   refused == EAGAIN ? "EAGAIN" : "never refused", (int)sent, (unsigned)sizeof(big));
    if (refused == EAGAIN) {
        verdict(sent == (int32_t)sizeof(big), line);
    } else {
        // Loopback frees a datagram's send-buffer charge as it is sent, so the buffer may never fill here: no verdict.
        printf("rx_direct_check: SKIP - %s\n", line);
    }
    setenv("TT_RX_NONBLOCK", "0", 1);
    static struct tt_Context blocking;
    if (tt_Context_create(&blocking) != tt_RET_OK) {
        printf("rx_direct_check: SETUP FAILED - cannot create the blocking context\n");
        setup_failed = true;
        return;
    }
    verdict(!socket_nonblocking(blocking.hal.sock) && !socket_nonblocking(blocking.hal.data_sock),
            "nonblock: TT_RX_NONBLOCK=0 leaves both sockets blocking");
    tt_Context_destroy(&blocking);
    unsetenv("TT_RX_NONBLOCK");
}

// 12. deadline
static void set_wait_until(uint64_t until) {
    node.wait_until_hi = (uint32_t)(until >> 32U);
    node.wait_until_lo = (uint32_t)until;
}

// As a poll waits: tt_receive() until it returns `len` bytes or `until` has passed - -1 then. A wait that comes back
// early with nothing (an interrupt: io_uring task work reads as one) is made again, as the poll loop makes it.
static int32_t receive_until(uint32_t len, uint64_t until) {
    set_wait_until(until);
    for (int i = 0; i < MAX_READS; i++) {
        uint64_t now = tt_get_ns();
        if (now >= until) {
            return -1;
        }
        int32_t got = tt_receive(&node, buf, sizeof(buf), &any_ip, &any_port, (int64_t)(until - now));
        if (len != 0 && got == (int32_t)len) {
            return got;
        }
    }
    return -2;
}

static void check_deadline_after_no_data(void);
static void check_deadline_fire_and_data(void);

static void check_deadline(void) {
    if (node.hal.timer_fd_plus1 == 0) {
        printf("rx_direct_check: SKIP - deadline: no timer in the wait set on this kernel\n");
        return;
    }
    while (tt_try_receive(&node, buf, sizeof(buf), &any_ip, &any_port) >= 0) {
    }
    __atomic_store_n(&node.large.direct_bytes, 0U, __ATOMIC_SEQ_CST);
    // The timer as a stream subscriber has it, after a run of waits that ended for data (12b tests that rule).
    node.hal.timer_data_streak = UINT8_MAX;
    // A: deadline in 200 ms, data at once: the wait ends for the data, the timer stays armed (one set).
    uint64_t sets = node.hal.timer_sets;
    uint64_t start = tt_get_ns();
    uint32_t len = build(framing, FRAMING + UNIT);
    bool sent = send_built(len, "deadline");
    int32_t got = receive_until(len, start + DEADLINE_FIRST_NS);
    bool data_arms = sent && got == (int32_t)len && node.hal.timer_sets == sets + 1U && node.hal.timer_armed_ns != 0;
    // B: a later deadline (300 ms): nothing set until the old timer fires at 200 ms; re-armed once; ends at 300.
    const uint64_t until = start + DEADLINE_SECOND_NS;
    sets = node.hal.timer_sets;
    uint64_t fires = node.hal.timer_fires;
    int32_t timed_out = receive_until(0, until);
    uint64_t ended = tt_get_ns();
    int b_sets = (int)(node.hal.timer_sets - sets);
    int b_fires = (int)(node.hal.timer_fires - fires);
    bool later_waits = timed_out == -1 && ended >= until && b_sets == 1 && b_fires == 2 && node.hal.timer_armed_ns == 0;
    // C: data under a 300 ms deadline (armed for it), then an EARLIER deadline, 20 ms: re-armed at once (one set).
    node.hal.timer_data_streak = UINT8_MAX; // B ended in a fire, which starts the run again
    (void)send_built(len, "deadline C");
    (void)receive_until(len, tt_get_ns() + DEADLINE_SECOND_NS);
    sets = node.hal.timer_sets;
    uint64_t short_until = tt_get_ns() + DEADLINE_SHORT_NS;
    int32_t short_out = receive_until(0, short_until);
    uint64_t short_ended = tt_get_ns();
    bool earlier_rearms =
        short_out == -1 && short_ended < short_until + DEADLINE_SHORT_NS && node.hal.timer_sets == sets + 1U;
    // D: outside a poll (wait_until 0) the timer is not used.
    set_wait_until(0);
    uint64_t waits = node.hal.timer_waits;
    (void)tt_receive(&node, buf, sizeof(buf), &any_ip, &any_port, (int64_t)DEADLINE_SHORT_NS);
    bool outside_none = node.hal.timer_waits == waits;
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line),
                   "deadline: data leaves it armed %d; a later deadline: %d set, %d fires, ended %+lld us from it %d; "
                   "an earlier one re-arms at once %d; none outside a poll %d",
                   data_arms ? 1 : 0, b_sets, b_fires, (long long)((int64_t)(ended - until) / NS_PER_US),
                   later_waits ? 1 : 0, earlier_rearms ? 1 : 0, outside_none ? 1 : 0);
    verdict(data_arms && later_waits && earlier_rearms && outside_none, line);
    check_deadline_after_no_data();
}

// 12b. The timer only after TT_RX_LAZY_STREAK (8) waits in a row ended for data (hal_linux.c rx_wait_epoll()). E: C's
// last wait ended in a fire, so the next takes its timeout: no set, no timer wait, one plain wait, ended at its
// deadline. P: a ping-pong - a wait ending for a datagram, then one ending at its deadline, three times - never uses
// the timer (no set, no timer wait: ae5defc8 cost the latency client 2 sets and a read a round trip), and its run is 1
// after each datagram and 0 after each deadline: a plain wait that ended without data starts it again. F: eight
// datagrams in a row put it back; a wait woken by tt_wake_signal() leaves it armed (50 ms) and the next is plain; that
// one (150 ms) sees the stale fire at 50 ms, reads it - no set - and goes on to its own deadline. G: with the rule off
// (TT_RX_LAZY_ADAPT=0, here the flag) a wait after a timeout uses the timer again.
#define DEADLINE_STALE_NS 50000000ULL  // F: the deadline the woken wait leaves armed
#define DEADLINE_PLAIN_NS 150000000ULL // F: the plain wait's own, later
#define DEADLINE_E_NS 30000000ULL      // E: a plain wait's deadline
#define LAZY_STREAK 8U                 // hal_linux.c TT_RX_LAZY_STREAK
#define PING_PONGS 3
static void check_deadline_after_no_data(void) {
    uint64_t sets = node.hal.timer_sets;
    uint64_t waits = node.hal.timer_waits;
    uint64_t plain = node.hal.timer_plain_waits;
    uint64_t e_until = tt_get_ns() + DEADLINE_E_NS;
    int32_t e_out = receive_until(0, e_until);
    bool e_plain = e_out == -1 && tt_get_ns() >= e_until && node.hal.timer_sets == sets &&
                   node.hal.timer_waits == waits && node.hal.timer_plain_waits == plain + 1U;
    // P
    uint32_t len = build(framing, FRAMING + UNIT);
    sets = node.hal.timer_sets;
    waits = node.hal.timer_waits;
    bool p_data = true;
    bool p_runs = true;
    for (int i = 0; i < PING_PONGS; i++) {
        (void)send_built(len, "deadline P");
        p_data = p_data && receive_until(len, tt_get_ns() + DEADLINE_FIRST_NS) == (int32_t)len;
        p_runs = p_runs && node.hal.timer_data_streak == 1U;
        (void)receive_until(0, tt_get_ns() + DEADLINE_SHORT_NS);
        p_runs = p_runs && node.hal.timer_data_streak == 0U;
    }
    bool p_plain = p_data && p_runs && node.hal.timer_sets == sets && node.hal.timer_waits == waits;
    // F
    bool f_data = true;
    for (uint32_t i = 0; i < LAZY_STREAK; i++) {
        (void)send_built(len, "deadline F");
        f_data = f_data && receive_until(len, tt_get_ns() + DEADLINE_FIRST_NS) == (int32_t)len;
    }
    f_data = f_data && node.hal.timer_data_streak >= LAZY_STREAK;
    (void)tt_wake_signal(&node);
    uint64_t stale = tt_get_ns() + DEADLINE_STALE_NS;
    set_wait_until(stale);
    sets = node.hal.timer_sets;
    int32_t woken = tt_receive(&node, buf, sizeof(buf), &any_ip, &any_port, (int64_t)DEADLINE_STALE_NS);
    bool f_armed = woken == -3 && node.hal.timer_sets == sets + 1U && node.hal.timer_armed_ns == stale &&
                   node.hal.timer_data_streak == 0;
    uint64_t fires = node.hal.timer_fires;
    sets = node.hal.timer_sets;
    plain = node.hal.timer_plain_waits;
    uint64_t f_until = tt_get_ns() + DEADLINE_PLAIN_NS;
    int32_t f_out = receive_until(0, f_until);
    uint64_t f_ended = tt_get_ns();
    bool f_stale = f_out == -1 && f_ended >= f_until && node.hal.timer_fires == fires + 1U &&
                   node.hal.timer_sets == sets && node.hal.timer_plain_waits == plain + 1U &&
                   node.hal.timer_armed_ns == 0;
    // G
    node.hal.timer_adapt = false;
    waits = node.hal.timer_waits;
    plain = node.hal.timer_plain_waits;
    (void)receive_until(0, tt_get_ns() + DEADLINE_SHORT_NS);
    bool g_off = node.hal.timer_waits == waits + 1U && node.hal.timer_plain_waits == plain;
    node.hal.timer_adapt = true;
    set_wait_until(0);
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line),
                   "deadline after no data: plain after a fire %d; a ping-pong never uses it %d; 8 datagrams put it "
                   "back %d; a wake leaves it armed %d; the stale fire read once, ended %+lld us from its own %d; the "
                   "rule off %d",
                   e_plain ? 1 : 0, p_plain ? 1 : 0, f_data ? 1 : 0, f_armed ? 1 : 0,
                   (long long)((int64_t)(f_ended - f_until) / NS_PER_US), f_stale ? 1 : 0, g_off ? 1 : 0);
    verdict(e_plain && p_plain && f_data && f_armed && f_stale && g_off, line);
    check_deadline_fire_and_data();
}

// 12c. A fire in a wait that ends for data (hal_linux.c rx_timer_take(), rx_wait_plain()): the timer cost that wait a
// read, so the run of waits that ended for data starts again and the next wait takes its timeout. Each case first arms
// the timer for 20 ms with a wait that ends for a datagram (the timer in use). L: 30 ms later a datagram, and a wait
// with a later deadline sees the stale fire and the datagram at once: it returns the datagram, the fire read, the run
// 0, and the next wait is plain. PL: the same with the timer not in use (a run of 0): the plain wait sees the stale
// fire and the datagram at once, returns the datagram, reads the fire, and the run stays 0. P: the timer not in use (a
// run of 0), a plain wait with a later deadline reads the
// fire at 20 ms and goes on, and a datagram sent at 50 ms (another thread) ends it: the run stays 0. Its control, the
// same plain wait with no fire: the run is 1 - the count does count, so P's 0 is the fire's doing.
#define FIRE_ARMED_NS 20000000ULL // the deadline the first wait leaves armed
#define FIRE_PAST_US 30000U       // L: then this long, so it has passed before the next wait
#define FIRE_LATER_US 50000U      // P: the datagram this long after the wait began, after the fire

static uint32_t later_len;
static void* send_later(void* arg) {
    (void)arg;
    usleep(FIRE_LATER_US);
    (void)send_built(later_len, "fire, then data");
    return NULL;
}

// The timer armed by a wait that ends for a datagram, then the run set to `streak`; then (later: from another thread
// once the wait has read the fire) a datagram, and one wait for it. Returns whether the first wait armed the timer.
static bool fire_and_data(uint32_t len, uint8_t streak, bool later, bool* fired, int32_t* got) {
    node.hal.timer_data_streak = UINT8_MAX;
    (void)send_built(len, "fire and data, first");
    bool armed = receive_until(len, tt_get_ns() + FIRE_ARMED_NS) == (int32_t)len && node.hal.timer_armed_ns != 0;
    node.hal.timer_data_streak = streak;
    pthread_t thread; // NOLINT(misc-include-cleaner) - <pthread.h> is included; the tool maps pthread_t elsewhere
    bool threaded = false;
    if (later) {
        later_len = len;
        threaded = pthread_create(&thread, NULL, send_later, NULL) == 0;
        armed = armed && threaded;
    } else {
        usleep(FIRE_PAST_US);
        (void)send_built(len, "fire and data, second");
    }
    uint64_t fires = node.hal.timer_fires;
    *got = receive_until(len, tt_get_ns() + DEADLINE_FIRST_NS);
    *fired = node.hal.timer_fires == fires + 1U;
    if (threaded) {
        (void)pthread_join(thread, NULL);
    }
    return armed;
}

static void check_deadline_fire_and_data(void) {
    uint32_t len = build(framing, FRAMING + UNIT);
    // L
    bool l_fired = false;
    int32_t l_got = 0;
    bool l_armed = fire_and_data(len, UINT8_MAX, false, &l_fired, &l_got);
    uint8_t l_streak = node.hal.timer_data_streak;
    uint64_t waits = node.hal.timer_waits;
    uint64_t plain = node.hal.timer_plain_waits;
    (void)receive_until(0, tt_get_ns() + DEADLINE_SHORT_NS);
    bool l_next_plain = node.hal.timer_plain_waits == plain + 1U && node.hal.timer_waits == waits;
    bool l_ok = l_armed && l_got == (int32_t)len && l_fired && l_streak == 0 && l_next_plain;
    // PL
    bool pl_fired = false;
    int32_t pl_got = 0;
    plain = node.hal.timer_plain_waits;
    bool pl_armed = fire_and_data(len, 0, false, &pl_fired, &pl_got);
    uint8_t pl_streak = node.hal.timer_data_streak;
    bool pl_ok =
        pl_armed && pl_got == (int32_t)len && pl_fired && pl_streak == 0 && node.hal.timer_plain_waits == plain + 1U;
    // P
    bool p_fired = false;
    int32_t p_got = 0;
    plain = node.hal.timer_plain_waits;
    bool p_armed = fire_and_data(len, 0, true, &p_fired, &p_got);
    uint8_t p_streak = node.hal.timer_data_streak;
    bool p_ok =
        p_armed && p_got == (int32_t)len && p_fired && p_streak == 0 && node.hal.timer_plain_waits == plain + 1U;
    // The control: the same plain wait for a datagram sent from another thread, no timer armed.
    node.hal.timer_data_streak = 0;
    later_len = len;
    pthread_t thread; // NOLINT(misc-include-cleaner) - <pthread.h> is included; the tool maps pthread_t elsewhere
    bool c_threaded = pthread_create(&thread, NULL, send_later, NULL) == 0;
    uint64_t fires = node.hal.timer_fires;
    bool c_data = receive_until(len, tt_get_ns() + DEADLINE_FIRST_NS) == (int32_t)len;
    if (c_threaded) {
        (void)pthread_join(thread, NULL);
    }
    bool c_ok = c_threaded && c_data && node.hal.timer_fires == fires && node.hal.timer_data_streak == 1U;
    set_wait_until(0);
    char line[LINE_BYTES];
    (void)snprintf(line, sizeof(line),
                   "fire and data: at once, timer in use - read %d, fired %d, run %u, next plain %d; at once, plain - "
                   "read %d, fired %d, run %u; fire then data, plain - read %d, fired %d, run %u; control (no fire) - "
                   "run %u",
                   (int)l_got, l_fired ? 1 : 0, (unsigned)l_streak, l_next_plain ? 1 : 0, (int)pl_got, pl_fired ? 1 : 0,
                   (unsigned)pl_streak, (int)p_got, p_fired ? 1 : 0, (unsigned)p_streak,
                   (unsigned)node.hal.timer_data_streak);
    verdict(l_ok && pl_ok && p_ok && c_ok, line);
}

// 13. again
enum again_shape { AGAIN_SEND, AGAIN_SEND_TO, AGAIN_SEND_IOV };
#define AGAIN_HEAD 5U   // tt_send_iov()'s header part
#define AGAIN_BYTES 40U // the datagram
#define AGAIN_FILL 0x41U

static const char* again_name(enum again_shape shape) {
    return shape == AGAIN_SEND ? "tt_send()" : shape == AGAIN_SEND_TO ? "tt_send_to()" : "tt_send_iov()";
}

struct again_result {
    int32_t sent;
    int send_errno;
    int calls;
    bool arrived;
};

// One send of `shape` with the next `refused` calls refused with `err`: its result, its errno, the calls it made, and
// whether the datagram came whole to `sink` (tt_send() goes to the broadcast address, the context's own well-known
// socket: read with tt_try_receive()).
static struct again_result again_send(enum again_shape shape, int refused, int err, int sink, uint16_t sink_port) {
    static uint8_t out[AGAIN_BYTES];
    memset(out, AGAIN_FILL, sizeof(out));
    out[0] = (uint8_t)shape;
    struct again_result result = {.sent = -1, .send_errno = 0, .calls = 0, .arrived = false};
    refuse_sends = refused;
    refuse_errno = err;
    send_calls = 0;
    errno = 0;
    if (shape == AGAIN_SEND) {
        result.sent = tt_send(&node, out, sizeof(out));
    } else if (shape == AGAIN_SEND_TO) {
        result.sent = tt_send_to(&node, out, sizeof(out), INADDR_LOOPBACK, sink_port);
    } else {
        result.sent =
            tt_send_iov(&node, out, AGAIN_HEAD, out + AGAIN_HEAD, sizeof(out) - AGAIN_HEAD, INADDR_LOOPBACK, sink_port);
    }
    result.send_errno = errno;
    result.calls = send_calls;
    refuse_sends = 0;
    usleep(SETTLE_US);
    uint8_t in[2U * AGAIN_BYTES];
    int32_t got = -1;
    if (shape == AGAIN_SEND) {
        uint32_t ip = 0;
        uint16_t port = 0;
        while ((got = tt_try_receive(&node, in, sizeof(in), &ip, &port)) >= 0 && got != (int32_t)sizeof(out)) {
        }
    } else {
        got = (int32_t)recv(sink, in, sizeof(in), MSG_DONTWAIT);
    }
    result.arrived = got == (int32_t)sizeof(out) && memcmp(in, out, sizeof(out)) == 0;
    return result;
}

static void check_again(void) {
    int sink = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in at = {.sin_family = AF_INET, .sin_port = 0, .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
    socklen_t at_len = sizeof(at);
    if (sink < 0 || bind(sink, (struct sockaddr*)&at, sizeof(at)) != 0 ||
        getsockname(sink, (struct sockaddr*)&at, &at_len) != 0) {
        printf("rx_direct_check: SETUP FAILED - again: no sink socket\n");
        setup_failed = true;
        return;
    }
    const uint16_t port = ntohs(at.sin_port);
    while (tt_try_receive(&node, buf, sizeof(buf), &any_ip, &any_port) >= 0) {
    }
    for (int shape = AGAIN_SEND; shape <= AGAIN_SEND_IOV; shape++) {
        struct again_result again = again_send((enum again_shape)shape, 1, EAGAIN, sink, port);
        struct again_result other = again_send((enum again_shape)shape, 1, EPERM, sink, port);
        struct again_result plain = again_send((enum again_shape)shape, 0, 0, sink, port);
        char line[LINE_BYTES];
        (void)snprintf(line, sizeof(line),
                       "again: %s refused once (EAGAIN) returned %d after %d calls, arrived %d; refused (EPERM) %d "
                       "errno %d after %d calls, arrived %d; not refused %d after %d calls, arrived %d",
                       again_name((enum again_shape)shape), (int)again.sent, again.calls, again.arrived ? 1 : 0,
                       (int)other.sent, other.send_errno, other.calls, other.arrived ? 1 : 0, (int)plain.sent,
                       plain.calls, plain.arrived ? 1 : 0);
        verdict(again.sent == (int32_t)AGAIN_BYTES && again.calls == 2 && again.arrived && other.sent == -1 &&
                    other.send_errno == EPERM && other.calls == 1 && !other.arrived &&
                    plain.sent == (int32_t)AGAIN_BYTES && plain.calls == 1 && plain.arrived,
                line);
    }
    (void)close(sink);
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
    check_not_a_run();
    check_gro_in_place();
    check_gro_merged();
    check_shorter();
    check_misses();
    check_short_buffer();
    check_withdrawn();
    check_nonblocking();
    check_deadline();
    check_again();
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
