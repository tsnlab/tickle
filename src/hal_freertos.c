/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#include <assert.h> // static_assert
// NOLINTNEXTLINE(misc-include-cleaner) -- picolibc routes EINTR/EAGAIN/EWOULDBLOCK through <sys/errno.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <lwip/def.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/hal_freertos.h> // tt_HAL_FREERTOS_NETCONN, tt_HAL_RX_*
#include <tickle/tickle.h>

// After tickle/hal.h, which decides tt_HAL_FREERTOS_NETCONN (hal_freertos.h).
#if tt_HAL_FREERTOS_NETCONN || tt_SEGMENT_ENABLED
// TickType_t, portMAX_DELAY, pdTRUE, configTICK_RATE_HZ and configSUPPORT_DYNAMIC_ALLOCATION live in projdefs.h,
// portmacro.h and FreeRTOSConfig.h, which FreeRTOS supports reaching only through FreeRTOS.h - hence the suppression
// here and the one around their uses (the same reason as hal_freertos.h's).
#include <FreeRTOS.h> // NOLINT(misc-include-cleaner)
#include <task.h>     // taskENTER_CRITICAL (the segment's tables), vTaskSetTimeOutState (tt_receive())
#endif
#if tt_SEGMENT_ENABLED
#include <portable.h> // pvPortMalloc(), vPortFree()
#endif
#if tt_HAL_FREERTOS_NETCONN
#include <semphr.h>

#include <lwip/api.h>
#include <lwip/err.h>
#include <lwip/ip.h>
#include <lwip/ip_addr.h>
#include <lwip/netbuf.h>
#include <lwip/pbuf.h>
#include <lwip/udp.h>
#else
#include <lwip/sockets.h>
#endif

#include "log.h"

struct _tt_Config _tt_CONFIG = {
    .addr = _tt_CONTEXT_ADDRESS,
    .port = _tt_CONTEXT_PORT,
    .broadcast = _tt_CONTEXT_BROADCAST,
    .context_id = tt_CONTEXT_ID_INVALID, // auto-detect by default; see its own comment in config.h
    .large_fragmentation = tt_LARGE_FRAGMENTATION_AUTO,
    .ip_reassembly_max = _tt_IP_REASSEMBLY_MAX,
};

// QEMU's `-machine virt` CLINT counts at a fixed 10MHz (RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ) -
// must match FreeRTOSConfig.h's configCPU_CLOCK_HZ, which platform/freertos/FreeRTOSConfig.h
// sets from the same constant (kept separate, rather than including FreeRTOS.h here, so this
// HAL file's only real dependency stays lwIP + raw CLINT registers). There's no RTC on this
// target, so this reads the same free-running hardware counter FreeRTOS's own tick interrupt is
// derived from - monotonic, as hal_linux.c's clock_gettime(CLOCK_MONOTONIC).
#define CLINT_MTIME ((volatile uint64_t*)0x0200BFF8UL)
#define CLINT_TIMEBASE_HZ 10000000ULL
#define CLINT_NS_PER_TICK (tt_SECOND / CLINT_TIMEBASE_HZ)

uint64_t tt_get_ns(void) {
    return *CLINT_MTIME * CLINT_NS_PER_TICK;
}

// No RTC and no SNTP client on this target, so the wire's timestamps are the same counter: time since boot. A board
// that keeps the date (an RTC, lwIP's SNTP app) returns it here, in ns since the Unix epoch, to stamp samples that a
// host with a real date can place - only this function changes; every deadline stays on tt_get_ns().
uint64_t tt_get_wall_ns(void) {
    return tt_get_ns();
}

// A timed wait here ends on an RTOS tick (wait_ticks(), below) - or, in the socket build, on lwIP's select(), which
// waits whole milliseconds - so that, not the CLINT's 100 ns, is the finest step its lateness can be told in.
uint64_t tt_timer_resolution_ns(void) {
#if tt_HAL_FREERTOS_NETCONN
    uint64_t tick_ns = tt_SECOND / configTICK_RATE_HZ; // NOLINT(misc-include-cleaner) - see the FreeRTOS.h include
#else
    uint64_t tick_ns = tt_MILLISECOND;
#endif
    return tick_ns > CLINT_NS_PER_TICK ? tick_ns : CLINT_NS_PER_TICK;
}

int32_t tt_get_node_id(void) {
    // Unlike hal_linux.c (which enumerates every interface with getifaddrs() to find the one on
    // the broadcast network), this target has exactly one netif, brought up statically by
    // net_init.c before tt_Context_create() ever runs - so there's nothing to search.
    if (netif_default == NULL) {
        TT_LOG_ERROR("No default netif - net_init() must run before tt_Context_create()");
        return -1;
    }

    // ip4_addr_get_u32() returns the address in network byte order (like sin_addr.s_addr on
    // Linux); on this little-endian target, shifting that raw value right by 24 bits lands on
    // the last transmitted octet - the same "node ID = last byte of the IP" convention
    // hal_linux.c uses for the (simpler, single-interface) common case.
    uint32_t addr = ip4_addr_get_u32(netif_ip4_addr(netif_default));
    return (int32_t)(addr >> 24);
}

bool tt_resolve_link(const char* broadcast, uint32_t* addr, uint32_t* netmask, uint32_t* bcast) {
    // One netif on this target (see tt_get_node_id()'s own comment), so there is nothing to search:
    // either the configured broadcast is this interface's, or no local interface owns it.
    if (broadcast == NULL || addr == NULL || netmask == NULL || bcast == NULL || netif_default == NULL) {
        return false;
    }
    uint32_t want = ntohl(ipaddr_addr(broadcast));
    // See hal_linux.c: always reported, so a caller can address a link the stack does not own.
    *bcast = want;
    uint32_t if_addr = ntohl(ip4_addr_get_u32(netif_ip4_addr(netif_default)));
    uint32_t if_mask = ntohl(ip4_addr_get_u32(netif_ip4_netmask(netif_default)));
    uint32_t if_bcast = if_addr | ~if_mask;
    if (if_bcast != want) {
        return false;
    }
    *addr = if_addr;
    *netmask = if_mask;
    return true;
}

// One netif on this target (see tt_resolve_link() above).
int32_t tt_link_mtu(uint32_t addr) {
    if (netif_default == NULL || ntohl(ip4_addr_get_u32(netif_ip4_addr(netif_default))) != addr) {
        return -1;
    }
    return (int32_t)netif_default->mtu;
}

// Contexts in one image (hal_freertos.h): the data ports of the contexts bound in this image, 0 for a free entry,
// written with compare-and-exchange so two tasks binding at once cannot take one entry. A broadcast is copied to each
// of the others (copy_to_local_contexts(), per build below). A port is the key because no two contexts share one, and
// because a port is all a copy needs: the address is the netif's, the same for all of them.
static uint16_t local_ports[tt_HAL_FREERTOS_LOCAL_CONTEXTS];

