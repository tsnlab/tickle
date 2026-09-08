#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox: process_packet()/process_submessage() are static, and this is exactly the
// untrusted-input boundary (broadcast UDP, no authentication - see README's "Security &
// concurrency model") that most needs coverage against malformed/adversarial input.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

#define LOCAL_NODE_ID 1
#define REMOTE_NODE_ID 2

static void init_node(struct tt_Node* node) {
    memset(node, 0, sizeof(*node));
    node->id = LOCAL_NODE_ID;
}

static void write_header(uint8_t* buf, uint16_t magic, uint8_t version, uint8_t source) {
    struct tt_Header* header = (struct tt_Header*)buf;
    header->magic_value = magic;
    header->version = version;
    header->source = source;
}

// Appends a submessage header at `offset` and returns the offset just past it (where the
// submessage body, if any, would go).
static uint32_t append_submessage_header(uint8_t* buf, uint32_t offset, uint8_t type, uint8_t receiver,
                                         uint16_t length) {
    struct tt_SubmessageHeader* submessage_header = (struct tt_SubmessageHeader*)(buf + offset);
    submessage_header->type = type;
    submessage_header->receiver = receiver;
    submessage_header->length = length;
    return offset + sizeof(struct tt_SubmessageHeader);
}

static uint32_t append_data_header(uint8_t* buf, uint32_t offset, uint32_t endpoint_id, uint32_t seq_no) {
    struct tt_DataHeader* data_header = (struct tt_DataHeader*)(buf + offset);
    data_header->endpoint_id = endpoint_id;
    data_header->seq_no = seq_no;
    data_header->timestamp = 0;
    return offset + sizeof(struct tt_DataHeader);
}

// A packet shorter than tt_Header itself must be rejected, not read past the buffer.
static void test_rejects_truncated_header(void) {
    struct tt_Node node;
    init_node(&node);

    uint8_t buf[sizeof(struct tt_Header) - 1];
    memset(buf, 0xff, sizeof(buf));

    EXPECT_TRUE(!process_packet(&node, buf, 0, sizeof(buf)));
}

// Neither the native nor byte-swapped magic value - not this protocol at all (random noise, or
// some other application broadcasting on the same port/address).
static void test_rejects_bad_magic(void) {
    struct tt_Node node;
    init_node(&node);

    uint8_t buf[sizeof(struct tt_Header)];
    write_header(buf, 0xdead, tt_VERSION, REMOTE_NODE_ID);

    EXPECT_TRUE(!process_packet(&node, buf, 0, sizeof(buf)));
}

// A peer speaking an older wire version than we understand must be rejected, not misparsed as
// if it were current-version.
static void test_rejects_old_version(void) {
    struct tt_Node node;
    init_node(&node);

    uint8_t buf[sizeof(struct tt_Header)];
    write_header(buf, NATIVE_MAGIC_VALUE, tt_VERSION - 1, REMOTE_NODE_ID);

    EXPECT_TRUE(!process_packet(&node, buf, 0, sizeof(buf)));
}

// A node hears its own broadcast back (normal on a shared broadcast domain) and must ignore it
// cleanly rather than treating it as an error or trying to process it as if from a peer.
static void test_ignores_self_sent_packet(void) {
    struct tt_Node node;
    init_node(&node);

    uint8_t buf[sizeof(struct tt_Header)];
    write_header(buf, NATIVE_MAGIC_VALUE, tt_VERSION, LOCAL_NODE_ID);

    EXPECT_TRUE(process_packet(&node, buf, 0, sizeof(buf)));
}

// A submessage claiming a length shorter than its own header can't be real - reject before the
// later `length - sizeof(header)` arithmetic can underflow.
static void test_rejects_submessage_length_too_small(void) {
    struct tt_Node node;
    init_node(&node);

    uint8_t buf[64];
    write_header(buf, NATIVE_MAGIC_VALUE, tt_VERSION, REMOTE_NODE_ID);
    uint32_t offset = sizeof(struct tt_Header);
    uint32_t tail = append_submessage_header(buf, offset, tt_SUBMESSAGE_TYPE_DATA, tt_SUBMESSAGE_ID_ALL,
                                             sizeof(struct tt_SubmessageHeader) - 1);

    EXPECT_TRUE(!process_packet(&node, buf, 0, tail));
}

