// Copyright (c) 2025-2026 TSN Lab, Inc.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3, as published by the Free
// Software Foundation. A proprietary license is also available on request - see README.md.

// The wait ring (hal_linux.c, "The wait ring") against the real Linux HAL, on loopback only. Run by test_samehost.sh,
// and by tests/mutants_wait_ring.py against one broken decision at a time.
//
// The ring is one thread's (IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN), the thread that waits in
// tt_receive() is not always the same, and the ring's recvmsg may still be in flight when tt_receive() returns. So:
//   1. first      - a new context's ring is made and not yet anyone's; the first thread to wait owns it.
//   2. owner      - the owner's datagram is read by the ring (ring_reads rises), with the sender's address; one sent to
//                   the well-known socket ends the owner's wait too, and is read after it.
//   3. other      - another thread's wait while the ring is owned reads with epoll and recvfrom(): the datagram
//   arrives,
//                   ring_others rises, the owner stays.
//   4. handover   - the context's first move: the first waiter stops waiting (as rmw's poll thread does when it hands
//                   the poll role to the executor) and another thread waits and reads - its first two datagrams come
//                   by epoll, and its next wait takes the ring and reads by it. The run counts datagrams read from the
//                   data socket, not waits (2026-10-29), and every wait here reads one.
//      move       - another thread whose waits read the run a move needs (wr_move_after: twice the last move's) takes
//                   the ring at its next wait, and reads by it (two shorter runs with a datagram the owner read between
//                   them do not); moving it back takes twice the run, not the same.
//      idle owner - (a new context) thread B reads one datagram a cycle and the owner A then waits twice with nothing
//                   to read, as a rmw ping's executor and its poll thread do: the ring is B's by the third cycle and
//                   reads B's datagrams. Until 2026-10-29 every wait of the owner broke B's run, so it never moved.
//      control    - (a new context) the same cycles with each of A's two waits reading a datagram too: the ring stays
//                   A's - the idle-owner check is not met by any alternation of the two threads.
//      well-known - (a new context) the idle-owner cycles with B's datagrams sent to the well-known socket: the ring
//                   stays A's - only what its recvmsg would have read (the data socket) counts towards a move.
//   5. wake       - tt_wake_signal() from another thread ends the owner's wait with no timeout (-3).
//   6. both       - a wake and a datagram ready in the same wait: the datagram is handed out (it was read), and the
//                   wake is reported by the next call - neither is lost.
//   7. deadline   - a wait with a timeout ends at it (not early, not 5 ms late); a long wait for a scheduler entry,
//                   which takes the deadline timer (the ring's multishot poll on it), ends at the entry.
//   8. bell       - the segment bell rung from another thread ends the owner's wait (0, the doorbell); after the bell
//                   is destroyed and made again, the new one is the one that rings; and one made on another thread
//                   while the owner sleeps (as a send can make it) rings it too, with no interrupt on the way.
//   9. stolen id  - the ring's owner gone and its id now another thread's (EEXIST from the kernel): the wait ends as
//                   interrupted, and the next one makes the ring that thread's.
//  10. alternate  - two threads take the poll role in turn, each for a random run of waits with short timeouts, two
//                   buffers each (a recvmsg cancelled for another buffer) and a drain after each datagram, while a
//                   third sends ALTERNATE_COUNT numbered datagrams: every one arrives exactly once and in order, and
//                   the run did what it is for - moves, cancels, reads by the ring and by epoll (else exit 3: void).
//  11. writable   - (large-sample builds) tt_tx_await_writable() from another thread ends the owner's wait.
//  12. refused    - io_uring refused (EPERM, as under seccomp), or only IORING_SETUP_DEFER_TASKRUN refused (EINVAL, as
//                   Linux < 6.1): no ring, and the datagram arrives; TT_RX_URING_WAIT=0: no ring.
// Exit 0 pass, 1 fail, 2 no ring here (io_uring compiled out, or this kernel refuses the ring), 3 setup failed or void.
//
// The check runs in one process, so every wait is bounded by an alarm: a mutant that never wakes fails (exit 1).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // NOLINT(bugprone-reserved-identifier, readability-identifier-naming) - RTLD_NEXT
#endif
#include <stdio.h>

#include <tickle/hal_linux.h> // tt_HAL_IO_URING, struct tt_hal

#if tt_HAL_IO_URING
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/io_uring.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/log.h> // TT_LOG_WARNING
#include <tickle/tickle.h>

#include "log.h" // tt_current_log_level: the contexts' own lines only at WARNING and above
#endif

enum check_exit { CHECK_PASS = 0, CHECK_FAIL = 1, CHECK_NO_RING = 2, CHECK_SETUP_FAILED = 3 };

#if tt_HAL_IO_URING
#define CHECK_PORT 47371
#define CHECK_CONTEXT_ID 61
#define MARK 0x5752C0DEU               // the first four bytes of every datagram this check sends
#define DATAGRAM_BYTES 16U             // mark, sequence number, padding
#define BUFFER_BYTES 2048U             // larger than anything sent here
#define NS_PER_MS 1000000ULL           // a millisecond
#define SHORT_WAIT_NS 100000LL         // 100 us: a wait that is expected to time out
#define ARRIVE_NS 1000000000LL         // how long a datagram sent may take to be received
#define PAUSE_US 20000U                // how long another thread waits before it acts on a sleeping owner
#define DEADLINE_NS 20000000LL         // check 7's timeout, 20 ms
#define ENTRY_NS 30000000LL            // check 7's scheduler entry, 30 ms ahead (more than TT_RX_TIMER_LONG_NS)
#define LATE_NS 5000000LL              // how late a deadline may be
#define ALARM_S 20U                    // any one check: a wait that never ends is a failure, not a hang
#define ALTERNATE_COUNT 20000U         // check 10's datagrams
#define ALTERNATE_RUN_MAX 200U         // the most waits one thread makes before it hands the role over
#define ALTERNATE_GAP_EVERY 4U         // check 10's sender pauses after this many datagrams
#define ALTERNATE_GAP_US 30U           // for this long
#define ALTERNATE_QUIET_NS 200000000LL // check 10 ends once nothing has arrived for this long after the sender ended
#define SYSCALL_ARGS 6                 // what syscall() passes on
#define NS_PER_S 1000000000ULL
#define BELL_PATH_BYTES 64
#define REFUSE_NONE 0
#define REFUSE_ALL 1   // io_uring_setup() refused: EPERM
#define REFUSE_DEFER 2 // io_uring_setup() with IORING_SETUP_DEFER_TASKRUN refused: EINVAL

