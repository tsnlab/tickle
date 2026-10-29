/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A writer never uses a segment whose owner is dead - on the REAL Linux HAL and the real core, with a real process
// death (SIGKILL), 2026-10-09.
//
// The defect: a context killed without unlinking leaves its segment file, with a valid header naming it, and a new
// context at the same (address, port, id) builds its own only when its first same-host peer appears. A writer that
// asked the name in between attached to the dead file and wrote into a ring nobody drains, while the two sides
// reported different segment ids (~2% of test_loaned_messages runs). Revalidation could not save it either: it
// re-attached to the same dead file, whose header still matched. The owner now holds an exclusive flock() for as
// long as its region is mapped, and tt_segment_attach() refuses a file nobody holds (tt_SEGMENT_ORPHANED).
//
// Checked:
// - a live owner in another process is attached, although its creating descriptor was closed long ago (the lock is
//   held by the mapping);
// - once it is SIGKILLed, a new writer does not attach - the control shows the file is still there and its header
//   still passes every header check, so ownership is the only thing refusing it;
// - a writer attached BEFORE the death leaves at its next revalidation instead of re-attaching to the corpse;
// - a successor at the same name is attached, at its own incarnation;
// - a revalidation that finds the same file, owned, keeps the mapping and its bell and attaches nothing, and one that
//   finds a successor's file (its owner alive) or no file leaves it (2026-10-23);
// - in one process, unmapping the owner's region is what ends ownership, with the name still in place;
// - a writer's check does not refuse its neighbour's: another holder of a shared check lock changes nothing;
// - owners restarting in a loop while a writer attaches concurrently: every owner's create succeeds, and the writer
//   sees owned and orphaned files both.
// Mutants: tests/mutants_segment_owner.py.
//
// Nothing here touches the host's /dev/shm: TT_SEGMENT_DIR puts the segments in /tmp under a name made unique by
// this process's id, and every file this test makes it removes by that exact name. No socket is opened.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // NOLINT(bugprone-reserved-identifier) - the real HAL below needs ppoll() declared
#endif
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <tickle/tickle.h>

#define TT_SEGMENT_DIR "/tmp/"
#define TEST_COMMON_DEFINE_STORAGE
#include "../src/hal_linux.c" // NOLINT(bugprone-suspicious-include) - the real HAL, deliberately
#include "../src/tickle.c"    // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions
#include "test_common.h"

#if tt_SEGMENT_ENABLED

#define OWNER_ID 9
#define WRITER_ID 10
// 198.18.x.y (RFC 2544 benchmarking range: no real context is ever at it), x.y from this process's id. Per process,
// because a context building its segment sweeps every orphan at its address (sweep_orphaned_segments()): two runs of
// this test at one address - parallel build configurations - would sweep each other's deliberate corpses.
#define OWNER_IP_BASE 0xC6120000U
#define OWNER_IP owner_ip
#define RACE_OWNERS 200
#define PORT_BASE 10000
#define PORT_SPAN 50000

static uint16_t owner_port;
static uint32_t owner_ip;

static void init_context(struct tt_Context* node, uint8_t id, uint32_t incarnation) {
    memset(node, 0, sizeof(*node));
    node_init_locks(node);
    node->id = id;
    node->entity_id_base = incarnation;
    node->hal.own_ip = OWNER_IP;
    node->hal.own_port = owner_port;
}

// `writer` has heard an announce of the owner whose incarnation (pid) is `incarnation`: what lets it attach to that
// owner's segment (segment_vouched()). A successor's announce supersedes its predecessor's (process_announce()).
static void hear_owner(struct tt_Context* writer, uint32_t incarnation) {
    segment_vouch_forget(writer, OWNER_ID);
    segment_vouch_note(writer, OWNER_ID, incarnation, owner_port);
}

static void owner_path(char* path, size_t size) {
    (void)segment_name(path, size, OWNER_IP, owner_port, OWNER_ID);
}

