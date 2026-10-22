/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// A same-host rmw publisher and subscriber over the shared-memory segment, one of them SIGKILLed mid-stream
// (ROADMAP "Wired work"). Every role is a forked child with its own rmw context; this process creates none and
// only watches: a shared board the children count into, their /proc/PID/maps (which segment files each maps) and
// /proc/locks (a segment's owner holds an exclusive flock() for as long as it maps it, DESIGN.md section 10), so
// "the publisher writes into the subscriber's ring" is read from the kernel, not inferred.
//
// Arms, each with fresh children, run in order. A kill arm and its control are the same run, except that the
// control never kills: every post-kill reading is also taken in the control at the same moment.
//
//   sub-kill     publisher P and subscriber S1; S1 SIGKILLed once both are on the segment; S2 started.
//   pub-kill     publisher P1 and subscriber S; P1 SIGKILLed once both are on the segment; P2 started.
//   each at RATE_FAST_HZ (a ring that fills after the kill) and RATE_SLOW_HZ (one that never does, ROS's 10 Hz).
//
// What must hold - the reading rules, enforced below and printed as one verdict line each:
//
//   sub-kill, kill arm
//     K1  P stays alive and keeps publishing: every rmw_publish() returns RMW_RET_OK, none takes longer than
//         PUBLISH_BOUND_NS, and P never goes longer than stall_bound_ms() without publishing (a blocked one stops).
//     K2  P gives up the dead S1's ring: within RELEASE_BOUND_MS of the kill P no longer maps the file S1 owned.
//     K3  S2 receives: SAMPLES_AFTER_RESTART samples within RESTART_BOUND_MS of its start, and P writes into S2's
//         own ring (maps the file S2 owns) within the same bound - received over shared memory, not UDP only.
//     K4  nothing is left in /dev/shm once every child has gone (the killed one included): no segment file at the
//         inode one of our children owned, and no doorbell without its segment.
//   sub-kill, control (S1 never killed; S2 is a second subscriber): K1, K3 and K4 the same; at the moment K2 is read,
//     P STILL maps S1's file - so K2's "no longer maps" is made by the kill and not by an instrument that sees nothing.
//
//   pub-kill, kill arm
//     R1  S stays alive and keeps polling: no rmw_take() takes longer than PUBLISH_BOUND_NS, and S never goes longer
//         than that and a tick without completing a poll.
//     R2  S sees P1 gone: rmw_count_publishers() on the topic is 0 within GONE_BOUND_MS of the kill.
//     R3  S receives from P2: SAMPLES_AFTER_RESTART samples published by P2 within RESTART_BOUND_MS of its start,
//         and P2 maps the file S owns at that time within the same bound (S rebuilds its segment when its last
//         same-host peer has gone, so it is asked again, not remembered).
//     R4  as K4.
//   pub-kill, control: R1, R3 and R4 the same, R2 inverted (the count is still 1 when R2 is read).
//
// A premise that does not hold voids the arm rather than passing it: both sides must be on the segment before the
// kill (the publisher maps the subscriber's file, the subscriber has samples), or the arm tested UDP and says so.
//
// What would falsify "the segment survives a SIGKILLed peer": any of K1-K4/R1-R4 failing in a kill arm while its
// control passes. A failure in a control is an instrument or environment failure, not a finding. That each check can
// fail: test/mutants_shm_kill.py. Found by this test and fixed (2026-10-22): K2 at 10 Hz (a writer kept a dead
// reader's ring mapped) and K4/R4 (every killed context left its segment and doorbell in /dev/shm).
//
// Arguments, for reproduction: `test_shm_kill [sub|pub] [fast|slow] [kill|control]`, any subset; none runs every
// combination. Run it in a private network namespace with a default route (run_rmw_suite.sh): it broadcasts.

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <tickle/tickle.h>

#include "rcutils/allocator.h"
#include "rcutils/strdup.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/publisher_options.h"
#include "rmw/qos_profiles.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/subscription_options.h"
#include "rmw/types.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_tickle_c/identifier.h"
#include "rosidl_typesupport_tickle_c/message_type_support.h"