static int refuse_mode = REFUSE_NONE;
static const char* check_name = "";

// io_uring_setup() goes through syscall() in the HAL (its hot io_uring_enter() is a raw instruction, never this).
long syscall(long number, ...) { // NOLINT(readability-inconsistent-declaration-parameter-name, cert-dcl37-c)
    va_list args;
    va_start(args, number);
    long arg[SYSCALL_ARGS];
    for (int i = 0; i < SYSCALL_ARGS; i++) {
        arg[i] = va_arg(args, long);
    }
    va_end(args);
    // NOLINTNEXTLINE(misc-include-cleaner) - __NR_io_uring_setup: <sys/syscall.h>, above
    if (number == __NR_io_uring_setup && refuse_mode != REFUSE_NONE) {
        // NOLINTNEXTLINE(performance-no-int-to-ptr) - io_uring_setup()'s second argument is its params pointer
        const struct io_uring_params* params = (const struct io_uring_params*)arg[1];
        if (refuse_mode == REFUSE_ALL) {
            errno = EPERM;
            return -1;
        }
        if ((params->flags & IORING_SETUP_DEFER_TASKRUN) != 0) {
            errno = EINVAL;
            return -1;
        }
    }
    long (*real)(long, ...) = (long (*)(long, ...))dlsym(RTLD_NEXT, "syscall");
    return real(number, arg[0], arg[1], arg[2], arg[3], arg[4], arg[5]);
}

static void on_alarm(int signo) {
    (void)signo;
    static const char said[] = "wr_check: FAIL - a wait did not end (alarm)\n";
    (void)!write(STDOUT_FILENO, said, sizeof(said) - 1U);
    _exit(CHECK_FAIL);
}

static uint64_t now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now); // NOLINT(misc-include-cleaner) - <time.h>
    return ((uint64_t)now.tv_sec * NS_PER_S) + (uint64_t)now.tv_nsec;
}

static int self_id(void) {
    return (int)syscall(SYS_gettid);
}

static bool failed = false;
static void fail(const char* why, long value) {
    printf("wr_check: FAIL [%s] %s (%ld)\n", check_name, why, value);
    failed = true;
}

static struct tt_Context ctx;
static int sender = -1;
static struct sockaddr_in sender_addr;

static tt_ret_t create(uint8_t uring_wait) {
    struct tt_InitOptions opts;
    tt_InitOptions_default(&opts);
    opts.net.broadcast = "127.255.255.255";
    opts.net.port = CHECK_PORT;
    opts.net.context_id = CHECK_CONTEXT_ID;
    opts.socket.rx_uring_wait = uring_wait;
    memset(&ctx, 0, sizeof(ctx));
    return tt_Context_create_with(&ctx, &opts);
}

// Datagram `seq` to the context's data socket, or to its well-known socket.
static void send_seq_to(uint32_t seq, bool well_known) {
    struct sockaddr_in dest;
    socklen_t dest_len = sizeof(dest);
    if (getsockname(well_known ? ctx.hal.sock : ctx.hal.data_sock, (struct sockaddr*)&dest, &dest_len) != 0) {
        fail("getsockname", errno);
        return;
    }
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    uint32_t datagram[DATAGRAM_BYTES / sizeof(uint32_t)] = {MARK, seq, 0, 0};
    if (sendto(sender, datagram, sizeof(datagram), 0, (struct sockaddr*)&dest, sizeof(dest)) !=
        (ssize_t)sizeof(datagram)) {
        fail("sendto", errno);
    }
}

static void send_seq(uint32_t seq) {
    send_seq_to(seq, false);
}

// One tt_receive() outcome, sorted: a datagram of this check's (its sequence number), or what else it was.
enum outcome { GOT_SEQ, GOT_OTHER, GOT_TIMEOUT, GOT_WAKE, GOT_BELL, GOT_ERROR };
static enum outcome receive(uint8_t* buf, int64_t timeout, uint32_t* seq, uint32_t* ip, uint16_t* port) {
    int32_t got = tt_receive(&ctx, buf, BUFFER_BYTES, ip, port, timeout);
    if (got == -1) {
        return GOT_TIMEOUT;
    }
    if (got == -3) {
        return GOT_WAKE;
    }
    if (got == 0) {
        return GOT_BELL;
    }
    if (got < 0) {
        return GOT_ERROR;
    }
    uint32_t words[2];
    memcpy(words, buf, sizeof(words));
    if ((size_t)got != DATAGRAM_BYTES || words[0] != MARK) {
        return GOT_OTHER;
    }
    *seq = words[1];
    return GOT_SEQ;
}

