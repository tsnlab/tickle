#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

static const uint32_t k_test_endpoint_id = 0x1234;
static const uint64_t k_test_data_timestamp = 1234;
static struct tt_Endpoint* find_endpoint(struct tt_Node* node, uint8_t kind, uint32_t endpoint_id);
static int32_t add_endpoint_to_node(struct tt_Node* node, struct tt_Endpoint* endpoint);
static bool remove_endpoint_from_node(struct tt_Node* node, struct tt_Endpoint* endpoint);
static bool process_data(struct tt_Node* node, struct tt_Header* header, uint8_t* buffer, uint32_t head, uint32_t tail);
static bool process_callrequest(struct tt_Node* node, struct tt_Header* header, uint8_t* buffer, uint32_t head,
                                uint32_t tail);

// This file owns the shared test assertion state for this test binary.
#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"

// This file owns the HAL/time mocks used by the included tickle.c implementation.
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h" // NOLINT(misc-include-cleaner)

static void test_add_endpoint_to_node_success(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Endpoint endpoint;
    endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    endpoint.id = k_test_endpoint_id;
    endpoint.name = "topic_pub";

    int32_t result = add_endpoint_to_node(&node, &endpoint);
    EXPECT_TRUE(result == tt_ERROR_NONE);
    EXPECT_EQ_U32(1, node.endpoint_count);
    EXPECT_TRUE(node.endpoints[0] == &endpoint);
}

static void test_add_endpoint_to_node_too_many_endpoints(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));
    node.endpoint_count = tt_MAX_ENDPOINT_COUNT;

    struct tt_Endpoint endpoint;
    endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    endpoint.id = k_test_endpoint_id;
    endpoint.name = "topic_pub";

    int32_t result = add_endpoint_to_node(&node, &endpoint);
    EXPECT_TRUE(result == tt_TOO_MANY_ENDPOINTS);
    EXPECT_EQ_U32(tt_MAX_ENDPOINT_COUNT, node.endpoint_count);
}

static void test_add_endpoint_to_node_duplicate_returns_duplicate_endpoint(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Endpoint endpoint1;
    endpoint1.kind = tt_KIND_SERVICE_SERVER;
    endpoint1.id = k_test_endpoint_id;
    endpoint1.name = "server";

    struct tt_Endpoint endpoint2 = endpoint1;

    EXPECT_TRUE(add_endpoint_to_node(&node, &endpoint1) == tt_ERROR_NONE);
    EXPECT_TRUE(add_endpoint_to_node(&node, &endpoint2) == tt_DUPLICATE_ENDPOINT);
    EXPECT_EQ_U32(1, node.endpoint_count);
}

static void test_add_endpoint_to_node_allows_same_topic_subscribers(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Endpoint subscriber1 = {.kind = tt_KIND_TOPIC_SUBSCRIBER,
                                      .id = tt_hash_id("topic", "subscriber1"),
                                      .name = "subscriber1"};
    struct tt_Endpoint subscriber2 = {.kind = tt_KIND_TOPIC_SUBSCRIBER,
                                      .id = tt_hash_id("topic", "subscriber2"),
                                      .name = "subscriber2"};

    EXPECT_TRUE(add_endpoint_to_node(&node, &subscriber1) == tt_ERROR_NONE);
    EXPECT_TRUE(add_endpoint_to_node(&node, &subscriber2) == tt_ERROR_NONE);
    EXPECT_EQ_U32(2, node.endpoint_count);
    EXPECT_TRUE(node.endpoints[0] == &subscriber1);
    EXPECT_TRUE(node.endpoints[1] == &subscriber2);
}

static void test_add_endpoint_to_node_locks_endpoint_registry(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Endpoint endpoint;
    endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    endpoint.id = k_test_endpoint_id;
    endpoint.name = "topic_pub";

    test_mock_reset();

    EXPECT_TRUE(add_endpoint_to_node(&node, &endpoint) == tt_ERROR_NONE);
    EXPECT_EQ_U32(1, test_mock_lock_count);
    EXPECT_EQ_U32(1, test_mock_unlock_count);
    EXPECT_EQ_U32(0, test_mock_lock_depth);
    EXPECT_EQ_U32(1, test_mock_max_lock_depth);
    EXPECT_TRUE(test_mock_last_lock == &node.endpoint_lock);
}

