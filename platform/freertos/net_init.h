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

// Starts lwIP's tcpip thread and brings up this milestone's netif, then returns. Must be called
// from a running FreeRTOS task (not before vTaskStartScheduler()) - it blocks on a semaphore
// that only the tcpip thread can signal.
//
// Milestone 2's netif is a software loopback: it has a real static IP/broadcast address and
// goes through lwIP's actual UDP/IP encode+decode path, but every packet sent on it is queued
// straight back to the same netif's input via lwIP's own netif_loop_output() instead of reaching
// any real link. It exists to prove hal_freertos.c's socket calls work end-to-end before
// milestone 3 replaces it with the real virtio-net driver.
void net_init(void);
