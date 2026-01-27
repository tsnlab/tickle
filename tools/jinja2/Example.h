#pragma once

#include <tickle/tickle.h>

struct ExampleData {
    bool data1;
    uint32_t data2;
    double data3;
};

extern struct tt_Topic ExampleTopic;

int32_t ExampleData_encode_size(struct ExampleData* data);
int32_t ExampleData_encode(struct ExampleData* data, uint8_t* payload, const int32_t len);
int32_t ExampleData_decode(struct ExampleData* data, const uint8_t* payload, const int32_t len, bool is_native_endian);
void ExampleData_free(struct ExampleData* data);
