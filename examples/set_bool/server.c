#include <stdio.h>

#include <tickle/tickle.h>

#include "SetBool.h"

// This file relies on transitive includes provided by SetBool.h and tickle headers.
// NOLINTBEGIN(misc-include-cleaner)

static int8_t set_bool_callback(struct tt_Server* server, struct SetBoolRequest* request,
                                struct SetBoolResponse* response) {
    (void)server; // NOLINT(misc-unused-parameters)
    printf("  data: %d\n", request->data);

    if (request->data) {
        response->success = true;
        response->message = "Succeed";
    } else {
        response->success = false;
        response->message = "Failed";
    }

    return 0;
}

int main(int argc, char** argv) {
    (void)argc; // NOLINT(misc-unused-parameters)
    (void)argv; // NOLINT(misc-unused-parameters)
    // _tt_CONFIG.addr = "192.168.10.1";
    _tt_CONFIG.broadcast = "192.168.10.255";

    struct tt_Node node;
    tt_ret_t ret = tt_Node_create(&node);
    if (ret != 0) {
        printf("Cannot create node: %d\n", ret);
        return ret;
    }

    printf("Node created(#%d)\n", node.id);

    struct tt_Server server;

    ret = tt_Node_create_server(&node, &server, &SetBoolService, "set_bool_server",
                                (tt_SERVER_CALLBACK)set_bool_callback);
    if (ret != 0) {
        printf("Cannot create server: %d\n", ret);
        return ret;
    }

    ret = tt_RET_OK;
    while (ret == tt_RET_OK || ret == tt_RET_TIMEOUT) {
        ret = tt_Node_poll(&node, -1);
    }

    tt_Node_destroy(&node);
    printf("Node destroyed(#%d): %d\n", node.id, ret);

    return 0;
}

// NOLINTEND(misc-include-cleaner)
