/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#pragma once

#include <pthread.h>
#include <sched.h>   // sched_yield() - tt_thread_yield()
#include <stdbool.h> // rx_prefer_data
#include <stdint.h>
#include <time.h>

#include <netinet/in.h>
#include <sys/socket.h> // struct msghdr, for struct tt_mmsghdr, and struct iovec
#include <tickle/config.h>

// Datagrams one recvmmsg() may read (tt_receive()/tt_try_receive(), hal_linux.c). The first lands in the
// caller's buffer; the other tt_RX_BATCH - 1 wait in struct tt_hal.rx_batch until asked for, so a
// backlogged receiver pays one syscall per batch rather than one per datagram. 1 is the plain recvfrom()
// path, exactly as before batching.
//
// 32 (2026-09-26, experiments/veth_rx_batch.sh): the rig's server drained ~30 datagrams per wake-up
// (1.066 receive syscalls a sample, CORE_HEADROOM.md), so 32 takes a typical drain in one call. On a
// receiver that keeps up, batches run short and the change costs nothing measurable; on a backlogged one
// receive syscalls fell from 1.08 to 0.06 a sample at 16. Costs (tt_RX_BATCH - 1) x tt_MAX_BUFFER_LENGTH
// of node memory - 45 KB at the 1472-byte datagram. A build with a larger datagram (rmw_tickle's 65507)
// would pay 2 MB, all of it resident where the node is zeroed on creation (rmw_init.c), so it stays at 1
// until measured on its own terms; -Dtt_RX_BATCH overrides either default.
#ifndef tt_RX_BATCH
#if tt_MAX_BUFFER_LENGTH > tt_CONTROL_MAX_LENGTH
#define tt_RX_BATCH 1
#else
#define tt_RX_BATCH 32
#endif
#endif

// UDP receive offload (hal_linux.c "UDP offload"): UDP_GRO on both sockets, so the kernel may hand several same-flow
// datagrams over in one read, which the HAL then gives core one at a time where they lie. Derived, not a setting:
//   - the receive buffer must hold the largest merged read, ~64 KB, or the kernel truncates it - so only a build whose
//     datagram is that large (rmw_tickle's 65507); core's default 1472 never merges;
//   - one read per call (tt_RX_BATCH 1, that build's default): recvmmsg() would need a 64 KB slot per datagram;
//   - nothing else may write the receive buffer while segments of a read are still to be handed out: without lending,
//     the segment drain copies its records into rx_buffer, so a segment build without lending goes without.
// The environment variable TT_UDP_OFFLOAD=0 turns it off at tt_bind() (the A/B control), as it does send offload.
#if tt_MAX_BUFFER_LENGTH >= 65507 && tt_RX_BATCH == 1 && (tt_SAMPLE_LENDING || !tt_SEGMENT_ENABLED)
#define TT_HAL_UDP_GRO 1
#else
#define TT_HAL_UDP_GRO 0
#endif

// glibc's struct mmsghdr, which it declares only under _GNU_SOURCE - a define this public header cannot
// require of everything that includes it. hal_linux.c checks the two layouts are the same.
struct tt_mmsghdr {
    struct msghdr msg_hdr;
    unsigned int msg_len;
};

// The lock TickLE core uses to be callable from several threads (tt_THREAD_SAFE, config.h). Defined
// per platform, next to struct tt_hal, because it is the one other thing core needs from the OS for
// threading; everything above it is portable. A recursive lock is re-entrant from the thread that
// holds it, which the node's state lock must be: user callbacks run with it held and may call back
// into core, exactly as they could when core was single-threaded.
#if tt_THREAD_SAFE
typedef pthread_mutex_t tt_lock_t; // NOLINT(misc-include-cleaner) - <pthread.h> above

static inline void tt_lock_init(tt_lock_t* lock, bool recursive) {
    pthread_mutexattr_t attr; // NOLINT(misc-include-cleaner)
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, recursive ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_NORMAL);
    pthread_mutex_init(lock, &attr);
    pthread_mutexattr_destroy(&attr);
}
static inline bool tt_lock_try(tt_lock_t* lock) {
    return pthread_mutex_trylock(lock) == 0;
}
static inline void tt_lock_acquire(tt_lock_t* lock) {
    pthread_mutex_lock(lock);
}
#define TT_LOCK_NS_PER_S 1000000000ULL