#define TOPIC "/shm_kill"
#define RATE_FAST_HZ 500
#define RATE_SLOW_HZ 10
#define NS_PER_MS 1000000LL
#define NS_PER_S 1000000000LL
#define MS_PER_S 1000LL
#define CHECK_TICK_MS 100  // the watcher's period
#define GRAPH_PERIOD_MS 50 // how often a subscriber asks rmw_count_publishers()
// A publish or take that takes longer than this has blocked: neither waits for anything on a small best-fit type
// (RMW.md: types that fit one datagram never wait). Generous, so a busy shared PC's scheduling delay does not read
// as blocking; a publisher that waits on a ring nobody drains waits for ever.
#define PUBLISH_BOUND_NS (250LL * NS_PER_MS)
// Before the kill: each side on the segment, and the subscriber holding this many samples.
#define SAMPLES_BEFORE_KILL 5
#define READY_BOUND_MS 15000LL
// The liveliness silence (tt_LIVELINESS_SILENCE_NS: 3.5 intervals of 1 s) plus 2.5 s: everything about a dead peer
// is settled by then. The dead-reader rule (tt_SEGMENT_DEAD_READER_NS, 1 s) is shorter and needs a full ring.
#define GONE_BOUND_MS 6000LL
#define RELEASE_BOUND_MS 6000LL
#define WINDOW_MS 6000LL // the post-kill window K1/R1 are read over; K2 and R2 are read at its end
// From a restarted child's fork to its samples: discovery (one interval) and matching, three times over.
#define RESTART_BOUND_MS 10000LL
#define SAMPLES_AFTER_RESTART 3
#define STOP_BOUND_MS 10000LL
// Test memory cap: each child and this process. A child that ran away is stopped by the kernel, not by the PC.
#define MEMORY_CAP_BYTES (2048ULL * 1024ULL * 1024ULL)
#define MAX_SEGMENTS 32
#define PATH_BYTES 128
#define NAME_BYTES 64
#define LINE_BYTES 512
#define LOCK_FIELDS 7 // what owned_inodes() reads from a /proc/locks line
#define NOT_YET (-1)  // a moment that has not come

struct sample {
    uint32_t generation; // which publisher: 1 or 2
    uint32_t count;
};

static bool convert(const void* source, void* dest) {
    *(struct sample*)dest = *(const struct sample*)source;
    return true;
}
static int32_t encode_size(struct tt_Data* data) {
    (void)data;
    return (int32_t)sizeof(struct sample);
}
// NOLINTNEXTLINE(readability-non-const-parameter) - must match tt_DATA_ENCODE's own fixed signature
static int32_t encode(struct tt_Data* data, uint8_t* payload, const uint32_t len) {
    if (len < sizeof(struct sample)) {
        return -1;
    }
    memcpy(payload, data, sizeof(struct sample));
    return (int32_t)sizeof(struct sample);
}
static int32_t decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len, bool is_native_endian) {
    (void)is_native_endian;
    if (len < sizeof(struct sample)) {
        return -1;
    }
    memcpy(data, payload, sizeof(struct sample));
    return (int32_t)sizeof(struct sample);
}
static void free_nothing(struct tt_Data* data) {
    (void)data;
}

static rosidl_typesupport_tickle_c_message_callbacks_t message_callbacks = {
    .ros_type_name = "shm_kill/msg/Sample",
    .tickle_struct_size = sizeof(struct sample),
    .ros_struct_size = sizeof(struct sample),
    .to_tickle = (rosidl_typesupport_tickle_c_to_tickle_function)&convert,
    .from_tickle = (rosidl_typesupport_tickle_c_from_tickle_function)&convert,
    .tickle_encode_size = (tt_DATA_ENCODE_SIZE)&encode_size,
    .tickle_encode = (tt_DATA_ENCODE)&encode,
    .tickle_decode = (tt_DATA_DECODE)&decode,
    .tickle_free = (tt_DATA_FREE)&free_nothing,
    .tickle_max_encoded_size = sizeof(struct sample),
    .tickle_max_buffer_length = tt_MAX_BUFFER_LENGTH,
};
static rosidl_message_type_support_t message_handle = {.data = &message_callbacks,
                                                       .func = get_message_typesupport_handle_function};

// What the children report, in MAP_SHARED memory made before the forks. Index: the role's slot (two of each kind).
#define ROLES 2
struct board {
    atomic_int stop_pub[ROLES];
    atomic_int stop_sub[ROLES];
    atomic_int pub_failed[ROLES]; // an rmw_publish() that returned anything but OK
    atomic_llong pub_count[ROLES];
    atomic_llong pub_max_ns[ROLES];
    atomic_llong sub_polls[ROLES];
    atomic_llong sub_max_take_ns[ROLES];
    atomic_llong sub_taken[ROLES][ROLES + 1]; // by generation 1..2
    atomic_llong sub_publishers[ROLES];       // the last rmw_count_publishers() answer
};
static struct board* board;

static int64_t now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now); // NOLINT(misc-include-cleaner) - <time.h> above
    return ((int64_t)now.tv_sec * NS_PER_S) + now.tv_nsec;
}

