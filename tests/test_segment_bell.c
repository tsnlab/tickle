/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// The segment doorbell on the REAL Linux HAL: ringing a peer whose owner has gone must not kill the ringer, and a
// ring stays one write().
//
// The bell is a FIFO. Its owner holds the read end; a writer opens its own end once and keeps it. When the owner
// exits, a write-only end is left with no reader, and write() on a FIFO with no reader fails with EPIPE AND raises
// SIGPIPE, whose default action ends the process. Found 2026-10-07 chasing a CI flake: on this PC a default
// rclcpp::Node publisher died with status 141 in 100 of 120 runs of check_ros2_interfaces.sh, every time a
// same-host subscriber exited first (its segment's reader_waiting flag stays set, so the publisher rings it). CI
// never showed it because the GitHub runner starts job steps with SIGPIPE ignored.
//
// Each case runs in a child, because the failure being tested is the death of the process. The syscall count is
// taken by the parent tracing the child (PTRACE_GET_SYSCALL_INFO, Linux 5.3), between two getppid() markers.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // NOLINT(bugprone-reserved-identifier)
#endif
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "../src/hal_linux.c" // NOLINT(bugprone-suspicious-include) - the real HAL, deliberately
#include "test_common.h"

#if tt_SEGMENT_ENABLED && tt_SEGMENT_BELL_FIFO

// What a child reports through its exit status, so the parent can say which check failed.
enum {
    CHILD_OK = 0,
    CHILD_SETUP_FAILED = 2,
    CHILD_SIGPIPE_LEFT_PENDING = 3,
    CHILD_NOT_RUNG = 6,
};

static char bell_path[64];

// A bell with its owner's end, and a writer's end opened as a peer opens it.
static int make_bell(struct tt_Context* owner, int32_t* writer) {
    memset(owner, 0, sizeof(*owner));
    (void)snprintf(bell_path, sizeof(bell_path), "/tmp/tickle-bell-test-%d.bell", (int)getpid());
    if (tt_segment_bell_create(owner, bell_path) != 0) {
        return -1;
    }
    *writer = tt_segment_bell_open(bell_path);
    return *writer >= 0 ? 0 : -1;
}

static bool sigpipe_pending(void) {
    sigset_t pending;
    sigemptyset(&pending);
    (void)sigpending(&pending);
    return sigismember(&pending, SIGPIPE) == 1;
}

// Runs one case in a child with SIGPIPE at its default action, whatever this process inherited (a GitHub runner
// starts steps with it ignored, which is exactly what hid the defect).
static int run_child(int (*body)(void)) {
    pid_t child = fork();
    if (child == 0) {
        (void)signal(SIGPIPE, SIG_DFL);
        _exit(body());
    }
    int status = 0;
    (void)waitpid(child, &status, 0);
    return status;
}

static int child_result(int status) {
    return WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
}

// The control: the condition is real. A write-only end - how the bell was opened before - written after its owner
// has gone kills the writer by SIGPIPE. So a ring that survives below survives because of how the HAL opens the bell,
// not because the case is benign.
static int write_only_after_owner_gone(void) {
    struct tt_Context owner;
    int32_t writer = -1;
    if (make_bell(&owner, &writer) != 0) {
        return CHILD_SETUP_FAILED;
    }
    int write_only = open(bell_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (write_only < 0) {
        return CHILD_SETUP_FAILED;
    }
    tt_segment_bell_close(writer);              // only the write-only end and the owner's are left
    tt_segment_bell_destroy(&owner, bell_path); // the owner exits: its read end closes
    const uint8_t one = 1;
    (void)write(write_only, &one, sizeof(one));
    return CHILD_OK; // reached only if the write did not kill us
}

static void test_control_a_write_only_end_dies_of_sigpipe_once_the_owner_has_gone(void) {
    int status = run_child(write_only_after_owner_gone);
    EXPECT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ_INT(-SIGPIPE, child_result(status));
}

static int ring_after_owner_gone(void) {
    struct tt_Context owner;
    int32_t writer = -1;
    if (make_bell(&owner, &writer) != 0) {
        return CHILD_SETUP_FAILED;
    }
    tt_segment_bell_destroy(&owner, bell_path);
    tt_segment_bell_ring(writer);
    if (sigpipe_pending()) {
        return CHILD_SIGPIPE_LEFT_PENDING; // it would land the moment anything unblocked it
    }
    tt_segment_bell_close(writer);
    return CHILD_OK;
}

static void test_ringing_a_gone_owners_bell_does_not_kill_the_ringer(void) {
    EXPECT_EQ_INT(CHILD_OK, child_result(run_child(ring_after_owner_gone)));
}

// And a live owner is still rung: the byte arrives on its end, not on the writer's - the writer's end never reads.
static int ring_a_live_owner(void) {
    struct tt_Context owner;
    int32_t writer = -1;
    if (make_bell(&owner, &writer) != 0) {
        return CHILD_SETUP_FAILED;
    }
    tt_segment_bell_ring(writer);
    uint8_t got = 0;
    ssize_t read_bytes = read(owner.hal.bell_fd_plus1 - 1, &got, sizeof(got));
    tt_segment_bell_close(writer);
    tt_segment_bell_destroy(&owner, bell_path);
    return read_bytes == 1 && got == 1 ? CHILD_OK : CHILD_NOT_RUNG;
}

static void test_a_live_owner_is_still_rung(void) {
    EXPECT_EQ_INT(CHILD_OK, child_result(run_child(ring_a_live_owner)));
}

// The ring's cost: the syscalls between two getppid() markers, live owner and gone owner alike. A ring is on every
// wake-up of a sleeping reader, so it must stay the one write() - not, for instance, a signal mask set and restored
// around it.
#define MAX_COUNTED 16

struct ring_cost {
    int count;
    long numbers[MAX_COUNTED];
    bool traced;
};

static bool ring_owner_gone;

static int traced_ring(void) {
    struct tt_Context owner;
    int32_t writer = -1;
    if (make_bell(&owner, &writer) != 0) {
        return CHILD_SETUP_FAILED;
    }
    if (ring_owner_gone) {
        tt_segment_bell_destroy(&owner, bell_path);
    }
    if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0) {
        return CHILD_SETUP_FAILED;
    }
    (void)raise(SIGSTOP); // the tracer takes over here
    (void)syscall(SYS_getppid);
    tt_segment_bell_ring(writer);
    (void)syscall(SYS_getppid);
    return CHILD_OK;
}