// An owner in a child process: it builds its segment through core, says whether it has one, and waits to be killed.
// Its pid is its incarnation, so the writer can tell which owner it attached to.
static pid_t spawn_owner(void) {
    int ready[2];
    if (pipe(ready) != 0) {
        return -1;
    }
    pid_t child = fork();
    if (child == 0) {
        (void)close(ready[0]);
        struct tt_Context owner;
        init_context(&owner, OWNER_ID, (uint32_t)getpid());
        create_own_segment(&owner);
        const char built = owner.own_segment != NULL ? 'y' : 'n';
        (void)write(ready[1], &built, 1);
        for (;;) {
            (void)pause(); // until SIGKILL: no teardown, no unlink - what a crash leaves
        }
    }
    (void)close(ready[1]);
    char built = 0;
    ssize_t got = child > 0 ? read(ready[0], &built, 1) : -1;
    (void)close(ready[0]);
    if (got != 1 || built != 'y') {
        if (child > 0) {
            (void)kill(child, SIGKILL);
            (void)waitpid(child, NULL, 0);
        }
        return -1;
    }
    return child;
}

static void kill_owner(pid_t owner) {
    (void)kill(owner, SIGKILL);
    (void)waitpid(owner, NULL, 0);
}

static void remove_owner_files(void) {
    char path[tt_SEGMENT_PATH_LENGTH];
    owner_path(path, sizeof(path));
    (void)unlink(path);
#if tt_SEGMENT_BELL_FIFO
    char bell[tt_SEGMENT_PATH_LENGTH];
    (void)bell_name(bell, sizeof(bell), OWNER_IP, owner_port, OWNER_ID);
    (void)unlink(bell);
#endif
}

// The header as a writer would see it, read WITHOUT tt_segment_attach(): the control that the file is there and
// that every header check passes, so whatever refuses it is the ownership check and nothing else.
static enum tt_SegmentAttach header_verdict_bypassing_ownership(uint32_t* incarnation) {
    char path[tt_SEGMENT_PATH_LENGTH];
    owner_path(path, sizeof(path));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return tt_SEGMENT_ABSENT;
    }
    void* raw = mmap(NULL, sizeof(struct tt_SegmentHeader), PROT_READ, MAP_SHARED, fd, 0);
    (void)close(fd);
    if (raw == MAP_FAILED) {
        return tt_SEGMENT_REFUSED;
    }
    enum tt_SegmentAttach verdict = segment_header_check(raw, OWNER_IP, owner_port, OWNER_ID, 0);
    *incarnation = ((struct tt_SegmentHeader*)raw)->incarnation;
    (void)munmap(raw, sizeof(struct tt_SegmentHeader));
    return verdict;
}

// Sends until peer_segment() gives a different answer from `current`, at most `bound` times, a summary tick aging the
// cached answer by its interval after each send (segment_recheck_sweep()). Returns the new answer (or `current` if
// none came).
#if tt_SEGMENT_ATTACH_RETRY_NS > tt_SEGMENT_REVALIDATE_NS
#define TICK_STEP_NS tt_SEGMENT_ATTACH_RETRY_NS
#else
#define TICK_STEP_NS tt_SEGMENT_REVALIDATE_NS
#endif
static uint64_t tick_ns = 1;
static struct tt_SegmentHeader* send_until_changed(struct tt_Context* writer, struct tt_SegmentHeader* current,
                                                   uint32_t bound) {
    for (uint32_t i = 0; i < bound; i++) {
        struct tt_SegmentHeader* now = peer_segment(writer, OWNER_ID, OWNER_IP, owner_port);
        if (now != current) {
            return now;
        }
        tick_ns += TICK_STEP_NS;
        segment_recheck_sweep(writer, tick_ns);
    }
    return current;
}

