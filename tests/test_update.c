#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

static const uint8_t k_test_update_source = 7;
static const uint8_t k_test_update_entity_count = 1;
static const uint32_t k_test_old_update_endpoint_id = 0x1111;
static const uint32_t k_test_new_update_endpoint_id = 0x2222;
static const uint64_t k_test_old_update_last_modified = 100;
static const uint64_t k_test_new_update_last_modified = 200;
static const uint64_t k_test_update_last_seen = 300;
static const uint64_t k_test_update_message_time = 400;

static bool process_update(struct tt_Node* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
                           uint32_t tail);
static void node_update(struct tt_Node* node, uint64_t time, void* param); // NOLINT(readability-redundant-declaration)

// This file owns the shared test assertion state for this test binary.
#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"

// This file owns the HAL/time mocks used by the included tickle.c implementation.
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h" // NOLINT(misc-include-cleaner)

static uint32_t append_test_update_string(uint8_t* buffer, uint32_t offset, const char* value) {
    size_t string_length = strlen(value) + 1;
    uint16_t encoded_length = (uint16_t)string_length;

    memcpy(buffer + offset, &encoded_length, sizeof(encoded_length));
    offset += sizeof(encoded_length);

    memcpy(buffer + offset, value, encoded_length);
    return offset + encoded_length;
}

static uint32_t build_test_update_payload(uint8_t* buffer, uint64_t last_modified, uint32_t endpoint_id,
                                          const char* type_name, const char* endpoint_name) {
    struct tt_UpdateHeader* update_header = (struct tt_UpdateHeader*)buffer;
    update_header->last_modified = last_modified;
    update_header->entity_count = k_test_update_entity_count;

    uint32_t offset = sizeof(*update_header);
    struct tt_UpdateEntity* update_entity = (struct tt_UpdateEntity*)(buffer + offset);
    update_entity->endpoint_id = endpoint_id;
    update_entity->kind = tt_KIND_SERVICE_SERVER;
    offset += sizeof(*update_entity);

    offset = append_test_update_string(buffer, offset, type_name);
    offset = append_test_update_string(buffer, offset, endpoint_name);
    return offset;
}

static struct tt_UpdateEntity* first_test_update_entity(struct tt_UpdateHeader* update_header) {
    return (struct tt_UpdateEntity*)((uint8_t*)update_header + sizeof(*update_header));
}

static struct tt_UpdateHeader* first_encoded_update_header(struct tt_Node* node,
                                                           struct tt_SubmessageHeader** submessage_header) {
    *submessage_header = (struct tt_SubmessageHeader*)(node->tx_buffer + sizeof(struct tt_Header));
    return (struct tt_UpdateHeader*)(node->tx_buffer + sizeof(struct tt_Header) + sizeof(struct tt_SubmessageHeader));
}

static void test_node_update_encodes_topic_publisher_entity(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = sizeof(node.tx_buffer);
    node.last_modified = k_test_new_update_last_modified;

    struct tt_Topic topic;
    memset(&topic, 0, sizeof(topic));
    topic.name = "topic";

    struct tt_Publisher pub;
    memset(&pub, 0, sizeof(pub));
    pub.endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    pub.endpoint.id = k_test_new_update_endpoint_id;
    pub.endpoint.name = "publisher";
    pub.topic = &topic;

    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&pub;

    test_mock_reset();
    test_mock_now = k_test_update_message_time;
    node_update(&node, 0, NULL);

    struct tt_SubmessageHeader* submessage_header = NULL;
    struct tt_UpdateHeader* update_header = first_encoded_update_header(&node, &submessage_header);
    struct tt_UpdateEntity* update_entity = first_test_update_entity(update_header);

    EXPECT_TRUE(submessage_header->type == tt_SUBMESSAGE_TYPE_UPDATE);
    EXPECT_TRUE(submessage_header->receiver == tt_SUBMESSAGE_ID_ALL);
    EXPECT_TRUE(update_header->last_modified == k_test_update_message_time);
    EXPECT_EQ_U32(1, update_header->entity_count);
    EXPECT_EQ_U32(k_test_new_update_endpoint_id, update_entity->endpoint_id);
    EXPECT_TRUE(update_entity->kind == tt_KIND_TOPIC_PUBLISHER);
}