static void sleep_ns(int64_t nanos) {
    struct timespec interval = {.tv_sec = (time_t)(nanos / NS_PER_S), .tv_nsec = (long)(nanos % NS_PER_S)};
    while (nanosleep(&interval, &interval) != 0 && errno == EINTR) {
    }
}

static void sleep_ms(long millis) {
    sleep_ns((int64_t)millis * NS_PER_MS);
}

static int64_t ms_since(int64_t start) {
    return (now_ns() - start) / NS_PER_MS;
}

static void atomic_max(atomic_llong* slot, long long value) {
    long long seen = atomic_load(slot);
    while (value > seen && !atomic_compare_exchange_weak(slot, &seen, value)) {
    }
}

static void init_context(rmw_init_options_t* options, rmw_context_t* context) {
    rcutils_allocator_t allocator = rcutils_get_default_allocator();
    *options = rmw_get_zero_initialized_init_options();
    assert(RMW_RET_OK == rmw_init_options_init(options, allocator));
    options->enclave = rcutils_strdup("/", allocator);
    *context = rmw_get_zero_initialized_context();
    assert(RMW_RET_OK == rmw_init(options, context));
}

static void fini_context(rmw_init_options_t* options, rmw_context_t* context) {
    assert(RMW_RET_OK == rmw_shutdown(context));
    assert(RMW_RET_OK == rmw_context_fini(context));
    assert(RMW_RET_OK == rmw_init_options_fini(options));
}

// A publisher child: generation `generation` at `rate_hz` until told to stop.
static int run_publisher(int slot, uint32_t generation, int rate_hz) {
    rmw_init_options_t options;
    rmw_context_t context;
    init_context(&options, &context);
    rmw_node_t* node = rmw_create_node(&context, generation == 1 ? "shm_kill_pub1" : "shm_kill_pub2", "/");
    assert(NULL != node);
    rmw_qos_profile_t qos = rmw_qos_profile_default; // RELIABLE, KEEP_LAST 10: what a ROS publisher gets
    rmw_publisher_options_t pub_options = rmw_get_default_publisher_options();
    rmw_publisher_t* pub = rmw_create_publisher(node, &message_handle, TOPIC, &qos, &pub_options);
    assert(NULL != pub);
    const int64_t period = NS_PER_S / rate_hz;
    int64_t next = now_ns();
    uint32_t count = 0;
    while (!atomic_load(&board->stop_pub[slot])) {
        struct sample message = {.generation = generation, .count = count++};
        int64_t before = now_ns();
        rmw_ret_t ret = rmw_publish(pub, &message, NULL);
        atomic_max(&board->pub_max_ns[slot], now_ns() - before);
        if (ret != RMW_RET_OK) {
            atomic_store(&board->pub_failed[slot], 1);
        }
        atomic_fetch_add(&board->pub_count[slot], 1);
        next += period;
        int64_t wait = next - now_ns();
        if (wait > 0) {
            sleep_ns(wait);
        } else {
            next = now_ns();
        }
    }
    assert(RMW_RET_OK == rmw_destroy_publisher(node, pub));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    fini_context(&options, &context);
    return 0;
}

// Takes everything queued, counting by generation.
static void take_all(int slot, rmw_subscription_t* sub) {
    bool taken = true;
    while (taken) {
        struct sample message;
        int64_t before = now_ns();
        assert(RMW_RET_OK == rmw_take(sub, &message, &taken, NULL));
        atomic_max(&board->sub_max_take_ns[slot], now_ns() - before);
        if (taken) {
            assert(message.generation >= 1 && message.generation <= ROLES);
            atomic_fetch_add(&board->sub_taken[slot][message.generation], 1);
        }
    }
}

// A subscriber child: takes everything, counts by generation, asks the graph how many publishers there are.
static int run_subscriber(int slot) {
    rmw_init_options_t options;
    rmw_context_t context;
    init_context(&options, &context);
    rmw_node_t* node = rmw_create_node(&context, slot == 0 ? "shm_kill_sub1" : "shm_kill_sub2", "/");
    assert(NULL != node);
    rmw_qos_profile_t qos = rmw_qos_profile_default;
    rmw_subscription_options_t sub_options = rmw_get_default_subscription_options();
    rmw_subscription_t* sub = rmw_create_subscription(node, &message_handle, TOPIC, &qos, &sub_options);
    assert(NULL != sub);
    int64_t next_graph = 0;
    while (!atomic_load(&board->stop_sub[slot])) {
        take_all(slot, sub);
        if (now_ns() >= next_graph) {
            size_t publishers = 0;
            assert(RMW_RET_OK == rmw_count_publishers(node, TOPIC, &publishers));
            atomic_store(&board->sub_publishers[slot], (long long)publishers);
            next_graph = now_ns() + (GRAPH_PERIOD_MS * NS_PER_MS);
        }
        atomic_fetch_add(&board->sub_polls[slot], 1);
        sleep_ms(1);
    }
    assert(RMW_RET_OK == rmw_destroy_subscription(node, sub));
    assert(RMW_RET_OK == rmw_destroy_node(node));
    fini_context(&options, &context);
    return 0;
}

