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

struct tt_Topic HeaderTopic = {
    .name = "HeaderTopic",
    .data_size = sizeof(struct HeaderData),
    .data_encode_size = (tt_DATA_ENCODE_SIZE)HeaderData_encode_size,
    .data_encode = (tt_DATA_ENCODE)HeaderData_encode,
    .data_decode = (tt_DATA_DECODE)HeaderData_decode,
    .data_free = (tt_DATA_FREE)HeaderData_free,
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

int32_t HeaderData_encode_size(struct HeaderData* data) {
    return sizeof(int32_t) + sizeof(uint32_t) + sizeof(uint16_t) + _tt_strnlen(data->frame_id, tt_MAX_STRING_LENGTH) + 1;
}

int32_t HeaderData_encode(struct HeaderData* data, uint8_t* payload, const int32_t len) {
    int32_t encoded = 0;

    // encode stamp.sec
    if (encoded + sizeof(int32_t) > len) {
        return -1;
    }

    *(int32_t*)payload = data->stamp.sec;

    encoded += sizeof(int32_t);
    payload += sizeof(int32_t);

    // encode stamp.nanosec
    if (encoded + sizeof(uint32_t) > len) {
        return -1;
    }

    *(uint32_t*)payload = data->stamp.nanosec;

    encoded += sizeof(uint32_t);
    payload += sizeof(uint32_t);

    // encode frame_id length
    if (encoded + sizeof(uint16_t) > len) {
        return -1;
    }

    uint16_t message_length = _tt_strnlen(data->frame_id, tt_MAX_STRING_LENGTH) + 1;
    if (message_length > tt_MAX_STRING_LENGTH) {
        return -2;
    } else {
        ; // Do nothing
    }

    *(uint16_t*)payload = message_length;

    encoded += sizeof(uint16_t);
    payload += sizeof(uint16_t);

    // encode frame_id
    if (encoded + message_length > len) {
        return -1;
    }

    // Check if message is valid before copying
    if (data->frame_id == NULL) {
        return -3; // Invalid message pointer
    }

    memcpy(payload, data->frame_id, message_length);
    payload += message_length;
    encoded += message_length;

    return encoded;
}

int32_t HeaderData_decode(struct HeaderData* data, const uint8_t* payload, const int32_t len, bool is_native_endian) {
    int32_t decoded = 0;

    // decode stamp.sec
    if (decoded + sizeof(int32_t) > len) {
        return -1;
    }

    data->stamp.sec = *(int32_t*)payload;
    if (!is_native_endian) {
        data->stamp.sec = _tt_bswap_32(data->stamp.sec);
    }

    decoded += sizeof(int32_t);
    payload += sizeof(int32_t);

    // decode stamp.nanosec
    if (decoded + sizeof(uint32_t) > len) {
        return -1;
    }

    data->stamp.nanosec = *(uint32_t*)payload;
    if (!is_native_endian) {
        data->stamp.nanosec = _tt_bswap_32(data->stamp.nanosec);
    }

    decoded += sizeof(uint32_t);
    payload += sizeof(uint32_t);

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

    // decode frame_id
    if (decoded + message_length > len) {
        return -1;
    }

    // data->frame_id = (char*)payload;
    data->frame_id = strndup((char*)payload, message_length);

    decoded += message_length;
    payload += message_length;

    return decoded;
}

void HeaderData_free(struct HeaderData* data) {
    // TODO: Free strings
    // Do nothing
}
