/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 *
 * The virtqueue/virtio-mmio protocol sequence below (status negotiation order, register
 * offsets, split-ring descriptor/avail/used layout) is ported from U-Boot's
 * drivers/virtio/{virtio_net,virtio_mmio,virtio_ring}.c, Copyright (C) 2018 Tuomas Tynkkynen
 * <tuomas.tynkkynen@iki.fi> and Bin Meng <bmeng.cn@gmail.com>, originally
 * SPDX-License-Identifier: GPL-2.0-or-later - relicensed here under this file's
 * GPL-3.0-or-later, which "-or-later" permits.
 *
 * Stripped out relative to that reference, and why: U-Boot's own driver-model glue (struct
 * udevice/uclass - replaced by the two plain functions below), DMA bounce buffers and cache
 * maintenance (QEMU TCG has no real cache-coherency or IOMMU to work around), legacy (v1)
 * virtio-mmio and all non-VIRTIO_F_VERSION_1 feature negotiation (QEMU's `-device
 * virtio-net-device` on `-machine virt` is a modern/v2 device; TickLE identifies nodes by IP,
 * not MAC, so VIRTIO_NET_F_MAC is never requested either), interrupt-driven notification
 * (avail.flags always carries VRING_AVAIL_F_NO_INTERRUPT - see net_poll_task in net_init.c for
 * how RX is drained by polling instead), and the guest-byte-order "shadow descriptor" indirection
 * (moot since virtio's modern wire format is little-endian and so is this target).
 */

#include "virtio_net.h"

#include <stdio.h>
#include <string.h>

// QEMU `-machine virt` provides 8 virtio-mmio slots at these fixed addresses; which one(s) are
// actually populated depends on `-device` ordering that isn't guaranteed stable across QEMU
// versions/configurations, so virtio_net_init() below probes all of them for a matching
// device rather than assuming a fixed slot the way a from-scratch driver targeting one known,
// hand-tested QEMU invocation might otherwise get away with.
#define VIRTIO_MMIO_SLOT_BASE 0x10001000UL
#define VIRTIO_MMIO_SLOT_STRIDE 0x1000UL
#define VIRTIO_MMIO_NUM_SLOTS 8

static uintptr_t mmio_base;

#define MMIO_REG(offset) (*(volatile uint32_t*)(mmio_base + (offset)))
#define MMIO_MAGIC_VALUE MMIO_REG(0x000)
#define MMIO_VERSION MMIO_REG(0x004)
#define MMIO_DEVICE_ID MMIO_REG(0x008)
#define MMIO_VENDOR_ID MMIO_REG(0x00c)
#define MMIO_DEVICE_FEATURES MMIO_REG(0x010)
#define MMIO_DEVICE_FEATURES_SEL MMIO_REG(0x014)
#define MMIO_DRIVER_FEATURES MMIO_REG(0x020)
#define MMIO_DRIVER_FEATURES_SEL MMIO_REG(0x024)
#define MMIO_QUEUE_SEL MMIO_REG(0x030)
#define MMIO_QUEUE_NUM_MAX MMIO_REG(0x034)
#define MMIO_QUEUE_NUM MMIO_REG(0x038)
#define MMIO_QUEUE_READY MMIO_REG(0x044)
#define MMIO_QUEUE_NOTIFY MMIO_REG(0x050)
#define MMIO_STATUS MMIO_REG(0x070)
#define MMIO_QUEUE_DESC_LOW MMIO_REG(0x080)
#define MMIO_QUEUE_DESC_HIGH MMIO_REG(0x084)
#define MMIO_QUEUE_AVAIL_LOW MMIO_REG(0x090)
#define MMIO_QUEUE_AVAIL_HIGH MMIO_REG(0x094)
#define MMIO_QUEUE_USED_LOW MMIO_REG(0x0a0)
#define MMIO_QUEUE_USED_HIGH MMIO_REG(0x0a4)

#define VIRTIO_MAGIC ('v' | ('i' << 8) | ('r' << 16) | ('t' << 24))
#define VIRTIO_MMIO_MODERN_VERSION 2
#define VIRTIO_DEVICE_ID_NET 1

#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8

#define VIRTIO_F_VERSION_1 32 // Feature word 1 (bits 32-63), bit 0

#define VQ_NUM_DESC 8 // Must be a power of 2; small since this is a correctness test, not perf
#define VQ_RX 0
#define VQ_TX 1

#define VRING_DESC_F_NEXT 1
#define VRING_DESC_F_WRITE 2
#define VRING_AVAIL_F_NO_INTERRUPT 1

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VQ_NUM_DESC];
};

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
};

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[VQ_NUM_DESC];
};

struct virtio_queue {
    struct vring_desc desc[VQ_NUM_DESC] __attribute__((aligned(16)));
    struct vring_avail avail __attribute__((aligned(2)));
    // volatile: the device writes idx/ring[] asynchronously (from this compiler's point of view,
    // "asynchronously" meaning "with no visible store in this translation unit at all") after
    // queue_notify() - without this, nothing stops the optimizer from proving queue_get_used()'s
    // busy-wait loop in virtio_net_send() never observes used.idx change and hoisting the load
    // out of the loop entirely, turning it into a real infinite loop. (Exactly what happened at
    // -O1 during this driver's own bring-up.)
    volatile struct vring_used used __attribute__((aligned(4)));
    uint16_t free_head;
    uint16_t last_used_idx;
};

static struct virtio_queue queues[2];

// Modern (VIRTIO_F_VERSION_1) header - always zeroed (no checksum offload, no GSO, not
// negotiating VIRTIO_NET_F_MRG_RXBUF) since this driver requests no feature that would need any
// other value here.
struct virtio_net_hdr_v1 {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
};

#define NUM_RX_BUFS VQ_NUM_DESC
#define RX_BUF_SIZE (sizeof(struct virtio_net_hdr_v1) + VIRTIO_NET_MAX_FRAME_SIZE)
static uint8_t rx_bufs[NUM_RX_BUFS][RX_BUF_SIZE];
static struct virtio_net_hdr_v1 tx_hdr;

static void queue_reset(struct virtio_queue* queue) {
    memset(queue, 0, sizeof(*queue));
    for (int i = 0; i < VQ_NUM_DESC - 1; i++) {
        queue->desc[i].next = (uint16_t)(i + 1);
    }
    // We poll both queues ourselves (see net_poll_task in net_init.c and virtio_net_send's own
    // busy-wait below) - never need the device to interrupt us.
    queue->avail.flags = VRING_AVAIL_F_NO_INTERRUPT;
}

// Claims the next free descriptor for one buffer and returns its index. The caller chains
// multiple calls together via queue_submit() below (see virtio_net_send's header+payload pair).
static uint16_t queue_add_desc(struct virtio_queue* queue, void* addr, uint32_t len, uint16_t flags) {
    uint16_t head = queue->free_head;
    queue->desc[head].addr = (uint64_t)(uintptr_t)addr;
    queue->desc[head].len = len;
    queue->desc[head].flags = flags;
    queue->free_head = queue->desc[head].next;
    return head;
}

// Chains descriptor indices desc_idx[0..count-1] (each already filled in by queue_add_desc, in
// the order they should be read/written) into one descriptor chain and publishes it to the
// device via the avail ring.
static void queue_submit(struct virtio_queue* queue, const uint16_t* desc_idx, int count) {
    for (int i = 0; i < count - 1; i++) {
        queue->desc[desc_idx[i]].flags |= VRING_DESC_F_NEXT;
        queue->desc[desc_idx[i]].next = desc_idx[i + 1];
    }
    queue->desc[desc_idx[count - 1]].flags &= ~VRING_DESC_F_NEXT;

    queue->avail.ring[queue->avail.idx % VQ_NUM_DESC] = desc_idx[0];
    __sync_synchronize(); // The descriptor chain must be visible before the device sees idx advance
    queue->avail.idx++;
}

static void queue_notify(int queue_index) {
    MMIO_QUEUE_NOTIFY = (uint32_t)queue_index;
}

// Returns the head descriptor index of the next device-completed chain (freeing its descriptors
// back onto this queue's free list first), or -1 if nothing is ready yet.
//
// Relies on one invariant callers must keep: a chain is always either exactly the fixed RX
// buffer at rx_bufs[<its head index>] (see virtio_net_init()/virtio_net_recv(), which always
// re-submits a drained RX buffer through the very descriptor slot that just freed it, so the
// index<->buffer mapping never drifts) or a TX chain nobody needs to map back to a buffer by
// index at all.
static int queue_get_used(struct virtio_queue* queue, uint32_t* len_out) {
    if (queue->last_used_idx == queue->used.idx) {
        return -1;
    }
    __sync_synchronize(); // Must not read the used entry's contents before idx says it's ready

    const volatile struct vring_used_elem* elem = &queue->used.ring[queue->last_used_idx % VQ_NUM_DESC];
    uint16_t head = (uint16_t)elem->id;
    if (len_out != NULL) {
        *len_out = elem->len;
    }
    queue->last_used_idx++;

    uint16_t i = head;
    while (queue->desc[i].flags & VRING_DESC_F_NEXT) {
        i = queue->desc[i].next;
    }
    queue->desc[i].next = queue->free_head;
    queue->free_head = head;

    return head;
}

static bool setup_queue(int index) {
    MMIO_QUEUE_SEL = (uint32_t)index;
    if (MMIO_QUEUE_NUM_MAX == 0) {
        printf("virtio_net: queue %d not available\n", index);
        return false;
    }
    MMIO_QUEUE_NUM = VQ_NUM_DESC; // Always <= QUEUE_NUM_MAX in practice for a device this simple

    struct virtio_queue* queue = &queues[index];
    queue_reset(queue);

    uint64_t desc_addr = (uint64_t)(uintptr_t)queue->desc;
    uint64_t avail_addr = (uint64_t)(uintptr_t)&queue->avail;
    uint64_t used_addr = (uint64_t)(uintptr_t)&queue->used;

    MMIO_QUEUE_DESC_LOW = (uint32_t)desc_addr;
    MMIO_QUEUE_DESC_HIGH = (uint32_t)(desc_addr >> 32);
    MMIO_QUEUE_AVAIL_LOW = (uint32_t)avail_addr;
    MMIO_QUEUE_AVAIL_HIGH = (uint32_t)(avail_addr >> 32);
    MMIO_QUEUE_USED_LOW = (uint32_t)used_addr;
    MMIO_QUEUE_USED_HIGH = (uint32_t)(used_addr >> 32);
    MMIO_QUEUE_READY = 1;

    return true;
}

// Scans every virtio-mmio slot QEMU's `-machine virt` provides for one presenting a modern (v2)
// virtio-net device, and sets mmio_base to it. Slots with no device attached read back
// DEVICE_ID 0 (a defined "empty placeholder" per the virtio-mmio spec, not an error) - those are
// silently skipped, not logged.
static bool find_virtio_net_slot(void) {
    for (int i = 0; i < VIRTIO_MMIO_NUM_SLOTS; i++) {
        mmio_base = VIRTIO_MMIO_SLOT_BASE + ((uintptr_t)i * VIRTIO_MMIO_SLOT_STRIDE);

        if (MMIO_MAGIC_VALUE != VIRTIO_MAGIC || MMIO_DEVICE_ID != VIRTIO_DEVICE_ID_NET) {
            continue;
        }
        if (MMIO_VERSION != VIRTIO_MMIO_MODERN_VERSION) {
            printf("virtio_net: found virtio-net at 0x%lx, but as legacy (v%u) - only modern/v2 is "
                   "supported (pass -global virtio-mmio.force-legacy=off to QEMU)\n",
                   mmio_base, (unsigned int)MMIO_VERSION);
            continue;
        }

        printf("virtio_net: found device at 0x%lx\n", mmio_base);
        return true;
    }

    printf("virtio_net: no modern virtio-net device found in any of the %d virtio-mmio slots\n", VIRTIO_MMIO_NUM_SLOTS);
    return false;
}

bool virtio_net_init(void) {
    if (!find_virtio_net_slot()) {
        return false;
    }

    MMIO_STATUS = 0; // Reset
    MMIO_STATUS = VIRTIO_STATUS_ACKNOWLEDGE;
    MMIO_STATUS |= VIRTIO_STATUS_DRIVER;

    // Feature word 1 (bits 32-63) is the only one with anything we might want
    // (VIRTIO_F_VERSION_1 itself); word 0 is left entirely unnegotiated.
    MMIO_DEVICE_FEATURES_SEL = 1;
    uint32_t features_hi = MMIO_DEVICE_FEATURES;
    if (!(features_hi & (1U << (VIRTIO_F_VERSION_1 - 32)))) {
        printf("virtio_net: device doesn't offer VIRTIO_F_VERSION_1 - legacy-only devices aren't supported\n");
        MMIO_STATUS = 0;
        return false;
    }

    MMIO_DRIVER_FEATURES_SEL = 1;
    MMIO_DRIVER_FEATURES = (1U << (VIRTIO_F_VERSION_1 - 32));
    MMIO_DRIVER_FEATURES_SEL = 0;
    MMIO_DRIVER_FEATURES = 0;

    MMIO_STATUS |= VIRTIO_STATUS_FEATURES_OK;
    if (!(MMIO_STATUS & VIRTIO_STATUS_FEATURES_OK)) {
        printf("virtio_net: device rejected our feature set\n");
        MMIO_STATUS = 0;
        return false;
    }

    if (!setup_queue(VQ_RX) || !setup_queue(VQ_TX)) {
        MMIO_STATUS = 0;
        return false;
    }

    for (int i = 0; i < NUM_RX_BUFS; i++) {
        uint16_t idx = queue_add_desc(&queues[VQ_RX], rx_bufs[i], sizeof(rx_bufs[i]), VRING_DESC_F_WRITE);
        queue_submit(&queues[VQ_RX], &idx, 1);
    }
    queue_notify(VQ_RX);

    MMIO_STATUS |= VIRTIO_STATUS_DRIVER_OK;

    printf("virtio_net: up (device id=%u, vendor=0x%08x)\n", (unsigned int)MMIO_DEVICE_ID,
           (unsigned int)MMIO_VENDOR_ID);
    return true;
}

void virtio_net_send(const void* frame, uint16_t len) {
    memset(&tx_hdr, 0, sizeof(tx_hdr));

    uint16_t desc_idx[2];
    desc_idx[0] = queue_add_desc(&queues[VQ_TX], &tx_hdr, sizeof(tx_hdr), 0);
    desc_idx[1] = queue_add_desc(&queues[VQ_TX], (void*)(uintptr_t)frame, len, 0);
    queue_submit(&queues[VQ_TX], desc_idx, 2);
    queue_notify(VQ_TX);

    // Synchronous by design (see this driver's file-level comment) - busy-wait for the device to
    // consume it rather than returning early and risking tx_hdr/frame being reused too soon.
    while (queue_get_used(&queues[VQ_TX], NULL) < 0) {
    }
}

int32_t virtio_net_recv(uint8_t* buf, uint16_t buf_len) {
    uint32_t len = 0;
    int head = queue_get_used(&queues[VQ_RX], &len);

    int32_t frame_len = -1;
    if (head >= 0 && len > sizeof(struct virtio_net_hdr_v1)) {
        frame_len = (int32_t)(len - sizeof(struct virtio_net_hdr_v1));
        if (frame_len <= buf_len) {
            memcpy(buf, rx_bufs[head] + sizeof(struct virtio_net_hdr_v1), (size_t)frame_len);
        } else {
            frame_len = -1; // Drop: shouldn't happen with VIRTIO_NET_MAX_FRAME_SIZE-sized callers
        }
    }

    if (head >= 0) {
        // Re-queue this exact buffer through this exact descriptor slot - see queue_get_used()'s
        // comment on why that mapping has to stay this way.
        uint16_t idx = queue_add_desc(&queues[VQ_RX], rx_bufs[head], sizeof(rx_bufs[head]), VRING_DESC_F_WRITE);
        queue_submit(&queues[VQ_RX], &idx, 1);
        queue_notify(VQ_RX);
    }

    return frame_len;
}