// ---- the watcher's instruments: what the kernel says each child maps and owns

struct segment {
    char path[PATH_BYTES];
    unsigned long inode;
    pid_t owner;
};

// The inodes `pid` holds an exclusive flock() on, as /proc/locks lists them. A segment's owner is the one
// context holding such a lock on it (hal_linux.c, tt_segment_create()).
static int owned_inodes(pid_t pid, unsigned long* out, int max) {
    FILE* locks = fopen("/proc/locks", "r");
    assert(NULL != locks);
    char line[LINE_BYTES];
    int found = 0;
    while (fgets(line, sizeof(line), locks) != NULL) {
        char kind[16];
        char mode[16];
        char access[16];
        int holder = 0;
        unsigned major = 0;
        unsigned minor = 0;
        unsigned long inode = 0;
        if (sscanf(line, "%*d: %15s %15s %15s %d %x:%x:%lu", kind, mode, access, &holder, &major, &minor, &inode) ==
                LOCK_FIELDS &&
            0 == strcmp(kind, "FLOCK") && 0 == strcmp(access, "WRITE") && holder == pid && found < max) {
            out[found++] = inode;
        }
    }
    fclose(locks);
    return found;
}

// Calls `each` for every segment file (not a doorbell) in /proc/<pid>/maps, with its inode.
static void for_each_mapped_segment(pid_t pid, void (*each)(const char* path, unsigned long inode, void* arg),
                                    void* arg) {
    char maps_path[NAME_BYTES];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", (int)pid);
    FILE* maps = fopen(maps_path, "r");
    if (NULL == maps) {
        return; // gone: maps nothing
    }
    char line[LINE_BYTES];
    while (fgets(line, sizeof(line), maps) != NULL) {
        unsigned long inode = 0;
        int offset = 0;
        if (sscanf(line, "%*s %*s %*s %*s %lu %n", &inode, &offset) < 1 || offset == 0) {
            continue;
        }
        char* path = line + offset;
        path[strcspn(path, "\n")] = '\0';
        if (0 == strncmp(path, "/dev/shm/tickle-seg-", strlen("/dev/shm/tickle-seg-"))) {
            each(path, inode, arg);
        }
    }
    fclose(maps);
}

struct maps_query {
    unsigned long inode;
    bool found;
};
static void match_inode(const char* path, unsigned long inode, void* arg) {
    (void)path;
    struct maps_query* query = arg;
    query->found = query->found || inode == query->inode;
}
static bool maps_inode(pid_t pid, unsigned long inode) {
    struct maps_query query = {.inode = inode, .found = false};
    for_each_mapped_segment(pid, match_inode, &query);
    return query.found;
}

// Every segment any of our children ever owned, so the leak check can name each.
static struct segment seen[MAX_SEGMENTS];
static int seen_count;

struct own_query {
    pid_t pid;
    unsigned long inodes[MAX_SEGMENTS];
    int inode_count;
    struct segment found; // the last owned segment found
};
static void remember_segment(const struct segment* segment) {
    for (int idx = 0; idx < seen_count; idx++) {
        if (seen[idx].inode == segment->inode) {
            return;
        }
    }
    if (seen_count < MAX_SEGMENTS) {
        seen[seen_count++] = *segment;
        printf("  segment owned by pid %d: %s (inode %lu)\n", (int)segment->owner, segment->path, segment->inode);
    }
}
static void record_owned(const char* path, unsigned long inode, void* arg) {
    struct own_query* query = arg;
    for (int idx = 0; idx < query->inode_count; idx++) {
        if (query->inodes[idx] != inode) {
            continue;
        }
        snprintf(query->found.path, sizeof(query->found.path), "%s", path);
        // A mapping of an unlinked file says so; the name is still what the leak check must look at.
        char* deleted = strstr(query->found.path, " (deleted)");
        if (NULL != deleted) {
            *deleted = '\0';
        }
        query->found.inode = inode;
        query->found.owner = query->pid;
        remember_segment(&query->found);
    }
}
// The segment `pid` owns and maps, if any (inode 0: none); recorded in seen[].
static struct segment owned_segment(pid_t pid) {
    struct own_query query;
    memset(&query, 0, sizeof(query));
    query.pid = pid;
    query.inode_count = owned_inodes(pid, query.inodes, MAX_SEGMENTS);
    if (query.inode_count > 0) {
        for_each_mapped_segment(pid, record_owned, &query);
    }
    return query.found;
}

