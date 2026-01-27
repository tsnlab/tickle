#include "Example.h"

#include <tickle/hal.h>

struct tt_Topic ExampleTopic = {
    .name = "ExampleTopic",
    .data_size = sizeof(struct ExampleData),
    .data_encode_size = (tt_DATA_ENCODE_SIZE)ExampleData_encode_size,
    .data_encode = (tt_DATA_ENCODE)ExampleData_encode,
    .data_decode = (tt_DATA_DECODE)ExampleData_decode,
    .data_free = (tt_DATA_FREE)ExampleData_free,
};

int32_t ExampleData_encode_size(struct ExampleData* data) {
    return sizeof(data->data1) + sizeof(data->data2) + sizeof(data->data3);
}

int32_t ExampleData_encode(struct ExampleData* data, uint8_t* payload, const int32_t len) {
    int32_t encoded = 0;

    // encode - bool data1
    if (encoded + sizeof(bool) > len) {
        return -1;
    }
    memcpy(payload, &data->data1, sizeof(bool));
    encoded += sizeof(bool);
    payload += sizeof(bool);

    // encode - uint32_t data2
    if (encoded + sizeof(uint32_t) > len) {
        return -1;
    }
    memcpy(payload, &data->data2, sizeof(uint32_t));
    encoded += sizeof(uint32_t);
    payload += sizeof(uint32_t);

    // encode - double data3
    if (encoded + sizeof(double) > len) {
        return -1;
    }
    memcpy(payload, &data->data3, sizeof(double));
    encoded += sizeof(double);
    payload += sizeof(double);

    return encoded;
}

int32_t ExampleData_decode(struct ExampleData* data, const uint8_t* payload, const int32_t len, bool is_native_endian) {
    int32_t decoded = 0;

    /*
    NOTE: 1. is this function allowed to return error?
          2. misaligned dereferencing
    if (len < ExampleData_encode_size(data)) {
        return -1;
    }
    */

    // decode - bool data1
    memcpy(&data->data1, payload, sizeof(bool));
    decoded += sizeof(bool);
    payload += sizeof(bool);

    // decode - uint32_t data2
    memcpy(&data->data2, payload, sizeof(uint32_t));
    decoded += sizeof(uint32_t);
    payload += sizeof(uint32_t);

    // decode - double data3
    memcpy(&data->data3, payload, sizeof(double));
    decoded += sizeof(double);
    payload += sizeof(double);

    return decoded;
}

void ExampleData_free(struct ExampleData* data) {
}