static void test_add_endpoint_to_node_unlocks_on_error(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));
    node.endpoint_count = tt_MAX_ENDPOINT_COUNT;

    struct tt_Endpoint endpoint;
    endpoint.kind = tt_KIND_TOPIC_PUBLISHER;
    endpoint.id = k_test_endpoint_id;
    endpoint.name = "topic_pub";

    test_mock_reset();

    EXPECT_TRUE(add_endpoint_to_node(&node, &endpoint) == tt_TOO_MANY_ENDPOINTS);
    EXPECT_EQ_U32(1, test_mock_lock_count);
    EXPECT_EQ_U32(1, test_mock_unlock_count);
    EXPECT_EQ_U32(0, test_mock_lock_depth);
    EXPECT_TRUE(test_mock_last_lock == &node.endpoint_lock);
}

static void test_find_endpoint_locks_endpoint_registry(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Endpoint endpoint;
    endpoint.kind = tt_KIND_SERVICE_SERVER;
    endpoint.id = k_test_endpoint_id;
    endpoint.name = "server";

    node.endpoint_count = 1;
    node.endpoints[0] = &endpoint;

    test_mock_reset();

    EXPECT_TRUE(find_endpoint(&node, endpoint.kind, endpoint.id) == &endpoint);
    EXPECT_EQ_U32(1, test_mock_lock_count);
    EXPECT_EQ_U32(1, test_mock_unlock_count);
    EXPECT_EQ_U32(0, test_mock_lock_depth);
    EXPECT_TRUE(test_mock_last_lock == &node.endpoint_lock);
}

static void expect_endpoint_registry_shifted_after_middle_remove(const struct tt_Node* node,
                                                                 const struct tt_Endpoint* endpoint1,
                                                                 const struct tt_Endpoint* endpoint3) {
    EXPECT_EQ_U32(2, node->endpoint_count);
    EXPECT_TRUE(node->endpoints[0] == endpoint1);
    EXPECT_TRUE(node->endpoints[1] == endpoint3);
    EXPECT_TRUE(node->endpoints[2] == NULL);
}

static void expect_endpoint_registry_lock_used(const struct tt_Node* node) {
    EXPECT_EQ_U32(1, test_mock_lock_count);
    EXPECT_EQ_U32(1, test_mock_unlock_count);
    EXPECT_EQ_U32(0, test_mock_lock_depth);
    EXPECT_TRUE(test_mock_last_lock == &node->endpoint_lock);
}

static void test_remove_endpoint_from_node_locks_and_shifts_endpoint_registry(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Endpoint endpoint1 = {.kind = tt_KIND_TOPIC_PUBLISHER, .id = 1, .name = "publisher1"};
    struct tt_Endpoint endpoint2 = {.kind = tt_KIND_TOPIC_PUBLISHER, .id = 2, .name = "publisher2"};
    struct tt_Endpoint endpoint3 = {.kind = tt_KIND_TOPIC_PUBLISHER, .id = 3, .name = "publisher3"};

    node.endpoint_count = 3;
    node.endpoints[0] = &endpoint1;
    node.endpoints[1] = &endpoint2;
    node.endpoints[2] = &endpoint3;

    test_mock_reset();

    EXPECT_TRUE(remove_endpoint_from_node(&node, &endpoint2));
    expect_endpoint_registry_shifted_after_middle_remove(&node, &endpoint1, &endpoint3);
    expect_endpoint_registry_lock_used(&node);
}

static void test_tt_node_create_cleans_up_endpoint_lock_on_invalid_node_id(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    test_mock_reset();
    test_mock_node_id = tt_NODE_ID_INVALID;

    EXPECT_TRUE(tt_Node_create(&node) == -2);
    EXPECT_EQ_U32(1, test_mock_lock_init_count);
    EXPECT_EQ_U32(1, test_mock_lock_deinit_count);
    EXPECT_TRUE(test_mock_last_lock == &node.endpoint_lock);
}

