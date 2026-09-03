// Generated code
#pragma once

#include <stdint.h>

#include <tickle/tickle.h>

struct PingPongRequest {
    uint32_t seq;
    uint64_t timestamp; // sender's tt_get_ns() at send time, echoed back for RTT measurement
};

struct PingPongResponse {
    uint32_t seq;
    uint64_t timestamp; // echo of the request's timestamp
};

extern struct tt_Service PingPongService;

int32_t PingPongRequest_encode_size(struct PingPongRequest* request);
int32_t PingPongRequest_encode(struct PingPongRequest* request, uint8_t* payload, int32_t len);
int32_t PingPongRequest_decode(struct PingPongRequest* request, const uint8_t* payload, int32_t len,
                               bool is_native_endian);
void PingPongRequest_free(struct PingPongRequest* request);
int32_t PingPongResponse_encode_size(struct PingPongResponse* response);
int32_t PingPongResponse_encode(struct PingPongResponse* response, uint8_t* payload, int32_t len);
int32_t PingPongResponse_decode(struct PingPongResponse* response, const uint8_t* payload, int32_t len,
                                bool is_native_endian);
void PingPongResponse_free(struct PingPongResponse* response);
