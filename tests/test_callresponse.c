#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/hal.h>
#include <tickle/tickle.h>

// This file owns the shared test assertion state for this test binary.
#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"

// This file owns the HAL/time mocks used by the included tickle.c implementation.
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

static int callback_count = 0;
static int8_t callback_return_code = 0;
static const uint32_t test_client_id = 0x12345678; // NOLINT(readability-magic-numbers)
static const uint32_t test_cache_time = 1000;      // NOLINT(readability-magic-numbers)
static const uint32_t test_response_seq_no = 7;    // NOLINT(readability-magic-numbers)
static const uint32_t test_return_code = 1;
static const uint32_t test_receive_time = 1250; // NOLINT(readability-magic-numbers)

static struct tt_Response* callback_response = (struct tt_Response*)1;

static void test_client_callback(struct tt_Client* client, int8_t return_code, struct tt_Response* response) {
    (void)client;
    callback_count++;
    callback_return_code = return_code;
    callback_response = response;
}

// Include the implementation to exercise static call-response code without widening production visibility.
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../src/tickle.c"

// Regression test for latency measured from request cache time to response receive time.
static void test_callresponse_updates_latency_from_cache_time_to_now(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Service service;
    memset(&service, 0, sizeof(service));

    struct tt_Client client;
    memset(&client, 0, sizeof(client));
    client.endpoint.kind = tt_KIND_SERVICE_CLIENT;
    client.endpoint.id = test_client_id;
    client.service = &service;
    client.callback = test_client_callback;
    client.cache = malloc(1);
    client.cache_time = test_cache_time;
    client.latency = 0;

    node.id = 1;
    node.endpoint_count = 1;
    node.endpoints[0] = (struct tt_Endpoint*)&client;

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = 2;

    struct tt_CallResponseHeader response_header;
    memset(&response_header, 0, sizeof(response_header));
    response_header.endpoint_id = client.endpoint.id;
    response_header.seq_no = test_response_seq_no;
    response_header.return_code = (int8_t)test_return_code;

    test_mock_reset();
    test_mock_now = test_receive_time;
    callback_count = 0;
    callback_return_code = 0;
    callback_response = (struct tt_Response*)1;

    EXPECT_TRUE(process_callresponse(&node, &header, (uint8_t*)&response_header, 0, sizeof(response_header)));
    EXPECT_EQ_U32(test_receive_time - test_cache_time, client.latency);
    EXPECT_TRUE(client.cache == NULL);
    EXPECT_EQ_U32(1, callback_count);
    EXPECT_EQ_U32(test_return_code, (uint8_t)callback_return_code);
    EXPECT_TRUE(callback_response == NULL);
}

int main(void) {
    test_callresponse_updates_latency_from_cache_time_to_now();

    if (test_result() != 0) {
        return 1;
    }

    printf("callresponse tests passed\n");
    return 0;
}
