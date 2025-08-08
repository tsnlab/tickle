// Generated code

#include "SetBool.h"

#include <hal.h>

struct tt_Service SetBoolService = {
    .name = "SetBoolService",
    .request_size = sizeof(struct SetBoolRequest),
    .response_size = sizeof(struct SetBoolResponse),
    .request_encode_size = (tt_REQUEST_ENCODE_SIZE)SetBoolRequest_encode_size,
    .request_encode = (tt_REQUEST_ENCODE)SetBoolRequest_encode,
    .request_decode = (tt_REQUEST_DECODE)SetBoolRequest_decode,
    .request_free = (tt_REQUEST_FREE)SetBoolRequest_free,
    .response_encode_size = (tt_RESPONSE_ENCODE_SIZE)SetBoolResponse_encode_size,
    .response_encode = (tt_RESPONSE_ENCODE)SetBoolResponse_encode,
    .response_decode = (tt_RESPONSE_DECODE)SetBoolResponse_decode,
    .response_free = (tt_RESPONSE_FREE)SetBoolResponse_free,
    .call_retry_interval = 0, // 0 means auto
    .call_retry_count = 3,
};

int32_t SetBoolRequest_encode_size(struct SetBoolRequest* request) {
    return sizeof(bool); // encode data
}

int32_t SetBoolRequest_encode(struct SetBoolRequest* request, uint8_t* payload, const int32_t len) {
    int32_t encoded = 0;

    // encode data
    if (encoded + sizeof(bool) > len) {
        return -1;
    }

    *(bool*)payload = request->data;

    encoded += sizeof(bool);
    payload += sizeof(bool);

    return encoded;
}

int32_t SetBoolRequest_decode(struct SetBoolRequest* request, const uint8_t* payload, const int32_t len,
                              bool is_native_endian) {
    int32_t decoded = 0;

    // decode data
    if (decoded + sizeof(bool) > len) {
        return -1;
    }

    request->data = *(bool*)payload;

    decoded += sizeof(bool);
    payload += sizeof(bool);

    return decoded;
}

void SetBoolRequest_free(struct SetBoolRequest* request) {
    // Do nothing
}

int32_t SetBoolResponse_encode_size(struct SetBoolResponse* response) {
    return sizeof(bool) +                                            // encode success
           sizeof(uint16_t) +                                        // encode message length
           _tt_strnlen(response->message, tt_MAX_STRING_LENGTH) + 1; // encode message
}

int32_t SetBoolResponse_encode(struct SetBoolResponse* response, uint8_t* payload, const int32_t len) {
    int32_t encoded = 0;

    // encode success
    if (encoded + sizeof(bool) > len) {
        return -1;
    }

    *(bool*)payload = response->success;

    encoded += sizeof(bool);
    payload += sizeof(bool);

    // encode message length
    if (encoded + sizeof(uint16_t) > len) {
        return -1;
    }

    uint16_t message_length = _tt_strnlen(response->message, tt_MAX_STRING_LENGTH) + 1; // including '\0'
    if (message_length > tt_MAX_STRING_LENGTH) {
        return -2;
    } else {
        ; // Do nothing
    }

    *(uint16_t*)payload = message_length;

    encoded += sizeof(uint16_t);
    payload += sizeof(uint16_t);

    // encode message
    if (encoded + message_length > len) {
        return -1;
    }

    if (memcpy(payload, response->message, message_length) != NULL) {
        payload += message_length;
        encoded += message_length;
    } else {
        return -2;
    }

    return encoded;
}

int32_t SetBoolResponse_decode(struct SetBoolResponse* response, const uint8_t* payload, const int32_t len,
                               bool is_native_endian) {
    int32_t decoded = 0;

    // decode success
    if (decoded + sizeof(bool) > len) {
        return -1;
    }

    response->success = *(bool*)payload;

    decoded += sizeof(bool);
    payload += sizeof(bool);

    // decode message length
    if (decoded + sizeof(uint16_t) > len) {
        return -1;
    }

    uint16_t message_length = *(uint16_t*)payload;
    if (!is_native_endian) {
        message_length = _tt_bswap_16(message_length);
    }

    decoded += sizeof(uint16_t);
    payload += sizeof(uint16_t);

    // decode message
    if (decoded + message_length > len) {
        return -1;
    }

    response->message = (char*)payload;

    decoded += message_length;
    payload += message_length;

    return decoded;
}

void SetBoolResponse_free(struct SetBoolResponse* response) {
    // Do nothing
}
