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
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
} tt_ret_t;

struct tt_Node;
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
int32_t tt_get_node_id(void);
tt_ret_t tt_bind(struct tt_Node* node);
void tt_close(struct tt_Node* node);
int32_t tt_send(struct tt_Node* node, const void* buf, size_t len);
// Sends buf to a specific unicast destination instead of the node's usual broadcast address -
// used only where the destination is already known precisely (a server's CallResponse, unicast
// straight back to the CallRequest's own source - see process_callrequest() in tickle.c) rather
// than needing the broadcast that discovery/pub-sub still relies on. ip/port are host byte order,
// matching tt_receive()'s own ip/port out-params (the two are meant to be used together: the ip/
// port a packet arrived with are exactly what a reply back to it should be sent with).
int32_t tt_send_to(struct tt_Node* node, const void* buf, size_t len, uint32_t ip, uint16_t port);

// Scatter-gather send: one datagram made of `hdr` (framing this node built) followed by `body`
// (payload the publisher handed over, still in its own memory) - no staging copy into one
// contiguous buffer first. ip == 0 means the usual broadcast address, otherwise that unicast
// destination (same convention as flush_tx()'s peer list). Returns total bytes sent, negative on
// error. Used by tt_Publisher_publish()'s standalone-packet path when the topic offers
// data_encode_inplace.
int32_t tt_send_iov(struct tt_Node* node, const void* hdr, size_t hdr_len, const void* body, size_t body_len,
                    uint32_t ip, uint16_t port);
/**
 * @timeout I/O timeout in nanoseconds, -1 for use default timeout value, 0 for no timeout
 * @return received bytes, -1 for timeout, other negative values for I/O error
 */
int32_t tt_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout);

// Non-blocking single receive: pulls one datagram if one is already waiting, without any poll()
// wait. tt_Node_poll() uses this to drain whatever else the kernel has buffered after tt_receive()
// hands it the first packet, so a saturated receiver pays one poll() per drain rather than one
// per packet. Returns received bytes, -1 if nothing is waiting, other negatives for I/O error.
int32_t tt_try_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port);