static void local_register(uint16_t port) {
    for (uint32_t i = 0; i < tt_HAL_FREERTOS_LOCAL_CONTEXTS && port != 0; i++) {
        uint16_t expected = 0;
        if (__atomic_compare_exchange_n(&local_ports[i], &expected, port, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            return;
        }
    }
}

static void local_unregister(uint16_t port) {
    for (uint32_t i = 0; i < tt_HAL_FREERTOS_LOCAL_CONTEXTS && port != 0; i++) {
        uint16_t expected = port;
        (void)__atomic_compare_exchange_n(&local_ports[i], &expected, 0U, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
}

// The data port's address as a peer sees it: bound to a specific address, that one; bound to any, the netif's - the
// only one this image has (tt_get_node_id()).
static uint32_t own_address_of(uint32_t bound_ip) {
    if (bound_ip != 0 || netif_default == NULL) {
        return bound_ip;
    }
    return ntohl(ip4_addr_get_u32(netif_ip4_addr(netif_default)));
}

#if tt_CONTEXT_ID_CLAIM || tt_SEGMENT_ENABLED
// See hal_linux.c's: one address per image, so a port tells two contexts of it apart.
bool tt_is_own_address(const struct tt_Context* node, uint32_t ip, uint16_t port) {
    return port == node->hal.own_port && (ip == node->hal.own_ip || node->hal.own_ip == 0);
}

void tt_own_address(const struct tt_Context* node, uint32_t* ip, uint16_t* port) {
    *ip = node->hal.own_ip;
    *port = node->hal.own_port;
}
#endif

#if tt_SEGMENT_ENABLED
// The segment's platform half (hal.h) on FreeRTOS. Every task of an image shares one address space, so a region is a
// block of RAM, and the "name" a writer attaches by is a key in this table rather than a file: create puts a region
// under a name, attach finds it and counts one more user, detach counts one fewer, and unlink takes the name away. The
// memory goes back when it has neither a name nor a user - the lifetime a file in /dev/shm has on Linux, kept so that
// core's own create/attach/detach/unlink sequence means here what it means there. Core's ring, header checks and
// incarnation are unchanged: they see a region exactly as they see a mapping.
//
// Never tt_SEGMENT_ORPHANED: a region here cannot outlive the context that created it except by that context never
// being destroyed, and then its sockets are still open too - nothing a name could tell apart (hal.h).
//
// Every table operation runs in a critical section: a handful of string compares over tt_HAL_FREERTOS_SEGMENTS
// entries, and never an allocation, which is made before and released after.
struct segment_region {
    void* base; // NULL: a free entry
    size_t bytes;
    uint16_t users; // creator and attachers whose detach has not come yet
    bool named;     // still under its name: a new attach can find it
    char name[tt_SEGMENT_PATH_LENGTH];
};
static struct segment_region segment_regions[tt_HAL_FREERTOS_SEGMENTS];

// The default memory (hal_freertos.h): the FreeRTOS heap, over-allocated by a line so the region can start on one,
// with the block's own address kept in the word before the region for the release.
static void* heap_segment_alloc(size_t bytes) {
#if configSUPPORT_DYNAMIC_ALLOCATION // NOLINT(misc-include-cleaner) - FreeRTOSConfig.h, through FreeRTOS.h
    uint8_t* raw = (uint8_t*)pvPortMalloc(bytes + tt_SEGMENT_CACHE_LINE);
    if (raw == NULL) {
        return NULL;
    }
    // Strictly above raw, by at least the heap's own alignment, which is at least a pointer: room for the word below.
    uintptr_t base = ((uintptr_t)raw + tt_SEGMENT_CACHE_LINE) & ~(uintptr_t)(tt_SEGMENT_CACHE_LINE - 1);
    ((void**)base)[-1] = raw; // NOLINT(performance-no-int-to-ptr)
    return (void*)base;       // NOLINT(performance-no-int-to-ptr)
#else
    (void)bytes;
    return NULL; // no heap: no segment unless the application gives memory (tt_freertos_set_segment_memory())
#endif
}

static void heap_segment_release(void* region, size_t bytes) {
    (void)bytes;
#if configSUPPORT_DYNAMIC_ALLOCATION // NOLINT(misc-include-cleaner)
    vPortFree(((void**)region)[-1]);
#else
    (void)region;
#endif
}

static tt_HAL_SEGMENT_ALLOC segment_alloc = heap_segment_alloc;
static tt_HAL_SEGMENT_RELEASE segment_release_memory = heap_segment_release;

void tt_freertos_set_segment_memory(tt_HAL_SEGMENT_ALLOC alloc, tt_HAL_SEGMENT_RELEASE release) {
    const bool both = alloc != NULL && release != NULL;
    segment_alloc = both ? alloc : heap_segment_alloc;
    segment_release_memory = both ? release : heap_segment_release;
}

static bool name_is(const char* name, const char* path) {
    return strncmp(name, path, tt_SEGMENT_PATH_LENGTH) == 0;
}

// The entry under `path`, or NULL. Called in the critical section.
static struct segment_region* region_named(const char* path) {
    for (uint32_t i = 0; i < tt_HAL_FREERTOS_SEGMENTS; i++) {
        struct segment_region* region = &segment_regions[i];
        if (region->base != NULL && region->named && name_is(region->name, path)) {
            return region;
        }
    }
    return NULL;
}

// An entry with neither a name nor a user, emptied; its memory into *gone for release outside the critical section.
static void region_retire_if_unused(struct segment_region* region, void** gone, size_t* gone_bytes) {
    if (region->named || region->users != 0) {
        return;
    }
    *gone = region->base;
    *gone_bytes = region->bytes;
    memset(region, 0, sizeof(*region));
}

void* tt_segment_create(const char* path, size_t bytes) {
    if (strnlen(path, tt_SEGMENT_PATH_LENGTH) >= tt_SEGMENT_PATH_LENGTH) {
        return NULL; // core never forms one this long (segment_name()); a truncated key could name another segment
    }
    void* region = segment_alloc(bytes);
    if (region == NULL) {
        return NULL; // no segment: this context's peers reach it over UDP
    }
    if (((uintptr_t)region & (tt_SEGMENT_CACHE_LINE - 1U)) != 0) {
        TT_LOG_WARNING("Segment memory is not aligned to %u bytes - no segment", (unsigned)tt_SEGMENT_CACHE_LINE);
        segment_release_memory(region, bytes);
        return NULL;
    }
    // Zeroed, as a new file is: core writes every field it reads before the magic, but a field added later should not
    // have to remember that this platform hands out whatever the heap held.
    memset(region, 0, bytes);
    void* replaced = NULL;
    size_t replaced_bytes = 0;
    bool placed = false;
    taskENTER_CRITICAL();
    // Replaces a region left under this name, as Linux's create unlinks first: whoever still uses it keeps it.
    struct segment_region* previous = region_named(path);
    if (previous != NULL) {
        previous->named = false;
        region_retire_if_unused(previous, &replaced, &replaced_bytes);
    }
    for (uint32_t i = 0; i < tt_HAL_FREERTOS_SEGMENTS && !placed; i++) {
        struct segment_region* entry = &segment_regions[i];
        if (entry->base == NULL) {
            entry->base = region;
            entry->bytes = bytes;
            entry->users = 1;
            entry->named = true;
            strncpy(entry->name, path, sizeof(entry->name) - 1U);
            placed = true;
        }
    }
    taskEXIT_CRITICAL();
    if (replaced != NULL) {
        segment_release_memory(replaced, replaced_bytes);
    }
    if (!placed) {
        TT_LOG_WARNING("No free segment entry (tt_HAL_FREERTOS_SEGMENTS = %u) - no segment",
                       (unsigned)tt_HAL_FREERTOS_SEGMENTS);
        segment_release_memory(region, bytes);
        return NULL;
    }
    return region;
}

void* tt_segment_attach(const char* path, size_t bytes, uint8_t* why) {
    void* base = NULL;
    *why = (uint8_t)tt_SEGMENT_ABSENT; // the ordinary answer: a peer in another image, or one without a segment
    taskENTER_CRITICAL();
    struct segment_region* region = region_named(path);
    if (region != NULL && region->bytes < bytes) {
        *why = (uint8_t)tt_SEGMENT_BAD_HEADER; // smaller than the caller is about to index: as Linux refuses it
    } else if (region != NULL && region->users < UINT16_MAX) {
        region->users++;
        base = region->base;
        *why = (uint8_t)tt_SEGMENT_ATTACHED;
    } else if (region != NULL) {
        *why = (uint8_t)tt_SEGMENT_REFUSED;
    }
    taskEXIT_CRITICAL();
    return base;
}

void tt_segment_detach(void* mapping, size_t bytes) {
    (void)bytes; // a region is detached whole, whatever part of it the attach asked for (the header probe)
    if (mapping == NULL) {
        return;
    }
    void* gone = NULL;
    size_t gone_bytes = 0;
    taskENTER_CRITICAL();
    for (uint32_t i = 0; i < tt_HAL_FREERTOS_SEGMENTS; i++) {
        struct segment_region* region = &segment_regions[i];
        if (region->base == mapping && region->users != 0) {
            region->users--;
            region_retire_if_unused(region, &gone, &gone_bytes);
            break;
        }
    }
    taskEXIT_CRITICAL();
    if (gone != NULL) {
        segment_release_memory(gone, gone_bytes);
    }
}

void tt_segment_unlink(const char* path) {
    void* gone = NULL;
    size_t gone_bytes = 0;
    taskENTER_CRITICAL();
    struct segment_region* region = region_named(path);
    if (region != NULL) {
        region->named = false;
        region_retire_if_unused(region, &gone, &gone_bytes);
    }
    taskEXIT_CRITICAL();
    if (gone != NULL) {
        segment_release_memory(gone, gone_bytes);
    }
}

// Not told on this target: the reader's waits are compiled out here (tt_SEGMENT_READER_WAITS, config.h), which is the
// only thing that asks.
uint64_t tt_thread_cpu_ns(void) {
    return 0;
}
#endif

#if tt_HAL_FREERTOS_NETCONN

// The netconn build (hal_freertos.h, tt_HAL_FREERTOS_NETCONN). Same two receive sockets, same alternation, same
// contract (hal.h); what changes is how an arrival is noticed. lwIP calls rx_event() for every datagram it queues on
// one of this node's netconns, so the HAL knows how many are waiting without asking lwIP, and a task blocked in
// tt_receive() sleeps on one semaphore that both an arrival and tt_wake_signal() give.

// lwIP's netconn callback, run by the tcpip thread for an arrival (RCVPLUS, after the datagram is in the receive
// mailbox) and by the reading task for a take (RCVMINUS, from netconn_recv). The node and which netconn this is ride
// in callback_arg (struct tt_hal.rx_tags), set in open_conn() before the netconn is bound, so no arrival comes first.
static void rx_event(struct netconn* conn, enum netconn_evt evt, u16_t len) {
    (void)len; // a zero-length datagram is an arrival too
    const struct tt_HalRxTag* tag = (const struct tt_HalRxTag*)netconn_get_callback_arg(conn);
    if (tag == NULL || tag->node == NULL) {
        return;
    }
    struct tt_Context* node = tag->node;
    uint8_t which = tag->which;
    if (evt == NETCONN_EVT_RCVPLUS) {
        __atomic_add_fetch(&node->hal.rx_arrivals[which], 1U, __ATOMIC_SEQ_CST);
        xSemaphoreGive(node->hal.rx_sem);
    } else if (evt == NETCONN_EVT_RCVMINUS) {
        // Never below zero: rx_read() may have cleared a count a spurious RCVPLUS left - see there.
        uint32_t seen = __atomic_load_n(&node->hal.rx_arrivals[which], __ATOMIC_SEQ_CST);
        while (seen != 0 && !__atomic_compare_exchange_n(&node->hal.rx_arrivals[which], &seen, seen - 1U, false,
                                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        }
    }
}

// A UDP netconn bound to addr:port, with rx_event() as its callback and rx_tags[which] as its argument. NULL on
// failure, with nothing left allocated.
static struct netconn* open_conn(struct tt_Context* node, uint8_t which, const ip_addr_t* addr, uint16_t port) {
    struct netconn* conn = netconn_new_with_callback(NETCONN_UDP, rx_event);
    if (conn == NULL) {
        return NULL;
    }
    node->hal.rx_tags[which].node = node;
    node->hal.rx_tags[which].which = which;
    netconn_set_callback_arg(conn, &node->hal.rx_tags[which]);
    netconn_set_nonblocking(conn, 1); // every read here passes NETCONN_DONTBLOCK anyway
    // SO_REUSEADDR/SO_BROADCAST as the socket build sets them: lwip_setsockopt() sets exactly these pcb options. The
    // pcb is not bound yet, so nothing on the tcpip thread can be looking at it.
    if (which == tt_HAL_RX_WELL_KNOWN) {
        ip_set_option(conn->pcb.udp, SOF_REUSEADDR); // shared with every node on this host; the data port never is
    }
    ip_set_option(conn->pcb.udp, SOF_BROADCAST);
    if (netconn_bind(conn, addr, port) != ERR_OK) {
        netconn_delete(conn);
        return NULL;
    }
    return conn;
}

tt_ret_t tt_bind(struct tt_Context* node) {
    struct tt_hal* hal = &node->hal;
    hal->conn = NULL;
    hal->data_conn = NULL;
    hal->rx_arrivals[tt_HAL_RX_WELL_KNOWN] = 0;
    hal->rx_arrivals[tt_HAL_RX_DATA] = 0;
    hal->wake_pending = 0;
    hal->rx_prefer_data = false;
    hal->rx_reads = 0;
    hal->rx_empty_reads = 0;
    hal->rx_quiet = 0;
    hal->own_ip = 0;
    hal->own_port = 0;
#if tt_SEGMENT_ENABLED
    hal->bell_pending = 0;
    hal->bell_wakes = 0;
#endif
    hal->rx_sem = xSemaphoreCreateBinaryStatic(&hal->rx_sem_storage);
    if (hal->rx_sem == NULL) {
        TT_LOG_ERROR("Cannot create receive semaphore");
        return tt_RET_IO_ERROR;
    }

    ip_addr_set_ip4_u32_val(hal->broadcast_ip, ipaddr_addr(_tt_CONFIG.broadcast));

    // The well-known port on the wildcard address, shared through SO_REUSEADDR, and this node's own data port scoped
    // to _tt_CONFIG.addr with the stack choosing the port - see the socket build's tt_bind() below, and hal_linux.c,
    // for why each is bound the way it is.
    hal->conn = open_conn(node, tt_HAL_RX_WELL_KNOWN, IP4_ADDR_ANY, _tt_CONFIG.port);
    if (hal->conn == NULL) {
        TT_LOG_ERROR("Cannot bind netconn to 0.0.0.0:%d", _tt_CONFIG.port);
        tt_close(node);
        return tt_RET_IO_ERROR;
    }
    ip_addr_t data_ip;
    ip_addr_set_ip4_u32_val(data_ip, ipaddr_addr(_tt_CONFIG.addr));
    hal->data_conn = open_conn(node, tt_HAL_RX_DATA, &data_ip, 0);
    if (hal->data_conn == NULL) {
        TT_LOG_ERROR("Cannot bind data netconn to %s:0", _tt_CONFIG.addr);
        tt_close(node);
        return tt_RET_IO_ERROR;
    }
    ip_addr_t bound_ip;
    u16_t bound_port = 0;
    if (netconn_getaddr(hal->data_conn, &bound_ip, &bound_port, 1) == ERR_OK) {
        hal->own_ip = own_address_of(lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(&bound_ip))));
        hal->own_port = bound_port;
    }
    local_register(hal->own_port);
    return tt_RET_OK;
}

void tt_close(struct tt_Context* node) {
    struct tt_hal* hal = &node->hal;
    local_unregister(hal->own_port);
    hal->own_port = 0;
    if (hal->data_conn != NULL && netconn_delete(hal->data_conn) != ERR_OK) {
        TT_LOG_WARNING("Cannot close data netconn");
    }
    hal->data_conn = NULL;
    if (hal->conn != NULL && netconn_delete(hal->conn) != ERR_OK) {
        TT_LOG_ERROR("Cannot close netconn");
    }
    hal->conn = NULL;
    if (hal->rx_sem != NULL) {
        vSemaphoreDelete(hal->rx_sem); // after the netconns: no callback can give it any more
        hal->rx_sem = NULL;
    }
}

// One datagram made of `count` pieces, sent from the data port without copying them: each piece is a PBUF_REF over
// the caller's memory, chained the way lwip_sendmsg() chains an iovec. netconn_sendto() returns once the tcpip thread
// has sent the chain (anything that keeps it, such as the loopback queue, copies it), so the memory is the caller's
// again on return. Returns the bytes sent, or -1.
static int32_t send_pieces(struct tt_Context* node, const void* const* pieces, const size_t* lens, uint32_t count,
                           const ip_addr_t* dest, uint16_t port) {
    struct netbuf chain;
    memset(&chain, 0, sizeof(chain));
    size_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (lens[i] == 0 && chain.p != NULL) {
            continue; // an empty body adds nothing; an empty head still makes the zero-length datagram asked for
        }
        total += lens[i];
        struct pbuf* piece = total > UINT16_MAX ? NULL : pbuf_alloc(PBUF_TRANSPORT, 0, PBUF_REF);
        if (piece == NULL) {
            netbuf_free(&chain);
            return -1;
        }
        // PBUF_REF payloads are read only on the send path; casting away const is the same idiom as tt_send_iov()'s.
        piece->payload = (void*)(uintptr_t)pieces[i]; // NOLINT(performance-no-int-to-ptr)
        piece->len = (u16_t)lens[i];
        piece->tot_len = (u16_t)lens[i];
        if (chain.p == NULL) {
            chain.p = piece;
            chain.ptr = piece;
        } else {
            pbuf_cat(chain.p, piece);
        }
    }
    err_t err = netconn_sendto(node->hal.data_conn, &chain, dest, port);
    netbuf_free(&chain);
    return err == ERR_OK ? (int32_t)total : -1;
}

// A broadcast, copied to every other context bound in this image (hal_freertos.h, "Contexts in one image"): unicast to
// the netif's own address, which lwIP loops back, at each one's data port. The copies' results are not the send's: a
// sibling closing meanwhile is not this send failing. Returns the broadcast's result.
static int32_t send_broadcast(struct tt_Context* node, const void* const* pieces, const size_t* lens, uint32_t count) {
    int32_t sent = send_pieces(node, pieces, lens, count, &node->hal.broadcast_ip, _tt_CONFIG.port);
    if (node->hal.own_ip == 0) {
        return sent;
    }
    ip_addr_t self;
    ip_addr_set_ip4_u32_val(self, lwip_htonl(node->hal.own_ip));
    for (uint32_t i = 0; i < tt_HAL_FREERTOS_LOCAL_CONTEXTS; i++) {
        uint16_t port = __atomic_load_n(&local_ports[i], __ATOMIC_SEQ_CST);
        if (port != 0 && port != node->hal.own_port) {
            (void)send_pieces(node, pieces, lens, count, &self, port);
        }
    }
    return sent;
}

int32_t tt_send(struct tt_Context* node, const void* buf, size_t len) {
    const void* pieces[1] = {buf};
    size_t lens[1] = {len};
    return send_broadcast(node, pieces, lens, 1);
}

int32_t tt_send_to(struct tt_Context* node, const void* buf, size_t len, uint32_t ip, uint16_t port) {
    ip_addr_t dest;
    ip_addr_set_ip4_u32_val(dest, lwip_htonl(ip));
    const void* pieces[1] = {buf};
    size_t lens[1] = {len};
    return send_pieces(node, pieces, lens, 1, &dest, port);
}

int32_t tt_send_iov(struct tt_Context* node, const void* hdr, size_t hdr_len, const void* body, size_t body_len,
                    uint32_t ip, uint16_t port) {
    const void* pieces[2] = {hdr, body};
    size_t lens[2] = {hdr_len, body_len};
    if (ip == 0) {
        return send_broadcast(node, pieces, lens, 2);
    }
    ip_addr_t dest;
    ip_addr_set_ip4_u32_val(dest, lwip_htonl(ip));
    return send_pieces(node, pieces, lens, 2, &dest, port);
}

// One datagram from whichever netconn has one counted, alternating when both do (hal_linux.h's rx_prefer_data).
// Returns its length; -1 when nothing is counted - answered from the counts, without entering lwIP - or when the
// counted datagram was not there; -2 on an lwIP error. tt_receive() and tt_try_receive() both read through here.
static int32_t rx_read(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    struct tt_hal* hal = &node->hal;
    uint32_t well_known = __atomic_load_n(&hal->rx_arrivals[tt_HAL_RX_WELL_KNOWN], __ATOMIC_SEQ_CST);
    uint32_t data = __atomic_load_n(&hal->rx_arrivals[tt_HAL_RX_DATA], __ATOMIC_SEQ_CST);
    if (well_known == 0 && data == 0) {
        hal->rx_quiet++;
        return -1;
    }
    int which = (data != 0 && (well_known == 0 || hal->rx_prefer_data)) ? tt_HAL_RX_DATA : tt_HAL_RX_WELL_KNOWN;
    if (well_known != 0 && data != 0) {
        hal->rx_prefer_data = !hal->rx_prefer_data;
    }
    uint32_t seen = which == tt_HAL_RX_DATA ? data : well_known;

    struct netbuf* datagram = NULL;
    err_t err = netconn_recv_udp_raw_netbuf_flags(which == tt_HAL_RX_DATA ? hal->data_conn : hal->conn, &datagram,
                                                  NETCONN_DONTBLOCK);
    if (err == ERR_WOULDBLOCK) {
        // Counted, and not there: lwIP raises RCVPLUS without queueing anything on a few error and close paths
        // (api_msg.c). Clear what that left, or the hint would answer "maybe" for ever. Only the value seen above is
        // cleared, so an arrival counted since then survives - its datagram was queued before it was counted.
        hal->rx_empty_reads++;
        (void)__atomic_compare_exchange_n(&hal->rx_arrivals[which], &seen, 0U, false, __ATOMIC_SEQ_CST,
                                          __ATOMIC_SEQ_CST);
        return -1;
    }
    if (err != ERR_OK || datagram == NULL) {
        return -2;
    }
    // A datagram longer than buf is cut to fit, as recvfrom() cuts it.
    u16_t length = netbuf_len(datagram);
    u16_t copied = netbuf_copy(datagram, buf, (u16_t)(len < length ? len : length));
    *ip = lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(netbuf_fromaddr(datagram))));
    *port = netbuf_fromport(datagram);
    netbuf_delete(datagram);
    node->rx_via_data_port = which == tt_HAL_RX_DATA;
    hal->rx_reads++;
    return (int32_t)copied;
}

