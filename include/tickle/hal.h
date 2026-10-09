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

#include <byteswap.h>
#include <stdbool.h> // tt_resolve_link()
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h> // tt_CONTEXT_ID_CLAIM, tt_MAX_CONTEXT_IDS

// Platform detection macros. Only these two platforms have a real HAL (src/hal_freertos.c,
// src/hal_linux.c) - there is no generic/fallback implementation, so an unsupported host fails
// here at compile time instead of later at link time with a confusing "undefined reference to
// tt_bind" (or, worse, silently picking up whatever hal_*.c happens to be on the include path).
#ifdef TT_PLATFORM_FREERTOS
// Set by platform/freertos's own build (-DTT_PLATFORM_FREERTOS) - unlike __linux__ below,
// FreeRTOS itself defines no standard compiler macro to detect it by.
#define TT_PLATFORM_NAME "freertos"
#elif defined(__linux__)
#define TT_PLATFORM_LINUX
#define TT_PLATFORM_NAME "linux"
#else
#error \
    "No TickLE HAL for this platform - supported: Linux (native build), FreeRTOS (-DTT_PLATFORM_FREERTOS, see platform/freertos)"
#endif

#define _tt_bswap_16(x) bswap_16((x))
#define _tt_bswap_32(x) bswap_32((x))
#define _tt_bswap_64(x) bswap_64((x))
#define _tt_strnlen(s, maxlen) strnlen((s), (maxlen))
#define _tt_strncmp(s1, s2, n) strncmp((s1), (s2), (n))
#define _tt_malloc(size) malloc((size))
#define _tt_memcpy(dest, src, n) memcpy((dest), (src), (n))
#define _tt_memmove(dest, src, n) memmove((dest), (src), (n))
#define _tt_free(ptr) free((ptr))

// Memory alignment macros
#define ALIGN(n) ((n) & ~(4 - 1))     // 4 bytes alignment
#define ROUNDUP(n) ALIGN((n) + 4 - 1) // 4 bytes roundup

#define NATIVE_MAGIC_VALUE (((uint16_t)'T' << 8) | 'K')
#define REVERSE_MAGIC_VALUE (((uint16_t)'K' << 8) | 'T')

typedef enum tt_ret_t {
    tt_RET_OK = 0,
    tt_RET_TIMEOUT = -1,
    tt_RET_IO_ERROR = -2,
    tt_RET_PROTOCOL_ERROR = -3,
    tt_RET_OUT_OF_MEMORY = -4,
    tt_RET_OUT_OF_BUFFER = -5,
    tt_RET_OUT_OF_SCHEDULE = -6,
    tt_RET_IILEGAL_NODE_ID = -7,
    tt_RET_IILEGAL_ENDPOINT_ID = -8,
    tt_RET_ILLEGAL_STATUS = -9,
    tt_RET_INVALID_ARGUMENT = -10, // NULL pointer, or an out-of-range size in a tt_Service/tt_Topic
    tt_RET_INTERRUPTED = -11,      // tt_Context_poll() was woken by tt_Context_interrupt() rather than by
                                   // data, a due scheduler entry, or its own timeout - see tt_Context_
                                   // interrupt()'s own comment in tickle.h.
    tt_RET_NOT_FOUND = -12,        // tt_Server_send_response()'s own request_id doesn't match any
                                   // request still waiting on a response - already answered, timed
                                   // out, or never deferred in the first place. See tickle.h's own
                                   // tt_Server_send_response() doc comment.
    tt_RET_NO_SUCH_LINK = -14,     // A configured link names a broadcast address no local interface
                                   // owns (struct _tt_Link, config.h). Its own code rather than
                                   // tt_RET_INVALID_ARGUMENT deliberately: the caller has to be
                                   // able to tell "the interface I was told to use is not here
                                   // yet" from "your arguments are malformed", because the first
                                   // is worth retrying after a boot race - an interface brought up
                                   // by DHCP or a network manager may simply not exist when a
                                   // service starts - and the second never is. Collapsing them
                                   // would force a caller to treat every creation failure as
                                   // retryable or none of them.
                                   //
                                   // The limited broadcast 255.255.255.255 is NOT this error: no
                                   // interface owns it by definition, it is the compiled-in
                                   // default, and it is the catch-all that makes an unconfigured
                                   // node work at all.
    tt_RET_UNSUPPORTED = -16,      // This build requires something the running system refuses - today only
                                   // tt_HAL_RX_HINT=tt_RX_HINT_URING on a kernel or container that does not allow
                                   // io_uring (hal_linux.h). Not retryable: the same system refuses it again.
    tt_RET_WOULD_BLOCK = -13,      // Phase 3 (rmw_tickle/PLAN.md) - tt_Publisher_publish() on a
                                   // KEEP_ALL Publisher whose next write would have to evict a
                                   // sample no matched Subscriber has acknowledged yet. Nothing was
                                   // sent and nothing was cached; the caller decides whether to
                                   // wait, drop or retry. Deliberately its own code rather than a
                                   // generic error: it is the normal, expected outcome of flow
                                   // control, not a failure. See tt_Publisher.keep_all and
                                   // tt_Publisher_writable() (tickle.h).
    tt_RET_BUSY = -15,             // tt_Context_poll() called while another thread is already polling
                                   // the same node. One poller at a time is part of the threading
                                   // contract ("Threading", tickle.h): the two would share rx_buffer.
                                   // Nothing was done; the call is a caller bug, reported rather than
                                   // allowed to corrupt a datagram mid-decode.
} tt_ret_t;