// Waits until datagram `want` arrives: true when it does, false on anything else that is not a timeout, a wake or
// another context's datagram - or when ARRIVE_NS passes.
static bool expect_seq(uint8_t* buf, uint32_t want) {
    const uint64_t give_up = now_ns() + (uint64_t)ARRIVE_NS;
    while (now_ns() < give_up) {
        uint32_t seq = 0;
        uint32_t ip = 0;
        uint16_t port = 0;
        enum outcome what = receive(buf, SHORT_WAIT_NS, &seq, &ip, &port);
        if (what == GOT_SEQ) {
            if (seq != want) {
                fail("a datagram out of order", (long)seq);
                return false;
            }
            if (ip != INADDR_LOOPBACK || port != ntohs(sender_addr.sin_port)) {
                fail("the sender's address is wrong", (long)port);
                return false;
            }
            return true;
        }
        if (what == GOT_ERROR) {
            fail("tt_receive() failed", 0);
            return false;
        }
    }
    fail("a datagram did not arrive", (long)want);
    return false;
}

static uint32_t next_seq = 0;

// ---- 1-3

static void* other_receives(void* arg) {
    (void)arg;
    static uint8_t buf[BUFFER_BYTES];
    send_seq(next_seq);
    (void)expect_seq(buf, next_seq++);
    return NULL;
}

static void check_first_owner_other(void) {
    check_name = "first";
    static uint8_t buf[BUFFER_BYTES];
    if (ctx.hal.wr_fd_plus1 <= 0 || ctx.hal.wr_owner != 0) {
        fail("a new context's ring is not made, or already someone's", ctx.hal.wr_fd_plus1);
        return;
    }
    uint32_t seq = 0;
    uint32_t ip = 0;
    uint16_t port = 0;
    (void)receive(buf, SHORT_WAIT_NS, &seq, &ip, &port);
    if (ctx.hal.wr_owner != self_id()) {
        fail("the first waiter does not own the ring", ctx.hal.wr_owner);
    }
    check_name = "owner";
    const uint64_t reads = ctx.hal.wr_reads;
    send_seq(next_seq);
    (void)expect_seq(buf, next_seq++);
    if (ctx.hal.wr_reads != reads + 1U) {
        fail("the owner's datagram was not read by the ring", (long)(ctx.hal.wr_reads - reads));
    }
    // The well-known socket (broadcasts): the ring polls it, and the read after the wait takes the datagram.
    const uint64_t waits = ctx.hal.wr_waits;
    send_seq_to(next_seq, true);
    (void)expect_seq(buf, next_seq++);
    if (ctx.hal.wr_waits == waits) {
        fail("the owner did not wait on the ring for the well-known socket", 0);
    }
    check_name = "other";
    const uint64_t others = ctx.hal.wr_others;
    const int owner = ctx.hal.wr_owner;
    pthread_t thread; // NOLINT(misc-include-cleaner) - <pthread.h> is included; the tool maps pthread_t elsewhere
    pthread_create(&thread, NULL, other_receives, NULL);
    pthread_join(thread, NULL);
    if (ctx.hal.wr_owner != owner || ctx.hal.wr_others <= others || ctx.hal.wr_reads != reads + 1U) {
        fail("another thread's wait did not take the epoll path", (long)(ctx.hal.wr_others - others));
    }
}

// ---- 4

#define HANDOVER_WAITS 2U // the datagrams the first move needs (TT_RX_RING_MOVE_WAITS, hal_linux.h)

// A wait that reads one datagram sent to the data socket: the unit a move's run counts.
static void receive_one(uint8_t* buf) {
    send_seq(next_seq);
    (void)expect_seq(buf, next_seq++);
}

static uint64_t handover_moves = 0;
static uint64_t handover_others = 0;
static int handover_id = 0;
static void* handover_waits(void* arg) {
    (void)arg;
    static uint8_t buf[BUFFER_BYTES];
    handover_id = self_id();
    const uint64_t moves = ctx.hal.wr_moves;
    for (uint32_t wait = 0; wait < HANDOVER_WAITS; wait++) {
        receive_one(buf);
    }
    if (ctx.hal.wr_moves != moves) {
        fail("the ring moved before the run was read", (long)(ctx.hal.wr_moves - moves));
    }
    handover_others = ctx.hal.wr_others;
    const uint64_t reads = ctx.hal.wr_reads;
    receive_one(buf);
    handover_moves = ctx.hal.wr_moves;
    if (ctx.hal.wr_owner == handover_id && ctx.hal.wr_reads != reads + 1U) {
        fail("the thread the ring moved to did not read by it", (long)(ctx.hal.wr_reads - reads));
    }
    return NULL;
}

static void check_handover(void) {
    check_name = "handover";
    if (ctx.hal.wr_moves != 0) {
        fail("not the context's first move", (long)ctx.hal.wr_moves);
        return;
    }
    const uint64_t others = ctx.hal.wr_others;
    pthread_t thread; // NOLINT(misc-include-cleaner) - <pthread.h> is included; the tool maps pthread_t elsewhere
    pthread_create(&thread, NULL, handover_waits, NULL);
    pthread_join(thread, NULL);
    if (handover_moves != 1U || ctx.hal.wr_owner != handover_id) {
        fail("the ring did not move at the wait after two datagrams of a thread that took over from the first waiter",
             (long)handover_moves);
    }
    if (handover_others - others != HANDOVER_WAITS) {
        fail("the waits before the move were not on the epoll path, or not the run expected",
             (long)(handover_others - others));
    }
    // Back to this thread, for the checks after: the next move needs twice the run.
    static uint8_t buf[BUFFER_BYTES];
    for (uint32_t wait = 0; wait <= 2U * HANDOVER_WAITS; wait++) {
        // After the wait a run as long as the last move's would have moved it at (the one after its datagrams).
        if (wait == HANDOVER_WAITS + 1U && ctx.hal.wr_owner != handover_id) {
            fail("the ring moved back without twice the run", (long)wait);
        }
        receive_one(buf);
    }
    if (ctx.hal.wr_owner != self_id() || ctx.hal.wr_moves != 2U) {
        fail("the ring did not move back after twice the run", ctx.hal.wr_owner);
    }
}

