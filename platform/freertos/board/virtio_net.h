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

#include <stdbool.h>
#include <stdint.h>

// Minimal polling-only virtio-net driver: modern (v2) virtio-mmio transport only, no
// interrupts (both queues are set up with VRING_AVAIL_F_NO_INTERRUPT - see net_poll_task in
// net_init.c for how RX is drained instead), no offloads/mergeable-buffers negotiated. Ported
// from U-Boot's drivers/virtio/{virtio_net,virtio_mmio,virtio_ring}.c (GPL-2.0-or-later,
// (C) 2018 Tuomas Tynkkynen, Bin Meng - relicensed here under this project's GPL-3.0-or-later,
// which "-or-later" permits) - see virtio_net.c for what was stripped out and why.

// Probes the fixed QEMU `virt` virtio-mmio slot this platform targets, negotiates the one
// feature bit modern transport requires, and sets up both queues. Logs and returns false if no
// virtio-net device is found there (e.g. QEMU wasn't given `-device virtio-net-device`).
bool virtio_net_init(void);

// Sends one already Ethernet-framed packet (14-byte header + payload). Blocks (busy-polls the TX
// queue) until the device has consumed it - matches this driver's synchronous, interrupt-free
// design; never partially sends.
void virtio_net_send(const void* frame, uint16_t len);

// Non-blocking: copies the next received Ethernet frame (if any) into `buf` (which must be at
// least VIRTIO_NET_MAX_FRAME_SIZE bytes) and returns its length, or -1 if none is pending right
// now. The drained receive buffer is re-queued before returning, whether or not a frame was
// found.
int32_t virtio_net_recv(uint8_t* buf, uint16_t buf_len);

#define VIRTIO_NET_MAX_FRAME_SIZE 1518 // 14 (Ethernet header) + 1500 (MTU) + 4 (FCS, unused here)