// ---- children

static pid_t spawn_publisher(int slot, uint32_t generation, int rate_hz) {
    fflush(stdout);
    pid_t pid = fork();
    assert(pid >= 0);
    if (0 == pid) {
        _exit(run_publisher(slot, generation, rate_hz));
    }
    return pid;
}

static pid_t spawn_subscriber(int slot) {
    fflush(stdout);
    pid_t pid = fork();
    assert(pid >= 0);
    if (0 == pid) {
        _exit(run_subscriber(slot));
    }
    return pid;
}

// SIGKILL to the PID fork() returned - after checking that it is still our own child running this binary, so a
// recycled PID cannot be hit.
static void kill_child(pid_t pid) {
    char link[NAME_BYTES];
    char exe[PATH_BYTES];
    char self[PATH_BYTES];
    snprintf(link, sizeof(link), "/proc/%d/exe", (int)pid);
    ssize_t exe_length = readlink(link, exe, sizeof(exe) - 1);
    ssize_t self_length = readlink("/proc/self/exe", self, sizeof(self) - 1);
    assert(exe_length > 0 && self_length > 0);
    exe[exe_length] = '\0';
    self[self_length] = '\0';
    assert(0 == strcmp(exe, self));
    assert(0 == waitpid(pid, NULL, WNOHANG)); // alive and ours: an exited child would be reaped here
    assert(0 == kill(pid, SIGKILL));
    int status = 0;
    assert(pid == waitpid(pid, &status, 0));
    assert(WIFSIGNALED(status) && SIGKILL == WTERMSIG(status));
}

// Waits for a child told to stop; true when it exited 0 within the bound.
static bool reap(pid_t pid) {
    int64_t deadline = now_ns() + (STOP_BOUND_MS * NS_PER_MS);
    int status = 0;
    while (now_ns() < deadline) {
        pid_t got = waitpid(pid, &status, WNOHANG);
        if (got == pid) {
            return WIFEXITED(status) && 0 == WEXITSTATUS(status);
        }
        sleep_ms(CHECK_TICK_MS);
    }
    (void)kill(pid, SIGKILL); // our child, by the PID fork() gave; it did not stop
    (void)waitpid(pid, &status, 0);
    return false;
}

static int failures;
static int voids;
static void verdict(const char* arm, const char* check, bool pass, const char* detail) {
    printf("%-26s %-4s %s  %s\n", arm, check, pass ? "PASS" : "FAIL", detail);
    failures += pass ? 0 : 1;
}

// K4/R4: no file one of our children owned is still at its name with its inode, and no doorbell is left without
// its segment. Read once every child has gone.
static bool nothing_left(char* detail, size_t size) {
    int left = 0;
    detail[0] = '\0';
    for (int idx = 0; idx < seen_count; idx++) {
        struct stat info;
        char bell[PATH_BYTES + 8];
        snprintf(bell, sizeof(bell), "%s.bell", seen[idx].path);
        bool segment_there = 0 == stat(seen[idx].path, &info);
        bool ours = segment_there && (unsigned long)info.st_ino == seen[idx].inode;
        bool bell_alone = !segment_there && 0 == lstat(bell, &info);
        if (ours || bell_alone) {
            left++;
            size_t used = strlen(detail);
            snprintf(detail + used, size - used, "%s%s%s", used ? ", " : "", ours ? seen[idx].path : bell,
                     ours ? " (segment)" : " (doorbell)");
        }
    }
    if (0 == left) {
        snprintf(detail, size, "%d segment files checked, none left", seen_count);
    }
    return 0 == left;
}

// The longest a live publisher at `rate_hz` may go without publishing, as the watcher sees it: a period, a publish at
// its bound, and a tick of the watcher's own resolution.
static int64_t stall_bound_ms(int rate_hz) {
    return (MS_PER_S / rate_hz) + (PUBLISH_BOUND_NS / NS_PER_MS) + CHECK_TICK_MS;
}

static void reset_board(void) {
    memset(board, 0, sizeof(*board));
    seen_count = 0;
}

// ---- the phases an arm is made of