static void test_process_update_replaces_cached_update_when_last_modified_changes(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.source = k_test_update_source;

    uint8_t old_buffer[tt_MAX_NAME_LENGTH * 4];
    uint32_t old_length = build_test_update_payload(old_buffer, k_test_old_update_last_modified,
                                                    k_test_old_update_endpoint_id, "old_type", "old_endpoint");
    struct tt_UpdateHeader* old_update = _tt_malloc(old_length);
    memcpy(old_update, old_buffer, old_length);
    node.remotes[header.source].update = old_update;
    node.remotes[header.source].update_length = old_length;

    uint8_t new_buffer[tt_MAX_NAME_LENGTH * 4];
    uint32_t new_length = build_test_update_payload(new_buffer, k_test_new_update_last_modified,
                                                    k_test_new_update_endpoint_id, "new_type", "new_endpoint");

    test_mock_now = k_test_update_last_seen;
    EXPECT_TRUE(process_update(&node, &header, new_buffer, 0, new_length));
    EXPECT_TRUE(node.remotes[header.source].update != old_update);
    EXPECT_TRUE(node.remotes[header.source].update->last_modified == k_test_new_update_last_modified);
    EXPECT_TRUE(node.remotes[header.source].last_modified == k_test_new_update_last_modified);
    EXPECT_TRUE(node.remotes[header.source].last_seen == k_test_update_last_seen);
    EXPECT_EQ_U32(new_length, node.remotes[header.source].update_length);
    EXPECT_EQ_U32(k_test_update_entity_count, node.remotes[header.source].update->entity_count);
    EXPECT_EQ_U32(k_test_new_update_endpoint_id,
                  first_test_update_entity(node.remotes[header.source].update)->endpoint_id);

    _tt_free(node.remotes[header.source].update);
    node.remotes[header.source].update = NULL;
}

static void test_process_update_keeps_cached_update_when_last_modified_matches(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.source = k_test_update_source;

    uint8_t old_buffer[tt_MAX_NAME_LENGTH * 4];
    uint32_t old_length = build_test_update_payload(old_buffer, k_test_old_update_last_modified,
                                                    k_test_old_update_endpoint_id, "old_type", "old_endpoint");
    struct tt_UpdateHeader* old_update = _tt_malloc(old_length);
    memcpy(old_update, old_buffer, old_length);
    node.remotes[header.source].update = old_update;
    node.remotes[header.source].update_length = old_length;

    uint8_t new_buffer[tt_MAX_NAME_LENGTH * 4];
    uint32_t new_length = build_test_update_payload(new_buffer, k_test_old_update_last_modified,
                                                    k_test_new_update_endpoint_id, "new_type", "new_endpoint");

    test_mock_now = k_test_update_last_seen;
    EXPECT_TRUE(process_update(&node, &header, new_buffer, 0, new_length));
    EXPECT_TRUE(node.remotes[header.source].update == old_update);
    EXPECT_TRUE(node.remotes[header.source].update->last_modified == k_test_old_update_last_modified);
    EXPECT_TRUE(node.remotes[header.source].last_modified == k_test_old_update_last_modified);
    EXPECT_TRUE(node.remotes[header.source].last_seen == k_test_update_last_seen);
    EXPECT_EQ_U32(old_length, node.remotes[header.source].update_length);
    EXPECT_EQ_U32(k_test_old_update_endpoint_id,
                  first_test_update_entity(node.remotes[header.source].update)->endpoint_id);

    _tt_free(node.remotes[header.source].update);
    node.remotes[header.source].update = NULL;
}

static void test_tt_node_destroy_clears_cached_updates(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));
    node.tx_tail = sizeof(struct tt_Header);
    node.tx_size = sizeof(node.tx_buffer);

    uint8_t update_buffer[tt_MAX_NAME_LENGTH * 4];
    uint32_t update_length = build_test_update_payload(update_buffer, k_test_old_update_last_modified,
                                                       k_test_old_update_endpoint_id, "old_type", "old_endpoint");
    struct tt_UpdateHeader* update = _tt_malloc(update_length);
    memcpy(update, update_buffer, update_length);
    node.remotes[k_test_update_source].update = update;
    node.remotes[k_test_update_source].update_length = update_length;

    EXPECT_TRUE(tt_Node_destroy(&node) == 0);
    EXPECT_TRUE(node.remotes[k_test_update_source].update == NULL);
    EXPECT_TRUE(node.remotes[k_test_update_source].last_seen == 0);
    EXPECT_TRUE(node.remotes[k_test_update_source].last_modified == 0);
    EXPECT_EQ_U32(0, node.remotes[k_test_update_source].update_length);
}

// Include the implementation to exercise static update helpers directly.
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../src/tickle.c"

int main(void) {
    test_node_update_encodes_topic_publisher_entity();
    test_process_update_replaces_cached_update_when_last_modified_changes();
    test_process_update_keeps_cached_update_when_last_modified_matches();
    test_tt_node_destroy_clears_cached_updates();

    if (test_result() != 0) {
        return 1;
    }

    printf("update tests passed\n");
    return 0;
}