// NOLINTBEGIN(misc-include-cleaner) - see the FreeRTOS.h include
// FreeRTOS ticks for a wait of `timeout` ns, rounded up so a short wait is never zero: a zero-tick take would not
// wait at all, where select() in the socket build waited at least its millisecond.
static TickType_t wait_ticks(int64_t timeout) {
    const uint64_t tick_ns = tt_SECOND / configTICK_RATE_HZ;
    uint64_t ticks = ((uint64_t)timeout + tick_ns - 1U) / tick_ns;
    if (ticks >= (uint64_t)portMAX_DELAY) {
        return portMAX_DELAY - 1U; // finite, however long
    }
    return (TickType_t)ticks;
}

int32_t tt_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout) {
    struct tt_hal* hal = &node->hal;
    // 0 blocks until data or a wake (hal.h). Negative blocks too, as in hal_linux.c.
    TickType_t remaining = timeout > 0 ? wait_ticks(timeout) : portMAX_DELAY;
    TimeOut_t start;
    vTaskSetTimeOutState(&start);
    while (true) {
        // The wake first, as the socket build's select() handling reports it ahead of data.
        if (__atomic_exchange_n(&hal->wake_pending, 0U, __ATOMIC_SEQ_CST) != 0) {
            return -3;
        }
#if tt_SEGMENT_ENABLED
        // A rung doorbell is taken whether or not a datagram is waiting too, as hal_linux.c's edge-triggered bell is:
        // core drains the segment before it sleeps again, whatever woke it.
        const bool rung = __atomic_exchange_n(&hal->bell_pending, 0U, __ATOMIC_SEQ_CST) != 0;
#endif
        if ((__atomic_load_n(&hal->rx_arrivals[tt_HAL_RX_WELL_KNOWN], __ATOMIC_SEQ_CST) |
             __atomic_load_n(&hal->rx_arrivals[tt_HAL_RX_DATA], __ATOMIC_SEQ_CST)) != 0) {
            int32_t got = rx_read(node, buf, len, ip, port);
            if (got != -1) {
                return got;
            }
        }
#if tt_SEGMENT_ENABLED
        if (rung) {
            hal->bell_wakes++;
            *ip = 0;
            *port = 0;
            return 0; // a rung bell is a zero-length datagram to core, which is what a doorbell always was (hal.h)
        }
#endif
        // A give left over from a datagram tt_try_receive() already took wakes this once for nothing; the loop then
        // finds no count and waits again for what is left of the timeout.
        if (xTaskCheckForTimeOut(&start, &remaining) != pdFALSE || xSemaphoreTake(hal->rx_sem, remaining) != pdTRUE) {
            return -1;
        }
    }
}

