#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/tickle.h>

#include "msg/Rtt.h"

#define PING_ENDPOINT_NAME "rtt_ping"
#define PONG_ENDPOINT_NAME "rtt_pong"

#define MAX_PINGS_PER_SECOND 1000
#define PENDING_CAPACITY MAX_PINGS_PER_SECOND
#define PING_EXPIRATION tt_SECOND
#define DEFAULT_INTERVAL_MS 500
#define DEFAULT_COUNT 10

typedef struct pending_ping {
    uint64_t sequence;
    uint64_t sent_at;
    bool valid;
} pending_ping_t;

typedef struct ping_context {
    struct tt_Publisher *publisher;
    pending_ping_t pending[PENDING_CAPACITY];
    uint64_t ping_count;
    uint64_t sent_count;
    uint64_t pending_count;
    uint64_t next_expiry;
    uint64_t loss_count;
    uint64_t rtt_sum_ns;
    uint64_t samples_per_report;
    uint64_t interval_ns;
    uint64_t next_ping_due;
    uint64_t next_report_due;
    FILE *csv;
} ping_context_t;

static ping_context_t *active_context;

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s [-i interval_ms] [-c count]\n", program);
}

static bool parse_uint64(const char *text, uint64_t *value)
{
    char *end;
    unsigned long long parsed;

    if (*text == 0 || *text == '-')
        return false;

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || *end != 0)
        return false;

    *value = (uint64_t) parsed;
    return true;
}

static void print_report(ping_context_t *context)
{
    const uint64_t rtt_sum_ns = context->rtt_sum_ns;
    const uint64_t received_count = context->samples_per_report;

    context->rtt_sum_ns = 0;
    context->samples_per_report = 0;

    printf("avg_rtt_ms=%.3f received=%" PRIu64 " loss=%" PRIu64 "\n",
           received_count == 0 ? 0.0 :
           (double) rtt_sum_ns / received_count / tt_MILLISECOND,
           received_count, context->loss_count);
    fflush(stdout);
}

static void finish(struct tt_Node *node, ping_context_t *context, int status)
{
    if (context->csv != NULL) {
        if (fclose(context->csv) != 0)
            fprintf(stderr, "failed to close RTT CSV: %s\n", strerror(errno));
        context->csv = NULL;
    }
    tt_Node_destroy(node);
    exit(status);
}

static void send_ping(struct tt_Node *node, uint64_t now, void *param);

static void schedule_tick(struct tt_Node *node, uint64_t time, ping_context_t *context)
{
    if (!tt_Node_schedule(node, time, send_ping, context)) {
        fprintf(stderr, "TickLE scheduler is full\n");
        finish(node, context, EXIT_FAILURE);
    }
}

static void expire_pings(ping_context_t *context, uint64_t now)
{
    while (context->next_expiry < context->sent_count) {
        pending_ping_t *entry =
            &context->pending[context->next_expiry % PENDING_CAPACITY];

        if (!entry->valid || entry->sequence != context->next_expiry) {
            context->next_expiry++;
            continue;
        }
        if (now - entry->sent_at < PING_EXPIRATION)
            break;

        entry->valid = false;
        context->pending_count--;
        context->loss_count++;
        context->next_expiry++;
    }
}

static uint64_t next_wakeup(const ping_context_t *context, uint64_t now)
{
    uint64_t wakeup = now + (100 * tt_MILLISECOND);

    if (context->sent_count < context->ping_count &&
            context->next_ping_due < wakeup)
        wakeup = context->next_ping_due;
    if (context->next_report_due < wakeup)
        wakeup = context->next_report_due;
    if (context->pending_count != 0) {
        const pending_ping_t *entry =
            &context->pending[context->next_expiry % PENDING_CAPACITY];

        if (entry->valid && entry->sequence == context->next_expiry &&
                entry->sent_at + PING_EXPIRATION < wakeup)
            wakeup = entry->sent_at + PING_EXPIRATION;
    }
    return wakeup > now ? wakeup : now + tt_MICROSECOND;
}

static void send_ping(struct tt_Node *node, uint64_t now, void *param)
{
    ping_context_t *context = param;
    struct rtt__msg__RttData data;
    pending_ping_t *slot;
    int32_t status;

    expire_pings(context, now);

    if (now >= context->next_report_due) {
        print_report(context);
        context->next_report_due = now + tt_SECOND;
    }

    if (context->sent_count == context->ping_count &&
            context->pending_count == 0) {
        print_report(context);
        finish(node, context, EXIT_SUCCESS);
    }

    if (context->sent_count < context->ping_count &&
            now >= context->next_ping_due) {
        slot = &context->pending[context->sent_count % PENDING_CAPACITY];
        if (slot->valid) {
            context->next_ping_due = now + tt_MILLISECOND;
        } else {
            slot->sequence = context->sent_count;
            slot->sent_at = tt_get_ns();
            slot->valid = true;
            context->pending_count++;
            data.payload = context->sent_count;

            status = tt_Publisher_publish(context->publisher, (struct tt_Data *) &data);
            if (status != 0) {
                fprintf(stderr, "failed to publish ping: %d\n", status);
                finish(node, context, EXIT_FAILURE);
            }
            tt_Node_flush(node);

            context->sent_count++;
            context->next_ping_due = slot->sent_at + context->interval_ns;
        }
    }

    schedule_tick(node, next_wakeup(context, tt_get_ns()), context);
}