static void test_tt_node_create_client_adds_endpoint(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Service service;
    memset(&service, 0, sizeof(service));
    service.name = "service";

    struct tt_Client client;
    memset(&client, 0, sizeof(client));

    int32_t result = tt_Node_create_client(&node, &client, &service, "client", NULL);
    EXPECT_TRUE(result == tt_ERROR_NONE);
    EXPECT_EQ_U32(1, node.endpoint_count);
    EXPECT_TRUE(node.endpoints[0] == (struct tt_Endpoint*)&client);
}

static void test_tt_node_create_server_adds_endpoint(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Service service;
    memset(&service, 0, sizeof(service));
    service.name = "service";

    struct tt_Server server;
    memset(&server, 0, sizeof(server));

    int32_t result = tt_Node_create_server(&node, &server, &service, "server", NULL);
    EXPECT_TRUE(result == tt_ERROR_NONE);
    EXPECT_EQ_U32(1, node.endpoint_count);
    EXPECT_TRUE(node.endpoints[0] == (struct tt_Endpoint*)&server);
}

static void test_tt_node_create_publisher_adds_endpoint(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Topic topic;
    memset(&topic, 0, sizeof(topic));
    topic.name = "topic";

    struct tt_Publisher pub;
    memset(&pub, 0, sizeof(pub));

    int32_t result = tt_Node_create_publisher(&node, &pub, &topic, "publisher");
    EXPECT_TRUE(result == tt_ERROR_NONE);
    EXPECT_EQ_U32(1, node.endpoint_count);
    EXPECT_TRUE(node.endpoints[0] == (struct tt_Endpoint*)&pub);
}

static void test_tt_node_create_subscriber_adds_endpoint(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Topic topic;
    memset(&topic, 0, sizeof(topic));
    topic.name = "topic";

    struct tt_Subscriber sub;
    memset(&sub, 0, sizeof(sub));

    int32_t result = tt_Node_create_subscriber(&node, &sub, &topic, "subscriber", NULL);
    EXPECT_TRUE(result == tt_ERROR_NONE);
    EXPECT_EQ_U32(1, node.endpoint_count);
    EXPECT_TRUE(node.endpoints[0] == (struct tt_Endpoint*)&sub);
}

static void test_tt_node_create_publisher_allows_multiple_endpoints_same_topic(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Topic topic;
    memset(&topic, 0, sizeof(topic));
    topic.name = "topic";

    struct tt_Publisher pub1;
    memset(&pub1, 0, sizeof(pub1));
    struct tt_Publisher pub2;
    memset(&pub2, 0, sizeof(pub2));

    EXPECT_TRUE(tt_Node_create_publisher(&node, &pub1, &topic, "publisher1") == tt_ERROR_NONE);
    EXPECT_TRUE(tt_Node_create_publisher(&node, &pub2, &topic, "publisher2") == tt_ERROR_NONE);
    EXPECT_EQ_U32(2, node.endpoint_count);
    EXPECT_TRUE(node.endpoints[0] == (struct tt_Endpoint*)&pub1);
    EXPECT_TRUE(node.endpoints[1] == (struct tt_Endpoint*)&pub2);
}

static void test_tt_node_create_publisher_duplicate_name_same_topic_returns_duplicate_endpoint(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Topic topic;
    memset(&topic, 0, sizeof(topic));
    topic.name = "topic";

    struct tt_Publisher pub1;
    memset(&pub1, 0, sizeof(pub1));
    struct tt_Publisher pub2;
    memset(&pub2, 0, sizeof(pub2));

    EXPECT_TRUE(tt_Node_create_publisher(&node, &pub1, &topic, "publisher") == tt_ERROR_NONE);
    EXPECT_TRUE(tt_Node_create_publisher(&node, &pub2, &topic, "publisher") == tt_DUPLICATE_ENDPOINT);
    EXPECT_EQ_U32(1, node.endpoint_count);
    EXPECT_TRUE(node.endpoints[0] == (struct tt_Endpoint*)&pub1);
}