// NOLINTEND(misc-include-cleaner)

int32_t tt_try_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    return rx_read(node, buf, len, ip, port);
}

// Answered from the counts rx_event() keeps: zero on both netconns means nothing has been queued since this HAL last
// took a datagram, so "no" costs two loads and no call into lwIP (hal.h).
bool tt_rx_maybe_ready(struct tt_Context* node) {
    struct tt_hal* hal = &node->hal;
    if ((__atomic_load_n(&hal->rx_arrivals[tt_HAL_RX_WELL_KNOWN], __ATOMIC_SEQ_CST) |
         __atomic_load_n(&hal->rx_arrivals[tt_HAL_RX_DATA], __ATOMIC_SEQ_CST)) == 0) {
        hal->rx_quiet++;
        return false;
    }
    return true;
}

// The flag, then the semaphore: a tt_receive() that is blocked wakes and finds the flag, and one that is not finds it
// on its next call - the contract's "waiting for the next one" (hal.h). Neither step blocks, so any task may call
// this, the poller itself included.
tt_ret_t tt_wake_signal(struct tt_Context* node) {
    __atomic_store_n(&node->hal.wake_pending, 1U, __ATOMIC_SEQ_CST);
    xSemaphoreGive(node->hal.rx_sem);
    return tt_RET_OK;
}

