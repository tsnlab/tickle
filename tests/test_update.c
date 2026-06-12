#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "../src/encoding.h"

// This file owns the shared test assertion state for this test binary.
#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"

// This file owns the HAL/time mocks used by the included tickle.c implementation.
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h" // NOLINT(misc-include-cleaner)

// Include the implementation to exercise static update helpers directly.
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../src/tickle.c"

// Test assertion macros and protocol constants intentionally trip these clang-tidy checks.
// NOLINTBEGIN(readability-function-cognitive-complexity,readability-magic-numbers,performance-no-int-to-ptr)

static uint32_t write_update_buffer(uint8_t* buffer, uint32_t buffer_size, uint64_t last_modified, uint32_t endpoint_id,
                                    uint8_t kind, const char* type, const char* name) {
    uint32_t tail = 0;

    struct tt_UpdateHeader* update_header = (struct tt_UpdateHeader*)buffer;
    update_header->last_modified = last_modified;
    update_header->entity_count = 1;
    tail += sizeof(*update_header);

    struct tt_UpdateEntity* update_entity = (struct tt_UpdateEntity*)(buffer + tail);
    update_entity->endpoint_id = endpoint_id;
    update_entity->kind = kind;
    tail += sizeof(*update_entity);

    EXPECT_TRUE(tt_encode_string(buffer, &tail, buffer_size, type));
    EXPECT_TRUE(tt_encode_string(buffer, &tail, buffer_size, name));

    return tail;
}

static void expect_cached_update_entity(struct tt_UpdateHeader* update, uint64_t last_modified, uint32_t endpoint_id,
                                        uint8_t kind, const char* expected_type, const char* expected_name) {
    EXPECT_TRUE(update != NULL);
    EXPECT_EQ_U32((uint32_t)last_modified, (uint32_t)update->last_modified);
    EXPECT_EQ_U32(1, update->entity_count);

    uint8_t* cursor = (uint8_t*)update + sizeof(*update);
    struct tt_UpdateEntity* entity = (struct tt_UpdateEntity*)cursor;
    EXPECT_EQ_U32(endpoint_id, entity->endpoint_id);
    EXPECT_EQ_U32(kind, entity->kind);
    cursor += sizeof(*entity);

    uint16_t type_len = *(uint16_t*)cursor;
    cursor += sizeof(type_len);
    char* type = (char*)cursor;
    cursor += type_len;
    EXPECT_EQ_U32(_tt_strnlen(expected_type, tt_MAX_STRING_LENGTH) + 1, type_len);
    EXPECT_TRUE(_tt_strncmp(type, expected_type, type_len) == 0);

    uint16_t name_len = *(uint16_t*)cursor;
    cursor += sizeof(name_len);
    char* name = (char*)cursor;
    EXPECT_EQ_U32(_tt_strnlen(expected_name, tt_MAX_STRING_LENGTH) + 1, name_len);
    EXPECT_TRUE(_tt_strncmp(name, expected_name, name_len) == 0);
}

static void expect_encoded_update_entity(uint8_t* buffer, uint32_t* head, uint32_t tail, uint32_t endpoint_id,
                                         uint8_t kind, const char* expected_type, const char* expected_name) {
    struct tt_UpdateEntity* entity = tt_decode_buffer(buffer, head, tail, sizeof(*entity));
    EXPECT_TRUE(entity != NULL);
    if (entity == NULL) {
        return;
    }

    EXPECT_EQ_U32(endpoint_id, entity->endpoint_id);
    EXPECT_EQ_U32(kind, entity->kind);

    uint16_t type_len = 0;
    char* type = NULL;
    EXPECT_TRUE(tt_decode_string(buffer, head, tail, &type_len, &type));
    if (type == NULL) {
        return;
    }
    EXPECT_EQ_U32(_tt_strnlen(expected_type, tt_MAX_STRING_LENGTH) + 1, type_len);
    EXPECT_TRUE(_tt_strncmp(type, expected_type, type_len) == 0);

    uint16_t name_len = 0;
    char* name = NULL;
    EXPECT_TRUE(tt_decode_string(buffer, head, tail, &name_len, &name));
    if (name == NULL) {
        return;
    }
    EXPECT_EQ_U32(_tt_strnlen(expected_name, tt_MAX_STRING_LENGTH) + 1, name_len);
    EXPECT_TRUE(_tt_strncmp(name, expected_name, name_len) == 0);
}

