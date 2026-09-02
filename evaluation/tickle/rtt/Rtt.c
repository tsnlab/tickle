#include "rtt/msg/Rtt.h"

#include <tickle/hal.h>

static int32_t RttData_encode_size(struct rtt__msg__RttData* data);
static int32_t RttData_encode(struct rtt__msg__RttData* data, uint8_t* payload, const int32_t len);
static int32_t RttData_decode(struct rtt__msg__RttData* data, const uint8_t* payload, const int32_t len, bool is_native_endian);
static void RttData_free(struct rtt__msg__RttData* data);

struct tt_Topic rtt__msg__RttTopic = {
    .name = "rtt__msg__RttTopic",
    .data_size = sizeof(struct rtt__msg__RttData),
    .data_encode_size = (tt_DATA_ENCODE_SIZE)RttData_encode_size,
    .data_encode = (tt_DATA_ENCODE)RttData_encode,
    .data_decode = (tt_DATA_DECODE)RttData_decode,
    .data_free = (tt_DATA_FREE)RttData_free,
};

typedef struct rtt__msg__RttData RttData;

int32_t RttData_encode_size(RttData* data) {
    return sizeof(uint64_t);
}

int32_t RttData_encode(RttData* data, uint8_t* payload, const int32_t len) {
    int32_t encoded = 0;
    int32_t ret;
    int32_t size;

    if (RttData_encode_size(data) > len) {
        return -1;
    }
    // encode - uint64_t payload
    _tt_memcpy(payload, &data->payload, sizeof(uint64_t));
    encoded += sizeof(uint64_t);
    payload += sizeof(uint64_t);

    return encoded;
}

int32_t RttData_decode(RttData* data, const uint8_t* payload, const int32_t len, bool is_native_endian) {
    int32_t decoded = 0;
    int32_t ret;
    int32_t size;

    if (RttData_encode_size(data) > len) {
        return -1;
    }

    // NOTE: using memcpy to avoid misaligned dereferencing
    // decode - uint64_t payload
    _tt_memcpy(&data->payload, payload, sizeof(uint64_t));
    decoded += sizeof(uint64_t);
    payload += sizeof(uint64_t);

    return decoded;
}

void RttData_free(RttData* data) {
}