// The whole life of one name: a live owner, its death, a writer that was attached and one that was not, a successor.
static void test_a_dead_owners_segment_is_never_written(void) {
    remove_owner_files();
    pid_t first = spawn_owner();
    EXPECT_TRUE(first > 0);
    if (first <= 0) {
        return;
    }

    // Live: attached, and to this owner. Its creating descriptor was closed inside tt_segment_create() long before.
    struct tt_Context attached_before;
    init_context(&attached_before, WRITER_ID, 1U);
    hear_owner(&attached_before, (uint32_t)first);
    struct tt_SegmentHeader* mapping = peer_segment(&attached_before, OWNER_ID, OWNER_IP, owner_port);
    EXPECT_TRUE(mapping != NULL);
    EXPECT_EQ_U32((uint32_t)first, mapping != NULL ? mapping->incarnation : 0U);
    EXPECT_EQ_U32(0, attached_before.segment_attach[tt_SEGMENT_ORPHANED]);

    kill_owner(first);

    // Control: the corpse is still there, under the name, and passes every header check.
    uint32_t incarnation = 0;
    EXPECT_EQ_INT((int)tt_SEGMENT_ATTACHED, (int)header_verdict_bypassing_ownership(&incarnation));
    EXPECT_EQ_U32((uint32_t)first, incarnation);

    // A writer asking now does not attach: the datagram goes over UDP (peer_segment() NULL is the unattached path).
    struct tt_Context fresh;
    init_context(&fresh, WRITER_ID, 2U);
    hear_owner(&fresh, (uint32_t)first); // the corpse is vouched for: ownership alone must refuse it
    EXPECT_TRUE(peer_segment(&fresh, OWNER_ID, OWNER_IP, owner_port) == NULL);
    EXPECT_EQ_U32(1, fresh.segment_attach[tt_SEGMENT_ORPHANED]);
    EXPECT_EQ_U32(0, fresh.segment_attach[tt_SEGMENT_ATTACHED]);

    // The writer attached before the death leaves at its next revalidation rather than re-attaching to the corpse,
    // whose header would have matched it again.
    struct tt_SegmentHeader* after = send_until_changed(&attached_before, mapping, 4U);
    EXPECT_TRUE(after == NULL);
    EXPECT_EQ_U32(1, attached_before.segment_attach[tt_SEGMENT_ORPHANED]);

    // A successor at the same name: both writers find it, at its incarnation, within one attach retry.
    pid_t second = spawn_owner();
    EXPECT_TRUE(second > 0);
    if (second > 0) {
        hear_owner(&fresh, (uint32_t)second);
        hear_owner(&attached_before, (uint32_t)second);
        struct tt_SegmentHeader* found = send_until_changed(&fresh, NULL, 4U);
        EXPECT_TRUE(found != NULL);
        EXPECT_EQ_U32((uint32_t)second, found != NULL ? found->incarnation : 0U);
        found = send_until_changed(&attached_before, NULL, 4U);
        EXPECT_TRUE(found != NULL);
        EXPECT_EQ_U32((uint32_t)second, found != NULL ? found->incarnation : 0U);
        kill_owner(second);
    }
    release_segments(&fresh);
    release_segments(&attached_before);
    remove_owner_files();
}

// Every attach this context made, by any outcome: the bench's shm_attach_attempts.
static uint32_t attach_attempts(const struct tt_Context* node) {
    uint32_t total = 0;
    for (int reason = 0; reason < tt_SEGMENT_ATTACH_COUNT; reason++) {
        total += node->segment_attach[reason];
    }
    return total;
}

// Makes the writer's answer about the owner due - two summary ticks, the first stamping it - and asks again. *was_due
// says the treatment was applied: a revalidation that never fell due would keep every mapping and prove nothing.
static struct tt_SegmentHeader* revalidate(struct tt_Context* writer, bool* was_due) {
    tick_ns += TICK_STEP_NS;
    segment_recheck_sweep(writer, tick_ns);
    tick_ns += TICK_STEP_NS;
    segment_recheck_sweep(writer, tick_ns);
    *was_due = writer->segment_peers[OWNER_ID].recheck_due;
    return peer_segment(writer, OWNER_ID, OWNER_IP, owner_port);
}

