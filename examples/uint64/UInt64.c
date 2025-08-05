// Generated code

#include "UInt64.h"

#include <hal.h>

struct tt_Topic UInt64Topic = {
    .name = "UInt64Topic",
    .data_size = sizeof(struct UInt64Data),
    .data_encode_size = (tt_DATA_ENCODE_SIZE)UInt64Data_encode_size,
    .data_encode = (tt_DATA_ENCODE)UInt64Data_encode,
    .data_decode = (tt_DATA_DECODE)UInt64Data_decode,
    .data_free = (tt_DATA_FREE)UInt64Data_free,
};

int32_t UInt64Data_encode_size(struct UInt64Data* data) {
    return sizeof(uint64_t); // encode data
}

int32_t UInt64Data_encode(struct UInt64Data* data, uint8_t* payload, const int32_t len) {
    int32_t encoded = 0;

    // encode data
    if (encoded + sizeof(uint64_t) > len) {
        return -1;
    }

    *(uint64_t*)payload = data->data;

    encoded += sizeof(uint64_t);
    payload += sizeof(uint64_t);

    return encoded;
}

int32_t UInt64Data_decode(struct UInt64Data* data, const uint8_t* payload, const int32_t len, bool is_native_endian) {
    int32_t decoded = 0;

    // decode data
    if (decoded + sizeof(uint64_t) > len) {
        return -1;
    }

    data->data = *(uint64_t*)payload;

    decoded += sizeof(uint64_t);
    payload += sizeof(uint64_t);

    return decoded;
}

void UInt64Data_free(struct UInt64Data* data) {
    // Do nothing
}