// Waits at most timeout_ns for the lock; true if it was taken.
static inline bool tt_lock_acquire_timed(tt_lock_t* lock, uint64_t timeout_ns) {
    struct timespec deadline;
    // pthread_mutex_timedlock() measures CLOCK_REALTIME; glibc defines it in bits/time.h, behind <time.h>
    clock_gettime(CLOCK_REALTIME, &deadline); // NOLINT(misc-include-cleaner)
    uint64_t nsec = (uint64_t)deadline.tv_nsec + (timeout_ns % TT_LOCK_NS_PER_S);
    deadline.tv_sec += (time_t)(timeout_ns / TT_LOCK_NS_PER_S) + (time_t)(nsec / TT_LOCK_NS_PER_S);
    deadline.tv_nsec = (long)(nsec % TT_LOCK_NS_PER_S);
    return pthread_mutex_timedlock(lock, &deadline) == 0;
}
static inline void tt_lock_release(tt_lock_t* lock) {
    pthread_mutex_unlock(lock);
}
static inline void tt_lock_destroy(tt_lock_t* lock) {
    pthread_mutex_destroy(lock);
}
// The calling thread, as a number no live thread shares and that is never 0. The node's state lock
// records its owner with this, so a callback re-entering core on the thread that already holds it costs
// a compare rather than an atomic (tickle.c, state_lock()).
static inline uintptr_t tt_thread_self(void) {
    return (uintptr_t)pthread_self();
}
// Gives the CPU to another runnable thread (state_lock()'s hand-off to a waiting poller, tickle.c). Linux schedules
// preemptively across cores, so a thread that yields in a loop cannot keep the one it waits for from running.
#define tt_HAL_THREAD_YIELD 1
static inline void tt_thread_yield(void) {
    (void)sched_yield();
}
#else
typedef struct {
    char unused;
} tt_lock_t;
static inline void tt_lock_init(tt_lock_t* lock, bool recursive) {
    (void)lock;
    (void)recursive;
}
static inline bool tt_lock_try(tt_lock_t* lock) {
    (void)lock;
    return true;
}
static inline void tt_lock_acquire(tt_lock_t* lock) {
    (void)lock;
}
static inline bool tt_lock_acquire_timed(tt_lock_t* lock, uint64_t timeout_ns) {
    (void)lock;
    (void)timeout_ns;
    return true;
}
static inline void tt_lock_release(tt_lock_t* lock) {
    (void)lock;
}
static inline void tt_lock_destroy(tt_lock_t* lock) {
    (void)lock;
}
static inline uintptr_t tt_thread_self(void) {
    return 1;
}
#define tt_HAL_THREAD_YIELD 0 // one thread: nobody to yield to
static inline void tt_thread_yield(void) {
}
#endif

// Linux-specific hardware abstraction layer structure
// How a busy poll loop learns that a datagram arrived (tt_rx_maybe_ready(), README.md "io_uring"):
//   tt_RX_HINT_AUTO  0, the default: io_uring when the kernel allows it; when it is refused (Docker's default
//                    seccomp profile, Kubernetes RuntimeDefault, kernel.io_uring_disabled, Linux < 5.1) one warning,
//                    and every check reads the socket instead. Default because it works for a user who never read
//                    README.md, at the price of ~1.5 KB of code that tt_RX_HINT_READ leaves out.
//   tt_RX_HINT_READ  1: never io_uring - every check reads the socket, and none of the io_uring code is compiled.
//   tt_RX_HINT_URING 2: io_uring or nothing - a refusal fails context creation with tt_RET_UNSUPPORTED, so a
//                    deployment that must have it finds out at start-up rather than by being slower.
#define tt_RX_HINT_AUTO 0
#define tt_RX_HINT_READ 1
#define tt_RX_HINT_URING 2
#ifndef tt_HAL_RX_HINT
#define tt_HAL_RX_HINT tt_RX_HINT_AUTO
#endif
#if tt_HAL_RX_HINT != tt_RX_HINT_READ && tt_HAL_RX_HINT != tt_RX_HINT_AUTO && tt_HAL_RX_HINT != tt_RX_HINT_URING
#error "tt_HAL_RX_HINT must be tt_RX_HINT_AUTO (0), tt_RX_HINT_READ (1) or tt_RX_HINT_URING (2)"
#endif
#ifdef tt_HAL_IO_URING
#error "tt_HAL_IO_URING is derived from tt_HAL_RX_HINT; set tt_HAL_RX_HINT instead"
#endif
#define tt_HAL_IO_URING (tt_HAL_RX_HINT != tt_RX_HINT_READ)