static void on_data_available(struct tt_Subscriber *subscriber, uint64_t timestamp,
                              uint16_t seq_no, struct rtt__msg__RttData *data)
{
    ping_context_t *context = active_context;
    pending_ping_t *entry;
    const uint64_t sequence = data->payload;
    const uint64_t received_at = tt_get_ns();
    uint64_t rtt_ns;

    (void) subscriber;
    (void) timestamp;
    (void) seq_no;

    if (context == NULL)
        return;

    entry = &context->pending[sequence % PENDING_CAPACITY];
    if (!entry->valid || entry->sequence != sequence)
        return;

    rtt_ns = received_at - entry->sent_at;
    entry->valid = false;
    context->pending_count--;
    context->rtt_sum_ns += rtt_ns;
    context->samples_per_report++;

    if (fprintf(context->csv, "%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                sequence, entry->sent_at, rtt_ns) < 0)
        fprintf(stderr, "failed to write RTT CSV row\n");
}

int main(int argc, char *argv[])
{
    uint64_t interval_ms = DEFAULT_INTERVAL_MS;
    uint64_t ping_count = DEFAULT_COUNT;
    struct tt_Node node;
    struct tt_Publisher ping_publisher;
    struct tt_Subscriber pong_subscriber;
    ping_context_t context = {0};
    char csv_filename[128];
    int32_t status;

    _tt_CONFIG.broadcast = "192.168.10.255";
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        if (strcmp(argv[i], "-i") == 0) {
            if (!parse_uint64(argv[i + 1], &interval_ms)) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[i], "-c") == 0) {
            if (!parse_uint64(argv[i + 1], &ping_count)) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (interval_ms == 0 || ping_count == 0 ||
            interval_ms > UINT64_MAX / tt_MILLISECOND) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (snprintf(csv_filename, sizeof(csv_filename),
                 "rtt_tickle_i%" PRIu64 "_s8_c%" PRIu64 ".csv",
                 interval_ms, ping_count) >= (int) sizeof(csv_filename)) {
        fprintf(stderr, "RTT CSV filename is too long\n");
        return EXIT_FAILURE;
    }
    context.csv = fopen(csv_filename, "w");
    if (context.csv == NULL) {
        fprintf(stderr, "fopen(%s): %s\n", csv_filename, strerror(errno));
        return EXIT_FAILURE;
    }
    if (fprintf(context.csv, "count,timestamp_ns,rtt_ns\n") < 0) {
        fprintf(stderr, "failed to write RTT CSV header\n");
        fclose(context.csv);
        return EXIT_FAILURE;
    }

    status = tt_Node_create(&node);
    if (status != 0) {
        fprintf(stderr, "cannot create TickLE node: %d\n", status);
        fclose(context.csv);
        return EXIT_FAILURE;
    }

    status = tt_Node_create_publisher(
        &node, &ping_publisher, &rtt__msg__RttTopic, PING_ENDPOINT_NAME);
    if (status != 0) {
        fprintf(stderr, "cannot create ping publisher: %d\n", status);
        tt_Node_destroy(&node);
        fclose(context.csv);
        return EXIT_FAILURE;
    }

    active_context = &context;
    status = tt_Node_create_subscriber(
        &node, &pong_subscriber, &rtt__msg__RttTopic, PONG_ENDPOINT_NAME,
        (tt_SUBSCRIBER_CALLBACK) on_data_available);
    if (status != 0) {
        fprintf(stderr, "cannot create pong subscriber: %d\n", status);
        tt_Node_destroy(&node);
        fclose(context.csv);
        return EXIT_FAILURE;
    }

    context.publisher = &ping_publisher;
    context.ping_count = ping_count;
    context.interval_ns = interval_ms * tt_MILLISECOND;
    context.next_ping_due = tt_get_ns() + tt_SECOND;
    context.next_report_due = context.next_ping_due + tt_SECOND;

    printf("Sending %" PRIu64 " pings every %" PRIu64 " ms.\n",
           ping_count, interval_ms);
    fflush(stdout);

    schedule_tick(&node, context.next_ping_due, &context);
    tt_Node_poll(&node);
    return EXIT_FAILURE;
}
