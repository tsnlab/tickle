#include <stdio.h>
#include <stdlib.h>

#include <tickle/tickle.h>

#include "msg/Rtt.h"

#define PING_ENDPOINT_NAME "rtt_ping"
#define PONG_ENDPOINT_NAME "rtt_pong"

static struct tt_Publisher *pong_publisher;

static void on_data_available(struct tt_Subscriber *subscriber, uint64_t timestamp,
                              uint16_t seq_no, struct rtt__msg__RttData *data)
{
    int32_t status;

    (void) subscriber;
    (void) timestamp;
    (void) seq_no;

    status = tt_Publisher_publish(pong_publisher, (struct tt_Data *) data);
    if (status != 0)
        fprintf(stderr, "failed to publish pong: %d\n", status);
    tt_Node_flush(subscriber->node);
}

int main(void)
{
    struct tt_Node node;
    struct tt_Publisher publisher;
    struct tt_Subscriber subscriber;
    int32_t status;

    _tt_CONFIG.broadcast = "192.168.10.255";
    status = tt_Node_create(&node);
    if (status != 0) {
        fprintf(stderr, "cannot create TickLE node: %d\n", status);
        return EXIT_FAILURE;
    }

    status = tt_Node_create_publisher(
        &node, &publisher, &rtt__msg__RttTopic, PONG_ENDPOINT_NAME);
    if (status != 0) {
        fprintf(stderr, "cannot create pong publisher: %d\n", status);
        tt_Node_destroy(&node);
        return EXIT_FAILURE;
    }

    pong_publisher = &publisher;
    status = tt_Node_create_subscriber(
        &node, &subscriber, &rtt__msg__RttTopic, PING_ENDPOINT_NAME,
        (tt_SUBSCRIBER_CALLBACK) on_data_available);
    if (status != 0) {
        fprintf(stderr, "cannot create ping subscriber: %d\n", status);
        tt_Node_destroy(&node);
        return EXIT_FAILURE;
    }

    printf("Waiting for pings; press Ctrl-C to stop.\n");
    fflush(stdout);
    tt_Node_poll(&node);

    tt_Node_destroy(&node);
    return EXIT_SUCCESS;
}
