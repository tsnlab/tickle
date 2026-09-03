#include <stdint.h>
#include <stdio.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "PingPong.h"

static int8_t pong_callback(struct tt_Server* server, struct PingPongRequest* request,
                            struct PingPongResponse* response) {
    (void)server;
    response->seq = request->seq;
    response->timestamp = request->timestamp;
    return 0;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    _tt_CONFIG.broadcast = "192.168.10.255";

    struct tt_Node node;
    tt_ret_t ret = tt_Node_create(&node);
    if (ret != 0) {
        printf("Cannot create node: %d\n", ret);
        return ret;
    }

    printf("Node created(#%d)\n", node.id);

    struct tt_Server server;
    ret =
        tt_Node_create_server(&node, &server, &PingPongService, "ping_pong_server", (tt_SERVER_CALLBACK)pong_callback);
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