#if tt_SEGMENT_ENABLED
// The segment's doorbell (hal.h) in the netconn build: no FIFO and no second wait point - a ring sets the owner's
// bell_pending and gives the semaphore its tt_receive() already waits on, as tt_wake_signal() does with its own flag.
// A writer finds the owner by the bell's name, which core builds from the segment's; the handle it keeps is the
// entry's index and a generation, so a handle kept past the owner's destroy rings nobody rather than whoever took the
// entry since.
struct segment_bell {
    struct tt_Context* node; // NULL: a free entry
    uint32_t generation;     // 1 to 0x7FFFFF: the handle's upper bits
    char name[tt_SEGMENT_PATH_LENGTH];
};
static struct segment_bell segment_bells[tt_HAL_FREERTOS_SEGMENTS];
static uint32_t segment_bell_generation;
#define BELL_INDEX_BITS 8U
#define BELL_INDEX_MASK ((1U << BELL_INDEX_BITS) - 1U)
#define BELL_GENERATION_MASK 0x7FFFFFU // keeps a handle positive, which is what says "a bell" to core
static_assert(tt_HAL_FREERTOS_SEGMENTS <= BELL_INDEX_MASK + 1U, "a bell's index must fit its handle's low bits");

int32_t tt_segment_bell_create(struct tt_Context* node, const char* path) {
    int32_t result = -1;
    taskENTER_CRITICAL();
    for (uint32_t i = 0; i < tt_HAL_FREERTOS_SEGMENTS; i++) {
        if (segment_bells[i].node != NULL && name_is(segment_bells[i].name, path)) {
            segment_bells[i].node = NULL; // a bell left under this name is replaced, as its segment is
        }
    }
    for (uint32_t i = 0; i < tt_HAL_FREERTOS_SEGMENTS && result < 0; i++) {
        struct segment_bell* bell = &segment_bells[i];
        if (bell->node == NULL) {
            segment_bell_generation = (segment_bell_generation % BELL_GENERATION_MASK) + 1U;
            bell->generation = segment_bell_generation;
            strncpy(bell->name, path, sizeof(bell->name) - 1U);
            bell->name[sizeof(bell->name) - 1U] = '\0';
            bell->node = node;
            result = 0;
        }
    }
    taskEXIT_CRITICAL();
    if (result < 0) {
        TT_LOG_WARNING("No free doorbell entry - peers will ring over UDP");
    }
    return result;
}