struct tt_Context;
struct tt_Header;

// Platform-specific HAL structure inclusion
#ifdef TT_PLATFORM_LINUX
#include <tickle/hal_linux.h> // NOLINT(misc-include-cleaner)
#elif defined(TT_PLATFORM_FREERTOS)
#include <tickle/hal_freertos.h> // NOLINT(misc-include-cleaner)
#endif

// Network functions - every one of these is implemented per platform (src/hal_linux.c,
// src/hal_freertos.c, .../tests/test_mock.h's mock) against this same contract.
uint64_t tt_get_ns(void);
// The finest step a timed wait in tt_receive() can end on, in ns, as tt_get_ns() can see it: the coarser of the
// clock's resolution and the wait's own (one RTOS tick on FreeRTOS). The floor of the measured timer lateness that
// the retry timers use as G (struct tt_Context.timer_lateness_ns). Never 0.
uint64_t tt_timer_resolution_ns(void);
int32_t tt_get_node_id(void);
tt_ret_t tt_bind(struct tt_Context* node);
void tt_close(struct tt_Context* node);
int32_t tt_send(struct tt_Context* node, const void* buf, size_t len);
// Sends buf to a specific unicast destination instead of the node's usual broadcast address -
// used only where the destination is already known precisely (a server's CallResponse, unicast
// straight back to the CallRequest's own source - see process_callrequest() in tickle.c) rather
// than needing the broadcast that discovery/pub-sub still relies on. ip/port are host byte order,
// matching tt_receive()'s own ip/port out-params (the two are meant to be used together: the ip/
// port a packet arrived with are exactly what a reply back to it should be sent with).
int32_t tt_send_to(struct tt_Context* node, const void* buf, size_t len, uint32_t ip, uint16_t port);

// Scatter-gather send: one datagram made of `hdr` (framing this node built) followed by `body`
// (payload the publisher handed over, still in its own memory) - no staging copy into one
// contiguous buffer first. ip == 0 means the usual broadcast address, otherwise that unicast
// destination (same convention as flush_tx()'s peer list). Returns total bytes sent, negative on
// error. Used by tt_Publisher_publish()'s standalone-packet path when the topic offers
// data_encode_inplace.
int32_t tt_send_iov(struct tt_Context* node, const void* hdr, size_t hdr_len, const void* body, size_t body_len,
                    uint32_t ip, uint16_t port);

// One datagram for tt_send_batch(): `head`, followed by `body` unless body_len is 0, to ip/port - or, with ip
// 0, to the node's usual broadcast address, the convention tt_send_iov() uses.
struct tt_OutDatagram {
    const void* head;
    size_t head_len;
    const void* body;
    size_t body_len;
    uint32_t ip;
    uint16_t port;
};

