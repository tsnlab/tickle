#include "dds/dds.h"
#include "rtt.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keep the DDS topic/type choices in one place. */
#define RTT_TOPIC_NAME "rtt_topic"
#define RTT_TYPE rttModule_DataType
#define RTT_TYPE_DESCRIPTOR rttModule_DataType_desc
#define RTT_TYPE_FREE rttModule_DataType_free

#define PING_PARTITION "ping"
#define PONG_PARTITION "pong"
#define MAX_SAMPLES 16
#define PENDING_PINGS 1024
#define SEQUENCE_SIZE 8

typedef struct pending_ping {
    uint64_t sequence;
    dds_time_t sent_at;
    bool valid;
} pending_ping_t;

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s -i <interval_ms> -c <count>\n", program);
}

static bool parse_uint64(const char *text, uint64_t *value)
{
    char *end;
    unsigned long long parsed;

    if (*text == '\0' || *text == '-')
        return false;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0')
        return false;
    *value = (uint64_t)parsed;
    return true;
}

static void encode_sequence(uint8_t payload[SEQUENCE_SIZE], uint64_t sequence)
{
    for (size_t i = 0; i < SEQUENCE_SIZE; i++)
        payload[i] = (uint8_t)(sequence >> (i * 8));
}

static uint64_t decode_sequence(const uint8_t payload[SEQUENCE_SIZE])
{
    uint64_t sequence = 0;

    for (size_t i = 0; i < SEQUENCE_SIZE; i++)
        sequence |= (uint64_t)payload[i] << (i * 8);
    return sequence;
}

static dds_entity_t create_topic(dds_entity_t participant)
{
    dds_qos_t *qos = dds_create_qos();
    dds_entity_t topic;

    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
    topic = dds_create_topic(participant, &RTT_TYPE_DESCRIPTOR, RTT_TOPIC_NAME, qos, NULL);
    dds_delete_qos(qos);
    if (topic < 0)
        DDS_FATAL("dds_create_topic: %s\n", dds_strretcode(-topic));
    return topic;
}

static dds_entity_t create_writer(dds_entity_t participant, dds_entity_t topic)
{
    const char *partitions[] = {PING_PARTITION};
    dds_qos_t *qos = dds_create_qos();
    dds_entity_t publisher;
    dds_entity_t writer;

    dds_qset_partition(qos, 1, partitions);
    publisher = dds_create_publisher(participant, qos, NULL);
    dds_delete_qos(qos);
    if (publisher < 0)
        DDS_FATAL("dds_create_publisher: %s\n", dds_strretcode(-publisher));

    writer = dds_create_writer(publisher, topic, NULL, NULL);
    if (writer < 0)
        DDS_FATAL("dds_create_writer: %s\n", dds_strretcode(-writer));
    return writer;
}

static dds_entity_t create_reader(dds_entity_t participant, dds_entity_t topic)
{
    const char *partitions[] = {PONG_PARTITION};
    dds_qos_t *qos = dds_create_qos();
    dds_entity_t subscriber;
    dds_entity_t reader;

    dds_qset_partition(qos, 1, partitions);
    subscriber = dds_create_subscriber(participant, qos, NULL);
    dds_delete_qos(qos);
    if (subscriber < 0)
        DDS_FATAL("dds_create_subscriber: %s\n", dds_strretcode(-subscriber));

    reader = dds_create_reader(subscriber, topic, NULL, NULL);
    if (reader < 0)
        DDS_FATAL("dds_create_reader: %s\n", dds_strretcode(-reader));
    return reader;
}

static dds_time_t send_ping(
    dds_entity_t writer,
    RTT_TYPE *message,
    pending_ping_t pending[PENDING_PINGS],
    uint64_t sequence)
{
    pending_ping_t *entry = &pending[sequence % PENDING_PINGS];

    encode_sequence(message->payload._buffer, sequence);
    entry->sequence = sequence;
    entry->sent_at = dds_time();
    entry->valid = true;

    const dds_return_t status = dds_write(writer, message);
    if (status < 0)
        DDS_FATAL("dds_write: %s\n", dds_strretcode(-status));
    return entry->sent_at;
}

