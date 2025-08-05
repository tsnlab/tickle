// Generated code
#pragma oncde

#include <tickle.h>

struct UInt64Data {
    uint64_t data;
};

extern struct tt_Topic UInt64Topic;

int32_t UInt64Data_encode_size(struct UInt64Data* request);
int32_t UInt64Data_encode(struct UInt64Data* request, uint8_t* payload, const int32_t len);
int32_t UInt64Data_decode(struct UInt64Data* request, const uint8_t* payload, const int32_t len, bool is_native_endian);
void UInt64Data_free(struct UInt64Data* request);
