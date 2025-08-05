#include <stdio.h>

#include "SetBool.h"

static void set_bool_callback(struct tt_Client* client, int32_t return_code, struct SetBoolResponse* response) {
    printf("  return_code: %d\n", return_code);
    if (response != NULL) {
        printf("  response: %d, %s\n", response->success, response->message);
    }
}

int main(int argc, char* argv) {
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

    tt_Node_poll(&node);

    tt_Node_free(&node);

    return 0;
}
