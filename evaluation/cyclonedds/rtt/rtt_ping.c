#include "dds/dds.h"
#include "rtt.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PING_TOPIC_NAME "PingTopic"
#define PONG_TOPIC_NAME "PongTopic"
#define RTT_TYPE rttModule_DataType
#define RTT_TYPE_DESCRIPTOR rttModule_DataType_desc

#define MAX_SAMPLES 16
#define MAX_PINGS_PER_SECOND 1000
#define PENDING_CAPACITY MAX_PINGS_PER_SECOND
#define PING_EXPIRATION DDS_SECS(1)
#define DEFAULT_INTERVAL_MS 500
#define DEFAULT_COUNT 10

typedef struct pending_ping {
    uint64_t sequence;
    dds_time_t sent_at;
    bool valid;
} pending_ping_t;

typedef struct ping_context {
    pending_ping_t *pending;
    uint64_t pending_capacity;
    uint64_t pending_count;
    uint64_t next_expiry;
    uint64_t pong_count;
    uint64_t loss_count;
    uint64_t rtt_sum_ns;
    uint64_t samples_per_report;
    FILE *csv;
    dds_return_t error;
    pthread_mutex_t mutex;
} ping_context_t;

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

static dds_entity_t create_topic(dds_entity_t participant, const char *name)
{
    dds_qos_t *qos = dds_create_qos();
    dds_entity_t topic;

    dds_qset_reliability(qos, DDS_RELIABILITY_BEST_EFFORT, DDS_SECS(10));
    dds_qset_durability(qos, DDS_DURABILITY_VOLATILE);
    dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 1);
    topic = dds_create_topic(participant, &RTT_TYPE_DESCRIPTOR, name, qos, NULL);
    dds_delete_qos(qos);
    if (topic < 0)
        DDS_FATAL("dds_create_topic: %s\n", dds_strretcode(-topic));
    return topic;
}

static dds_entity_t create_writer(dds_entity_t participant, dds_entity_t topic)
{
    dds_entity_t publisher = dds_create_publisher(participant, NULL, NULL);
    dds_entity_t writer;

    if (publisher < 0)
        DDS_FATAL("dds_create_publisher: %s\n", dds_strretcode(-publisher));

    writer = dds_create_writer(publisher, topic, NULL, NULL);
    if (writer < 0)
        DDS_FATAL("dds_create_writer: %s\n", dds_strretcode(-writer));
    return writer;
}

static void on_data_available(dds_entity_t reader, void *arg)
{
    ping_context_t *context = arg;
    RTT_TYPE received[MAX_SAMPLES] = {0};
    void *samples[MAX_SAMPLES];
    dds_sample_info_t sample_info[MAX_SAMPLES];
    const dds_time_t received_at = dds_time();

    for (size_t i = 0; i < MAX_SAMPLES; i++)
        samples[i] = &received[i];

    const int sample_count = dds_take(
        reader, samples, sample_info, MAX_SAMPLES, MAX_SAMPLES);
    if (sample_count < 0) {
        pthread_mutex_lock(&context->mutex);
        context->error = sample_count;
        pthread_mutex_unlock(&context->mutex);
        return;
    }

    pthread_mutex_lock(&context->mutex);
    for (int i = 0; i < sample_count; i++) {
        const uint64_t sequence = received[i].payload;
        pending_ping_t *entry;
        dds_time_t rtt_ns;

        if (!sample_info[i].valid_data)
            continue;

        entry = &context->pending[sequence % context->pending_capacity];
        if (!entry->valid || entry->sequence != sequence)
            continue;

        rtt_ns = received_at - entry->sent_at;
        entry->valid = false;
        context->pending_count--;
        context->pong_count++;
        context->rtt_sum_ns += (uint64_t) rtt_ns;
        context->samples_per_report++;

        if (fprintf(context->csv, "%" PRIu64 ",%" PRIi64 ",%" PRIi64 "\n",
                    sequence, (int64_t) entry->sent_at, (int64_t) rtt_ns) < 0) {
            context->error = DDS_RETCODE_ERROR;
            break;
        }
    }
    pthread_mutex_unlock(&context->mutex);
}

static dds_entity_t create_reader(
    dds_entity_t participant, dds_entity_t topic, ping_context_t *context)
{
    dds_entity_t subscriber = dds_create_subscriber(participant, NULL, NULL);
    dds_entity_t reader;
    dds_listener_t *listener;

    if (subscriber < 0)
        DDS_FATAL("dds_create_subscriber: %s\n", dds_strretcode(-subscriber));

    listener = dds_create_listener(context);
    if (listener == NULL)
        DDS_FATAL("dds_create_listener failed\n");
    dds_lset_data_available(listener, on_data_available);

    reader = dds_create_reader(subscriber, topic, NULL, listener);
    dds_delete_listener(listener);
    if (reader < 0)
        DDS_FATAL("dds_create_reader: %s\n", dds_strretcode(-reader));
    return reader;
}

