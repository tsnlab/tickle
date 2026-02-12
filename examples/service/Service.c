#include "srv/Service.h"

#include <tickle/hal.h>

int32_t ServiceRequest_encode_size(struct service__srv__ServiceRequest* data);
int32_t ServiceRequest_encode(struct service__srv__ServiceRequest* data, uint8_t* payload, const int32_t len);
int32_t ServiceRequest_decode(struct service__srv__ServiceRequest* data, const uint8_t* payload, const int32_t len, bool is_native_endian);
void ServiceRequest_free(struct service__srv__ServiceRequest* data);

int32_t ServiceResponse_encode_size(struct service__srv__ServiceResponse* data);
int32_t ServiceResponse_encode(struct service__srv__ServiceResponse* data, uint8_t* payload, const int32_t len);
int32_t ServiceResponse_decode(struct service__srv__ServiceResponse* data, const uint8_t* payload, const int32_t len, bool is_native_endian);
void ServiceResponse_free(struct service__srv__ServiceResponse* data);

struct tt_Service service__srv__ServiceService = {
    .name = "service__srv__ServiceService",
    .request_size = sizeof(struct service__srv__ServiceRequest),
    .response_size = sizeof(struct service__srv__ServiceResponse),
    .request_encode_size = (tt_REQUEST_ENCODE_SIZE)ServiceRequest_encode_size,
    .request_encode = (tt_REQUEST_ENCODE)ServiceRequest_encode,
    .request_decode = (tt_REQUEST_DECODE)ServiceRequest_decode,
    .request_free = (tt_REQUEST_FREE)ServiceRequest_free,
    .response_encode_size = (tt_RESPONSE_ENCODE_SIZE)ServiceResponse_encode_size,
    .response_encode = (tt_RESPONSE_ENCODE)ServiceResponse_encode,
    .response_decode = (tt_RESPONSE_DECODE)ServiceResponse_decode,
    .response_free = (tt_RESPONSE_FREE)ServiceResponse_free,
    .call_retry_interval = 0, // 0 means auto
    .call_retry_count = 3,
};

typedef struct service__srv__ServiceRequest ServiceRequest;
typedef struct service__srv__ServiceResponse ServiceResponse;

int32_t ServiceRequest_encode_size(ServiceRequest* data) {
    return sizeof(bool) +
           sizeof(float) +
           10 * sizeof(uint32_t) +
           32 * sizeof(char);
}

int32_t ServiceRequest_encode(ServiceRequest* data, uint8_t* payload, const int32_t len) {
    int32_t encoded = 0;

    if (ServiceRequest_encode_size(data) > len) {
        return -1;
    }
    // encode - bool status
    _tt_memcpy(payload, &data->status, sizeof(bool));
    encoded += sizeof(bool);
    payload += sizeof(bool);

    // encode - float fp
    _tt_memcpy(payload, &data->fp, sizeof(float));
    encoded += sizeof(float);
    payload += sizeof(float);

    // encode - uint32_t array
    _tt_memcpy(payload, &data->array, 10 * sizeof(uint32_t));
    encoded += 10 * sizeof(uint32_t);
    payload += 10 * sizeof(uint32_t);

    // encode - char* msg
    _tt_memcpy(payload, &data->msg, 32 * sizeof(char));
    encoded += 32 * sizeof(char);
    payload += 32 * sizeof(char);

    return encoded;
}

int32_t ServiceRequest_decode(ServiceRequest* data, const uint8_t* payload, const int32_t len, bool is_native_endian) {
    int32_t decoded = 0;

    if (ServiceRequest_encode_size(data) > len) {
        return -1;
    }

    // NOTE: using memcpy to avoid misaligned dereferencing
    // decode - bool status
    _tt_memcpy(&data->status, payload, sizeof(bool));
    decoded += sizeof(bool);
    payload += sizeof(bool);

    // decode - float fp
    _tt_memcpy(&data->fp, payload, sizeof(float));
    decoded += sizeof(float);
    payload += sizeof(float);

    // decode - uint32_t array
    _tt_memcpy(&data->array, payload, 10 * sizeof(uint32_t));
    decoded += 10 * sizeof(uint32_t);
    payload += 10 * sizeof(uint32_t);

    // decode - char* msg
    _tt_memcpy(&data->msg, payload, 32 * sizeof(char));
    decoded += 32 * sizeof(char);
    payload += 32 * sizeof(char);

    return decoded;
}

void ServiceRequest_free(ServiceRequest* data) {
}
int32_t ServiceResponse_encode_size(ServiceResponse* data) {
    return sizeof(bool) +
           sizeof(float) +
           10 * sizeof(uint32_t) +
           32 * sizeof(char);
}

int32_t ServiceResponse_encode(ServiceResponse* data, uint8_t* payload, const int32_t len) {
    int32_t encoded = 0;

    if (ServiceResponse_encode_size(data) > len) {
        return -1;
    }
    // encode - bool status
    _tt_memcpy(payload, &data->status, sizeof(bool));
    encoded += sizeof(bool);
    payload += sizeof(bool);

    // encode - float fp
    _tt_memcpy(payload, &data->fp, sizeof(float));
    encoded += sizeof(float);
    payload += sizeof(float);

    // encode - uint32_t array
    _tt_memcpy(payload, &data->array, 10 * sizeof(uint32_t));
    encoded += 10 * sizeof(uint32_t);
    payload += 10 * sizeof(uint32_t);

    // encode - char* msg
    _tt_memcpy(payload, &data->msg, 32 * sizeof(char));
    encoded += 32 * sizeof(char);
    payload += 32 * sizeof(char);

    return encoded;
}

int32_t ServiceResponse_decode(ServiceResponse* data, const uint8_t* payload, const int32_t len, bool is_native_endian) {
    int32_t decoded = 0;

    if (ServiceResponse_encode_size(data) > len) {
        return -1;
    }

    // NOTE: using memcpy to avoid misaligned dereferencing
    // decode - bool status
    _tt_memcpy(&data->status, payload, sizeof(bool));
    decoded += sizeof(bool);
    payload += sizeof(bool);

    // decode - float fp
    _tt_memcpy(&data->fp, payload, sizeof(float));
    decoded += sizeof(float);
    payload += sizeof(float);

    // decode - uint32_t array
    _tt_memcpy(&data->array, payload, 10 * sizeof(uint32_t));
    decoded += 10 * sizeof(uint32_t);
    payload += 10 * sizeof(uint32_t);

    // decode - char* msg
    _tt_memcpy(&data->msg, payload, 32 * sizeof(char));
    decoded += 32 * sizeof(char);
    payload += 32 * sizeof(char);

    return decoded;
}

void ServiceResponse_free(ServiceResponse* data) {
}