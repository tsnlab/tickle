#include <stdio.h>

#include "Example.h"

static void example_data_callback(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no,
                                 struct ExampleData* data) {
    printf("  timestamp: %ld\n", timestamp);
    printf("  seq_no: %d\n", seq_no);
    printf("  data->data1: %lx\n", data->data1);
    printf("  data->data2: %lx\n", data->data2);
    printf("  data->data3: %lx\n", data->data3);
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

    struct tt_Subscriber sub;

    ret = tt_Node_create_subscriber(&node, &sub, &ExampleTopic, "example_topic",
                                    (tt_SUBSCRIBER_CALLBACK)example_data_callback);
    if (ret != 0) {
        printf("Cannot create subscriber: %d\n", ret);
        return ret;
    }

    tt_Node_poll(&node);

    tt_Node_destroy(&node);

    return 0;
}