static void test_process_update_replaces_cached_update_without_losing_existing_on_error(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Header header;
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = 1;

    uint8_t buffer1[128];
    uint32_t tail1 =
        write_update_buffer(buffer1, sizeof(buffer1), 100, 0x1111, tt_KIND_TOPIC_PUBLISHER, "topic", "publisher1");

    EXPECT_TRUE(process_update(&node, &header, buffer1, 0, tail1));
    struct tt_UpdateHeader* first_update = node.updates[header.source];
    expect_cached_update_entity(first_update, 100, 0x1111, tt_KIND_TOPIC_PUBLISHER, "topic", "publisher1");

    uint8_t same_time_buffer[128];
    uint32_t same_time_tail = write_update_buffer(same_time_buffer, sizeof(same_time_buffer), 100, 0x2222,
                                                  tt_KIND_SERVICE_SERVER, "service", "server1");

    EXPECT_TRUE(process_update(&node, &header, same_time_buffer, 0, same_time_tail));
    EXPECT_TRUE(node.updates[header.source] == first_update);
    expect_cached_update_entity(node.updates[header.source], 100, 0x1111, tt_KIND_TOPIC_PUBLISHER, "topic",
                                "publisher1");

    uint8_t
        malformed_buffer[sizeof(struct tt_UpdateHeader) + sizeof(struct tt_UpdateEntity) + (2 * sizeof(uint16_t)) + 1];
    struct tt_UpdateHeader* malformed_header = (struct tt_UpdateHeader*)malformed_buffer;
    malformed_header->last_modified = 200;
    malformed_header->entity_count = 1;
    struct tt_UpdateEntity* malformed_entity =
        (struct tt_UpdateEntity*)(malformed_buffer + sizeof(struct tt_UpdateHeader));
    malformed_entity->endpoint_id = 0x2222;
    malformed_entity->kind = tt_KIND_TOPIC_PUBLISHER;
    uint16_t* malformed_type_len =
        (uint16_t*)(malformed_buffer + sizeof(struct tt_UpdateHeader) + sizeof(struct tt_UpdateEntity));
    *malformed_type_len = 4;

    EXPECT_TRUE(!process_update(&node, &header, malformed_buffer, 0, sizeof(malformed_buffer)));
    EXPECT_TRUE(node.updates[header.source] == first_update);

    uint8_t buffer2[128];
    uint32_t tail2 =
        write_update_buffer(buffer2, sizeof(buffer2), 300, 0x3333, tt_KIND_TOPIC_PUBLISHER, "topic", "publisher2");

    EXPECT_TRUE(process_update(&node, &header, buffer2, 0, tail2));
    EXPECT_TRUE(node.updates[header.source] != first_update);
    expect_cached_update_entity(node.updates[header.source], 300, 0x3333, tt_KIND_TOPIC_PUBLISHER, "topic",
                                "publisher2");

    _tt_free(node.updates[header.source]);
    node.updates[header.source] = NULL;
}

static void test_node_update_encodes_all_endpoint_kinds_with_last_modified(void) {
    test_mock_reset();

    struct tt_Node node;
    test_mock_now = 10;
    EXPECT_TRUE(tt_Node_create(&node) == 0);

    struct tt_Service service = {.name = "service"};
    struct tt_Topic topic = {.name = "topic"};
    struct tt_Client client;
    struct tt_Server server;
    struct tt_Publisher publisher;
    struct tt_Subscriber subscriber;

    test_mock_now = 100;
    EXPECT_TRUE(tt_Node_create_client(&node, &client, &service, "client", NULL) == 0);
    EXPECT_TRUE(node.last_modified == 100);

    test_mock_now = 200;
    EXPECT_TRUE(tt_Node_create_server(&node, &server, &service, "server", NULL) == 0);
    EXPECT_TRUE(node.last_modified == 200);

    test_mock_now = 300;
    EXPECT_TRUE(tt_Node_create_publisher(&node, &publisher, &topic, "publisher") == 0);
    EXPECT_TRUE(node.last_modified == 300);

    test_mock_now = 400;
    EXPECT_TRUE(tt_Node_create_subscriber(&node, &subscriber, &topic, "subscriber", NULL) == 0);
    EXPECT_TRUE(node.last_modified == 400);

    uint32_t tx_start = node.tx_tail;
    node_update(&node, 1000, NULL);
    EXPECT_TRUE(node.tx_tail > tx_start);

    uint32_t head = sizeof(struct tt_Header);
    struct tt_SubmessageHeader* submessage_header =
        tt_decode_buffer(node.tx_buffer, &head, node.tx_tail, sizeof(*submessage_header));
    EXPECT_TRUE(submessage_header != NULL);
    if (submessage_header == NULL) {
        return;
    }

    EXPECT_EQ_U32(tt_SUBMESSAGE_TYPE_UPDATE, submessage_header->type);
    EXPECT_EQ_U32(tt_SUBMESSAGE_ID_ALL, submessage_header->receiver);

    uint32_t update_tail = sizeof(struct tt_Header) + submessage_header->length;
    struct tt_UpdateHeader* update_header =
        tt_decode_buffer(node.tx_buffer, &head, update_tail, sizeof(*update_header));
    EXPECT_TRUE(update_header != NULL);
    if (update_header == NULL) {
        return;
    }

    EXPECT_TRUE(update_header->last_modified == 400);
    EXPECT_EQ_U32(4, update_header->entity_count);

    expect_encoded_update_entity(node.tx_buffer, &head, update_tail, client.endpoint.id, tt_KIND_SERVICE_CLIENT,
                                 "service", "client");
    expect_encoded_update_entity(node.tx_buffer, &head, update_tail, server.endpoint.id, tt_KIND_SERVICE_SERVER,
                                 "service", "server");
    expect_encoded_update_entity(node.tx_buffer, &head, update_tail, publisher.endpoint.id, tt_KIND_TOPIC_PUBLISHER,
                                 "topic", "publisher");
    expect_encoded_update_entity(node.tx_buffer, &head, update_tail, subscriber.endpoint.id, tt_KIND_TOPIC_SUBSCRIBER,
                                 "topic", "subscriber");
}

// NOLINTEND(readability-function-cognitive-complexity,readability-magic-numbers,performance-no-int-to-ptr)

int main(void) {
    test_process_update_replaces_cached_update_without_losing_existing_on_error();
    test_node_update_encodes_all_endpoint_kinds_with_last_modified();

    if (test_result() != 0) {
        return 1;
    }

    printf("update tests passed\n");
    return 0;
}