static void print_average(uint64_t rtt_sum_ns, uint64_t received_count)
{
    const double average_us = received_count == 0 ? 0.0 :
        (double)rtt_sum_ns / (double)received_count / (double)DDS_NSECS_IN_USEC;
    printf("avg_rtt_us=%.3f samples=%" PRIu64 "\n", average_us, received_count);
    fflush(stdout);
}

int main(int argc, char *argv[])
{
    RTT_TYPE received[MAX_SAMPLES] = {0};
    void *samples[MAX_SAMPLES];
    dds_sample_info_t sample_info[MAX_SAMPLES];
    pending_ping_t pending[PENDING_PINGS] = {0};
    uint8_t payload[SEQUENCE_SIZE];
    RTT_TYPE ping = {
        .payload = {
            ._maximum = SEQUENCE_SIZE,
            ._length = SEQUENCE_SIZE,
            ._buffer = payload,
            ._release = false
        }
    };
    uint64_t interval_ms = 0;
    uint64_t ping_count = 0;
    uint64_t sent_count = 0;
    uint64_t pong_count = 0;
    uint64_t pending_count = 0;
    uint64_t next_sequence = 0;
    uint64_t rtt_sum_ns = 0;
    uint64_t received_count = 0;
    bool have_interval = false;
    bool have_count = false;
    char *csv_filename;
    FILE *csv;
    dds_duration_t interval;
    dds_time_t next_ping_due;
    dds_time_t report_started;
    dds_entity_t participant;
    dds_entity_t topic;
    dds_entity_t writer;
    dds_entity_t reader;
    dds_entity_t condition;
    dds_entity_t waitset;
    dds_attach_t attachments[1];

    for (int i = 1; i < argc; i++) {
        const char *option;
        const char *argument;

        if (i + 1 == argc) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        option = argv[i];
        argument = argv[++i];
        if (strcmp(option, "-i") == 0) {
            have_interval = parse_uint64(argument, &interval_ms);
        } else if (strcmp(option, "-c") == 0) {
            have_count = parse_uint64(argument, &ping_count);
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        if ((!have_interval && strcmp(option, "-i") == 0) ||
                (!have_count && strcmp(option, "-c") == 0)) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (!have_interval || !have_count || ping_count == 0 ||
            interval_ms > (uint64_t)INT64_MAX / DDS_NSECS_IN_MSEC) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    interval = (dds_duration_t)interval_ms * DDS_NSECS_IN_MSEC;

    const int filename_length = snprintf(
        NULL, 0, "rtt_cyclonedds_i%" PRIu64 "_s8_c%" PRIu64 ".csv", interval_ms, ping_count);
    if (filename_length < 0)
        DDS_FATAL("snprintf failed while creating the CSV filename\n");
    csv_filename = malloc((size_t)filename_length + 1);
    if (csv_filename == NULL)
        DDS_FATAL("malloc failed while creating the CSV filename\n");
    snprintf(
        csv_filename, (size_t)filename_length + 1,
        "rtt_cyclonedds_i%" PRIu64 "_s8_c%" PRIu64 ".csv", interval_ms, ping_count);
    csv = fopen(csv_filename, "w");
    if (csv == NULL)
        DDS_FATAL("fopen(%s): %s\n", csv_filename, strerror(errno));
    if (fprintf(csv, "count,timestamp_ns,rtt_ns\n") < 0)
        DDS_FATAL("failed to write CSV header\n");

    for (size_t i = 0; i < MAX_SAMPLES; i++)
        samples[i] = &received[i];

    participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    if (participant < 0)
        DDS_FATAL("dds_create_participant: %s\n", dds_strretcode(-participant));

    topic = create_topic(participant);
    writer = create_writer(participant, topic);
    reader = create_reader(participant, topic);
    condition = dds_create_readcondition(reader, DDS_ANY_STATE);
    if (condition < 0)
        DDS_FATAL("dds_create_readcondition: %s\n", dds_strretcode(-condition));

    waitset = dds_create_waitset(participant);
    if (waitset < 0)
        DDS_FATAL("dds_create_waitset: %s\n", dds_strretcode(-waitset));
    const dds_return_t attach_status = dds_waitset_attach(waitset, condition, reader);
    if (attach_status < 0)
        DDS_FATAL("dds_waitset_attach: %s\n", dds_strretcode(-attach_status));

    dds_return_t matched_readers;
    dds_return_t matched_writers;
    printf("Waiting for ping and pong endpoints to be discovered...\n");
    fflush(stdout);
    do {
        matched_readers = dds_get_matched_subscriptions(writer, NULL, 0);
        matched_writers = dds_get_matched_publications(reader, NULL, 0);
        if (matched_readers < 0 || matched_writers < 0)
            DDS_FATAL("failed to get DDS endpoint matches\n");
        if (matched_readers == 0 || matched_writers == 0)
            dds_sleepfor(DDS_MSECS(100));
    } while (matched_readers == 0 || matched_writers == 0);

    printf("Sending %" PRIu64 " pings every %" PRIu64 " ms.\n", ping_count, interval_ms);
    report_started = dds_time();
    next_ping_due = report_started;

    while (pong_count < ping_count) {
        dds_time_t now = dds_time();
        if (now - report_started >= DDS_SECS(1)) {
            print_average(rtt_sum_ns, received_count);
            rtt_sum_ns = 0;
            received_count = 0;
            report_started = now;
        }

        while (sent_count < ping_count && pending_count < PENDING_PINGS && now >= next_ping_due) {
            next_ping_due = send_ping(writer, &ping, pending, next_sequence++) + interval;
            sent_count++;
            pending_count++;
            now = dds_time();
        }

        dds_duration_t wait_timeout = DDS_SECS(1) - (now - report_started);
        if (sent_count < ping_count && pending_count < PENDING_PINGS &&
                next_ping_due > now && next_ping_due - now < wait_timeout) {
            wait_timeout = next_ping_due - now;
        }
        const int wait_status = dds_waitset_wait(waitset, attachments, 1, wait_timeout);
        if (wait_status < 0)
            DDS_FATAL("dds_waitset_wait: %s\n", dds_strretcode(-wait_status));

        if (wait_status > 0) {
            const int sample_count = dds_take(reader, samples, sample_info, MAX_SAMPLES, MAX_SAMPLES);
            if (sample_count < 0)
                DDS_FATAL("dds_take: %s\n", dds_strretcode(-sample_count));

            for (int i = 0; i < sample_count; i++) {
                if (!sample_info[i].valid_data || received[i].payload._length != SEQUENCE_SIZE)
                    continue;

                const uint64_t sequence = decode_sequence(received[i].payload._buffer);
                pending_ping_t *entry = &pending[sequence % PENDING_PINGS];
                if (!entry->valid || entry->sequence != sequence)
                    continue;

                const dds_time_t arrival_timestamp_ns = dds_time();
                const dds_time_t rtt_ns = arrival_timestamp_ns - entry->sent_at;
                entry->valid = false;
                pending_count--;
                pong_count++;
                rtt_sum_ns += (uint64_t)rtt_ns;
                received_count++;
                if (fprintf(csv, "%" PRIu64 ",%" PRIi64 ",%" PRIi64 "\n",
                            sequence, (int64_t)arrival_timestamp_ns, (int64_t)rtt_ns) < 0) {
                    DDS_FATAL("failed to write CSV row\n");
                }
            }
        }
    }

    if (received_count != 0)
        print_average(rtt_sum_ns, received_count);
    if (fclose(csv) != 0)
        DDS_FATAL("fclose(%s): %s\n", csv_filename, strerror(errno));
    free(csv_filename);

    for (size_t i = 0; i < MAX_SAMPLES; i++)
        RTT_TYPE_FREE(&received[i], DDS_FREE_CONTENTS);
    const dds_return_t delete_status = dds_delete(participant);
    if (delete_status < 0)
        DDS_FATAL("dds_delete: %s\n", dds_strretcode(-delete_status));
    return EXIT_SUCCESS;
}