static uint32_t move_need = 0; // the run the next move needs, when check_move() starts
static int mover_id = 0;
static void* mover(void* arg) {
    (void)arg;
    static uint8_t buf[BUFFER_BYTES];
    mover_id = self_id();
    for (uint32_t wait = 0; wait < move_need; wait++) {
        receive_one(buf);
    }
    const uint64_t reads = ctx.hal.wr_reads;
    send_seq(next_seq);
    (void)expect_seq(buf, next_seq++);
    if (ctx.hal.wr_reads != reads + 1U) {
        fail("the ring's new owner did not read by it", (long)(ctx.hal.wr_reads - reads));
    }
    return NULL;
}

// Short runs: move_need - 1 datagrams, then the owner reads one, then as many again - never a run long enough.
static pthread_barrier_t halves; // NOLINT(misc-include-cleaner) - <pthread.h> is included
static void short_waits(void) {
    static uint8_t buf[BUFFER_BYTES];
    for (uint32_t wait = 0; wait + 1U < move_need; wait++) {
        receive_one(buf);
    }
}
static void* short_runs(void* arg) {
    (void)arg;
    short_waits();
    pthread_barrier_wait(&halves);
    pthread_barrier_wait(&halves); // the owner's datagram in between
    short_waits();
    return NULL;
}

static void check_move(void) {
    check_name = "move";
    move_need = ctx.hal.wr_move_after;
    if (move_need < 2U) {
        fail("a move needs a run of fewer than two waits", (long)move_need);
        return;
    }
    const uint64_t moves = ctx.hal.wr_moves;
    const int owner = ctx.hal.wr_owner;
    pthread_barrier_init(&halves, NULL, 2);
    pthread_t thread; // NOLINT(misc-include-cleaner) - <pthread.h> is included; the tool maps pthread_t elsewhere
    pthread_create(&thread, NULL, short_runs, NULL);
    pthread_barrier_wait(&halves);
    static uint8_t between[BUFFER_BYTES];
    receive_one(between);
    pthread_barrier_wait(&halves);
    pthread_join(thread, NULL);
    pthread_barrier_destroy(&halves);
    if (ctx.hal.wr_owner != owner || ctx.hal.wr_moves != moves) {
        fail("the ring moved on a run the owner's datagram had broken", (long)(ctx.hal.wr_moves - moves));
    }
    pthread_create(&thread, NULL, mover, NULL);
    pthread_join(thread, NULL);
    if (ctx.hal.wr_owner != mover_id || ctx.hal.wr_moves != moves + 1U) {
        fail("the ring did not move to the thread that kept reading", (long)(ctx.hal.wr_moves - moves));
    }
    // Back to this thread for the checks below: not after as long a run, only after twice as long.
    static uint8_t buf[BUFFER_BYTES];
    for (uint32_t wait = 0; wait <= 2U * move_need; wait++) {
        if (wait == move_need + 1U && ctx.hal.wr_owner != mover_id) { // as in check_handover()
            fail("the ring moved back without twice the run", (long)wait);
        }
        receive_one(buf);
    }
    if (ctx.hal.wr_owner != self_id()) {
        fail("the ring did not move back after twice the run", ctx.hal.wr_owner);
    }
}

// Idle owner and its controls, each on a new context (the first move's run, TT_RX_RING_MOVE_WAITS): IDLE_CYCLES of
// [B reads one datagram; A, the owner, waits twice] - A's waits idle, or (control) each reading a datagram too, or
// (well-known) A's waits idle and B's datagrams sent to the well-known socket, which the ring's recvmsg never reads.
enum idle_mode { IDLE_OWNER, IDLE_CONTROL, IDLE_WELL_KNOWN };
static enum idle_mode idle_mode = IDLE_OWNER;
#define IDLE_CYCLES 8U
#define IDLE_OWNER_WAITS 2U
#define IDLE_MOVED_BY 3U // the cycle by which B owns the ring: two datagrams by epoll, the third wait moves it
static pthread_barrier_t cycle_step; // NOLINT(misc-include-cleaner) - <pthread.h> is included
static int reader_id = 0;
static void* cycle_reader(void* arg) {
    (void)arg;
    static uint8_t buf[BUFFER_BYTES];
    reader_id = self_id();
    for (uint32_t cycle = 0; cycle < IDLE_CYCLES; cycle++) {
        pthread_barrier_wait(&cycle_step);
        if (idle_mode == IDLE_WELL_KNOWN) {
            send_seq_to(next_seq, true);
            (void)expect_seq(buf, next_seq++);
        } else {
            receive_one(buf);
        }
        pthread_barrier_wait(&cycle_step);
    }
    return NULL;
}

// One cycle's reading, after B's datagram: the controls keep the ring with A; the idle owner loses it by IDLE_MOVED_BY.
static void check_idle_cycle(enum idle_mode mode, int owner, uint32_t cycle, uint64_t reads) {
    if (mode != IDLE_OWNER) {
        if (ctx.hal.wr_owner != owner || ctx.hal.wr_moves != 0) {
            fail(mode == IDLE_CONTROL ? "the ring left an owner that reads as much as the other thread (cycle)"
                                      : "the ring moved for datagrams read from the well-known socket (cycle)",
                 (long)cycle);
        }
        return;
    }
    if (cycle < IDLE_MOVED_BY) {
        return;
    }
    if (ctx.hal.wr_owner != reader_id || ctx.hal.wr_moves != 1U) {
        fail("the ring did not follow the thread that reads while the owner waits idle (cycle)", (long)cycle);
    }
    if (ctx.hal.wr_reads != reads + 1U) {
        fail("the reader's datagram was not read by the ring (cycle)", (long)cycle);
    }
}

