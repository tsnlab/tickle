#pragma once

#include <byteswap.h>
#include <malloc.h>
#include <stdint.h>
#include <string.h>

#define _tt_bswap_16(x) bswap_16((x))
#define _tt_bswap_32(x) bswap_32((x))
#define _tt_bswap_64(x) bswap_64((x))
#define _tt_strnlen(s, maxlen) strnlen((s), (maxlen))
#define _tt_strncmp(s1, s2, n) strncmp((s1), (s2), (n))
#define _tt_malloc(size) malloc((size))
#define _tt_memcpy(dest, src, n) memcpy((dest), (src), (n))
#define _tt_memmove(dest, src, n) memmove((dest), (src), (n))
#define _tt_free(ptr) free((ptr))

enum tickle_error {
    tt_ERROR_NONE = 0,
    tt_CANNOT_CREATE_SOCK = -3,
    tt_CANNOT_SET_REUSEADDR = -4,
    tt_CANNOT_SET_BROADCAST = -5,
    tt_CANNOT_SET_TIMEOUT = -6,
    tt_CANNOT_BIND_SOCKET = -7,
};

struct tt_Node;
int32_t tt_get_node_id();
int32_t tt_bind(struct tt_Node* node);
void tt_close(struct tt_Node* node);
int32_t tt_send(struct tt_Node* node, const void* buf, size_t len);
int32_t tt_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port);