void tt_segment_bell_destroy(struct tt_Context* node, const char* path) {
    taskENTER_CRITICAL();
    for (uint32_t i = 0; i < tt_HAL_FREERTOS_SEGMENTS; i++) {
        if (segment_bells[i].node == node && name_is(segment_bells[i].name, path)) {
            segment_bells[i].node = NULL;
        }
    }
    taskEXIT_CRITICAL();
}

int32_t tt_segment_bell_open(const char* path) {
    int32_t handle = -1;
    taskENTER_CRITICAL();
    for (uint32_t i = 0; i < tt_HAL_FREERTOS_SEGMENTS && handle < 0; i++) {
        if (segment_bells[i].node != NULL && name_is(segment_bells[i].name, path)) {
            handle = (int32_t)((segment_bells[i].generation << BELL_INDEX_BITS) | i);
        }
    }
    taskEXIT_CRITICAL();
    return handle;
}

// Never blocks. The owner is looked up and its flag set in the critical section; the semaphore is given after it,
// since a give may switch to the owner's task. A destroy between the two leaves the give to a semaphore no tt_receive()
// waits on any more - one wake-up of nobody.
void tt_segment_bell_ring(int32_t bell) {
    const uint32_t index = (uint32_t)bell & BELL_INDEX_MASK;
    const uint32_t generation = (uint32_t)bell >> BELL_INDEX_BITS;
    if (bell < 0 || index >= tt_HAL_FREERTOS_SEGMENTS) {
        return;
    }
    SemaphoreHandle_t wake = NULL;
    taskENTER_CRITICAL();
    struct tt_Context* owner = segment_bells[index].node;
    if (owner != NULL && segment_bells[index].generation == generation) {
        __atomic_store_n(&owner->hal.bell_pending, 1U, __ATOMIC_SEQ_CST);
        wake = owner->hal.rx_sem;
    }
    taskEXIT_CRITICAL();
    if (wake != NULL) {
        xSemaphoreGive(wake);
    }
}

void tt_segment_bell_close(int32_t bell) {
    (void)bell; // a handle holds nothing: the entry is the owner's
}
#endif
#else // the BSD socket API

