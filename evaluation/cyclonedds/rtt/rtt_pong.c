#include "dds/dds.h"
#include "rtt.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define PING_TOPIC_NAME "PingTopic"
#define PONG_TOPIC_NAME "PongTopic"
#define RTT_TYPE rttModule_DataType
#define RTT_TYPE_DESCRIPTOR rttModule_DataType_desc
#define MAX_SAMPLES 16

typedef struct queued_ping {
    uint64_t sequence;
    struct queued_ping *next;
} queued_ping_t;

typedef struct pong_context {
    dds_entity_t writer;
    queued_ping_t *head;
    queued_ping_t *tail;
    dds_return_t error;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
} pong_context_t;

static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int signal_number)
{
    (void) signal_number;
    keep_running = 0;
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
    pong_context_t *context = arg;
    RTT_TYPE received[MAX_SAMPLES] = {0};
    void *samples[MAX_SAMPLES];
    dds_sample_info_t sample_info[MAX_SAMPLES];

    for (size_t i = 0; i < MAX_SAMPLES; i++)
        samples[i] = &received[i];

    const int sample_count = dds_take(
        reader, samples, sample_info, MAX_SAMPLES, MAX_SAMPLES);
    if (sample_count < 0) {
        pthread_mutex_lock(&context->mutex);
        context->error = sample_count;
        pthread_cond_signal(&context->condition);
        pthread_mutex_unlock(&context->mutex);
        return;
    }

    for (int i = 0; i < sample_count; i++) {
        queued_ping_t *queued;

        if (!sample_info[i].valid_data)
            continue;

        queued = malloc(sizeof(*queued));
        if (queued == NULL) {
            pthread_mutex_lock(&context->mutex);
            context->error = DDS_RETCODE_OUT_OF_RESOURCES;
            pthread_cond_signal(&context->condition);
            pthread_mutex_unlock(&context->mutex);
            return;
        }

        queued->sequence = received[i].payload;
        queued->next = NULL;

        pthread_mutex_lock(&context->mutex);
        if (context->tail == NULL)
            context->head = queued;
        else
            context->tail->next = queued;
        context->tail = queued;
        pthread_cond_signal(&context->condition);
        pthread_mutex_unlock(&context->mutex);
    }
}

static dds_entity_t create_reader(
    dds_entity_t participant, dds_entity_t topic, pong_context_t *context)
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

static queued_ping_t *dequeue_ping(pong_context_t *context)
{
    queued_ping_t *queued = context->head;

    if (queued != NULL) {
        context->head = queued->next;
        if (context->head == NULL)
            context->tail = NULL;
    }
    return queued;
}

static void wait_for_work(pong_context_t *context)
{
    struct timespec timeout;

    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_nsec += 100000000;
    if (timeout.tv_nsec >= 1000000000) {
        timeout.tv_sec++;
        timeout.tv_nsec -= 1000000000;
    }
    (void) pthread_cond_timedwait(&context->condition, &context->mutex, &timeout);
}

static void clear_queue(pong_context_t *context)
{
    queued_ping_t *queued;

    pthread_mutex_lock(&context->mutex);
    while ((queued = dequeue_ping(context)) != NULL)
        free(queued);
    pthread_mutex_unlock(&context->mutex);
}

int main(void)
{
    dds_entity_t participant;
    dds_entity_t ping_topic;
    dds_entity_t pong_topic;
    pong_context_t context = {
        .writer = DDS_ENTITY_NIL,
        .error = DDS_RETCODE_OK,
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER
    };

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    if (participant < 0)
        DDS_FATAL("dds_create_participant: %s\n", dds_strretcode(-participant));

    ping_topic = create_topic(participant, PING_TOPIC_NAME);
    pong_topic = create_topic(participant, PONG_TOPIC_NAME);
    context.writer = create_writer(participant, pong_topic);
    (void) create_reader(participant, ping_topic, &context);

    printf("Waiting for pings; press Ctrl-C to stop.\n");
    fflush(stdout);

    while (keep_running) {
        queued_ping_t *queued;
        dds_return_t callback_error;

        pthread_mutex_lock(&context.mutex);
        while (keep_running && context.head == NULL &&
                context.error == DDS_RETCODE_OK)
            wait_for_work(&context);

        callback_error = context.error;
        queued = dequeue_ping(&context);
        pthread_mutex_unlock(&context.mutex);

        if (callback_error < 0)
            DDS_FATAL("on_data_available: %s\n", dds_strretcode(-callback_error));
        if (queued == NULL)
            continue;

        RTT_TYPE pong = {.payload = queued->sequence};
        const dds_return_t write_status = dds_write(context.writer, &pong);
        free(queued);
        if (write_status < 0)
            DDS_FATAL("dds_write: %s\n", dds_strretcode(-write_status));
    }

    const dds_return_t delete_status = dds_delete(participant);
    if (delete_status < 0)
        DDS_FATAL("dds_delete: %s\n", dds_strretcode(-delete_status));
    clear_queue(&context);
    pthread_cond_destroy(&context.condition);
    pthread_mutex_destroy(&context.mutex);
    return EXIT_SUCCESS;
}