// A's IDLE_OWNER_WAITS waits of one cycle: each reads a datagram (control) or times out.
static void idle_owner_waits(bool owner_reads, uint32_t cycle) {
    static uint8_t buf[BUFFER_BYTES];
    uint32_t seq = 0;
    uint32_t ip = 0;
    uint16_t port = 0;
    for (uint32_t wait = 0; wait < IDLE_OWNER_WAITS; wait++) {
        if (owner_reads) {
            receive_one(buf);
        } else if (receive(buf, SHORT_WAIT_NS, &seq, &ip, &port) != GOT_TIMEOUT) {
            fail("the owner's idle wait did not time out (cycle)", (long)cycle);
        }
    }
}

static void check_idle_owner(enum idle_mode mode) {
    static const char* const names[] = {"idle owner", "control", "well-known"};
    check_name = names[mode];
    idle_mode = mode;
    const bool owner_reads = mode == IDLE_CONTROL;
    static uint8_t buf[BUFFER_BYTES];
    if (create(tt_SWITCH_ON) != tt_RET_OK) {
        fail("cannot create a context", 0);
        return;
    }
    uint32_t seq = 0;
    uint32_t ip = 0;
    uint16_t port = 0;
    (void)receive(buf, SHORT_WAIT_NS, &seq, &ip, &port);
    const int owner = self_id();
    if (ctx.hal.wr_owner != owner || ctx.hal.wr_move_after != HANDOVER_WAITS) {
        fail("the first waiter does not own a new ring, or its first move needs another run",
             (long)ctx.hal.wr_move_after);
    }
    pthread_barrier_init(&cycle_step, NULL, 2);
    pthread_t thread; // NOLINT(misc-include-cleaner) - <pthread.h> is included; the tool maps pthread_t elsewhere
    pthread_create(&thread, NULL, cycle_reader, NULL);
    for (uint32_t cycle = 1; cycle <= IDLE_CYCLES; cycle++) {
        const uint64_t reads = ctx.hal.wr_reads;
        pthread_barrier_wait(&cycle_step);
        pthread_barrier_wait(&cycle_step); // B has read its datagram
        check_idle_cycle(mode, owner, cycle, reads);
        idle_owner_waits(owner_reads, cycle);
    }
    pthread_join(thread, NULL);
    pthread_barrier_destroy(&cycle_step);
    printf("wr_check: [%s] ring_moves=%lu ring_reads=%lu ring_others=%lu\n", check_name,
           (unsigned long)ctx.hal.wr_moves, (unsigned long)ctx.hal.wr_reads, (unsigned long)ctx.hal.wr_others);
    (void)tt_Context_destroy(&ctx);
}

// ---- 5, 6

static void* wake_later(void* arg) {
    (void)arg;
    usleep(PAUSE_US);
    (void)tt_wake_signal(&ctx);
    return NULL;
}

static void check_wake(void) {
    check_name = "wake";
    static uint8_t buf[BUFFER_BYTES];
    const uint64_t waits = ctx.hal.wr_waits;
    pthread_t thread; // NOLINT(misc-include-cleaner) - <pthread.h> is included; the tool maps pthread_t elsewhere
    pthread_create(&thread, NULL, wake_later, NULL);
    uint32_t seq = 0;
    uint32_t ip = 0;
    uint16_t port = 0;
    enum outcome what = receive(buf, 0, &seq, &ip, &port);
    pthread_join(thread, NULL);
    if (what != GOT_WAKE || ctx.hal.wr_waits == waits) {
        fail("the owner's wait on the ring did not end at the wake", (long)what);
    }

    check_name = "both";
    (void)receive(buf, SHORT_WAIT_NS, &seq, &ip, &port); // the recvmsg in flight, into buf
    (void)tt_wake_signal(&ctx);
    send_seq(next_seq);
    usleep(PAUSE_US);
    bool woke = false;
    bool arrived = false;
    for (int call = 0; call < 3 && !(woke && arrived); call++) {
        what = receive(buf, SHORT_WAIT_NS, &seq, &ip, &port);
        woke = woke || what == GOT_WAKE;
        if (what == GOT_SEQ) {
            arrived = seq == next_seq;
        }
    }
    next_seq++;
    if (!woke || !arrived) {
        fail(woke ? "the datagram ready with a wake was lost" : "the wake ready with a datagram was lost", 0);
    }
}

// ---- 7

static void check_deadline(void) {
    check_name = "deadline";
    static uint8_t buf[BUFFER_BYTES];
    uint32_t seq = 0;
    uint32_t ip = 0;
    uint16_t port = 0;
    const uint64_t waits = ctx.hal.wr_waits;
    uint64_t start = now_ns();
    enum outcome what = receive(buf, DEADLINE_NS, &seq, &ip, &port);
    uint64_t took = now_ns() - start;
    if (what != GOT_TIMEOUT || took < (uint64_t)DEADLINE_NS || took > (uint64_t)(DEADLINE_NS + LATE_NS) ||
        ctx.hal.wr_waits == waits) {
        fail("a wait with a timeout did not end at it (ns)", (long)took);
    }
    // A long wait for a scheduler entry: the deadline timer in the set, as poll_wait_io() asks for it.
    const uint64_t fires = ctx.hal.timer_fires;
    start = now_ns();
    const uint64_t until = start + (uint64_t)ENTRY_NS;
    __atomic_store_n(&ctx.wait_until_hi, (uint32_t)(until >> 32U), __ATOMIC_RELAXED);
    __atomic_store_n(&ctx.wait_until_lo, (uint32_t)until, __ATOMIC_RELAXED);
    ctx.wait_until_entry = true;
    what = receive(buf, ENTRY_NS, &seq, &ip, &port);
    took = now_ns() - start;
    ctx.wait_until_entry = false;
    __atomic_store_n(&ctx.wait_until_hi, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ctx.wait_until_lo, 0, __ATOMIC_RELAXED);
    if (ctx.hal.timer_fd_plus1 == 0) {
        printf("wr_check: NOTE [deadline] no deadline timer here (TT_RX_LAZY_TIMER=0): only the timeout was checked\n");
        return;
    }
    if (what != GOT_TIMEOUT || took < (uint64_t)ENTRY_NS || took > (uint64_t)(ENTRY_NS + LATE_NS) ||
        ctx.hal.timer_fires == fires) {
        fail("a long wait did not end at its entry by the timer (ns)", (long)took);
    }
}

