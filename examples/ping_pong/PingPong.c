// Generated code

#include "PingPong.h"

#include <stdint.h>

#include <tickle/hal.h>
#include <tickle/tickle.h>

struct tt_Service PingPongService = {
    .name = "PingPongService",
    .request_size = sizeof(struct PingPongRequest),
    .response_size = sizeof(struct PingPongResponse),
    .request_encode_size = (tt_REQUEST_ENCODE_SIZE)PingPongRequest_encode_size,
    .request_encode = (tt_REQUEST_ENCODE)PingPongRequest_encode,
    .request_decode = (tt_REQUEST_DECODE)PingPongRequest_decode,
    .request_free = (tt_REQUEST_FREE)PingPongRequest_free,
    .response_encode_size = (tt_RESPONSE_ENCODE_SIZE)PingPongResponse_encode_size,
    .response_encode = (tt_RESPONSE_ENCODE)PingPongResponse_encode,
    .response_decode = (tt_RESPONSE_DECODE)PingPongResponse_decode,
    .response_free = (tt_RESPONSE_FREE)PingPongResponse_free,
    // call_retry_count is 0 here so a ping is a single send-and-wait, like real ping's ICMP
    // echo: no silent retransmit on loss, one probe, one outcome (reply or drop).
    // Note: the "0 means tt_CALL_RETRY_COUNT" doc comment on this field (tickle.h) isn't
    // actually honored by call_retry() - it's compared directly, so 0 truly means zero
    // retries (give up on the very first timeout check), not "use the default".
    .call_retry_interval = 0, // 0 means auto
    .call_retry_count = 0,
};

int32_t PingPongRequest_encode_size(struct PingPongRequest* request) {
    (void)request;
    return sizeof(uint32_t) + sizeof(uint64_t); // encode seq + timestamp
}

int32_t PingPongRequest_encode(struct PingPongRequest* request, uint8_t* payload, uint32_t len) {
    int32_t encoded = 0;

    if (encoded + sizeof(uint32_t) > len) {
        return -1;
    }

    *(uint32_t*)payload = request->seq;

    encoded += sizeof(uint32_t);
    payload += sizeof(uint32_t);

    if (encoded + sizeof(uint64_t) > len) {
        return -1;
    }

    *(uint64_t*)payload = request->timestamp;

    encoded += sizeof(uint64_t);
    payload += sizeof(uint64_t);

    return encoded;
}

int32_t PingPongRequest_decode(struct PingPongRequest* request, const uint8_t* payload, uint32_t len,
                               bool is_native_endian) {
    int32_t decoded = 0;

    if (decoded + sizeof(uint32_t) > len) {
        return -1;
    }

    uint32_t seq = *(uint32_t*)payload;
    if (!is_native_endian) {
        seq = _tt_bswap_32(seq);
    }
    request->seq = seq;

    decoded += sizeof(uint32_t);
    payload += sizeof(uint32_t);

    if (decoded + sizeof(uint64_t) > len) {
        return -1;
    }

    uint64_t timestamp = *(uint64_t*)payload;
    if (!is_native_endian) {
        timestamp = _tt_bswap_64(timestamp);
    }
    request->timestamp = timestamp;

    decoded += sizeof(uint64_t);
    payload += sizeof(uint64_t);

    return decoded;
}

void PingPongRequest_free(struct PingPongRequest* request) {
    (void)request;
    // Do nothing
}

int32_t PingPongResponse_encode_size(struct PingPongResponse* response) {
    (void)response;
    return sizeof(uint32_t) + sizeof(uint64_t); // encode seq + timestamp
}

int32_t PingPongResponse_encode(struct PingPongResponse* response, uint8_t* payload, uint32_t len) {
    int32_t encoded = 0;

    if (encoded + sizeof(uint32_t) > len) {
        return -1;
    }

    *(uint32_t*)payload = response->seq;

    encoded += sizeof(uint32_t);
    payload += sizeof(uint32_t);

    if (encoded + sizeof(uint64_t) > len) {
        return -1;
    }

    *(uint64_t*)payload = response->timestamp;

    encoded += sizeof(uint64_t);
    payload += sizeof(uint64_t);

    return encoded;
}

int32_t PingPongResponse_decode(struct PingPongResponse* response, const uint8_t* payload, uint32_t len,
                                bool is_native_endian) {
    int32_t decoded = 0;

    if (decoded + sizeof(uint32_t) > len) {
        return -1;
    }

    uint32_t seq = *(uint32_t*)payload;
    if (!is_native_endian) {
        seq = _tt_bswap_32(seq);
    }
    response->seq = seq;

    decoded += sizeof(uint32_t);
    payload += sizeof(uint32_t);

    if (decoded + sizeof(uint64_t) > len) {
        return -1;
    }

    uint64_t timestamp = *(uint64_t*)payload;
    if (!is_native_endian) {
        timestamp = _tt_bswap_64(timestamp);
    }
    response->timestamp = timestamp;

    decoded += sizeof(uint64_t);
    payload += sizeof(uint64_t);

    return decoded;
}

void PingPongResponse_free(struct PingPongResponse* response) {
    (void)response;
    // Do nothing
}
