#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <tickle/config.h>
#include <tickle/tickle.h>

#include "UInt64.h"

// Compound subscriber example:
// - Creates two callback-less subscribers for the same UInt64 topic with
//   different endpoint names.
// - Calls tt_Subscriber_take() after each poll to pull matching DATA submessages.
// - Prints rx_buffer_list state before and after each take so compound packet
//   consumption is visible.

static void print_rx_buffer_state(struct tt_Node* node, const char* phase, uint64_t poll_count) {
    printf("[poll %" PRIu64 "] %s: rx_buffer_count=%" PRIu32 "\n", poll_count, phase, node->rx_buffer_count);

    uint32_t index = 0;
    struct tt_RxBuffer* rx_buffer = node->rx_buffer_list;
    while (rx_buffer != NULL) {
        printf("  rx_buffer[%" PRIu32 "]: len=%" PRIu32 ", remaining_topic_count=%d\n", index, rx_buffer->len,
               rx_buffer->remaining_topic_count);
        index++;
        rx_buffer = rx_buffer->next_buffer;
    }
}

static bool take_and_print(struct tt_Subscriber* sub, const char* label, uint64_t poll_count) {
    struct UInt64Data data;
    int32_t ret = tt_Subscriber_take(sub, &data);
    if (ret >= 0) {
        printf("[poll %" PRIu64 "] take %s: data=%" PRIx64 ", decoded=%d bytes\n", poll_count, label, data.data, ret);
        return true;
    }
    return false;
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

    printf("Compound subscriber node created(#%d)\n", node.id);

    struct tt_Subscriber sub_a;
    ret = tt_Node_create_subscriber(&node, &sub_a, &UInt64Topic, "uint64_compound_a", NULL);
    if (ret != 0) {
        printf("Cannot create subscriber A: %d\n", ret);
        return ret;
    }

    struct tt_Subscriber sub_b;
    ret = tt_Node_create_subscriber(&node, &sub_b, &UInt64Topic, "uint64_compound_b", NULL);
    if (ret != 0) {
        printf("Cannot create subscriber B: %d\n", ret);
        return ret;
    }

    uint64_t poll_count = 0;
    while (tt_Node_poll(&node) == 0) {
        poll_count++;
        if (node.rx_buffer_count > 0) {
            print_rx_buffer_state(&node, "before take", poll_count);
        }
        if (take_and_print(&sub_a, "uint64_compound_a", poll_count)) {
            print_rx_buffer_state(&node, "after take uint64_compound_a", poll_count);
        }
        if (take_and_print(&sub_b, "uint64_compound_b", poll_count)) {
            print_rx_buffer_state(&node, "after take uint64_compound_b", poll_count);
        }
    }

    tt_Node_destroy(&node);

    return 0;
}
