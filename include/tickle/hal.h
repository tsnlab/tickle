#pragma once

#include <stdbool.h>
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

// Memory alignment macros
#define ALIGN(n) (((n) + 4 - 1) & ~(4 - 1)) // 4 bytes alignment
#define ROUNDUP(n) ALIGN((n) + 4 - 1)       // 4 bytes roundup

enum tickle_error {
    tt_ERROR_NONE = 0,
    tt_CANNOT_CREATE_SOCK = -3,
    tt_CANNOT_SET_REUSEADDR = -4,
    tt_CANNOT_SET_BROADCAST = -5,
    tt_CANNOT_SET_TIMEOUT = -6,
    tt_CANNOT_BIND_SOCKET = -7,
};

struct tt_Node;
struct tt_Header;

// Endian checking functions
bool tt_is_native_endian(struct tt_Header* header);
bool tt_is_reverse_endian(struct tt_Header* header);

// Hash function
uint32_t tt_hash_id(const char* type, const char* name);

// Basic encoding/decoding utility functions
void* tt_encode_buffer(void* buffer, uint32_t* tail, uint32_t len);
void* tt_decode_buffer(void* buffer, uint32_t* head, uint32_t tail, uint32_t length);

// String encoding/decoding functions
bool tt_encode_string(void* buffer, uint32_t* tail, uint32_t buffer_size, const char* str);
bool tt_decode_string(void* buffer, uint32_t* head, uint32_t tail, uint16_t* str_len, char** str);

// Network functions
int32_t tt_get_node_id();
int32_t tt_bind(struct tt_Node* node);
void tt_close(struct tt_Node* node);
int32_t tt_send(struct tt_Node* node, const void* buf, size_t len);
int32_t tt_receive(struct tt_Node* node, void* buf, size_t len, uint32_t* ip, uint16_t* port);
