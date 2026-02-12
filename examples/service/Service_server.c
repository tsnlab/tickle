#include <stdio.h>
#include <string.h>

#include "srv/Service.h"

static int8_t service_callback(struct tt_Server* server, struct service__srv__ServiceRequest* request,
                                struct service__srv__ServiceResponse* response) {
    if (request->status) {
        response->status = 0;
        strncpy(response->msg, "Status filp 1 -> 0\n", sizeof(response->msg));
        response->fp = request->fp * 2.0f;
        for (int i = 0; i < sizeof(request->array) / sizeof(request->array[0]); ++i) {
            response->array[i] = 10 + request->array[i];
        }
        strncpy(response->msg, "response message", sizeof(response->msg));
        printf("request:\n");
        printf("  status=%d\n", request->status);
        printf("  fp=%f\n", request->fp);
        printf("  array=");
        for (int i = 0; i < sizeof(request->array) / sizeof(request->array[0]); ++i) {
            printf(" %x,", request->array[i]);
        }
        printf("\n");
        printf("  msg=%s\n\n", request->msg);

        printf("response:\n");
        printf("  status=%d\n", response->status);
        printf("  fp=%f\n", response->fp);
        printf("  array=");
        for (int i = 0; i < sizeof(response->array) / sizeof(response->array[0]); ++i) {
            printf(" %x,", response->array[i]);
        }
        printf("\n");
        printf("  msg=%s\n\n", response->msg);
    } else {
        response->status = 1;
        strncpy(response->msg, "Status flip 0 -> 1\n", sizeof(response->msg));
        response->fp = request->fp * 0.5f;
        for (int i = 0; i < sizeof(request->array) / sizeof(request->array[0]); ++i) {
            response->array[i] = 11 + request->array[i];
        }
        strncpy(response->msg, "RESPONSE MESSAGE", sizeof(response->msg));
        printf("request:\n");
        printf("  status=%d\n", request->status);
        printf("  fp=%f\n", request->fp);
        printf("  array=");
        for (int i = 0; i < sizeof(request->array) / sizeof(request->array[0]); ++i) {
            printf(" %x,", request->array[i]);
        }
        printf("\n");
        printf("  msg=%s\n\n", request->msg);

        printf("response:\n");
        printf("  status=%d\n", response->status);
        printf("  fp=%f\n", response->fp);
        printf("  array=");
        for (int i = 0; i < sizeof(response->array) / sizeof(response->array[0]); ++i) {
            printf(" %x,", response->array[i]);
        }
        printf("\n");
        printf("  msg=%s\n\n", response->msg);
    }
    return 0;
}

int main(int argc, char* argv) {
    // _tt_CONFIG.addr = "192.168.10.1";
    _tt_CONFIG.broadcast = "192.168.10.255";

    struct tt_Node node;
    int32_t ret = tt_Node_create(&node);
    if (ret != 0) {
        printf("Cannot create node: %d\n", ret);
        return ret;
    }

    printf("Node created(#%d)\n", node.id);

    struct tt_Server server;

    ret = tt_Node_create_server(&node, &server, &service__srv__ServiceService, "service_server",
                                (tt_SERVER_CALLBACK)service_callback);
    if (ret != 0) {
        printf("Cannot create server: %d\n", ret);
        return ret;
    }

    tt_Node_poll(&node);

    tt_Node_destroy(&node);

    return 0;
}
