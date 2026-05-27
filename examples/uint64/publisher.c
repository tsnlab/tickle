#include <stdio.h>

#include "UInt64.h"

// This file relies on transitive includes provided by UInt64.h and tickle headers.
// NOLINTBEGIN(misc-include-cleaner)

int main(int argc, char** argv) {
    (void)argc; // NOLINT(misc-unused-parameters)
    (void)argv; // NOLINT(misc-unused-parameters)
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

    const uint64_t example_data = 0xdeadbeef;
    struct UInt64Data data = {.data = example_data};
    ret = tt_Publisher_publish(&pub, (struct tt_Data*)&data);
    if (ret < 0) {
        printf("Cannot publish: %d\n", ret);
    }

    tt_Node_poll(&node);

    tt_Node_destroy(&node);

    return 0;
}

// NOLINTEND(misc-include-cleaner)