// 2026-10-23: a revalidation that finds the name still holding the region it maps, owned, keeps the mapping and its
// bell - no attach, no munmap - and each of the three things a revalidation exists to catch is still caught: the file
// replaced by a successor's (with the successor alive, so only the file's identity can tell), the file gone, and (in
// test_a_dead_owners_segment_is_never_written above) the file orphaned.
static void test_a_revalidation_keeps_a_live_owners_mapping(void) {
    remove_owner_files();
    pid_t first = spawn_owner();
    EXPECT_TRUE(first > 0);
    if (first <= 0) {
        return;
    }
    struct tt_Context writer;
    init_context(&writer, WRITER_ID, 1U);
    hear_owner(&writer, (uint32_t)first);
    struct tt_SegmentHeader* mapping = peer_segment(&writer, OWNER_ID, OWNER_IP, owner_port);
    EXPECT_TRUE(mapping != NULL);
    const uint32_t attached = attach_attempts(&writer);
    EXPECT_EQ_U32(1, writer.segment_attach[tt_SEGMENT_ATTACHED]);
    const int32_t bell = writer.segment_peers[OWNER_ID].bell_fd_plus1;

    // The same file, its owner alive: kept, three times over, and nothing attached.
    for (int round = 0; round < 3; round++) {
        bool was_due = false;
        EXPECT_TRUE(revalidate(&writer, &was_due) == mapping);
        EXPECT_TRUE(was_due); // the treatment: it was asked
    }
    EXPECT_EQ_U32(attached, attach_attempts(&writer)); // no attach: on origin/main each revalidation added one
    EXPECT_EQ_U32(3, writer.segment_revalidated);
    EXPECT_EQ_INT(bell, writer.segment_peers[OWNER_ID].bell_fd_plus1); // the bell kept with it
    EXPECT_EQ_U32((uint32_t)first, mapping != NULL ? mapping->incarnation : 0U);

    // Replaced: the owner dies and a successor builds its own file at the name. The old mapping still reads the old
    // incarnation and the successor is alive, so the file's identity is all that tells them apart.
    kill_owner(first);
    pid_t second = spawn_owner();
    EXPECT_TRUE(second > 0);
    if (second > 0) {
        hear_owner(&writer, (uint32_t)second);
        bool was_due = false;
        struct tt_SegmentHeader* now = revalidate(&writer, &was_due);
        EXPECT_TRUE(was_due);
        EXPECT_TRUE(now != NULL);
        EXPECT_EQ_U32((uint32_t)second, now != NULL ? now->incarnation : 0U);
        EXPECT_EQ_U32(2, writer.segment_attach[tt_SEGMENT_ATTACHED]);
        EXPECT_EQ_U32(3, writer.segment_revalidated);

        // Gone: the successor dies and its files are removed. The next revalidation finds no file and leaves.
        kill_owner(second);
        remove_owner_files();
        now = revalidate(&writer, &was_due);
        EXPECT_TRUE(was_due);
        EXPECT_TRUE(now == NULL);
        EXPECT_EQ_U32(1, writer.segment_attach[tt_SEGMENT_ABSENT]);
        EXPECT_EQ_U32(3, writer.segment_revalidated);
    }
    release_segments(&writer);
    remove_owner_files();
}