tt_ret_t tt_bind(struct tt_Context* node) {
    // See hal_linux.c's own tt_bind() comment on this same line - node->hal.sock relies on the
    // identical "only touched after it's known-good" convention.
    node->hal.wake_sock = -1;
    node->hal.data_sock = -1;
    node->hal.rx_prefer_data = false;
    node->hal.rx_reads = 0;
    node->hal.rx_empty_reads = 0;
    node->hal.rx_quiet = 0;
    node->hal.own_ip = 0;
    node->hal.own_port = 0;

    node->hal.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (node->hal.sock < 0) {
        TT_LOG_ERROR("Cannot create UDP socket: %s", strerror(errno));
        return tt_RET_IO_ERROR;
    }

    int optval = 1;
    if (setsockopt(node->hal.sock, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int)) < 0) {
        TT_LOG_ERROR("Cannot set socket reuseaddr: %s", strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    optval = 1;
    if (setsockopt(node->hal.sock, SOL_SOCKET, SO_BROADCAST, (const void*)&optval, sizeof(int)) < 0) {
        TT_LOG_ERROR("Cannot set socket broadcast: %s", strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    // Unlike hal_linux.c, there's no SO_SNDBUF/SO_RCVBUF tuning here: lwIP's socket layer has no
    // SO_SNDBUF at all (UDP send never queues), and SO_RCVBUF support is compiled out by default
    // (LWIP_SO_RCVBUF, off in platform/freertos/lwipopts.h) - its rx buffering is sized instead by
    // lwipopts.h's own compile-time knobs (e.g. the UDP recvmbox size). Not an oversight.
    // The well-known socket always binds the wildcard, never _tt_CONFIG.addr - see hal_linux.c's
    // own comment on this same bind for the measured reason: a socket bound to a unicast address
    // receives no broadcasts, so binding this one would stop every announce arriving while unicast
    // kept working. _tt_CONFIG.addr scopes the data socket instead, below.
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    // inet_addr()/htons() come from lwip/sockets.h's LWIP_COMPAT_SOCKETS aliasing.
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(_tt_CONFIG.port);
    addr.sin_len = sizeof(addr);

    if (bind(node->hal.sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_in)) < 0) {
        TT_LOG_ERROR("Cannot bind socket to 0.0.0.0:%d: %s", _tt_CONFIG.port, strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    // Precompute the broadcast destination once instead of re-parsing _tt_CONFIG.broadcast with
    // inet_addr() on every single tt_send() call - same reasoning as hal_linux.c.
    memset(&node->hal.broadcast_addr, 0, sizeof(node->hal.broadcast_addr));
    node->hal.broadcast_addr.sin_family = AF_INET;
    node->hal.broadcast_addr.sin_addr.s_addr = inet_addr(_tt_CONFIG.broadcast);
    node->hal.broadcast_addr.sin_port = htons(_tt_CONFIG.port);

    // This node's own data port - mirrors hal_linux.c's own data_sock, for the same measured
    // reason (see struct tt_hal.data_sock, hal_linux.h): everything is sent from here so a peer
    // records this node at a port that belongs to it alone, which is what lets a unicast reach
    // this node rather than whichever node on the host the stack happens to pick. Port 0 lets the
    // stack choose, and no SO_REUSEADDR, because a second node sharing this port is exactly the
    // failure being prevented.
    node->hal.data_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (node->hal.data_sock < 0) {
        TT_LOG_ERROR("Cannot create UDP data socket: %d", errno);
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    optval = 1;
    if (setsockopt(node->hal.data_sock, SOL_SOCKET, SO_BROADCAST, (const void*)&optval, sizeof(int)) < 0) {
        TT_LOG_ERROR("Cannot set data socket broadcast: %d", errno);
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    // This one does honour _tt_CONFIG.addr - see hal_linux.c's own comment on the same bind.
    struct sockaddr_in data_addr;
    data_addr.sin_family = AF_INET;
    data_addr.sin_addr.s_addr = inet_addr(_tt_CONFIG.addr);
    data_addr.sin_port = 0; // stack-assigned
    if (bind(node->hal.data_sock, (struct sockaddr*)&data_addr, sizeof(struct sockaddr_in)) < 0) {
        TT_LOG_ERROR("Cannot bind data socket to %s:0: %d", _tt_CONFIG.addr, errno);
        tt_close(node);
        return tt_RET_IO_ERROR;
    }
    node->hal.broadcast_addr.sin_len = sizeof(node->hal.broadcast_addr);

    // See hal_linux.c's own tt_bind() comment - a private loopback socket purely so
    // tt_wake_signal() has something to write to that wakes up a blocked tt_receive().
    // LWIP_NETIF_LOOPBACK is on (platform/freertos/lwipopts.h), so this works the same way here.
    node->hal.wake_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (node->hal.wake_sock < 0) {
        TT_LOG_ERROR("Cannot create wake socket: %s", strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    struct sockaddr_in wake_bind_addr;
    memset(&wake_bind_addr, 0, sizeof(wake_bind_addr));
    wake_bind_addr.sin_family = AF_INET;
    wake_bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    wake_bind_addr.sin_port = 0;
    wake_bind_addr.sin_len = sizeof(wake_bind_addr);

    if (bind(node->hal.wake_sock, (struct sockaddr*)&wake_bind_addr, sizeof(wake_bind_addr)) < 0) {
        TT_LOG_ERROR("Cannot bind wake socket: %s", strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    socklen_t wake_addr_len = sizeof(node->hal.wake_addr);
    if (getsockname(node->hal.wake_sock, (struct sockaddr*)&node->hal.wake_addr, &wake_addr_len) < 0) {
        TT_LOG_ERROR("Cannot get wake socket address: %s", strerror(errno));
        tt_close(node);
        return tt_RET_IO_ERROR;
    }

    struct sockaddr_in bound;
    socklen_t bound_len = sizeof(bound);
    if (getsockname(node->hal.data_sock, (struct sockaddr*)&bound, &bound_len) == 0) {
        node->hal.own_ip = own_address_of(ntohl(bound.sin_addr.s_addr));
        node->hal.own_port = ntohs(bound.sin_port);
    }
    local_register(node->hal.own_port);
    return tt_RET_OK;
}

void tt_close(struct tt_Context* node) {
    local_unregister(node->hal.own_port);
    node->hal.own_port = 0;
    if (node->hal.data_sock >= 0 && close(node->hal.data_sock) < 0) {
        TT_LOG_WARNING("Cannot close data socket: %d", errno);
    }
    node->hal.data_sock = -1;

    if (close(node->hal.sock) < 0) {
        TT_LOG_ERROR("Cannot close socket: %s", strerror(errno));
    }
    if (node->hal.wake_sock >= 0 && close(node->hal.wake_sock) < 0) {
        TT_LOG_ERROR("Cannot close wake socket: %s", strerror(errno));
    }
}

// The netconn build's send_broadcast(): `msg`, already sent to the broadcast address, again to every other context
// bound in this image, at the netif's own address. Its destination is replaced.
static void copy_to_local_contexts(struct tt_Context* node, struct msghdr* msg) {
    if (node->hal.own_ip == 0) {
        return;
    }
    struct sockaddr_in self;
    memset(&self, 0, sizeof(self));
    self.sin_family = AF_INET;
    self.sin_addr.s_addr = htonl(node->hal.own_ip);
    self.sin_len = sizeof(self);
    msg->msg_name = &self;
    msg->msg_namelen = sizeof(self);
    for (uint32_t i = 0; i < tt_HAL_FREERTOS_LOCAL_CONTEXTS; i++) {
        uint16_t port = __atomic_load_n(&local_ports[i], __ATOMIC_SEQ_CST);
        if (port != 0 && port != node->hal.own_port) {
            self.sin_port = htons(port);
            (void)sendmsg(node->hal.data_sock, msg, 0);
        }
    }
}

int32_t tt_send(struct tt_Context* node, const void* buf, size_t len) {
    int32_t sent = (int32_t)sendto(node->hal.data_sock, buf, len, 0, (struct sockaddr*)&node->hal.broadcast_addr,
                                   sizeof(struct sockaddr_in));
    // NOLINTNEXTLINE(performance-no-int-to-ptr) - sendmsg() only reads it; see tt_send_iov()
    struct iovec iov = {.iov_base = (void*)(uintptr_t)buf, .iov_len = len};
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    copy_to_local_contexts(node, &msg);
    return sent;
}

int32_t tt_send_to(struct tt_Context* node, const void* buf, size_t len, uint32_t ip, uint16_t port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(ip);
    addr.sin_port = htons(port);
    addr.sin_len = sizeof(addr);

    return (int32_t)sendto(node->hal.data_sock, buf, len, 0, (struct sockaddr*)&addr, sizeof(struct sockaddr_in));
}

int32_t tt_send_iov(struct tt_Context* node, const void* hdr, size_t hdr_len, const void* body, size_t body_len,
                    uint32_t ip, uint16_t port) {
    // iovec.iov_base is non-const by POSIX, but sendmsg() only reads it - casting away const here
    // is the standard idiom, not a real int-to-pointer round trip.
    struct iovec iov[2] = {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        {.iov_base = (void*)(uintptr_t)hdr, .iov_len = hdr_len},
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        {.iov_base = (void*)(uintptr_t)body, .iov_len = body_len},
    };

    struct sockaddr_in unicast_addr;
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;
    if (ip != 0) {
        memset(&unicast_addr, 0, sizeof(unicast_addr));
        unicast_addr.sin_family = AF_INET;
        unicast_addr.sin_addr.s_addr = htonl(ip);
        unicast_addr.sin_port = htons(port);
        unicast_addr.sin_len = sizeof(unicast_addr);
        msg.msg_name = &unicast_addr;
        msg.msg_namelen = sizeof(unicast_addr);
    } else {
        msg.msg_name = &node->hal.broadcast_addr;
        msg.msg_namelen = sizeof(node->hal.broadcast_addr);
    }

    int32_t sent = (int32_t)sendmsg(node->hal.data_sock, &msg, 0);
    if (ip == 0) {
        copy_to_local_contexts(node, &msg);
    }
    return sent;
}

// tt_receive()'s wait: select() on both sockets and the wake socket for up to `timeout` (0 = no
// timeout), and pick which socket the read that follows should use. Returns 0 when a datagram is ready
// (with *read_fd set), otherwise the value tt_receive() returns: -1 timeout, -2 I/O error, -3 woken by
// tt_wake_signal(). Split out of tt_receive() for its size, not its behaviour.
static int32_t wait_readable(struct tt_Context* node, int64_t timeout, int* read_fd) {
    struct timeval wait_time;
    struct timeval* wait_time_ptr;
    if (timeout == 0) {
        wait_time_ptr = NULL; // select()'s own NULL timeout means "block until data arrives"
    } else {
        wait_time.tv_sec = (long)(timeout / tt_SECOND);
        wait_time.tv_usec = (long)((timeout % tt_SECOND) / tt_MICROSECOND);
        wait_time_ptr = &wait_time;
    }

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(node->hal.sock, &readfds);
    FD_SET(node->hal.wake_sock, &readfds);
    FD_SET(node->hal.data_sock, &readfds);
    int maxfd = node->hal.sock > node->hal.wake_sock ? node->hal.sock : node->hal.wake_sock;
    if (node->hal.data_sock > maxfd) {
        maxfd = node->hal.data_sock;
    }

    int select_ret = select(maxfd + 1, &readfds, NULL, NULL, wait_time_ptr);
    if (select_ret == 0) {
        return -1; // Timeout
    }
    if (select_ret < 0) {
        // NOLINTNEXTLINE(misc-include-cleaner)
        if (errno == EINTR) {
            return -1; // Treat an interrupted wait like a timeout; the caller just polls again
        }
        return -2; // I/O error
    }
    if (FD_ISSET(node->hal.wake_sock, &readfds)) {
        // tt_wake_signal() - see hal_linux.c's tt_receive() for the reasoning (identical here,
        // just select() instead of poll()).
        uint8_t discard;
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        (void)recvfrom(node->hal.wake_sock, &discard, sizeof(discard), 0, (struct sockaddr*)&from, &from_len);
        return -3; // Interrupted
    }
    // Broadcasts land on the well-known socket, unicast on this node's own. They alternate
    // when both are ready - see hal_linux.c's own comment on why a fixed preference starves
    // the other socket outright rather than merely delaying it.
    bool well_known_ready = FD_ISSET(node->hal.sock, &readfds) != 0;
    bool data_ready = FD_ISSET(node->hal.data_sock, &readfds) != 0;
    if (data_ready && (!well_known_ready || node->hal.rx_prefer_data)) {
        *read_fd = node->hal.data_sock;
    }
    if (well_known_ready && data_ready) {
        node->hal.rx_prefer_data = !node->hal.rx_prefer_data;
    }
    return 0;
}

int32_t tt_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout) {
    // Which socket this call will read from - see wait_readable(). Defaults to the well-known one so
    // the non-polling path behaves exactly as it did before data_sock existed.
    int read_fd = node->hal.sock;
    // Same "0 for no timeout" (block until data arrives) contract fix as hal_linux.c's poll()
    // rewrite, using lwIP's select() (LWIP_COMPAT_SOCKETS aliases it the same as the real thing)
    // since lwIP doesn't provide poll().
    if (timeout >= 0) {
        int32_t waited = wait_readable(node, timeout, &read_fd);
        if (waited != 0) {
            return waited;
        }
    }

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(struct sockaddr_in);
    node->rx_via_data_port = (read_fd == node->hal.data_sock);
    int32_t ret = (int32_t)recvfrom(read_fd, buf, len, 0, (struct sockaddr*)&addr, &addr_len);

    *ip = ntohl(addr.sin_addr.s_addr);
    *port = ntohs(addr.sin_port);

    if (ret < 0) {
        // NOLINTNEXTLINE(misc-include-cleaner)
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1; // Timeout
        }
        return -2; // I/O error
    }

    node->hal.rx_reads++;
    return ret;
}

// No cheaper way to know than reading, so "may be" - the behaviour before tt_rx_maybe_ready() existed.
bool tt_rx_maybe_ready(struct tt_Context* node) {
    (void)node;
    return true;
}

int32_t tt_try_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {
    // lwIP does NOT honor MSG_DONTWAIT per recvfrom() call the way Linux does - without O_NONBLOCK
    // on the socket it can still block on an empty socket, which hangs drain_rx()'s "keep calling
    // until nothing's left" loop forever. So gate the read on a zero-timeout select() (the same
    // lwIP primitive tt_receive() uses, just non-blocking): only recvfrom() when it reports the
    // socket readable, otherwise report "nothing waiting" immediately.
    // Both sockets, because either can have something waiting: broadcasts land on the well-known
    // one and unicast on this node's own data socket. Draining only one leaves the other's backlog
    // to the next select(), which is the per-packet round trip drain_rx() exists to avoid.
    struct timeval no_wait = {0, 0};
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(node->hal.sock, &readfds);
    FD_SET(node->hal.data_sock, &readfds);
    int maxfd = node->hal.sock > node->hal.data_sock ? node->hal.sock : node->hal.data_sock;

    int select_ret = select(maxfd + 1, &readfds, NULL, NULL, &no_wait);
    if (select_ret == 0) {
        node->hal.rx_empty_reads++;
        return -1; // Nothing waiting
    }
    if (select_ret < 0) {
        if (errno == EINTR) {
            return -1;
        }
        return -2; // I/O error
    }

    bool well_known_ready = FD_ISSET(node->hal.sock, &readfds) != 0;
    bool data_ready = FD_ISSET(node->hal.data_sock, &readfds) != 0;
    int read_fd = node->hal.sock;
    if (data_ready && (!well_known_ready || node->hal.rx_prefer_data)) {
        read_fd = node->hal.data_sock;
    }
    if (well_known_ready && data_ready) {
        node->hal.rx_prefer_data = !node->hal.rx_prefer_data;
    }

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(struct sockaddr_in);
    node->rx_via_data_port = (read_fd == node->hal.data_sock);
    int32_t ret = (int32_t)recvfrom(read_fd, buf, len, 0, (struct sockaddr*)&addr, &addr_len);

    *ip = ntohl(addr.sin_addr.s_addr);
    *port = ntohs(addr.sin_port);

    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            node->hal.rx_empty_reads++;
            return -1; // Nothing waiting
        }
        return -2; // I/O error
    }

    node->hal.rx_reads++;
    return ret;
}

tt_ret_t tt_wake_signal(struct tt_Context* node) {
    uint8_t one = 1;
    // See hal_linux.c's tt_wake_signal() - identical reasoning (a send to our own loopback
    // address never blocks, so this is safe from any task).
    if (sendto(node->hal.wake_sock, &one, sizeof(one), 0, (struct sockaddr*)&node->hal.wake_addr,
               sizeof(node->hal.wake_addr)) < 0) {
        TT_LOG_ERROR("Cannot send wake signal: %s", strerror(errno));
        return tt_RET_IO_ERROR;
    }
    return tt_RET_OK;
}

#if tt_SEGMENT_ENABLED
// No doorbell in the socket build: its tt_receive() waits in select(), which nothing but a socket can end, so a writer
// rings the owner over UDP - a zero-length datagram to its data port - as hal.h provides for a platform without one.
int32_t tt_segment_bell_create(struct tt_Context* node, const char* path) {
    (void)node;
    (void)path;
    return -1;
}

void tt_segment_bell_destroy(struct tt_Context* node, const char* path) {
    (void)node;
    (void)path;
}

int32_t tt_segment_bell_open(const char* path) {
    (void)path;
    return -1;
}

void tt_segment_bell_ring(int32_t bell) {
    (void)bell;
}

void tt_segment_bell_close(int32_t bell) {
    (void)bell;
}
#endif
#endif

// lwIP has no sendmmsg(), so a batch is one send each - the same number of calls core made before batching.
// UDP checksum 0 (hal_linux.c "UDP checksum 0", Linux's default) is not offered here: every datagram keeps its
// checksum, whatever a Linux peer was told.
int32_t tt_send_batch(struct tt_Context* node, const struct tt_OutDatagram* datagrams, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        const struct tt_OutDatagram* datagram = &datagrams[i];
        int32_t result;
        if (datagram->body_len != 0) {
            result = tt_send_iov(node, datagram->head, datagram->head_len, datagram->body, datagram->body_len,
                                 datagram->ip, datagram->port);
        } else if (datagram->ip == 0) {
            result = tt_send(node, datagram->head, datagram->head_len);
        } else {
            result = tt_send_to(node, datagram->head, datagram->head_len, datagram->ip, datagram->port);
        }
        if (result < 0) {
            return result;
        }
    }
    return (int32_t)count;
}

uint32_t tt_rx_buffered(const struct tt_Context* node) {
    (void)node;
    return 0; // every receive here asks the stack; nothing is held back
}

bool tt_rx_drain_start(struct tt_Context* node, uint32_t datagram_bytes) {
    (void)node;
    (void)datagram_bytes;
    return false; // lwIP's buffers are the stack's own: nothing to drain them into
}

uint32_t tt_rx_receiver_drops(const struct tt_Context* node) {
    (void)node;
    return 0; // not counted per connection here: every loss reads as path loss, as before
}
