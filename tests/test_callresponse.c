#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

// This file owns the shared test assertion state for this test binary.
#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"

// This file owns the HAL/time mocks used by the included tickle.c implementation.
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

static int callback_count = 0;
static int8_t callback_return_code = 0;
static struct tt_Response* callback_response = (struct tt_Response*)1;

static void test_client_callback(struct tt_Client* client, int8_t return_code, struct tt_Response* response) {
    (void)client;
    callback_count++;
    callback_return_code = return_code;
    callback_response = response;
}

// Include the implementation to exercise static call-response code without widening production visibility.
#include "../src/tickle.c"

// Regression test for latency measured from request cache time to response receive time.
static void test_callresponse_updates_latency_from_cache_time_to_now(void) {
    struct tt_Node node;
    memset(&node, 0, sizeof(node));

    struct tt_Service service;
    memset(&service, 0, sizeof(service));

    struct tt_Client client;
    memset(&client, 0, sizeof(client));
    client.super.kind = tt_KIND_SERVICE_CLIENT;
    client.super.id = 0x12345678;
    client.service = &service;
    client.callback = test_client_callback;
    client.cache = malloc(1);
    client.cache_time = 1000;
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
    response_header.id = client.super.id;
    response_header.seq_no = 7;
    response_header.return_code = 1;

    test_mock_reset();
    test_mock_now = 1250;
    callback_count = 0;
    callback_return_code = 0;
    callback_response = (struct tt_Response*)1;

    EXPECT_TRUE(process_callresponse(&node, &header, (uint8_t*)&response_header, 0, sizeof(response_header)));
    EXPECT_EQ_U32(250, client.latency);
    EXPECT_TRUE(client.cache == NULL);
    EXPECT_EQ_U32(1, callback_count);
    EXPECT_EQ_U32(1, callback_return_code);
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
