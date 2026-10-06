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

#include <stdbool.h> // rx_prefer_data
#include <stdint.h>

#include <tickle/config.h>

// See hal_linux.h's tt_lock_t - the same operations over a FreeRTOS mutex, statically allocated so a
// node needs no heap for it. Recursive and plain mutexes are separate object kinds in FreeRTOS, with
// separate take/give calls, so the lock remembers which one it is.
//
// pdTRUE, pdMS_TO_TICKS, TickType_t and portMAX_DELAY are defined in FreeRTOS's projdefs.h and the
// port's portmacro.h, which FreeRTOS supports reaching only through FreeRTOS.h - hence the NOLINTs.
#if tt_THREAD_SAFE
#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>

typedef struct {
    StaticSemaphore_t storage;
    SemaphoreHandle_t handle;
    bool recursive;
} tt_lock_t;

static inline void tt_lock_init(tt_lock_t* lock, bool recursive) {
    lock->recursive = recursive;
    lock->handle =
        recursive ? xSemaphoreCreateRecursiveMutexStatic(&lock->storage) : xSemaphoreCreateMutexStatic(&lock->storage);
}
static inline bool tt_lock_try(tt_lock_t* lock) {
    return (lock->recursive ? xSemaphoreTakeRecursive(lock->handle, 0) : xSemaphoreTake(lock->handle, 0)) ==
           pdTRUE; // NOLINT(misc-include-cleaner)
}
static inline void tt_lock_acquire(tt_lock_t* lock) {
    if (lock->recursive) {
        xSemaphoreTakeRecursive(lock->handle, portMAX_DELAY); // NOLINT(misc-include-cleaner)
    } else {
        xSemaphoreTake(lock->handle, portMAX_DELAY); // NOLINT(misc-include-cleaner)
    }
}
static inline bool tt_lock_acquire_timed(tt_lock_t* lock, uint64_t timeout_ns) {
    TickType_t ticks = pdMS_TO_TICKS((uint32_t)(timeout_ns / 1000000ULL)); // NOLINT(misc-include-cleaner)
    return (lock->recursive ? xSemaphoreTakeRecursive(lock->handle, ticks) : xSemaphoreTake(lock->handle, ticks)) ==
           pdTRUE; // NOLINT(misc-include-cleaner)
}
static inline void tt_lock_release(tt_lock_t* lock) {
    if (lock->recursive) {
        xSemaphoreGiveRecursive(lock->handle);
    } else {
        xSemaphoreGive(lock->handle);
    }
}
static inline void tt_lock_destroy(tt_lock_t* lock) {
    vSemaphoreDelete(lock->handle);
}
// See hal_linux.h's tt_thread_self(): the current task's handle, never NULL once the scheduler runs.
static inline uintptr_t tt_thread_self(void) {
    return (uintptr_t)xTaskGetCurrentTaskHandle();
}
// Not used for state_lock()'s hand-off: taskYIELD() only yields to tasks of equal or higher priority, so a
// higher-priority task yielding in a loop for a lower-priority poller would wait for ever. Priority already decides
// who runs here; the hand-off is compiled out (tickle.c).
#define tt_HAL_THREAD_YIELD 0
static inline void tt_thread_yield(void) {
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
#define tt_HAL_THREAD_YIELD 0
static inline void tt_thread_yield(void) {
}
#endif

// How this HAL talks to lwIP (2026-10-06, ROADMAP "Now" 7):
//   1, the default: the netconn API. Each socket is a netconn made with netconn_new_with_callback(), and the callback
//      counts arrivals and gives one semaphore. tt_receive() waits on that semaphore, which also carries
//      tt_wake_signal() - so there is no loopback wake socket - and tt_try_receive()/tt_rx_maybe_ready() answer
//      "nothing waiting" from the count without entering lwIP at all. The socket layer cannot do this: sockets.c
//      installs its own netconn callback (DEFAULT_SOCKET_EVENTCB) and offers no way to replace it, so a socket's
//      arrivals are visible only to select().
//   0: the BSD socket API (LWIP_COMPAT_SOCKETS), the HAL as it was before - select() on two sockets and a loopback
//      wake socket, and tt_rx_maybe_ready() always "maybe". Kept as the selftest's control (platform/freertos/test.sh
//      runs it on both) and for a port that wants TickLE on the same socket layer as the rest of its code.
#ifndef tt_HAL_FREERTOS_NETCONN
#define tt_HAL_FREERTOS_NETCONN 1
#endif

#if tt_HAL_FREERTOS_NETCONN
#include <FreeRTOS.h>
#include <semphr.h>

#include <lwip/api.h>
#include <lwip/ip_addr.h>
#else
#include <lwip/sockets.h>
#endif

// The two receive sockets, as indexes into struct tt_hal.rx_arrivals.
#define tt_HAL_RX_WELL_KNOWN 0
#define tt_HAL_RX_DATA 1

// FreeRTOS+lwIP hardware abstraction layer structure. The socket fields are named after hal_linux.h's, whose comments
// give the measured reasons the second socket exists; the netconn build keeps the same pair under the same roles.
struct tt_hal {
#if tt_HAL_FREERTOS_NETCONN
    // The well-known port (_tt_CONFIG.port, wildcard address) and this node's own data port - see hal_linux.h's
    // sock/data_sock. Every send goes out of data_conn.
    struct netconn* conn;
    struct netconn* data_conn;
    ip_addr_t broadcast_ip; // _tt_CONFIG.broadcast, parsed once in tt_bind()
    // Datagrams lwIP has queued on each netconn and this HAL has not yet taken, kept by the netconn callback: +1 per
    // NETCONN_EVT_RCVPLUS (posted by the tcpip thread after the datagram is in the receive mailbox), -1 per
    // NETCONN_EVT_RCVMINUS (raised by netconn_recv in the reading task). Zero means nothing has arrived since this HAL
    // last took one, which is what lets tt_rx_maybe_ready() say "no" without calling into lwIP. Atomic: written by two
    // tasks.
    uint32_t rx_arrivals[2];
    // Each netconn's callback_arg: the node, and which of the two netconns it is - set before the netconn is bound,
    // so the callback never has to work out which one called it.
    struct tt_HalRxTag {
        struct tt_Context* node;
        uint8_t which; // tt_HAL_RX_WELL_KNOWN or tt_HAL_RX_DATA
    } rx_tags[2];
    // tt_wake_signal()'s flag: set before rx_sem is given, taken (exchanged for 0) by tt_receive().
    uint8_t wake_pending;
    // Given on every arrival and every tt_wake_signal(); tt_receive() blocks on it. Binary: a give that finds it
    // already given is one wake-up, and tt_receive() re-reads the counts and the flag after every take anyway.
    SemaphoreHandle_t rx_sem;
    StaticSemaphore_t rx_sem_storage;
#else
    int sock;
    int data_sock;
    struct sockaddr_in broadcast_addr; // Precomputed once in tt_bind(), reused by every tt_send()
    // Mirrors hal_linux.h's own wake_sock/wake_addr - see its comment. lwIP has
    // LWIP_NETIF_LOOPBACK enabled (platform/freertos/lwipopts.h), so the same loopback-UDP-socket
    // trick works here, watched by the same select() as the real sockets.
    int wake_sock;
    struct sockaddr_in wake_addr;
#endif
    // See hal_linux.h's own rx_prefer_data - same alternation, same reason.
    bool rx_prefer_data;
    // What the receive path cost, read by platform/freertos/main.c's selftest; both builds count the same things.
    // Datagrams handed to core by tt_receive()/tt_try_receive():
    uint32_t rx_reads;
    // Non-blocking reads that entered lwIP (a select() or a NETCONN_DONTBLOCK receive) and found nothing:
    uint32_t rx_empty_reads;
    // Checks the arrival count answered "nothing" without entering lwIP (netconn build only):
    uint32_t rx_quiet;
};