static void test_tt_node_create_client_duplicate_returns_duplicate_endpoint(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Service service;
    memset(&service, 0, sizeof(service));
    service.name = "service";

    struct tt_Client client1;
    memset(&client1, 0, sizeof(client1));
    struct tt_Client client2;
    memset(&client2, 0, sizeof(client2));

    EXPECT_TRUE(tt_Node_create_client(&node, &client1, &service, "client", NULL) == tt_ERROR_NONE);
    EXPECT_TRUE(tt_Node_create_client(&node, &client2, &service, "client", NULL) == tt_DUPLICATE_ENDPOINT);
    EXPECT_EQ_U32(1, node.endpoint_count);
}

static void test_tt_node_create_server_duplicate_returns_duplicate_endpoint(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Service service;
    memset(&service, 0, sizeof(service));
    service.name = "service";

    struct tt_Server server1;
    memset(&server1, 0, sizeof(server1));
    struct tt_Server server2;
    memset(&server2, 0, sizeof(server2));

    EXPECT_TRUE(tt_Node_create_server(&node, &server1, &service, "server", NULL) == tt_ERROR_NONE);
    EXPECT_TRUE(tt_Node_create_server(&node, &server2, &service, "server", NULL) == tt_DUPLICATE_ENDPOINT);
    EXPECT_EQ_U32(1, node.endpoint_count);
}

static int g_sub1_invocations;
static int g_sub2_invocations;

static void test_subscriber_callback1(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no,
                                      struct tt_Data* data) {
    (void)subscriber;
    (void)time;
    (void)seq_no;
    (void)data;
    g_sub1_invocations++;
}

static void test_subscriber_callback2(struct tt_Subscriber* subscriber, uint64_t time, uint16_t seq_no,
                                      struct tt_Data* data) {
    (void)subscriber;
    (void)time;
    (void)seq_no;
    (void)data;
    g_sub2_invocations++;
}

static int32_t test_data_decode(struct tt_Data* data, const uint8_t* payload, const uint32_t len,
                                bool is_native_endian) {
    (void)is_native_endian;
    _tt_memcpy(data, payload, len);
    return (int32_t)len;
}

static void test_data_free(struct tt_Data* data) {
    (void)data;
}

static int g_server1_invocations;
static int g_server2_invocations;

static int8_t test_server_callback1(struct tt_Server* server, struct tt_Request* request,
                                    struct tt_Response* response) {
    (void)server;
    (void)request;
    (void)response;
    g_server1_invocations++;
    return 1;
}

static int8_t test_server_callback2(struct tt_Server* server, struct tt_Request* request,
                                    struct tt_Response* response) {
    (void)server;
    (void)request;
    (void)response;
    g_server2_invocations++;
    return 1;
}

static int32_t test_request_decode(struct tt_Request* request, const uint8_t* payload, const uint32_t len,
                                   bool is_native_endian) {
    (void)request;
    (void)payload;
    (void)len;
    (void)is_native_endian;
    return 0;
}

static void test_request_free(struct tt_Request* request) {
    (void)request;
}

static void test_process_callrequest_finds_correct_service_server_endpoint(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));
    node.tx_size = tt_MAX_BUFFER_LENGTH;
    node.tx_tail = 0;

    struct tt_Service service1;
    memset(&service1, 0, sizeof(service1));
    service1.name = "service1";
    service1.request_decode = test_request_decode;
    service1.request_free = test_request_free;

    struct tt_Service service2;
    memset(&service2, 0, sizeof(service2));
    service2.name = "service2";
    service2.request_decode = test_request_decode;
    service2.request_free = test_request_free;

    struct tt_Server server1;
    memset(&server1, 0, sizeof(server1));
    server1.endpoint.kind = tt_KIND_SERVICE_SERVER;
    server1.endpoint.id = tt_hash_id("service1", "server1");
    server1.node = &node;
    server1.service = &service1;
    server1.callback = test_server_callback1;

    struct tt_Server server2;
    memset(&server2, 0, sizeof(server2));
    server2.endpoint.kind = tt_KIND_SERVICE_SERVER;
    server2.endpoint.id = tt_hash_id("service2", "server2");
    server2.node = &node;
    server2.service = &service2;
    server2.callback = test_server_callback2;

    node.endpoint_count = 2;
    node.endpoints[0] = (struct tt_Endpoint*)&server1;
    node.endpoints[1] = (struct tt_Endpoint*)&server2;

    struct tt_CallRequestHeader request_header;
    memset(&request_header, 0, sizeof(request_header));
    request_header.endpoint_id = server2.endpoint.id;
    request_header.seq_no = 1;
    request_header.retry = 0;

    uint8_t buffer[sizeof(request_header)];
    memcpy(buffer, &request_header, sizeof(request_header));

    struct tt_Header header;
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = 1;

    g_server1_invocations = 0;
    g_server2_invocations = 0;
    test_mock_reset();

    EXPECT_TRUE(process_callrequest(&node, &header, buffer, 0, sizeof(buffer)));
    EXPECT_EQ_U32(0, g_server1_invocations);
    EXPECT_EQ_U32(1, g_server2_invocations);
}