// ---- 8

#if tt_SEGMENT_ENABLED
static char bell_path[BELL_PATH_BYTES];
static bool bell_made_by_ringer = false; // ring_later() also makes the bell first, while the owner sleeps

static void* ring_later(void* arg) {
    (void)arg;
    usleep(PAUSE_US);
    if (bell_made_by_ringer && tt_segment_bell_create(&ctx, bell_path) != 0) {
        fail("cannot create the bell on another thread", errno);
        return NULL;
    }
    usleep(PAUSE_US);
    int32_t bell = tt_segment_bell_open(bell_path);
    if (bell < 0) {
        fail("cannot open the bell", errno);
        return NULL;
    }
    tt_segment_bell_ring(bell);
    tt_segment_bell_close(bell);
    return NULL;
}

// The owner waits with no timeout until the bell rings: true when that is what ended the wait (no interrupt on the
// way: a bell made while it sleeps nudges the ring, it does not wake the poll).
static bool rung(bool made_by_ringer) {
    static uint8_t buf[BUFFER_BYTES];
    const uint64_t waits = ctx.hal.wr_waits;
    bell_made_by_ringer = made_by_ringer;
    pthread_t thread; // NOLINT(misc-include-cleaner) - <pthread.h> is included; the tool maps pthread_t elsewhere
    pthread_create(&thread, NULL, ring_later, NULL);
    uint32_t seq = 0;
    uint32_t ip = 0;
    uint16_t port = 0;
    enum outcome what = receive(buf, 0, &seq, &ip, &port);
    pthread_join(thread, NULL);
    return what == GOT_BELL && ctx.hal.wr_waits != waits;
}
#endif

static void check_bell(void) {
#if tt_SEGMENT_ENABLED
    check_name = "bell";
    if (ctx.hal.bell_fd_plus1 > 0) {
        printf("wr_check: NOTE [bell] the context made its own bell; checking a second one is not possible here\n");
        return;
    }
    (void)snprintf(bell_path, sizeof(bell_path), "/tmp/tt_wr_check_bell_%d", (int)getpid());
    if (tt_segment_bell_create(&ctx, bell_path) != 0) {
        fail("cannot create a bell", errno);
        return;
    }
    if (!rung(false)) {
        fail("the bell rung did not end the owner's wait", 0);
    }
    tt_segment_bell_destroy(&ctx, bell_path);
    if (tt_segment_bell_create(&ctx, bell_path) != 0) {
        fail("cannot create the bell again", errno);
        return;
    }
    if (!rung(false)) {
        fail("the bell made again did not end the owner's wait", 0);
    }
    tt_segment_bell_destroy(&ctx, bell_path);
    if (!rung(true)) {
        fail("a bell made on another thread while the owner slept did not end its wait", 0);
    }
    tt_segment_bell_destroy(&ctx, bell_path);
#endif
}

// ---- 9

static void* stolen(void* arg) {
    (void)arg;
    static uint8_t buf[BUFFER_BYTES];
    ctx.hal.wr_owner = self_id(); // as if the owner had exited and its id were this thread's now
    uint32_t seq = 0;
    uint32_t ip = 0;
    uint16_t port = 0;
    enum outcome what = receive(buf, SHORT_WAIT_NS, &seq, &ip, &port);
    if (what == GOT_ERROR) {
        fail("a ring that is another thread's made the wait fail", 0);
        return NULL;
    }
    const uint64_t reads = ctx.hal.wr_reads;
    send_seq(next_seq);
    (void)expect_seq(buf, next_seq++);
    if (ctx.hal.wr_owner != self_id() || ctx.hal.wr_reads != reads + 1U) {
        fail("the ring was not made this thread's", ctx.hal.wr_owner);
    }
    return NULL;
}

static void check_stolen_id(void) {
    check_name = "stolen id";
    pthread_t thread; // NOLINT(misc-include-cleaner) - <pthread.h> is included; the tool maps pthread_t elsewhere
    pthread_create(&thread, NULL, stolen, NULL);
    pthread_join(thread, NULL);
}

// ---- 10

// The poll role, handed from one receiver to the other at the end of each run (`turn`), as rmw hands it between its
// poll thread and an executor thread.
static pthread_mutex_t role = PTHREAD_MUTEX_INITIALIZER;      // NOLINT(misc-include-cleaner) - <pthread.h>
static pthread_cond_t role_handed = PTHREAD_COND_INITIALIZER; // NOLINT(misc-include-cleaner) - <pthread.h>
static uintptr_t turn = 0;
static uint32_t* arrivals = NULL;
static uint32_t arrived = 0;
static volatile bool sending_done = false;
static volatile bool receivers_stop = false;
static uint64_t last_arrival_ns = 0;

static void note(uint32_t seq) {
    if (arrived < ALTERNATE_COUNT + 16U) {
        arrivals[arrived] = seq;
    }
    arrived++;
    last_arrival_ns = now_ns();
}

static void* alternate_sender(void* arg) {
    (void)arg;
    for (uint32_t seq = 0; seq < ALTERNATE_COUNT; seq++) {
        send_seq(seq);
        if (seq % ALTERNATE_GAP_EVERY == 0) {
            usleep(ALTERNATE_GAP_US);
        }
    }
    sending_done = true;
    return NULL;
}

