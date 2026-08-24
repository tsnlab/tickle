#include "dds/dds.h"
#include "rtt.h"

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

/* Keep the DDS topic/type choices in one place. */
#define RTT_TOPIC_NAME "rtt_topic"
#define RTT_TYPE rttModule_DataType
#define RTT_TYPE_DESCRIPTOR rttModule_DataType_desc
#define RTT_TYPE_FREE rttModule_DataType_free

#define PING_PARTITION "ping"
#define PONG_PARTITION "pong"
#define MAX_SAMPLES 16

static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int signal_number)
{
  (void)signal_number;
  keep_running = 0;
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
  const char *partitions[] = {PONG_PARTITION};
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
  const char *partitions[] = {PING_PARTITION};
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

int main(void)
{
  RTT_TYPE received[MAX_SAMPLES] = {0};
  void *samples[MAX_SAMPLES];
  dds_sample_info_t sample_info[MAX_SAMPLES];
  dds_entity_t participant;
  dds_entity_t topic;
  dds_entity_t writer;
  dds_entity_t reader;
  dds_entity_t condition;
  dds_entity_t waitset;
  dds_attach_t attachments[1];

  for (size_t i = 0; i < MAX_SAMPLES; i++)
    samples[i] = &received[i];

  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);

  participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
  if (participant < 0)
    DDS_FATAL("dds_create_participant: %s\n", dds_strretcode(-participant));

  topic = create_topic(participant);
  reader = create_reader(participant, topic);
  writer = create_writer(participant, topic);
  condition = dds_create_readcondition(reader, DDS_ANY_STATE);
  if (condition < 0)
    DDS_FATAL("dds_create_readcondition: %s\n", dds_strretcode(-condition));

  waitset = dds_create_waitset(participant);
  if (waitset < 0)
    DDS_FATAL("dds_create_waitset: %s\n", dds_strretcode(-waitset));
  const dds_return_t attach_status = dds_waitset_attach(waitset, condition, reader);
  if (attach_status < 0)
    DDS_FATAL("dds_waitset_attach: %s\n", dds_strretcode(-attach_status));

  printf("Waiting for pings; press Ctrl-C to stop.\n");
  fflush(stdout);

  while (keep_running) {
    const int wait_status = dds_waitset_wait(waitset, attachments, 1, DDS_MSECS(100));
    if (wait_status < 0)
      DDS_FATAL("dds_waitset_wait: %s\n", dds_strretcode(-wait_status));
    if (wait_status == 0)
      continue;

    const int sample_count = dds_take(reader, samples, sample_info, MAX_SAMPLES, MAX_SAMPLES);
    if (sample_count < 0)
      DDS_FATAL("dds_take: %s\n", dds_strretcode(-sample_count));

    for (int i = 0; i < sample_count; i++) {
      if (!sample_info[i].valid_data)
        continue;

      const dds_return_t write_status = dds_write(writer, &received[i]);
      if (write_status < 0)
        DDS_FATAL("dds_write: %s\n", dds_strretcode(-write_status));
    }
  }

  for (size_t i = 0; i < MAX_SAMPLES; i++)
    RTT_TYPE_FREE(&received[i], DDS_FREE_CONTENTS);
  const dds_return_t delete_status = dds_delete(participant);
  if (delete_status < 0)
    DDS_FATAL("dds_delete: %s\n", dds_strretcode(-delete_status));
  return EXIT_SUCCESS;
}