static void test_tt_node_create_subscriber_allows_multiple_endpoints_same_topic(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Topic topic;
    memset(&topic, 0, sizeof(topic));
    topic.name = "topic";

    struct tt_Subscriber sub1;
    memset(&sub1, 0, sizeof(sub1));
    struct tt_Subscriber sub2;
    memset(&sub2, 0, sizeof(sub2));

    EXPECT_TRUE(tt_Node_create_subscriber(&node, &sub1, &topic, "subscriber1", NULL) == tt_ERROR_NONE);
    EXPECT_TRUE(tt_Node_create_subscriber(&node, &sub2, &topic, "subscriber2", NULL) == tt_ERROR_NONE);
    EXPECT_EQ_U32(2, node.endpoint_count);
    EXPECT_TRUE(node.endpoints[0] == (struct tt_Endpoint*)&sub1);
    EXPECT_TRUE(node.endpoints[1] == (struct tt_Endpoint*)&sub2);
}

static void test_tt_node_create_subscriber_duplicate_name_same_topic_returns_duplicate_endpoint(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Topic topic;
    memset(&topic, 0, sizeof(topic));
    topic.name = "topic";

    struct tt_Subscriber sub1;
    memset(&sub1, 0, sizeof(sub1));
    struct tt_Subscriber sub2;
    memset(&sub2, 0, sizeof(sub2));

    EXPECT_TRUE(tt_Node_create_subscriber(&node, &sub1, &topic, "subscriber", NULL) == tt_ERROR_NONE);
    EXPECT_TRUE(tt_Node_create_subscriber(&node, &sub2, &topic, "subscriber", NULL) == tt_DUPLICATE_ENDPOINT);
    EXPECT_EQ_U32(1, node.endpoint_count);
    EXPECT_TRUE(node.endpoints[0] == (struct tt_Endpoint*)&sub1);
}

static void expect_only_first_subscriber_dispatched(void) {
    EXPECT_TRUE(g_sub1_invocations == 1);
    EXPECT_TRUE(g_sub2_invocations == 0);
}

static void test_process_data_dispatches_matching_endpoint_to_subscriber(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Topic topic;
    memset(&topic, 0, sizeof(topic));
    topic.name = "topic";
    topic.data_size = 4;
    topic.data_decode = test_data_decode;
    topic.data_free = test_data_free;

    struct tt_Subscriber sub1;
    memset(&sub1, 0, sizeof(sub1));
    struct tt_Subscriber sub2;
    memset(&sub2, 0, sizeof(sub2));

    sub1.topic = &topic;
    sub1.callback = test_subscriber_callback1;
    sub2.topic = &topic;
    sub2.callback = test_subscriber_callback2;

    int32_t result1 = tt_Node_create_subscriber(&node, &sub1, &topic, "subscriber1", test_subscriber_callback1);
    int32_t result2 = tt_Node_create_subscriber(&node, &sub2, &topic, "subscriber2", test_subscriber_callback2);
    EXPECT_TRUE(result1 == tt_ERROR_NONE);
    EXPECT_TRUE(result2 == tt_ERROR_NONE);

    g_sub1_invocations = 0;
    g_sub2_invocations = 0;
    test_mock_reset();

    uint8_t buffer[sizeof(struct tt_DataHeader) + 4];
    struct tt_DataHeader* data_header = (struct tt_DataHeader*)buffer;
    data_header->endpoint_id = tt_hash_id(topic.name, "subscriber1");
    data_header->seq_no = 1;
    data_header->timestamp = k_test_data_timestamp;
    buffer[sizeof(struct tt_DataHeader) + 0] = 0x01;
    buffer[sizeof(struct tt_DataHeader) + 1] = 0x02;
    buffer[sizeof(struct tt_DataHeader) + 2] = 0x03;
    buffer[sizeof(struct tt_DataHeader) + 3] = 0x04;

    struct tt_Header header;
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = 0;

    bool process_ok = process_data(&node, &header, buffer, 0, sizeof(buffer));
    EXPECT_TRUE(process_ok);
    expect_only_first_subscriber_dispatched();
    expect_endpoint_registry_lock_used(&node);
}