// The drain core makes after a wake-up's datagram, `drains` reads of it at most.
static void alternate_drain(uint8_t* buf, int drains) {
    for (int read = 0; read < drains; read++) {
        uint32_t ip = 0;
        uint16_t port = 0;
        int32_t got = tt_try_receive(&ctx, buf, BUFFER_BYTES, &ip, &port);
        if (got < 0) {
            return;
        }
        uint32_t words[2];
        memcpy(words, buf, sizeof(words));
        if ((size_t)got == DATAGRAM_BYTES && words[0] == MARK) {
            note(words[1]);
        }
    }
}

// One run of waits while this thread holds the role: `run` of them, each into one of its two buffers at random, with a
// random timeout and a random part of the drain after a datagram.
static void alternate_run(uint8_t* const mine[2], unsigned int* random_state, uint32_t run) {
    for (uint32_t wait = 0; wait < run && !receivers_stop; wait++) {
        uint8_t* buf = mine[rand_r(random_state) % 2];
        uint32_t seq = 0;
        uint32_t ip = 0;
        uint16_t port = 0;
        const int64_t timeout = 20000LL + ((int64_t)rand_r(random_state) % 500000LL);
        enum outcome what = receive(buf, timeout, &seq, &ip, &port);
        if (what == GOT_ERROR) {
            fail("tt_receive() failed", 0);
            receivers_stop = true;
        }
        if (what != GOT_SEQ) {
            if (sending_done && now_ns() - last_arrival_ns > (uint64_t)ALTERNATE_QUIET_NS) {
                receivers_stop = true;
            }
            continue;
        }
        note(seq);
        alternate_drain(buf, rand_r(random_state) % 4);
    }
}

static void* alternate_receiver(void* arg) {
    const uintptr_t self = *(const uintptr_t*)arg;
    unsigned int random_state = (unsigned int)self;
    static uint8_t buffers[4][BUFFER_BYTES];
    uint8_t* const mine[2] = {buffers[2U * self], buffers[(2U * self) + 1U]};
    for (;;) {
        pthread_mutex_lock(&role);
        while (turn != self && !receivers_stop) {
            pthread_cond_wait(&role_handed, &role);
        }
        if (receivers_stop) {
            pthread_cond_broadcast(&role_handed);
            pthread_mutex_unlock(&role);
            return NULL;
        }
        pthread_mutex_unlock(&role); // the role is this thread's until it hands it over
        alternate_run(mine, &random_state, 1U + ((uint32_t)rand_r(&random_state) % ALTERNATE_RUN_MAX));
        pthread_mutex_lock(&role);
        turn = 1U - self;
        pthread_cond_broadcast(&role_handed);
        pthread_mutex_unlock(&role);
    }
}

static enum check_exit check_alternate(void) {
    check_name = "alternate";
    arrivals = calloc(ALTERNATE_COUNT + 16U, sizeof(uint32_t));
    if (arrivals == NULL) {
        return CHECK_SETUP_FAILED;
    }
    const uint32_t drops = tt_hal_rx_socket_drops(&ctx);
    const uint64_t moves = ctx.hal.wr_moves;
    const uint64_t reads = ctx.hal.wr_reads;
    const uint64_t others = ctx.hal.wr_others;
    const uint64_t cancels = ctx.hal.wr_cancels;
    last_arrival_ns = now_ns();
    pthread_t receivers[2]; // NOLINT(misc-include-cleaner) - <pthread.h> is included; the tool maps pthread_t elsewhere
    pthread_t sending;      // NOLINT(misc-include-cleaner) - <pthread.h> is included; the tool maps pthread_t elsewhere
    static uintptr_t ids[2] = {0, 1};
    pthread_create(&receivers[0], NULL, alternate_receiver, &ids[0]);
    pthread_create(&receivers[1], NULL, alternate_receiver, &ids[1]);
    pthread_create(&sending, NULL, alternate_sender, NULL);
    pthread_join(sending, NULL);
    pthread_join(receivers[0], NULL);
    pthread_join(receivers[1], NULL);
    uint32_t in_order = 0;
    for (uint32_t i = 0; i < arrived && i < ALTERNATE_COUNT; i++) {
        if (arrivals[i] != i) {
            fail("a datagram lost, duplicated or out of order at index", (long)i);
            printf("wr_check: [alternate] index %u holds %u\n", i, arrivals[i]);
            break;
        }
        in_order++;
    }
    free(arrivals);
    const uint32_t dropped = tt_hal_rx_socket_drops(&ctx) - drops;
    printf("wr_check: [alternate] arrived=%u of %u in_order=%u socket_drops=%u ring_moves=%lu ring_reads=%lu "
           "ring_others=%lu ring_cancels=%lu\n",
           arrived, ALTERNATE_COUNT, in_order, dropped, (unsigned long)(ctx.hal.wr_moves - moves),
           (unsigned long)(ctx.hal.wr_reads - reads), (unsigned long)(ctx.hal.wr_others - others),
           (unsigned long)(ctx.hal.wr_cancels - cancels));
    if (dropped != 0) {
        printf("wr_check: VOID [alternate] the socket dropped %u datagrams: what was lost cannot be told apart\n",
               dropped);
        return CHECK_SETUP_FAILED;
    }
    if (arrived != ALTERNATE_COUNT) {
        fail("datagrams arrived, not ALTERNATE_COUNT", (long)arrived);
    }
    if (ctx.hal.wr_moves - moves < 2U || ctx.hal.wr_reads == reads || ctx.hal.wr_others == others ||
        ctx.hal.wr_cancels == cancels) {
        printf("wr_check: VOID [alternate] the run did not move the ring twice, read by both paths and cancel\n");
        return CHECK_SETUP_FAILED;
    }
    return CHECK_PASS;
}

// ---- 11

