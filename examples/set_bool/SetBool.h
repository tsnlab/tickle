// Generated code
#pragma once

// This generated header relies on transitive includes from <tickle/tickle.h>.
// NOLINTBEGIN(misc-include-cleaner)
#include <tickle/tickle.h>

struct SetBoolRequest {
    bool data;
};

struct SetBoolResponse {
    bool success;
    char* message;
};

extern struct tt_Service SetBoolService;

int32_t SetBoolRequest_encode_size(struct SetBoolRequest* request);
int32_t SetBoolRequest_encode(struct SetBoolRequest* request, uint8_t* payload, int32_t len);
int32_t SetBoolRequest_decode(struct SetBoolRequest* request, const uint8_t* payload, int32_t len,
                              bool is_native_endian);
void SetBoolRequest_free(struct SetBoolRequest* request);
int32_t SetBoolResponse_encode_size(struct SetBoolResponse* response);
int32_t SetBoolResponse_encode(struct SetBoolResponse* response, uint8_t* payload, int32_t len);
int32_t SetBoolResponse_decode(struct SetBoolResponse* response, const uint8_t* payload, int32_t len,
                               bool is_native_endian);
void SetBoolResponse_free(struct SetBoolResponse* response);

// NOLINTEND(misc-include-cleaner)