// In one process: ownership ends exactly when the owner's region is unmapped, the name and the header untouched -
// which is what a process death does to it. And another writer's shared check, held at that moment, refuses nothing.
static void test_ownership_is_the_mapping(void) {
    remove_owner_files();
    struct tt_Context owner;
    init_context(&owner, OWNER_ID, 77U);
    create_own_segment(&owner);
    EXPECT_TRUE(owner.own_segment != NULL);
    if (owner.own_segment == NULL) {
        return;
    }
    char path[tt_SEGMENT_PATH_LENGTH];
    owner_path(path, sizeof(path));
    const size_t bytes = segment_bytes(owner.own_segment->slots, owner.own_segment->slot_bytes);

    // A neighbouring writer mid-check: it holds a shared lock on the file for the length of its look.
    int neighbour = open(path, O_RDONLY | O_CLOEXEC);
    EXPECT_TRUE(neighbour >= 0);

    uint8_t why = (uint8_t)tt_SEGMENT_ABSENT;
    void* seen = tt_segment_attach(path, bytes, &why);
    EXPECT_TRUE(seen != NULL);
    EXPECT_EQ_INT((int)tt_SEGMENT_ATTACHED, (int)why);
    tt_segment_detach(seen, bytes);

    // The owner unmaps without unlinking. Same name, same header, nobody owning it.
    tt_segment_detach(owner.own_segment, bytes);
    owner.own_segment = NULL;
    // The neighbour's look happens now, at the same time as ours.
    EXPECT_EQ_INT(0, flock(neighbour, LOCK_SH | LOCK_NB));
    uint32_t incarnation = 0;
    EXPECT_EQ_INT((int)tt_SEGMENT_ATTACHED, (int)header_verdict_bypassing_ownership(&incarnation));
    why = (uint8_t)tt_SEGMENT_ATTACHED;
    seen = tt_segment_attach(path, bytes, &why);
    EXPECT_TRUE(seen == NULL);
    EXPECT_EQ_INT((int)tt_SEGMENT_ORPHANED, (int)why);
    if (seen != NULL) {
        tt_segment_detach(seen, bytes);
    }
    if (neighbour >= 0) {
        (void)close(neighbour);
    }
#if tt_SEGMENT_BELL_FIFO
    char bell[tt_SEGMENT_PATH_LENGTH];
    (void)bell_name(bell, sizeof(bell), OWNER_IP, owner_port, OWNER_ID);
    tt_segment_bell_destroy(&owner, bell);
#endif
    remove_owner_files();
}

// Owners restarting at one name as fast as they can, each leaving its file behind, while a writer asks the name the
// whole time. No owner may lose its segment to the writer's looking, or to its own look at the file it replaces, and
// the writer must see both answers - otherwise the loop raced nothing.
static void test_owners_restarting_under_a_writer(void) {
    remove_owner_files();
    pid_t spawner = fork();
    if (spawner == 0) {
        int failed = 0;
        for (int i = 0; i < RACE_OWNERS; i++) {
            pid_t owner = fork();
            if (owner == 0) {
                struct tt_Context node;
                init_context(&node, OWNER_ID, (uint32_t)getpid());
                create_own_segment(&node);
                usleep(200);                             // alive for a moment, so a writer can find it owned
                _exit(node.own_segment != NULL ? 0 : 1); // and gone without teardown
            }
            int status = 1;
            if (owner < 0 || waitpid(owner, &status, 0) != owner || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                failed++;
            }
            usleep(200); // dead, file still there: the orphan the writer must refuse
        }
        _exit(failed > 255 ? 255 : failed);
    }
    EXPECT_TRUE(spawner > 0);
    if (spawner <= 0) {
        return;
    }
    char path[tt_SEGMENT_PATH_LENGTH];
    owner_path(path, sizeof(path));
    const size_t bytes = segment_bytes(tt_SEGMENT_SLOTS, (uint32_t)tt_SEGMENT_SLOT_BYTES);
    uint32_t verdicts[tt_SEGMENT_ATTACH_COUNT] = {0};
    int status = 0;
    while (waitpid(spawner, &status, WNOHANG) == 0) {
        uint8_t why = (uint8_t)tt_SEGMENT_ABSENT;
        void* seen = tt_segment_attach(path, bytes, &why);
        if (seen != NULL) {
            tt_segment_detach(seen, bytes);
        }
        if (why < (uint8_t)tt_SEGMENT_ATTACH_COUNT) {
            verdicts[why]++;
        }
    }
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ_INT(0, WIFEXITED(status) ? WEXITSTATUS(status) : -1); // owners whose create failed
    EXPECT_TRUE(verdicts[tt_SEGMENT_ATTACHED] > 0);
    EXPECT_TRUE(verdicts[tt_SEGMENT_ORPHANED] > 0);
    printf("test_segment_owner: race: attached=%u orphaned=%u absent=%u bad_header=%u\n", verdicts[tt_SEGMENT_ATTACHED],
           verdicts[tt_SEGMENT_ORPHANED], verdicts[tt_SEGMENT_ABSENT], verdicts[tt_SEGMENT_BAD_HEADER]);
    remove_owner_files();
}
// A context building its segment removes what dead contexts at its address left: a segment nobody owns with its bell.
// Not a live owner's file, and nothing at another address (the controls: the sweep is by owner and by address, not of
// everything that looks like a segment).
static bool owner_file_exists(uint32_t ip, uint16_t port, bool bell) {
    char path[tt_SEGMENT_PATH_LENGTH];
    if (bell) {
#if tt_SEGMENT_BELL_FIFO
        (void)bell_name(path, sizeof(path), ip, port, OWNER_ID);
#else
        return false;
#endif
    } else {
        (void)segment_name(path, sizeof(path), ip, port, OWNER_ID);
    }
    struct stat info;
    return lstat(path, &info) == 0;
}