#if tt_LARGE_SAMPLES
static void* await_later(void* arg) {
    (void)arg;
    usleep(PAUSE_US);
    if (!tt_tx_await_writable(&ctx)) {
        fail("tt_tx_await_writable() refused", 0);
    }
    return NULL;
}
#endif

static void check_writable(void) {
#if tt_LARGE_SAMPLES
    check_name = "writable";
    static uint8_t buf[BUFFER_BYTES];
    ctx.large.tx_writable = false;
    const uint64_t waits = ctx.hal.wr_waits;
    for (int round = 0; round < 2; round++) { // the watch added, then armed again
        pthread_t thread; // NOLINT(misc-include-cleaner) - <pthread.h> is included; the tool maps pthread_t elsewhere
        pthread_create(&thread, NULL, await_later, NULL);
        uint32_t seq = 0;
        uint32_t ip = 0;
        uint16_t port = 0;
        enum outcome what = receive(buf, 0, &seq, &ip, &port);
        pthread_join(thread, NULL);
        if (what != GOT_TIMEOUT || !ctx.large.tx_writable) {
            fail("the owner's wait did not end when the socket had room (round x 10 + outcome)", (round * 10) + what);
        }
        ctx.large.tx_writable = false;
    }
    if (ctx.hal.wr_waits == waits) {
        fail("the owner did not wait on the ring", 0);
    }
#endif
}

// ---- 12

static void check_refused(int mode, const char* name) {
    check_name = name;
    refuse_mode = mode;
    static uint8_t buf[BUFFER_BYTES];
    if (create(tt_SWITCH_ON) != tt_RET_OK) {
        fail("a context with the ring refused was not created", 0);
        refuse_mode = REFUSE_NONE;
        return;
    }
    refuse_mode = REFUSE_NONE;
    if (ctx.hal.wr_fd_plus1 != -1 || (mode == REFUSE_DEFER && ctx.hal.uring_fd < 0)) {
        fail("the refusal did not leave the context without the ring (and only without it)", ctx.hal.wr_fd_plus1);
    }
    send_seq(next_seq);
    (void)expect_seq(buf, next_seq++);
    if (ctx.hal.wr_reads != 0) {
        fail("a refused ring read", (long)ctx.hal.wr_reads);
    }
    (void)tt_Context_destroy(&ctx);
}

static void check_switch_off(void) {
    check_name = "switch";
    (void)setenv("TT_RX_URING_WAIT", "0", 1);
    if (create(tt_SWITCH_ON) == tt_RET_OK) {
        if (ctx.hal.wr_fd_plus1 != 0) {
            fail("TT_RX_URING_WAIT=0 left a ring", ctx.hal.wr_fd_plus1);
        }
        (void)tt_Context_destroy(&ctx);
    }
    (void)unsetenv("TT_RX_URING_WAIT");
    if (create(tt_SWITCH_OFF) == tt_RET_OK) {
        if (ctx.hal.wr_fd_plus1 != 0) {
            fail("rx_uring_wait off left a ring", ctx.hal.wr_fd_plus1);
        }
        (void)tt_Context_destroy(&ctx);
    }
}
#endif

int main(void) {
#if !tt_HAL_IO_URING
    printf("wr_check: SKIP - io_uring compiled out (tt_HAL_RX_HINT=tt_RX_HINT_READ)\n");
    return CHECK_NO_RING;
#else
    (void)setenv("TICKLE_ID_REGISTRY", "off", 1);
    tt_current_log_level = TT_LOG_WARNING;
    (void)unsetenv("TT_RX_URING_WAIT");
    signal(SIGALRM, on_alarm);
    sender = socket(AF_INET, SOCK_DGRAM, 0);
    sender_addr.sin_family = AF_INET;
    sender_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sender_addr.sin_port = 0;
    socklen_t sender_len = sizeof(sender_addr);
    if (sender < 0 || bind(sender, (struct sockaddr*)&sender_addr, sizeof(sender_addr)) != 0 ||
        getsockname(sender, (struct sockaddr*)&sender_addr, &sender_len) != 0) {
        printf("wr_check: SETUP FAILED - no sender socket\n");
        return CHECK_SETUP_FAILED;
    }
    if (create(tt_SWITCH_ON) != tt_RET_OK) {
        printf("wr_check: SETUP FAILED - cannot create a context\n");
        return CHECK_SETUP_FAILED;
    }
    if (ctx.hal.wr_fd_plus1 == -1) {
        printf("wr_check: SKIP - this kernel refuses the wait ring (io_uring, or IORING_SETUP_DEFER_TASKRUN)\n");
        (void)tt_Context_destroy(&ctx);
        return CHECK_NO_RING;
    }
    alarm(ALARM_S);
    check_first_owner_other();
    check_handover();
    check_move();
    check_wake();
    check_deadline();
    check_bell();
    check_writable();
    check_stolen_id();
    alarm(ALARM_S * 3U);
    enum check_exit alternate = check_alternate();
    alarm(ALARM_S);
    (void)tt_Context_destroy(&ctx);
    check_idle_owner(IDLE_OWNER);
    check_idle_owner(IDLE_CONTROL);
    check_idle_owner(IDLE_WELL_KNOWN);
    check_refused(REFUSE_ALL, "refused io_uring");
    check_refused(REFUSE_DEFER, "refused DEFER_TASKRUN");
    check_switch_off();
    alarm(0);
    if (failed) {
        return CHECK_FAIL;
    }
    if (alternate != CHECK_PASS) {
        return alternate;
    }
    printf("wr_check: PASS - first, owner, other, handover, move, idle owner, control, well-known, wake, both, "
           "deadline, bell, "
           "stolen id, alternate%s, "
           "refused, "
           "switch\n",
           tt_LARGE_SAMPLES ? ", writable" : "");
    return CHECK_PASS;
#endif
}
