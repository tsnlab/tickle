// Generated code
#pragma once

#include <stdint.h>

#include <tickle/config.h>
#include <tickle/tickle.h>

// Upper bound on the runtime-configurable payload size (see perf_client.c's -s flag): the most
// that fits in one message alongside its own framing - SubmessageHeader (4B) + DataHeader
// (16B) + BulkData's own seq/size fields (8B) = 32B - without exceeding tt_MAX_BUFFER_LENGTH,
// which is itself sized to the largest UDP payload a standard Ethernet frame can carry
// without IP fragmentation (see config.h). Derived rather than hardcoded so the two can't
// drift apart again. See "Message size: filling an Ethernet frame" in README.md.
#define BULK_MAX_PAYLOAD_SIZE (tt_MAX_BUFFER_LENGTH - 32)

struct BulkData {
    // Full 32-bit sequence number for loss detection. Not the same thing as the seq_no the
    // subscriber callback receives - that one comes off the wire as tt_DataHeader.seq_no
    // (uint32_t) but is narrowed to uint16_t by tt_SUBSCRIBER_CALLBACK's signature, so it
    // wraps every 65536 messages - easy to hit in a throughput test.
    uint32_t seq;
    uint32_t size; // actual number of meaningful bytes in `bytes` for this message
    uint8_t bytes[BULK_MAX_PAYLOAD_SIZE];
};

extern struct tt_Topic BulkTopic;

int32_t BulkData_encode_size(struct BulkData* data);
int32_t BulkData_encode(struct BulkData* data, uint8_t* payload, int32_t len);
int32_t BulkData_decode(struct BulkData* data, const uint8_t* payload, int32_t len, bool is_native_endian);
void BulkData_free(struct BulkData* data);