static void test_create_functions_return_too_many_when_endpoint_limit_reached(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));
    node.endpoint_count = tt_MAX_ENDPOINT_COUNT;

    struct tt_Service service;
    memset(&service, 0, sizeof(service));
    service.name = "service";

    struct tt_Topic topic;
    memset(&topic, 0, sizeof(topic));
    topic.name = "topic";

    struct tt_Client client;
    memset(&client, 0, sizeof(client));
    struct tt_Server server;
    memset(&server, 0, sizeof(server));
    struct tt_Publisher pub;
    memset(&pub, 0, sizeof(pub));
    struct tt_Subscriber sub;
    memset(&sub, 0, sizeof(sub));

    EXPECT_TRUE(tt_Node_create_client(&node, &client, &service, "client", NULL) == tt_TOO_MANY_ENDPOINTS);
    EXPECT_TRUE(tt_Node_create_server(&node, &server, &service, "server", NULL) == tt_TOO_MANY_ENDPOINTS);
    EXPECT_TRUE(tt_Node_create_publisher(&node, &pub, &topic, "publisher") == tt_TOO_MANY_ENDPOINTS);
    EXPECT_TRUE(tt_Node_create_subscriber(&node, &sub, &topic, "subscriber", NULL) == tt_TOO_MANY_ENDPOINTS);
}

// Include the implementation to exercise static helper and create functions directly.
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../src/tickle.c"

int main(void) {
    test_add_endpoint_to_node_success();
    test_add_endpoint_to_node_too_many_endpoints();
    test_add_endpoint_to_node_duplicate_returns_duplicate_endpoint();
    test_add_endpoint_to_node_allows_same_topic_subscribers();
    test_add_endpoint_to_node_locks_endpoint_registry();
    test_add_endpoint_to_node_unlocks_on_error();
    test_find_endpoint_locks_endpoint_registry();
    test_remove_endpoint_from_node_locks_and_shifts_endpoint_registry();
    test_tt_node_create_cleans_up_endpoint_lock_on_invalid_node_id();
    test_tt_node_create_client_adds_endpoint();
    test_tt_node_create_client_duplicate_returns_duplicate_endpoint();
    test_tt_node_create_server_adds_endpoint();
    test_tt_node_create_server_duplicate_returns_duplicate_endpoint();
    test_tt_node_create_publisher_adds_endpoint();
    test_tt_node_create_publisher_allows_multiple_endpoints_same_topic();
    test_tt_node_create_publisher_duplicate_name_same_topic_returns_duplicate_endpoint();
    test_tt_node_create_subscriber_adds_endpoint();
    test_tt_node_create_subscriber_allows_multiple_endpoints_same_topic();
    test_tt_node_create_subscriber_duplicate_name_same_topic_returns_duplicate_endpoint();
    test_process_data_dispatches_matching_endpoint_to_subscriber();
    test_process_callrequest_finds_correct_service_server_endpoint();
    test_create_functions_return_too_many_when_endpoint_limit_reached();

    if (test_result() != 0) {
        return 1;
    }

    printf("endpoint tests passed\n");
    return 0;
}