static void remove_files_at(uint32_t ip, uint16_t port) {
    const uint32_t saved_ip = owner_ip;
    const uint16_t saved_port = owner_port;
    owner_ip = ip;
    owner_port = port;
    remove_owner_files();
    owner_ip = saved_ip;
    owner_port = saved_port;
}

static pid_t spawn_owner_at(uint32_t ip, uint16_t port) {
    const uint32_t saved_ip = owner_ip;
    const uint16_t saved_port = owner_port;
    owner_ip = ip;
    owner_port = port;
    pid_t owner = spawn_owner();
    owner_ip = saved_ip;
    owner_port = saved_port;
    return owner;
}

static void test_dead_owners_files_are_swept(void) {
    const uint32_t here = owner_ip;
    const uint32_t elsewhere = owner_ip ^ 1U; // another address: its corpse is not this context's to remove
    const uint16_t dead_port = (uint16_t)(owner_port + 1U);
    const uint16_t live_port = (uint16_t)(owner_port + 2U);
    const uint16_t sweeper_port = (uint16_t)(owner_port + 3U);

    pid_t dead = spawn_owner_at(here, dead_port);
    pid_t live = spawn_owner_at(here, live_port);
    pid_t dead_elsewhere = spawn_owner_at(elsewhere, dead_port);
    EXPECT_TRUE(dead > 0 && live > 0 && dead_elsewhere > 0);
    if (dead > 0) {
        kill_owner(dead);
    }
    if (dead_elsewhere > 0) {
        kill_owner(dead_elsewhere);
    }
    // Before: the dead one's files are there, as a crash leaves them - the control that "gone" below is the sweep's.
    EXPECT_TRUE(owner_file_exists(here, dead_port, false));
#if tt_SEGMENT_BELL_FIFO
    EXPECT_TRUE(owner_file_exists(here, dead_port, true));
#endif

    struct tt_Context sweeper;
    init_context(&sweeper, OWNER_ID, 5U);
    sweeper.hal.own_port = sweeper_port;
    create_own_segment(&sweeper);
    EXPECT_TRUE(sweeper.own_segment != NULL);

    EXPECT_TRUE(!owner_file_exists(here, dead_port, false));     // the orphan, gone
    EXPECT_TRUE(!owner_file_exists(here, dead_port, true));      // with its bell
    EXPECT_TRUE(owner_file_exists(here, live_port, false));      // a live owner's, kept
    EXPECT_TRUE(owner_file_exists(elsewhere, dead_port, false)); // another address's orphan, kept
    EXPECT_TRUE(owner_file_exists(here, sweeper_port, false));   // and the sweeper's own, built
    if (live > 0) {
        kill_owner(live);
    }
    release_segments(&sweeper);
    remove_files_at(here, dead_port);
    remove_files_at(here, live_port);
    remove_files_at(here, sweeper_port);
    remove_files_at(elsewhere, dead_port);
}
#endif

int main(void) {
#if tt_SEGMENT_ENABLED
    owner_port = (uint16_t)(PORT_BASE + ((uint32_t)getpid() % PORT_SPAN));
    owner_ip = OWNER_IP_BASE | ((uint32_t)getpid() & 0xFFFFU);
    test_a_dead_owners_segment_is_never_written();
    test_a_revalidation_keeps_a_live_owners_mapping();
    test_ownership_is_the_mapping();
    test_owners_restarting_under_a_writer();
    test_dead_owners_files_are_swept();
#endif
    if (test_result() != 0) {
        return 1;
    }
    printf("test_segment_owner: all tests passed\n");
    return 0;
}
