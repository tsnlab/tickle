#include <stdio.h>

#include <tickle/tickle.h>

#include "SetBool.h"

// This file relies on transitive includes provided by SetBool.h and tickle headers.
// NOLINTBEGIN(misc-include-cleaner)

static void set_bool_callback(struct tt_Client* client, int8_t return_code, struct SetBoolResponse* response) {
    (void)client; // NOLINT(misc-unused-parameters)
    if (return_code == 0 && response == NULL) {
        printf("  Server not found\n");
    } else if (return_code != 0) {
        printf("  Error, return_code: %d\n", return_code);
    } else {
        printf("  return_code: %d\n", return_code);
        printf("  response: %d, %s\n", response->success, response->message);
    }
}

static void call(struct tt_Node* node, uint64_t time, void* param) {
    (void)node; // NOLINT(misc-unused-parameters)
    (void)time; // NOLINT(misc-unused-parameters)
    struct tt_Client* client = param;

    printf("Second call\n");
    struct SetBoolRequest request = {.data = false};
    int32_t ret = tt_Client_call(client, (struct tt_Request*)&request);
    if (ret < 0) {
        printf("Cannot call: %d\n", ret);
    }
}

int main(int argc, char** argv) {
    (void)argc; // NOLINT(misc-unused-parameters)
    (void)argv; // NOLINT(misc-unused-parameters)
    _tt_CONFIG.broadcast = "192.168.10.255";

    struct tt_Node node;
    int32_t ret = tt_Node_create(&node);
    if (ret != 0) {
        printf("Cannot create node: %d\n", ret);
        return ret;
    }

    printf("Node created(#%d)\n", node.id);

    struct tt_Client client;

    ret = tt_Node_create_client(&node, &client, &SetBoolService, "set_bool_server",
                                (tt_CLIENT_CALLBACK)set_bool_callback);
    if (ret != 0) {
        printf("Cannot create server: %d\n", ret);
        return ret;
    }

    struct SetBoolRequest request = {.data = true};
    ret = tt_Client_call(&client, (struct tt_Request*)&request);
    if (ret < 0) {
        printf("Cannot call: %d\n", ret);
    }

    tt_Node_schedule(&node, tt_get_ns() + tt_SECOND, call, &client);
    tt_Node_schedule(&node, tt_get_ns() + (2 * tt_SECOND), call, &client);
    tt_Node_schedule(&node, tt_get_ns() + (3 * tt_SECOND), call, &client);

    while (tt_Node_poll(&node) == 0) {
        // Application owns the poll loop; service responses are delivered by callback.
    }

    tt_Node_destroy(&node);

    return 0;
}

// NOLINTEND(misc-include-cleaner)
