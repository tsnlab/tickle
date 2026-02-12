#include <stdio.h>
#include <unistd.h>

#include "srv/Service.h"

static void service_callback(struct tt_Client* client, int8_t return_code, struct service__srv__ServiceResponse* response) {
    if (return_code == 0 && response == NULL) {
        printf("  Server not found\n");
    } else if (return_code != 0) {
        printf("  Error, return_code: %d\n", return_code);
    } else {
        printf("return_code: %d\n", return_code);
        printf("response:\n");
        printf("  status=%d\nfp=%f\n", response->status, response->fp);
        printf("  array=");
        for (int i = 0; i < sizeof(response->array) / sizeof(response->array[0]); ++i) {
            printf(" %x,", response->array[i]);
        }
        printf("\n");
        printf("  msg=%s\n\n", response->msg);
    }
}

static void call(struct tt_Node* node, uint64_t time, void* param) {
    struct tt_Client* client = param;

    printf("Second call\n");
    struct service__srv__ServiceRequest request;
    int32_t ret = tt_Client_call(client, (struct tt_Request*)&request);
    if (ret < 0) {
        printf("Cannot call: %d\n", ret);
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

    ret = tt_Node_create_client(&node, &client, &service__srv__ServiceService, "service_server",
                                (tt_CLIENT_CALLBACK)service_callback);
    if (ret != 0) {
        printf("Cannot create server: %d\n", ret);
        return ret;
    }

    struct service__srv__ServiceRequest request = {
        .status = 0,
        .fp = 1.0f,
        .array = {0,}
    };
    strncpy(request.msg, "request message", sizeof(request.msg));

    ret = tt_Client_call(&client, (struct tt_Request*)&request);
    if (ret < 0) {
        printf("Cannot call: %d\n", ret);
    }

    tt_Node_schedule(&node, tt_get_ns() + tt_SECOND, call, &client);
    tt_Node_schedule(&node, tt_get_ns() + 2 * tt_SECOND, call, &client);
    tt_Node_schedule(&node, tt_get_ns() + 3 * tt_SECOND, call, &client);

    tt_Node_poll(&node);

    tt_Node_destroy(&node);

    return 0;
}