static struct ring_cost count_ring_syscalls(bool owner_gone) {
    struct ring_cost cost = {0};
    ring_owner_gone = owner_gone;
    pid_t child = fork();
    if (child == 0) {
        (void)signal(SIGPIPE, SIG_DFL);
        _exit(traced_ring());
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFSTOPPED(status)) {
        return cost; // never stopped: not traced, and traced stays false so the caller says so
    }
    // ptrace() takes its integer arguments as pointers; the casts below are its calling convention, not addresses.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    (void)ptrace(PTRACE_SETOPTIONS, child, NULL, (void*)PTRACE_O_TRACESYSGOOD);
    int markers = 0;
    long deliver = 0; // a signal the child was sent is passed on, so tracing does not hide a SIGPIPE
    // NOLINTNEXTLINE(performance-no-int-to-ptr) - ptrace()'s convention, as above
    while (ptrace(PTRACE_SYSCALL, child, NULL, (void*)deliver) == 0 && waitpid(child, &status, 0) == child &&
           WIFSTOPPED(status)) {
        deliver = 0;
        if (WSTOPSIG(status) != (SIGTRAP | 0x80)) {
            deliver = WSTOPSIG(status); // a signal stop, not a syscall one
            continue;
        }
        struct __ptrace_syscall_info info;
        // NOLINTNEXTLINE(performance-no-int-to-ptr) - ptrace()'s convention, as above
        if (ptrace(PTRACE_GET_SYSCALL_INFO, child, (void*)sizeof(info), &info) <= 0 ||
            info.op != PTRACE_SYSCALL_INFO_ENTRY) {
            continue; // an exit stop
        }
        if ((long)info.entry.nr == SYS_getppid) {
            if (++markers == 2) {
                cost.traced = true;
                break;
            }
            continue;
        }
        if (markers == 1 && cost.count < MAX_COUNTED) {
            cost.numbers[cost.count++] = (long)info.entry.nr;
        }
    }
    (void)ptrace(PTRACE_DETACH, child, NULL, NULL);
    (void)waitpid(child, &status, 0);
    return cost;
}

static void expect_one_write(bool owner_gone) {
    struct ring_cost cost = count_ring_syscalls(owner_gone);
    if (!cost.traced) {
        // Not a pass: "could not look" is reported as a failure, never as a ring that cost nothing.
        fprintf(stderr, "test_segment_bell: could not trace the ring (owner %s)\n", owner_gone ? "gone" : "live");
    }
    EXPECT_TRUE(cost.traced);
    EXPECT_EQ_INT(1, cost.count);
    EXPECT_TRUE(cost.count >= 1 && cost.numbers[0] == SYS_write);
}

static void test_a_ring_is_one_write_syscall(void) {
    expect_one_write(false);
    expect_one_write(true);
}
#endif

int main(void) {
#if tt_SEGMENT_ENABLED && tt_SEGMENT_BELL_FIFO
    test_control_a_write_only_end_dies_of_sigpipe_once_the_owner_has_gone();
    test_ringing_a_gone_owners_bell_does_not_kill_the_ringer();
    test_a_live_owner_is_still_rung();
    test_a_ring_is_one_write_syscall();
#endif
    if (test_result() != 0) {
        return 1;
    }
    printf("test_segment_bell: all tests passed\n");
    return 0;
}
