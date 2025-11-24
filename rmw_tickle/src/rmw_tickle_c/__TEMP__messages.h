#pragma once

#include "rmw_tickle_c/rmw_tickle.h"

struct StringData {
    char* data;
};

extern struct tt_Topic StringTopic;

int32_t StringData_encode_size(struct StringData* data);
int32_t StringData_encode(struct StringData* data, uint8_t* payload, const int32_t len);
int32_t StringData_decode(struct StringData* data, const uint8_t* payload, const int32_t len, bool is_native_endian);
void StringData_free(struct StringData* data);