// The premise: the subscriber has samples of generation 1, the publisher maps the subscriber's own ring and, when
// asked, the subscriber's graph counts one publisher. The subscriber's ring, or inode 0 when the premise never held.
static struct segment wait_premise(const char* arm, pid_t pub, pid_t sub, bool count_one) {
    struct segment ring = {0};
    int64_t start = now_ns();
    while (ms_since(start) < READY_BOUND_MS) {
        sleep_ms(CHECK_TICK_MS);
        if (ring.inode == 0) {
            ring = owned_segment(sub);
        }
        (void)owned_segment(pub);
        bool counted = !count_one || 1 == atomic_load(&board->sub_publishers[0]);
        if (ring.inode != 0 && maps_inode(pub, ring.inode) &&
            atomic_load(&board->sub_taken[0][1]) >= SAMPLES_BEFORE_KILL && counted) {
            printf("  premise held after %lld ms: the subscriber took %lld, the publisher maps its ring\n",
                   (long long)ms_since(start), atomic_load(&board->sub_taken[0][1]));
            return ring;
        }
    }
    printf("%-26s VOID premise: subscriber ring inode %lu, the publisher maps it %d, the subscriber took %lld and "
           "counts %lld publishers - not both on the segment\n",
           arm, ring.inode, ring.inode != 0 && maps_inode(pub, ring.inode), atomic_load(&board->sub_taken[0][1]),
           atomic_load(&board->sub_publishers[0]));
    voids++;
    ring.inode = 0;
    return ring;
}

// What the window after the kill saw: the longest time the watched counter stood still, and when the watched event
// came first (NOT_YET: never).
struct window {
    int64_t longest_gap_ms;
    int64_t event_ms;
};

// Watches `counter` move for WINDOW_MS, and `event` (with `pid` and `inode`) come.
static struct window watch(atomic_llong* counter, bool (*event)(pid_t pid, unsigned long inode), pid_t pid,
                           unsigned long inode) {
    struct window seen_window = {.longest_gap_ms = 0, .event_ms = NOT_YET};
    int64_t start = now_ns();
    int64_t last_progress = start;
    long long last = atomic_load(counter);
    while (ms_since(start) < WINDOW_MS) {
        sleep_ms(CHECK_TICK_MS);
        long long value = atomic_load(counter);
        if (value != last) {
            last_progress = now_ns();
            last = value;
        }
        if (ms_since(last_progress) > seen_window.longest_gap_ms) {
            seen_window.longest_gap_ms = ms_since(last_progress);
        }
        if (seen_window.event_ms == NOT_YET && event(pid, inode)) {
            seen_window.event_ms = ms_since(start);
        }
    }
    return seen_window;
}

static bool ring_unmapped(pid_t pid, unsigned long inode) {
    return !maps_inode(pid, inode);
}
static bool no_publishers(pid_t pid, unsigned long inode) {
    (void)pid;
    (void)inode;
    return 0 == atomic_load(&board->sub_publishers[0]);
}

// After a restart: when `sub_slot` had SAMPLES_AFTER_RESTART samples of `generation`, and when `writer` first mapped
// the ring `reader` owns at that moment (asked again each tick). Each NOT_YET when it did not come within the bound.
struct restart {
    int64_t received_ms;
    int64_t attached_ms;
    unsigned long ring;
};
static struct restart wait_restart(int sub_slot, int generation, pid_t writer, pid_t reader) {
    struct restart result = {.received_ms = NOT_YET, .attached_ms = NOT_YET, .ring = 0};
    int64_t start = now_ns();
    while (ms_since(start) < RESTART_BOUND_MS && (result.received_ms == NOT_YET || result.attached_ms == NOT_YET)) {
        sleep_ms(CHECK_TICK_MS);
        (void)owned_segment(writer);
        struct segment ring = owned_segment(reader);
        if (result.received_ms == NOT_YET &&
            atomic_load(&board->sub_taken[sub_slot][generation]) >= SAMPLES_AFTER_RESTART) {
            result.received_ms = ms_since(start);
        }
        if (result.attached_ms == NOT_YET && ring.inode != 0 && maps_inode(writer, ring.inode)) {
            result.attached_ms = ms_since(start);
            result.ring = ring.inode;
        }
    }
    return result;
}

// Stops every child still running and reaps it; then K4/R4.
static void finish(const char* arm, const char* check, const pid_t* children, int count) {
    for (int slot = 0; slot < ROLES; slot++) {
        atomic_store(&board->stop_pub[slot], 1);
        atomic_store(&board->stop_sub[slot], 1);
    }
    bool clean = true;
    for (int idx = 0; idx < count; idx++) {
        clean = reap(children[idx]) && clean;
    }
    char detail[LINE_BYTES * 2];
    bool left = nothing_left(detail, sizeof(detail));
    verdict(arm, check, clean && left, detail);
    if (!clean) {
        printf("  a child did not stop cleanly\n");
    }
}

// ---- sub-kill: P publishes, S1 dies (or not), S2 comes