static void expire_pings(ping_context_t *context, uint64_t sent_count, dds_time_t now)
{
    pthread_mutex_lock(&context->mutex);
    while (context->next_expiry < sent_count) {
        pending_ping_t *entry =
            &context->pending[context->next_expiry % context->pending_capacity];

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
    pthread_mutex_unlock(&context->mutex);
}

static void print_report(ping_context_t *context)
{
    uint64_t rtt_sum_ns;
    uint64_t received_count;
    uint64_t loss_count;

    pthread_mutex_lock(&context->mutex);
    rtt_sum_ns = context->rtt_sum_ns;
    received_count = context->samples_per_report;
    loss_count = context->loss_count;
    context->rtt_sum_ns = 0;
    context->samples_per_report = 0;
    pthread_mutex_unlock(&context->mutex);

    printf("avg_rtt_ms=%.3f received=%" PRIu64 " loss=%" PRIu64 "\n",
           received_count == 0 ? 0.0 :
           (double) rtt_sum_ns / received_count / DDS_NSECS_IN_MSEC,
           received_count, loss_count);
    fflush(stdout);
}

int main(int argc, char *argv[])
{
    uint64_t interval_ms = DEFAULT_INTERVAL_MS;
    uint64_t ping_count = DEFAULT_COUNT;
    uint64_t sent_count = 0;
    dds_duration_t interval;
    dds_time_t next_ping_due;
    dds_time_t next_report_due;
    dds_entity_t participant;
    dds_entity_t ping_topic;
    dds_entity_t pong_topic;
    dds_entity_t writer;
    dds_entity_t reader;
    RTT_TYPE ping = {0};
    pending_ping_t pending[PENDING_CAPACITY] = {0};
    ping_context_t context;
    char csv_filename[128];
    FILE *csv;

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

    if (ping_count == 0 || interval_ms == 0 ||
            interval_ms > INT64_MAX / DDS_NSECS_IN_MSEC) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (snprintf(csv_filename, sizeof(csv_filename),
                 "rtt_cyclonedds_i%" PRIu64 "_s8_c%" PRIu64 ".csv",
                 interval_ms, ping_count) >= (int) sizeof(csv_filename))
        DDS_FATAL("CSV filename is too long\n");

    csv = fopen(csv_filename, "w");
    if (csv == NULL)
        DDS_FATAL("fopen(%s): %s\n", csv_filename, strerror(errno));
    if (fprintf(csv, "count,timestamp_ns,rtt_ns\n") < 0)
        DDS_FATAL("failed to write CSV header\n");

    context = (ping_context_t) {
        .pending = pending,
        .pending_capacity = PENDING_CAPACITY,
        .csv = csv,
        .error = DDS_RETCODE_OK
    };
    if (pthread_mutex_init(&context.mutex, NULL) != 0)
        DDS_FATAL("pthread_mutex_init failed\n");

    interval = (dds_duration_t) interval_ms * DDS_NSECS_IN_MSEC;
    participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    if (participant < 0)
        DDS_FATAL("dds_create_participant: %s\n", dds_strretcode(-participant));

    ping_topic = create_topic(participant, PING_TOPIC_NAME);
    pong_topic = create_topic(participant, PONG_TOPIC_NAME);
    writer = create_writer(participant, ping_topic);
    reader = create_reader(participant, pong_topic, &context);

    printf("Waiting for ping writer and pong reader to be discovered...\n");
    fflush(stdout);
    while (dds_get_matched_subscriptions(writer, NULL, 0) == 0)
        dds_sleepfor(DDS_MSECS(100));

    printf("Sending %" PRIu64 " pings every %" PRIu64 " ms.\n",
           ping_count, interval_ms);
    next_ping_due = dds_time();
    next_report_due = next_ping_due + DDS_SECS(1);

    for (;;) {
        dds_time_t now = dds_time();
        uint64_t pending_count;

        expire_pings(&context, sent_count, now);

        pthread_mutex_lock(&context.mutex);
        pending_count = context.pending_count;
        pthread_mutex_unlock(&context.mutex);
        if (now >= next_report_due) {
            print_report(&context);
            next_report_due = now + DDS_SECS(1);
        }

        if (sent_count == ping_count && pending_count == 0)
            break;

        if (sent_count < ping_count && now >= next_ping_due) {
            dds_return_t write_status;
            pending_ping_t *slot;

            pthread_mutex_lock(&context.mutex);
            slot = &pending[sent_count % PENDING_CAPACITY];
            if (slot->valid) {
                next_ping_due = now + DDS_MSECS(1);
                pthread_mutex_unlock(&context.mutex);
            } else {
                slot->sequence = sent_count;
                slot->sent_at = dds_time();
                slot->valid = true;
                context.pending_count++;
                ping.payload = sent_count;
                next_ping_due = slot->sent_at + interval;
                pthread_mutex_unlock(&context.mutex);

                write_status = dds_write(writer, &ping);
                if (write_status < 0)
                    DDS_FATAL("dds_write: %s\n", dds_strretcode(-write_status));
                sent_count++;
                continue;
            }
        }

        dds_duration_t sleep_duration = DDS_MSECS(100);
        if (sent_count < ping_count && next_ping_due > now &&
                next_ping_due - now < sleep_duration)
            sleep_duration = next_ping_due - now;
        if (next_report_due > now && next_report_due - now < sleep_duration)
            sleep_duration = next_report_due - now;
        if (sleep_duration > 0)
            dds_sleepfor(sleep_duration);
    }

    print_report(&context);

    const dds_return_t delete_status = dds_delete(participant);
    if (delete_status < 0)
        DDS_FATAL("dds_delete: %s\n", dds_strretcode(-delete_status));
    if (fclose(csv) != 0)
        DDS_FATAL("fclose(%s): %s\n", csv_filename, strerror(errno));
    pthread_mutex_destroy(&context.mutex);
    return EXIT_SUCCESS;
}
