#include <inttypes.h>
#include <stdio.h>

#include "UInt64.h"

// Compound publisher example:
// - Creates two publishers for the same UInt64 topic with different endpoint names.
// - Publishes both values during one scheduled callback so one outgoing packet
//   carries multiple DATA submessages.
// - Use with subscriber_compound to observe per-subscriber take behavior.
//
// This file relies on transitive includes provided by UInt64.h and tickle headers.
// NOLINTBEGIN(misc-include-cleaner)

static const uint64_t k_initial_pub_a_data = 0xaaa00000;
static const uint64_t k_initial_pub_b_data = 0xbbb00000;

struct compound_publish_context {
    struct tt_Publisher* pub_a;
    struct tt_Publisher* pub_b;
    uint64_t data_a;
    uint64_t data_b;
};

static void publish_compound_periodic(struct tt_Node* node, uint64_t time, void* param) {
    struct compound_publish_context* context = param;
    struct UInt64Data data_a = {.data = context->data_a};
    struct UInt64Data data_b = {.data = context->data_b};

    int32_t ret = tt_Publisher_publish(context->pub_a, (struct tt_Data*)&data_a);
    if (ret < 0) {
        printf("Cannot publish pub_a: %d\n", ret);
    }

    ret = tt_Publisher_publish(context->pub_b, (struct tt_Data*)&data_b);
    if (ret < 0) {
        printf("Cannot publish pub_b: %d\n", ret);
    }

    printf("Queued one compound packet: pub_a=%" PRIx64 ", pub_b=%" PRIx64 "\n", data_a.data, data_b.data);

    context->data_a++;
    context->data_b++;
    if (!tt_Node_schedule(node, time + tt_SECOND, publish_compound_periodic, context)) {
        printf("Cannot schedule next compound publish\n");
    }
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

    printf("Compound publisher node created(#%d)\n", node.id);

    struct tt_Publisher pub_a;
    ret = tt_Node_create_publisher(&node, &pub_a, &UInt64Topic, "uint64_compound_a");
    if (ret != 0) {
        printf("Cannot create publisher A: %d\n", ret);
        return ret;
    }

    struct tt_Publisher pub_b;
    ret = tt_Node_create_publisher(&node, &pub_b, &UInt64Topic, "uint64_compound_b");
    if (ret != 0) {
        printf("Cannot create publisher B: %d\n", ret);
        return ret;
    }

    struct compound_publish_context publish_context = {
        .pub_a = &pub_a,
        .pub_b = &pub_b,
        .data_a = k_initial_pub_a_data,
        .data_b = k_initial_pub_b_data,
    };
    if (!tt_Node_schedule(&node, tt_get_ns(), publish_compound_periodic, &publish_context)) {
        printf("Cannot schedule compound publish\n");
        return -1;
    }

    while (tt_Node_poll(&node) == 0) {
        // Application owns the poll loop; scheduled publish queues two DATA submessages.
    }

    tt_Node_destroy(&node);

    return 0;
}

// NOLINTEND(misc-include-cleaner)
