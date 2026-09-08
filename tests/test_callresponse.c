#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tickle/tickle.h>

// This file owns the shared test-assertion and HAL-mock storage for this test binary.
#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"
#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

// Whitebox test: include the implementation directly so static functions like
// process_callresponse() are reachable, without widening their visibility in production.
#include "../src/tickle.c" // NOLINT(bugprone-suspicious-include) -- whitebox: reaches tickle.c's static functions

static int callback_count = 0;
static int callback_return_code = 0; // widened from the wire's int8_t to avoid a signed-char/int mix in EXPECT_EQ_INT
static struct tt_Response* callback_response = (struct tt_Response*)1; // sentinel != NULL

static void test_client_callback(struct tt_Client* client, int8_t return_code, struct tt_Response* response) {
    (void)client;
    callback_count++;
    callback_return_code = (int)return_code; // sign-extend explicitly; the wire value can be negative
    callback_response = response;
}

// Regression test for the latency EWMA and cache-clearing on a successful (well, "answered")
// call response: cache_time -> now must measure forward, not backward (see calculate_latency()),
// and clearing client->cache must not try to free it (it's a fixed buffer, see cache_buf).
static void test_callresponse_updates_latency_and_clears_cache(void) {
    test_mock_reset();
    test_mock_now = 1250;

    struct tt_Node node;
    memset(&node, 0, sizeof(node));
    node.id = 1;

    struct tt_Service service;
    memset(&service, 0, sizeof(service));
    service.response_size = 1; // unused here (return_code != 0 skips decode) but must be >0 for the VLA

    struct tt_Client client;
    memset(&client, 0, sizeof(client));
    client.endpoint.kind = tt_KIND_SERVICE_CLIENT;
    client.endpoint.id = 0x12345678;
    client.node = &node;
    client.service = &service;
    client.callback = test_client_callback;
    client.cache = (struct tt_SubmessageHeader*)client.cache_buf; // a call is outstanding
    client.cache_time = 1000;
    client.latency = 0;

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
    response_header.seq_no = 7;
    response_header.retry = 0;
    response_header.return_code = 1; // non-zero: process_callresponse skips response_decode

    callback_count = 0;
    callback_return_code = 0;
    callback_response = (struct tt_Response*)1;

    EXPECT_TRUE(process_callresponse(&node, &header, (uint8_t*)&response_header, 0, sizeof(response_header)));
    EXPECT_EQ_U32(250, client.latency); // now(1250) - cache_time(1000)
    EXPECT_TRUE(client.cache == NULL);
    EXPECT_EQ_U32(1, callback_count);
    EXPECT_EQ_INT(1, callback_return_code);
    EXPECT_TRUE(callback_response == NULL);
}

// A response for an endpoint_id nobody registered (e.g. meant for a different node sharing the
// broadcast domain) must be ignored, not treated as an error.
static void test_callresponse_ignores_unknown_endpoint(void) {
    test_mock_reset();

    struct tt_Node node;
    memset(&node, 0, sizeof(node));
    node.id = 1;
    node.endpoint_count = 0; // nothing registered

    struct tt_Header header;
    memset(&header, 0, sizeof(header));
    header.magic_value = NATIVE_MAGIC_VALUE;
    header.version = tt_VERSION;
    header.source = 2;

    struct tt_CallResponseHeader response_header;
    memset(&response_header, 0, sizeof(response_header));
    response_header.endpoint_id = 0xdeadbeef;

    EXPECT_TRUE(process_callresponse(&node, &header, (uint8_t*)&response_header, 0, sizeof(response_header)));
}

int main(void) {
    test_callresponse_updates_latency_and_clears_cache();
    test_callresponse_ignores_unknown_endpoint();

    if (test_result() != 0) {
        return 1;
    }

    printf("test_callresponse: all tests passed\n");
    return 0;
}