// Sends `count` datagrams in order, in as few system calls as the platform allows: sendmmsg() on Linux, one
// send each where there is no such call. Returns count, or a negative value if any failed, in which case
// that datagram and every one after it may be unsent. Used where core has several datagrams ready at once
// - a sample's fragments, or one datagram to several peers - so that the number of system calls stops
// following the number of datagrams.
int32_t tt_send_batch(struct tt_Context* node, const struct tt_OutDatagram* datagrams, uint32_t count);
/**
 * @timeout I/O timeout in nanoseconds, -1 for use default timeout value, 0 for no timeout
 * @return received bytes, -1 for timeout, -3 if woken by tt_wake_signal() rather than data,
 *         other negative values for I/O error
 */
int32_t tt_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout);

// Resolves a configured broadcast address to the local interface that owns it, filling *addr and
// *netmask (host byte order) from that interface. Returns false when no local interface has that
// broadcast address - which includes the limited broadcast 255.255.255.255, since no interface
// owns it.
//
// *bcast is filled with the parsed broadcast address either way, so a caller can still address a
// link whose broadcast no interface owns without having to parse the string itself. Only *addr and
// *netmask depend on the return value.
//
// Exists because matching a peer to a link needs the link's netmask, and a broadcast address does
// not carry one: x.y.z.255 implies /24 only by convention. The operating system already knows
// every interface's address, netmask and broadcast together, so this asks it rather than guessing.
bool tt_resolve_link(const char* broadcast, uint32_t* addr, uint32_t* netmask, uint32_t* bcast);

#if tt_CONTEXT_ID_CLAIM
// (g8) Claims a context id on this host for `node`: `preferred` when no other live context holds it and `avoid` (a
// tt_MAX_CONTEXT_IDS-bit set, or NULL) does not name it; otherwise a free id not in `avoid` - the highest, counting
// down from 254, or with `salt` non-zero the one salt picks among the free ones, so that two contexts choosing from
// the same view do not choose alike. The id this context held before, if any, is released. 0: none free. Without a
// host registry the choice is made from `avoid` alone, and the link settles any clash. tt_close() releases the id.
uint8_t tt_claim_context_id(struct tt_Context* node, uint8_t preferred, const uint8_t* avoid, uint32_t salt);

#if tt_SEGMENT_ENABLED
// The shared-memory segment's platform half (SHM_PLAN.md stage 1). Only the mapping is here; the
// ring, the naming and the header validation are core's, so they are the same code and the same
// tests on every platform and only the way pages are obtained differs.
//
// tt_segment_create(): the region this context owns, sized `bytes`, created fresh - any previous
// one of the same name is replaced, because a segment left by a dead context of the same name is
// exactly what the header's incarnation exists to detect and there is no reason to inherit it.
// Returns NULL on failure, which is not fatal: the context simply has no segment and its peers
// reach it over UDP.
void* tt_segment_create(const char* path, size_t bytes);
// tt_segment_attach(): map a peer's existing region read/write. Does not create. `bytes` is what
// the caller expects; a region smaller than that is refused, since the caller is about to index
// slots inside it. Returns NULL and sets *why to the reason, which the caller counts.
//
// *why carries an `enum tt_SegmentAttach` value (tickle.h) as a uint8_t rather than the enum
// itself: this header is included by tickle.h and cannot include it back, and the alternative -
// moving a transport type into config.h to break the cycle - would put it where nothing else of
// its kind lives. Only tt_SEGMENT_ABSENT, tt_SEGMENT_REFUSED and tt_SEGMENT_ORPHANED are produced
// here; the header checks that produce the rest are core's, because they are the same on every
// platform.
//
// Ownership: a region whose owner has gone must be refused with tt_SEGMENT_ORPHANED, because its
// header stays valid after the owner's death and nothing read through it can say otherwise. The
// region tt_segment_create() returns is therefore marked as owned for as long as the creating
// context maps it (Linux: an exclusive flock() held by the mapping), and attach asks before mapping.
// A platform whose regions cannot outlive their owner (no named files) never produces it.
void* tt_segment_attach(const char* path, size_t bytes, uint8_t* why);
void tt_segment_detach(void* mapping, size_t bytes);
// Removes the name. The mapping survives in anyone who still holds it, which is why a reader checks
// the incarnation rather than trusting that a name still resolves to the segment it attached to.
void tt_segment_unlink(const char* path);
// The doorbell beside a segment (SHM_PLAN.md 7.1, 2026-10-04): a named FIFO, so a writer that has the segment's name
// has the bell's too, and its read end joins the reader's existing wait (tt_receive()) - no second wait point, no
// thread. It replaces a zero-length UDP datagram, which cost the writer a trip through the socket layer and the reader
// a ppoll, a recvfrom and an empty recvmmsg per wake-up. A platform without one returns -1 from create and open, and
// the writer rings over UDP as before; tt_receive() reports a rung bell as a zero-length datagram, which is what a
// doorbell has always been to core.
//   create  - the owner's read end, kept in the HAL and added to tt_receive()'s wait. 0, or -1 if there is none.
//   destroy - closes it and removes the name.
//   open    - a writer's end of a peer's bell, or -1. ring writes one byte and never blocks; close closes it.
int32_t tt_segment_bell_create(struct tt_Context* node, const char* path);
void tt_segment_bell_destroy(struct tt_Context* node, const char* path);
int32_t tt_segment_bell_open(const char* path);
void tt_segment_bell_ring(int32_t bell);
void tt_segment_bell_close(int32_t bell);
// CPU time the calling thread has used, in ns: what a reader's own share of a waiting mode costs
// (segment_epoch_turn()). Read once per ring's worth of records, so it may be a system call. 0 where it cannot be
// told, which leaves only the wall-clock half of the comparison.
uint64_t tt_thread_cpu_ns(void);
#endif
// (g8) Whether ip:port - host order, as tt_receive() reports a sender - is `node`'s own data socket, which every
// datagram it sends comes from.
bool tt_is_own_address(const struct tt_Context* node, uint32_t ip, uint16_t port);
// (g8) `node`'s own data socket, for ordering two contexts that hold one id.
void tt_own_address(const struct tt_Context* node, uint32_t* ip, uint16_t* port);
#endif