// A submessage claiming to be far longer than the bytes actually available must be rejected,
// not trusted into reading (or letting a codec read) past the end of the real buffer.
static void test_rejects_submessage_length_exceeds_buffer(void) {
    struct tt_Node node;
    init_node(&node);

    uint8_t buf[64];
    write_header(buf, NATIVE_MAGIC_VALUE, tt_VERSION, REMOTE_NODE_ID);
    uint32_t offset = sizeof(struct tt_Header);
    // Only sizeof(struct tt_SubmessageHeader) bytes actually follow the header in this buffer,
    // but the submessage claims to be 0xffff bytes long.
    uint32_t tail = append_submessage_header(buf, offset, tt_SUBMESSAGE_TYPE_DATA, tt_SUBMESSAGE_ID_ALL, 0xffff);

    EXPECT_TRUE(!process_packet(&node, buf, 0, tail));
}

// A type value outside the known set entirely (not even a recognized-but-unimplemented one).
static void test_rejects_unknown_submessage_type(void) {
    struct tt_Node node;
    init_node(&node);

    uint8_t buf[64];
    write_header(buf, NATIVE_MAGIC_VALUE, tt_VERSION, REMOTE_NODE_ID);
    uint32_t offset = sizeof(struct tt_Header);
    uint32_t tail = append_submessage_header(buf, offset, 99, tt_SUBMESSAGE_ID_ALL, sizeof(struct tt_SubmessageHeader));

    EXPECT_TRUE(!process_packet(&node, buf, 0, tail));
}

// ACKNACK is a recognized type value with no implementation behind it - process_submessage()
// must say so and fail, not silently ignore it or fall through to some other handler.
static void test_rejects_acknack_as_unsupported(void) {
    struct tt_Node node;
    init_node(&node);

    uint8_t buf[64];
    write_header(buf, NATIVE_MAGIC_VALUE, tt_VERSION, REMOTE_NODE_ID);
    uint32_t offset = sizeof(struct tt_Header);
    uint32_t tail = append_submessage_header(buf, offset, tt_SUBMESSAGE_TYPE_ACKNACK, tt_SUBMESSAGE_ID_ALL,
                                             sizeof(struct tt_SubmessageHeader));

    EXPECT_TRUE(!process_packet(&node, buf, 0, tail));
}

// Sanity check in the other direction: two well-formed DATA submessages back to back in one
// packet (the batching case node_flush()/tx_buffer exist for) must both parse and the loop must
// terminate cleanly at the real end of the buffer - malformed-input rejection elsewhere in this
// file shouldn't come at the cost of also rejecting valid, non-trivial input.
static void test_accepts_two_valid_data_submessages(void) {
    struct tt_Node node;
    init_node(&node);
    // No subscriber is registered for either endpoint_id: process_data() treats that as
    // "not for me", not an error, so this only needs the parse itself to succeed.

    uint8_t buf[128];
    write_header(buf, NATIVE_MAGIC_VALUE, tt_VERSION, REMOTE_NODE_ID);
    uint32_t offset = sizeof(struct tt_Header);

    uint16_t submessage_length = sizeof(struct tt_SubmessageHeader) + sizeof(struct tt_DataHeader);
    offset = append_submessage_header(buf, offset, tt_SUBMESSAGE_TYPE_DATA, tt_SUBMESSAGE_ID_ALL, submessage_length);
    offset = append_data_header(buf, offset, 0x1111, 1);

    offset = append_submessage_header(buf, offset, tt_SUBMESSAGE_TYPE_DATA, tt_SUBMESSAGE_ID_ALL, submessage_length);
    offset = append_data_header(buf, offset, 0x2222, 2);

    EXPECT_TRUE(process_packet(&node, buf, 0, offset));
}

int main(void) {
    test_rejects_truncated_header();
    test_rejects_bad_magic();
    test_rejects_old_version();
    test_ignores_self_sent_packet();
    test_rejects_submessage_length_too_small();
    test_rejects_submessage_length_exceeds_buffer();
    test_rejects_unknown_submessage_type();
    test_rejects_acknack_as_unsupported();
    test_accepts_two_valid_data_submessages();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_malformed_packets: all tests passed\n");
    return 0;
}