static void arm_sub_kill(int rate_hz, bool do_kill) {
    char arm[NAME_BYTES];
    char detail[LINE_BYTES * 2];
    snprintf(arm, sizeof(arm), "sub-%s %dHz", do_kill ? "kill" : "ctrl", rate_hz);
    reset_board();
    printf("== %s\n", arm);
    pid_t pub = spawn_publisher(0, 1, rate_hz);
    pid_t first_sub = spawn_subscriber(0);
    struct segment first_ring = wait_premise(arm, pub, first_sub, false);
    if (first_ring.inode == 0) {
        const pid_t children[] = {pub, first_sub};
        finish(arm, "end", children, 2);
        return;
    }

    long long max_before = atomic_load(&board->pub_max_ns[0]);
    if (do_kill) {
        kill_child(first_sub);
    }
    struct window after = watch(&board->pub_count[0], ring_unmapped, pub, first_ring.inode);
    bool pub_alive = 0 == waitpid(pub, NULL, WNOHANG);
    long long max_ns = atomic_load(&board->pub_max_ns[0]);
    int64_t gap_bound_ms = stall_bound_ms(rate_hz);
    snprintf(detail, sizeof(detail),
             "P alive %d, publish failures %d, longest publish %.3f ms (%.3f before; bound %lld), longest time without "
             "a publish %lld ms (bound %lld)",
             pub_alive, atomic_load(&board->pub_failed[0]), (double)max_ns / NS_PER_MS, (double)max_before / NS_PER_MS,
             PUBLISH_BOUND_NS / NS_PER_MS, (long long)after.longest_gap_ms, (long long)gap_bound_ms);
    verdict(arm, "K1",
            pub_alive && !atomic_load(&board->pub_failed[0]) && max_ns <= PUBLISH_BOUND_NS &&
                after.longest_gap_ms <= gap_bound_ms,
            detail);
    if (do_kill) {
        snprintf(detail, sizeof(detail), "P %s S1's ring (inode %lu) %lld ms after the kill (bound %lld)",
                 after.event_ms != NOT_YET ? "unmapped" : "still maps", first_ring.inode, (long long)after.event_ms,
                 RELEASE_BOUND_MS);
        verdict(arm, "K2", after.event_ms != NOT_YET && after.event_ms <= RELEASE_BOUND_MS, detail);
    } else {
        snprintf(detail, sizeof(detail), "control: P %s S1's ring after the window",
                 after.event_ms == NOT_YET ? "still maps" : "unmapped");
        verdict(arm, "K2", after.event_ms == NOT_YET, detail);
    }

    pid_t second_sub = spawn_subscriber(1);
    struct restart again = wait_restart(1, 1, pub, second_sub);
    snprintf(detail, sizeof(detail),
             "S2 took %d samples at %lld ms, P mapped S2's ring (inode %lu) at %lld ms (bound %lld)",
             SAMPLES_AFTER_RESTART, (long long)again.received_ms, again.ring, (long long)again.attached_ms,
             RESTART_BOUND_MS);
    verdict(arm, "K3", again.received_ms != NOT_YET && again.attached_ms != NOT_YET, detail);

    const pid_t survivors[] = {pub, second_sub, first_sub};
    finish(arm, "K4", survivors, do_kill ? 2 : 3);
}

// ---- pub-kill: S subscribes, P1 dies (or not), P2 comes

