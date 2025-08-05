#include <stdio.h>
#include <string.h>

#include "UInt64.h"

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

    struct tt_Publisher pub;

    ret = tt_Node_create_publisher(&node, &pub, &UInt64Topic, "uint64_topic");
    if (ret != 0) {
        printf("Cannot create server: %d\n", ret);
        return ret;
    }

    struct UInt64Data data = {.data = 0xdeadbeef};
    ret = tt_Publisher_pub(&pub, (struct tt_Data*)&data);
    if (ret < 0) {
        printf("Cannot publish: %d\n", ret);
    }

    tt_Node_poll(&node);

    tt_Node_free(&node);

    return 0;
}
