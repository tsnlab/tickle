#include <stdint.h>
#include <stdio.h>

#include <tickle/config.h>
#include <tickle/tickle.h>

#include "UInt64.h"

static void uint64_data_callback(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no,
                                 struct UInt64Data* data) {
    (void)sub; // NOLINT(misc-unused-parameters)
    printf("  timestamp: %ld\n", timestamp);
    printf("  seq_no: %d\n", seq_no);
    printf("  data->data: %lx\n", data->data);
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

    struct tt_Subscriber sub;

    ret = tt_Node_create_subscriber(&node, &sub, &UInt64Topic, "uint64_topic",
                                    (tt_SUBSCRIBER_CALLBACK)uint64_data_callback);
    if (ret != 0) {
        printf("Cannot create subscriber: %d\n", ret);
        return ret;
    }

    tt_Node_poll(&node);

    tt_Node_destroy(&node);

    return 0;
}