static void arm_pub_kill(int rate_hz, bool do_kill) {
    char arm[NAME_BYTES];
    char detail[LINE_BYTES * 2];
    snprintf(arm, sizeof(arm), "pub-%s %dHz", do_kill ? "kill" : "ctrl", rate_hz);
    reset_board();
    printf("== %s\n", arm);
    pid_t sub = spawn_subscriber(0);
    pid_t first_pub = spawn_publisher(0, 1, rate_hz);
    struct segment ring = wait_premise(arm, first_pub, sub, true);
    if (ring.inode == 0) {
        const pid_t children[] = {first_pub, sub};
        finish(arm, "end", children, 2);
        return;
    }

    long long max_before = atomic_load(&board->sub_max_take_ns[0]);
    if (do_kill) {
        kill_child(first_pub);
    }
    struct window after = watch(&board->sub_polls[0], no_publishers, sub, 0);
    bool sub_alive = 0 == waitpid(sub, NULL, WNOHANG);
    long long max_ns = atomic_load(&board->sub_max_take_ns[0]);
    // The subscriber polls every millisecond: its bound is a take at its bound and a tick.
    int64_t gap_bound_ms = (PUBLISH_BOUND_NS / NS_PER_MS) + CHECK_TICK_MS;
    snprintf(detail, sizeof(detail),
             "S alive %d, longest take %.3f ms (%.3f before; bound %lld), longest time without a poll %lld ms (bound "
             "%lld)",
             sub_alive, (double)max_ns / NS_PER_MS, (double)max_before / NS_PER_MS, PUBLISH_BOUND_NS / NS_PER_MS,
             (long long)after.longest_gap_ms, (long long)gap_bound_ms);
    verdict(arm, "R1", sub_alive && max_ns <= PUBLISH_BOUND_NS && after.longest_gap_ms <= gap_bound_ms, detail);
    if (do_kill) {
        snprintf(detail, sizeof(detail), "S counted 0 publishers %lld ms after the kill (bound %lld)",
                 (long long)after.event_ms, GONE_BOUND_MS);
        verdict(arm, "R2", after.event_ms != NOT_YET && after.event_ms <= GONE_BOUND_MS, detail);
    } else {
        snprintf(detail, sizeof(detail), "control: S counts %lld publishers after the window",
                 atomic_load(&board->sub_publishers[0]));
        verdict(arm, "R2", after.event_ms == NOT_YET && 1 == atomic_load(&board->sub_publishers[0]), detail);
    }

    pid_t second_pub = spawn_publisher(1, 2, rate_hz);
    struct restart again = wait_restart(0, 2, second_pub, sub);
    snprintf(detail, sizeof(detail),
             "S took %d of P2's samples at %lld ms, P2 mapped S's ring (inode %lu) at %lld ms (bound %lld)",
             SAMPLES_AFTER_RESTART, (long long)again.received_ms, again.ring, (long long)again.attached_ms,
             RESTART_BOUND_MS);
    verdict(arm, "R3", again.received_ms != NOT_YET && again.attached_ms != NOT_YET, detail);

    const pid_t survivors[] = {second_pub, sub, first_pub};
    finish(arm, "R4", survivors, do_kill ? 2 : 3);
}

// ---- which arms: every one, or those the arguments name

struct arm_spec {
    const char* kind; // "sub" or "pub"
    const char* rate; // "fast" or "slow"
    const char* mode; // "kill" or "control"
};
static const struct arm_spec arms[] = {
    {"sub", "fast", "kill"}, {"sub", "fast", "control"}, {"sub", "slow", "kill"}, {"sub", "slow", "control"},
    {"pub", "fast", "kill"}, {"pub", "fast", "control"}, {"pub", "slow", "kill"}, {"pub", "slow", "control"},
};

// Whether `word`, one of `group`, is wanted: named in the arguments, or no member of its group is.
static bool wanted(int argc, char** argv, const char* word, const char* first, const char* second) {
    bool group_named = false;
    for (int idx = 1; idx < argc; idx++) {
        if (0 == strcmp(argv[idx], word)) {
            return true;
        }
        group_named = group_named || 0 == strcmp(argv[idx], first) || 0 == strcmp(argv[idx], second);
    }
    return !group_named;
}

static bool arm_wanted(int argc, char** argv, const struct arm_spec* spec) {
    return wanted(argc, argv, spec->kind, "sub", "pub") && wanted(argc, argv, spec->rate, "fast", "slow") &&
           wanted(argc, argv, spec->mode, "kill", "control");
}

int main(int argc, char** argv) {
    message_handle.typesupport_identifier = rosidl_typesupport_tickle_c__identifier;
    setvbuf(stdout, NULL, _IONBF, 0);
    struct rlimit cap = {.rlim_cur = MEMORY_CAP_BYTES, .rlim_max = MEMORY_CAP_BYTES};
    assert(0 == setrlimit(RLIMIT_AS, &cap)); // inherited by every child
    board = mmap(NULL, sizeof(*board), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(MAP_FAILED != board);

    for (size_t idx = 0; idx < sizeof(arms) / sizeof(arms[0]); idx++) {
        const struct arm_spec* spec = &arms[idx];
        if (!arm_wanted(argc, argv, spec)) {
            continue;
        }
        int rate = 0 == strcmp(spec->rate, "fast") ? RATE_FAST_HZ : RATE_SLOW_HZ;
        bool do_kill = 0 == strcmp(spec->mode, "kill");
        if (0 == strcmp(spec->kind, "sub")) {
            arm_sub_kill(rate, do_kill);
        } else {
            arm_pub_kill(rate, do_kill);
        }
    }
    printf("test_shm_kill: %d failed, %d void\n", failures, voids);
    if (failures != 0 || voids != 0) {
        printf("test_shm_kill: FAIL\n");
        return 1;
    }
    printf("test_shm_kill: PASS\n");
    return 0;
}
