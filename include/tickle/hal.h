#pragma once

#include <byteswap.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Platform detection macros
#ifdef __linux__
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
} tt_ret_t;

struct tt_Node;
struct tt_Header;

// Platform-specific HAL structure inclusion
#ifdef TT_PLATFORM_LINUX
#include <tickle/hal_linux.h> // NOLINT(misc-include-cleaner)
#elif defined(TT_PLATFORM_GENERIC)
#include <tickle/hal_generic.h> // NOLINT(misc-include-cleaner)
#endif

// Network functions
int32_t tt_get_node_id();
tt_ret_t tt_bind(struct tt_Node* node);
void tt_close(struct tt_Node* node);
int32_t tt_send(struct tt_Node* node, const void* buf, size_t len);
/**
 * @timeout I/O timeout in nanoseconds, -1 for use default timeout value, 0 for no timeout
 * @return received bytes, -1 for timeout, other negative values for I/O error
 */
int32_t tt_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout);
