#include <stdio.h>

#include "UInt64.h"

// This file relies on transitive includes provided by UInt64.h and tickle headers.
// NOLINTBEGIN(misc-include-cleaner)

static const uint64_t k_initial_publish_data = 0xdeadbeef;

struct publish_context {
    struct tt_Publisher* pub;
    uint64_t data;
};

static void publish_periodic(struct tt_Node* node, uint64_t time, void* param) {
    struct publish_context* context = param;
    struct UInt64Data data = {.data = context->data};

    int32_t ret = tt_Publisher_publish(context->pub, (struct tt_Data*)&data);
    if (ret < 0) {
        printf("Cannot publish: %d\n", ret);
    } else {
        printf("Published: %lx\n", data.data);
    }

    context->data++;
    if (!tt_Node_schedule(node, time + tt_SECOND, publish_periodic, context)) {
        printf("Cannot schedule next publish\n");
    }
}

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

    struct publish_context publish_context = {
        .pub = &pub,
        .data = k_initial_publish_data,
    };
    if (!tt_Node_schedule(&node, tt_get_ns(), publish_periodic, &publish_context)) {
        printf("Cannot schedule publish\n");
        return -1;
    }

    while (tt_Node_poll(&node) == 0) {
        // Application owns the poll loop; scheduled TickLE work runs here.
    }

    tt_Node_destroy(&node);

    return 0;
}

// NOLINTEND(misc-include-cleaner)