// The MTU of the local interface that owns `addr` (host byte order), or -1 when it cannot be told.
// Asked once per resolved link at node creation, so core can say when the link is narrower than the
// 1500-byte Ethernet MTU tt_ETHERNET_UDP_PAYLOAD assumes (config.h) - see resolve_links() (tickle.c).
int32_t tt_link_mtu(uint32_t addr);

// Non-blocking single receive: pulls one datagram if one is already waiting, without any poll()
// wait. tt_Context_poll() uses this to drain whatever else the kernel has buffered after tt_receive()
// hands it the first packet, so a saturated receiver pays one poll() per drain rather than one
// per packet. Returns received bytes, -1 if nothing is waiting, other negatives for I/O error.
int32_t tt_try_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port);

// How many datagrams the HAL already holds, read from the socket and not yet handed out, that the next
// tt_try_receive() calls return without a system call (hal_linux.c's recvmmsg() batch; 0 on a HAL with
// none). Core keeps its state lock across such datagrams and never across a read from the socket
// (OPTIMIZATION_PLAN.md 11.4, D4).
uint32_t tt_rx_buffered(const struct tt_Context* node);

// Could a non-blocking read find anything? Asked by a poll loop that is busy running scheduled work and wants to
// check its sockets without paying a system call to learn that nothing came (2026-10-04: a max-rate segment
// publisher spent a third of its system time on empty reads). true means "may be": the caller then reads with
// tt_try_receive(), which may still find nothing. false must mean nothing has arrived since the HAL last looked. A
// HAL with no cheaper way to know returns true, which is exactly the behaviour before this function existed.
bool tt_rx_maybe_ready(struct tt_Context* node);

// Wakes a concurrent tt_receive() blocked on this node (from any thread, including this one - a
// self-signal), making it return -3 immediately instead of waiting out the rest of its timeout.
// Safe to call whether or not a call is currently blocked; if none is, the signal is simply
// waiting for the next one (see each platform's own tt_bind()/tt_receive() for how - an eventfd
// on Linux, watched by the same poll() as the real sockets; on FreeRTOS a flag plus the semaphore that
// tt_receive() waits on, or a loopback UDP socket in its select() with tt_HAL_FREERTOS_NETCONN=0).
// tt_Context_interrupt() (tickle.h) is the public entry point that calls this.
tt_ret_t tt_wake_signal(struct tt_Context* node);
