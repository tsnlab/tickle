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
#endif

// Linux-specific hardware abstraction layer structure
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
};