struct tt_hal {
    // The well-known port (_tt_CONFIG.port), shared with every other node on this host via
    // SO_REUSEADDR. Broadcasts are addressed here, so this is how a node is reached before anyone
    // knows anything about it. Receive-only in practice: nothing is ever sent from it.
    int sock;
    // This node's own data port, bound to whatever the kernel hands out. Everything is sent from
    // here, which is what makes a peer addressable: upsert_peer() records a peer at the source
    // port of the packet it was heard on, so every peer learns this node's own port for free and
    // a unicast can then reach this node specifically.
    //
    // Why it has to exist (2026-09-23, measured, not reasoned): with every node bound only to the
    // shared port, two nodes on one host share an address, and a unicast to it is delivered by the
    // kernel to exactly one of the two sockets - which may be the sender's own. Measured directly
    // with two sockets bound to one UDP port with SO_REUSEADDR: a broadcast reaches both, a
    // unicast reaches one. In the rmw_tickle benchmark that showed up as a Publisher receiving
    // 10009 of its own 10010 datagrams while its Subscriber got 25, and - far worse - as roughly
    // 10% of the stream going to the sender in runs that passed and were recorded as clean.
    int data_sock;
    struct sockaddr_in broadcast_addr; // Precomputed once in tt_bind(), reused by every tt_send()
    // eventfd(2): tt_receive()'s poll() watches this alongside sock, and tt_wake_signal() writes
    // to it to interrupt a blocked receive. Deliberately not a loopback UDP socket the way
    // hal_freertos.h's own wake_sock is (the two HALs otherwise mirror each other closely) -
    // platform/linux/test.sh runs each side in its own network namespace with only the veth pair
    // brought up, not `lo` (see netns.mk), so binding anything to 127.0.0.1 there fails with
    // EADDRNOTAVAIL. eventfd needs no address or interface at all. -1 before tt_bind() creates it
    // (or if creation fails partway through), so tt_close() knows not to close it.
    int wake_fd;
    // The read end of this context's segment doorbell (tt_segment_bell_create()), stored PLUS ONE: 0 means none, so a
    // tt_hal that was only zeroed - test_poll_signal.c builds one that way - does not poll standard input, which is
    // what a plain fd field of 0 would be. In tt_receive()'s ppoll set always, as fd - 1: ppoll ignores -1.
    int bell_fd_plus1;
    // tt_receive()'s wait set kept in the kernel (epoll), stored PLUS ONE for the same reason as bell_fd_plus1: 0 means
    // none, and tt_receive() then builds a ppoll() set per call as it always did. ppoll() registers on and then leaves
    // the wait queue of every descriptor in its set on every call - four of them with the segment - which is work per
    // sleep for a set that never changes; epoll registers each once (hal_linux.c, "The wait set").
    int epoll_fd_plus1;
#if tt_SEGMENT_ENABLED
    // The bell in that set is edge-triggered, so a ring needs no read() to stop it being reported: a pipe reports
    // every write to an epoll that watches it, empty or not, which tt_segment_bell_create() checks on the running
    // kernel before relying on it (bell_edge_works()). The bytes rings leave behind are read only when they could
    // otherwise fill the pipe: every bell_drain_every sleep generations (struct tt_Context.segment_sleep_generation),
    // derived from the pipe's capacity and the most writers that can ring one generation - not a tuned interval.
    // 0: the kernel failed the check, or there is no epoll, and every ring is read as it always was.
    uint32_t bell_drain_every;
    uint32_t bell_drained_at; // the sleep generation at the last read of the bell
    uint64_t bell_drains;     // reads of the bell, for the test that a ring costs none (bell_wake_check.c)
#endif
    // Which socket gets first refusal on the next read, alternating. Without it, preferring one
    // socket whenever both are ready is not merely a delay: under a sustained stream on the
    // preferred socket the other is never read at all. That matters most exactly where it is
    // least visible - a Publisher above tt_UNICAST_PEER_THRESHOLD broadcasts its data while the
    // ACKNACKs and retransmit requests answering it arrive as unicast on the data socket, so the
    // starved path would be RELIABLE recovery. Alternating bounds the wait at one datagram.
    bool rx_prefer_data;
    // Sockets known to have nothing waiting until the next wait (TT_RX_IDLE_* bits, hal_linux.c): the
    // ones ppoll() did not report ready, and any a non-blocking read has since found empty. drain_rx()
    // then reads only what is there - see tt_try_receive().
    uint8_t rx_idle;
#define TT_RX_IDLE_WELL_KNOWN 1U
#define TT_RX_IDLE_DATA 2U
    // When the first datagram was returned while an rx_idle bit was set, by the running poll's clock (0: none
    // yet). A skipped socket is asked again once TT_RX_IDLE_RECHECK_NS has passed since, because "skipped until
    // the next wait" assumed a drain session ends: a socket refilled faster than it is read - a max-rate
    // broadcaster's own looped-back datagrams on the well-known one - kept one session going for a whole 20 s
    // run, and the data socket carrying every discovery reply and ACKNACK was never read again (2026-10-05, rmw
    // KEEP_ALL on the rig). A time, not a count of datagrams, so the bound does not stretch with the hardware.
    uint64_t rx_idle_since_ns;
    // Datagrams the last recvmmsg() read beyond the one it returned, handed out before any further wait
    // or read (rx_next of rx_count), all from one socket (rx_from_data). A pending datagram is always
    // returned before ppoll() is entered, so batching never holds one back behind a wait.
    uint16_t rx_count;
    uint16_t rx_next;
    bool rx_from_data;
    int32_t rx_len[tt_RX_BATCH > 1 ? tt_RX_BATCH - 1 : 1];
    uint32_t rx_ip[tt_RX_BATCH > 1 ? tt_RX_BATCH - 1 : 1];
    uint16_t rx_port[tt_RX_BATCH > 1 ? tt_RX_BATCH - 1 : 1];
    uint8_t rx_batch[tt_RX_BATCH > 1 ? tt_RX_BATCH - 1 : 1][tt_MAX_BUFFER_LENGTH];
    // recvmmsg()'s headers, set up once (rx_fill(), hal_linux.c) rather than per call: a call touches only
    // the entries the kernel filled. rx_headers_for is the node they were set up in, so a node that has
    // been moved since re-points them instead of writing through stale pointers.
    struct tt_mmsghdr rx_msgs[tt_RX_BATCH];
    struct iovec rx_iov[tt_RX_BATCH]; // NOLINT(misc-include-cleaner) - <sys/socket.h> above provides it
    struct sockaddr_in rx_addr[tt_RX_BATCH];
    const void* rx_headers_for;
    // How full the batches run, for sizing tt_RX_BATCH: recvmmsg() calls that read something, the
    // datagrams they read, and how many of them came back with every slot filled.
    uint64_t rx_batch_calls;
    uint64_t rx_batch_datagrams;
    uint64_t rx_batch_full;
    // Send offload (hal_linux.c "UDP offload"): runs of same-size datagrams to one destination go as one UDP_SEGMENT
    // send. Off with TT_UDP_OFFLOAD=0, on a kernel without it, and after the first refusal (no checksum offload).
    bool gso_on;
#if TT_HAL_UDP_GRO
    // The last merged read (UDP_GRO): gro_end bytes in gro_base, cut every gro_segment bytes (the last cut may be
    // shorter), handed out from gro_next; gro_left segments still to go, all from one sender on one socket. gro_on:
    // the sockets were asked to merge (tt_bind()), so reads take recvmsg() with a control buffer for the size.
    bool gro_on;
    bool gro_from_data;
    uint16_t gro_left;
    uint16_t gro_port;
    uint32_t gro_ip;
    uint32_t gro_segment;
    uint32_t gro_next;
    uint32_t gro_end;
    uint8_t* gro_base;
#endif
#if tt_HAL_IO_URING
    // tt_rx_maybe_ready() (hal_linux.c): an io_uring whose only job is one-shot POLLIN on the two receive sockets, so
    // a busy loop learns that a datagram arrived from a completion in shared memory rather than by asking the kernel.
    // uring_fd < 0 when the kernel refused it (seccomp, kernel.io_uring_disabled, an old kernel) - then every answer
    // is "may be", which is the behaviour without it. Bit 0 the well-known socket, bit 1 the data socket.
    int uring_fd;
    uint8_t uring_armed; // a poll is in flight for this socket: no completion yet means nothing has arrived
    void* uring_sq_map;
    size_t uring_sq_map_len;
    void* uring_cq_map; // == uring_sq_map when the kernel maps both rings at once
    size_t uring_cq_map_len;
    void* uring_sqes;
    size_t uring_sqes_len;
    uint32_t* uring_sq_tail;
    uint32_t* uring_sq_mask;
    uint32_t* uring_sq_array;
    uint32_t* uring_cq_head;
    uint32_t* uring_cq_tail;
    uint32_t* uring_cq_mask;
    void* uring_cqes;
    uint64_t uring_arms;    // poll submissions, each one io_uring_enter() - the cost side of the trade
    uint64_t uring_skipped; // tt_rx_maybe_ready() answers of "no", each one a read that was not made
#endif
#if tt_CONTEXT_ID_CLAIM
    // (g8) The data socket's own address, host order, read back after tt_bind() (the link's address when the socket
    // is bound to any address), and the id this context holds in the host registry (0: none).
    uint32_t own_ip;
    uint16_t own_port;
    uint8_t claimed_id;
#endif
};

#if tt_CONTEXT_ID_CLAIM
// (g8) The host registry behind tt_claim_context_id(), as plain functions of a file, so they can be tested alone:
// tt_MAX_CONTEXT_IDS int32_t pids, indexed by id, read and written under flock(). A pid that is not alive
// (kill(pid, 0) fails with ESRCH) holds nothing; one that cannot be checked (EPERM, another pid namespace) counts as
// alive - the link settles a mistake. `path` NULL, or a file that cannot be opened: the choice is made from `avoid`
// alone. Returns the id claimed for `pid`, or 0.
uint8_t tt_id_registry_claim(const char* path, uint8_t preferred, const uint8_t* avoid, uint32_t salt, int32_t pid);
void tt_id_registry_release(const char* path, uint8_t id, int32_t pid);
#endif
