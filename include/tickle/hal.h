#pragma once

#include <byteswap.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#if defined(__linux__)
#include <pthread.h> // NOLINT(misc-include-cleaner)
#endif
#include <string.h> // NOLINT(misc-include-cleaner)

// Platform detection macros
#if defined(__linux__)
#define TT_PLATFORM_LINUX
#define TT_PLATFORM_NAME "linux"
#else
#define TT_PLATFORM_GENERIC
#define TT_PLATFORM_NAME "generic"
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
#define ALIGN(n) (((n) + 4 - 1) & ~(4 - 1)) // 4 bytes alignment
#define ROUNDUP(n) ALIGN((n) + 4 - 1)       // 4 bytes roundup

#define NATIVE_MAGIC_VALUE (((uint16_t)'T' << 8) | 'K')
#define REVERSE_MAGIC_VALUE (((uint16_t)'K' << 8) | 'T')

enum tickle_error {
    tt_ERROR_NONE = 0,            // No error
    tt_CANNOT_CREATE_SOCK = -3,   // Failed to create the underlying socket
    tt_CANNOT_SET_REUSEADDR = -4, // Failed to enable socket address reuse
    tt_CANNOT_SET_BROADCAST = -5, // Failed to enable broadcast on the socket
    tt_CANNOT_SET_TIMEOUT = -6,   // Failed to configure socket timeout
    tt_CANNOT_BIND_SOCKET = -7,   // Failed to bind socket to local address/port
    tt_TOO_MANY_ENDPOINTS = -8,   // Node already has maximum endpoints registered
    tt_DUPLICATE_ENDPOINT = -9,   // Endpoint kind+id already exists in node registry
};

struct tt_Node;
struct tt_Header;

#ifdef TT_PLATFORM_LINUX
typedef pthread_mutex_t tt_lock_t; // NOLINT(misc-include-cleaner)
#else
typedef int tt_lock_t;
#endif

// Platform-specific HAL structure inclusion
#ifdef TT_PLATFORM_LINUX
#include <tickle/hal_linux.h> // NOLINT(misc-include-cleaner)
#elif defined(TT_PLATFORM_GENERIC)
#include <tickle/hal_generic.h> // NOLINT(misc-include-cleaner)
#endif

typedef uintptr_t tt_lock_state_t;

// Network functions
void tt_lock_init(tt_lock_t* lock);
void tt_lock_deinit(tt_lock_t* lock);
tt_lock_state_t tt_lock(tt_lock_t* lock);
void tt_unlock(tt_lock_t* lock, tt_lock_state_t state);
int32_t tt_get_node_id();
int32_t tt_bind(struct tt_Node* node);
void tt_close(struct tt_Node* node);
int32_t tt_send(struct tt_Node* node, const void* buf, size_t len);
int32_t tt_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port);
