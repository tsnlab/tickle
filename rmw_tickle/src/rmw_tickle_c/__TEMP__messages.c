#include "__TEMP__messages.h"

#include "rmw_tickle_c/rmw_tickle.h"

struct tt_Topic StringTopic = {
    .name = "StringTopic",
    .data_size = sizeof(struct StringData),
    .data_encode_size = (tt_DATA_ENCODE_SIZE)StringData_encode_size,
    .data_encode = (tt_DATA_ENCODE)StringData_encode,
    .data_decode = (tt_DATA_DECODE)StringData_decode,
    .data_free = (tt_DATA_FREE)StringData_free,
};

int32_t StringData_encode_size(struct StringData* data) {
    return sizeof(uint16_t) + _tt_strnlen(data->data, tt_MAX_STRING_LENGTH) + 1;
}

int32_t StringData_encode(struct StringData* data, uint8_t* payload, const int32_t len) {
    int32_t encoded = 0;

    // encode data
    if (encoded + sizeof(uint16_t) > len) {
        return -1;
    }

    uint16_t message_length = _tt_strnlen(data->data, tt_MAX_STRING_LENGTH) + 1;
    if (message_length > tt_MAX_STRING_LENGTH) {
        return -2;
    } else {
        ; // Do nothing
    }

    *(uint16_t*)payload = message_length;

    encoded += sizeof(uint16_t);
    payload += sizeof(uint16_t);

    // encode data
    if (encoded + message_length > len) {
        return -1;
    }

    // Check if message is valid before copying
    if (data->data == NULL) {
        return -3; // Invalid message pointer
    }

    memcpy(payload, data->data, message_length);
    payload += message_length;
    encoded += message_length;

    return encoded;
}

int32_t StringData_decode(struct StringData* data, const uint8_t* payload, const int32_t len, bool is_native_endian) {
    int32_t decoded = 0;

    // decode message length
    if (decoded + sizeof(uint16_t) > len) {
        return -1;
    }

    uint16_t message_length = *(uint16_t*)payload;
    if (!is_native_endian) {
        message_length = _tt_bswap_16(message_length);
    }

    // Validate message length
    if (message_length == 0 || message_length > tt_MAX_STRING_LENGTH) {
        return -2; // Invalid message length
    }

    decoded += sizeof(uint16_t);
    payload += sizeof(uint16_t);

    // decode message
    if (decoded + message_length > len) {
        return -1;
    }

    // data->data = (char*)payload;
    data->data = strndup((char*)payload, message_length);

    decoded += message_length;
    payload += message_length;

    return decoded;
}

void StringData_free(struct StringData* data) {
    // Do nothing
}
