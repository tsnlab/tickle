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

#include <lwip/sockets.h>
#include <tickle/config.h>

// See hal_linux.h's tt_lock_t - the same five operations over a FreeRTOS mutex, statically allocated
// so a node needs no heap for it. Recursive and plain mutexes are separate object kinds in FreeRTOS,
// with separate take/give calls, so the lock remembers which one it is.
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
    return (lock->recursive ? xSemaphoreTakeRecursive(lock->handle, 0) : xSemaphoreTake(lock->handle, 0)) == pdTRUE;
}
static inline void tt_lock_acquire(tt_lock_t* lock) {
    if (lock->recursive) {
        xSemaphoreTakeRecursive(lock->handle, portMAX_DELAY);
    } else {
        xSemaphoreTake(lock->handle, portMAX_DELAY);
    }
}
static inline bool tt_lock_acquire_timed(tt_lock_t* lock, uint64_t timeout_ns) {
    TickType_t ticks = pdMS_TO_TICKS((uint32_t)(timeout_ns / 1000000ULL));
    return (lock->recursive ? xSemaphoreTakeRecursive(lock->handle, ticks) : xSemaphoreTake(lock->handle, ticks)) ==
           pdTRUE;
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

// FreeRTOS+lwIP hardware abstraction layer structure - same shape as hal_linux.h's, since
// src/hal_freertos.c mirrors src/hal_linux.c almost line for line (lwIP's LWIP_COMPAT_SOCKETS
// option aliases socket()/bind()/sendto()/... straight onto lwip_socket()/lwip_bind()/... with
// the same signatures as the real POSIX calls).
struct tt_hal {
    // Mirrors hal_linux.h's own sock/data_sock split - see its comments for the measured reason
    // the second socket exists. The hazard is not Linux-specific: it is what happens whenever two
    // nodes share a host and therefore an address, so this target gets the same treatment rather
    // than an exemption it would have to be remembered.
    int sock;
    int data_sock;
    struct sockaddr_in broadcast_addr; // Precomputed once in tt_bind(), reused by every tt_send()
    // Mirrors hal_linux.h's own wake_sock/wake_addr - see its comment. lwIP has
    // LWIP_NETIF_LOOPBACK enabled (platform/freertos/lwipopts.h), so the same loopback-UDP-socket
    // trick works here unchanged, keeping this file's own tt_receive() select()-based rather than
    // needing a different, FreeRTOS-specific wake mechanism.
    int wake_sock;
    struct sockaddr_in wake_addr;
    // See hal_linux.h's own rx_prefer_data - same alternation, same reason.
    bool rx_prefer_data;
};
