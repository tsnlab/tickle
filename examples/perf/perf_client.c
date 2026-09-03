#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "Bulk.h"

// DEFAULT_MESSAGE_SIZE fills one standard (1500-byte MTU) Ethernet frame as full as this
// protocol allows, so every packet carries the most payload it can without IP fragmentation.
// See "Message size: filling an Ethernet frame" in README.md for the derivation.
#define DEFAULT_MESSAGE_SIZE BULK_MAX_PAYLOAD_SIZE

// DEFAULT_INTERVAL_SECONDS of 0 means no send interval at all: publish as fast as poll()
// allows, which is what a throughput benchmark should default to. Pass -i to rate-limit to a
// specific interval instead (e.g. to target a specific Mbps for a given -s).
#define DEFAULT_INTERVAL_SECONDS 0.0

static volatile sig_atomic_t g_interrupted = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static void handle_duration_elapsed(struct tt_Node* node, uint64_t time, void* param) {
    (void)node;
    (void)time;
    (void)param;
    g_interrupted = 1;
}

static struct BulkData bulk = {0}; // static: zero-initialized, reused for every publish

static uint64_t total_sent_msgs = 0;
static uint64_t total_sent_bytes = 0;
static uint64_t total_buffer_full = 0;

static uint64_t interval_sent_msgs = 0;
static uint64_t interval_sent_bytes = 0;

static void report(struct tt_Node* node, uint64_t time, void* param) {
    (void)param;

    const double bytes_per_mb = 1e6;
    double megabytes = (double)interval_sent_bytes / bytes_per_mb;
    double mbps = ((double)interval_sent_bytes * 8) / bytes_per_mb;
    printf("sent %llu msgs, %.3f MB, %.3f Mbps this interval (%llu buffer-full so far)\n",
           (unsigned long long)interval_sent_msgs, megabytes, mbps, (unsigned long long)total_buffer_full);

    interval_sent_msgs = 0;
    interval_sent_bytes = 0;

    tt_Node_schedule(node, time + tt_SECOND, report, NULL);
}

static void print_summary(uint64_t start_time) {
    const double bytes_per_mb = 1e6;
    double elapsed_s = (double)(tt_get_ns() - start_time) / (double)tt_SECOND;
    double megabytes = (double)total_sent_bytes / bytes_per_mb;
    double avg_mbps = elapsed_s > 0.0 ? ((double)total_sent_bytes * 8) / bytes_per_mb / elapsed_s : 0.0;

    printf("\n--- bulk_topic send statistics ---\n");
    printf("%llu messages sent, %.3f MB, %.3f sec, avg %.3f Mbps, %llu times tx buffer was full\n",
           (unsigned long long)total_sent_msgs, megabytes, elapsed_s, avg_mbps, (unsigned long long)total_buffer_full);
}

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s [-b broadcast] [-p port] [-a bind_addr] [-s message_size_bytes]\n", prog);
    fprintf(stderr, "                [-i interval_seconds] [-d duration_seconds]\n");
    fprintf(stderr, "  -b  broadcast address (default 192.168.10.255)\n");
    fprintf(stderr, "  -p  UDP port (default: compiled-in tt_NODE_PORT)\n");
    fprintf(stderr, "  -a  bind address (default: compiled-in tt_NODE_ADDRESS)\n");
    fprintf(stderr, "  -s  payload bytes per message (default/max %d: fills one Ethernet frame)\n",
            DEFAULT_MESSAGE_SIZE);
    fprintf(stderr, "  -i  seconds between sends (default %g = as fast as poll() allows)\n", DEFAULT_INTERVAL_SECONDS);
    fprintf(stderr, "  -d  exit automatically after this many seconds (default 0 = run until Ctrl+C)\n");
}

struct cli_options {
    char* broadcast;
    int port;        // 0 = keep the compiled-in default
    char* bind_addr; // NULL = keep the compiled-in default
    uint32_t message_size;
    double interval_s;
    double duration_s; // 0 = run until Ctrl+C
};

// Returns 0 on success, non-zero if argv held an unrecognized/incomplete option.
static int parse_args(int argc, char** argv, struct cli_options* opts) {
    opts->broadcast = "192.168.10.255";
    opts->port = 0;
    opts->bind_addr = NULL;
    opts->message_size = DEFAULT_MESSAGE_SIZE;
    opts->interval_s = DEFAULT_INTERVAL_SECONDS;
    opts->duration_s = 0.0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            opts->broadcast = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            opts->port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            opts->bind_addr = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            opts->message_size = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            opts->interval_s = strtod(argv[++i], NULL);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            opts->duration_s = strtod(argv[++i], NULL);
        } else {
            return 1;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    struct cli_options opts;
    if (parse_args(argc, argv, &opts) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    if (opts.message_size > BULK_MAX_PAYLOAD_SIZE) {
        printf("Requested message size %u exceeds the max %d; clamping.\n", opts.message_size, BULK_MAX_PAYLOAD_SIZE);
        opts.message_size = BULK_MAX_PAYLOAD_SIZE;
    }
    bulk.size = opts.message_size;

    uint64_t send_interval_ns = opts.interval_s > 0.0 ? (uint64_t)(opts.interval_s * (double)tt_SECOND) : 0;

    _tt_CONFIG.broadcast = opts.broadcast;
    if (opts.port != 0) {
        _tt_CONFIG.port = opts.port;
    }
    if (opts.bind_addr != NULL) {
        _tt_CONFIG.addr = opts.bind_addr;
    }

    signal(SIGINT, handle_sigint);

    struct tt_Node node;
    tt_ret_t ret = tt_Node_create(&node);
    if (ret != 0) {
        printf("Cannot create node: %d\n", ret);
        return ret;
    }

    printf("Node created(#%d)\n", node.id);

    struct tt_Publisher pub;
    ret = tt_Node_create_publisher(&node, &pub, &BulkTopic, "bulk_topic");
    if (ret != 0) {
        printf("Cannot create publisher: %d\n", ret);
        return ret;
    }

    const double bytes_per_mb = 1e6;
    double target_mbps = opts.interval_s > 0.0 ? ((double)opts.message_size * 8) / bytes_per_mb / opts.interval_s : 0.0;
    if (opts.interval_s > 0.0) {
        printf("Sending %u-byte messages every %g sec (target %.3f Mbps)\n", opts.message_size, opts.interval_s,
               target_mbps);
    } else {
        printf("Sending %u-byte messages as fast as poll() allows\n", opts.message_size);
    }

    uint64_t start_time = tt_get_ns();
    uint64_t next_send_time = start_time;
    tt_Node_schedule(&node, start_time + tt_SECOND, report, NULL);
    if (opts.duration_s > 0.0) {
        tt_Node_schedule(&node, start_time + (uint64_t)(opts.duration_s * (double)tt_SECOND), handle_duration_elapsed,
                         NULL);
    }

    ret = tt_RET_OK;
    while (!g_interrupted && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        // One publish attempt per due tick, then let poll() flush/schedule/receive: pacing
        // comes from comparing wall-clock time against next_send_time, not from a tight
        // send-more-if-you-can loop (that would starve poll() - and this interrupt check).
        uint64_t now = tt_get_ns();
        if (now >= next_send_time) {
            tt_ret_t pub_ret = tt_Publisher_publish(&pub, (struct tt_Data*)&bulk);
            if (pub_ret == tt_RET_OK) {
                bulk.seq++;
                total_sent_msgs++;
                total_sent_bytes += bulk.size;
                interval_sent_msgs++;
                interval_sent_bytes += bulk.size;
            } else if (pub_ret == tt_RET_OUT_OF_BUFFER) {
                total_buffer_full++;
            }
            next_send_time += send_interval_ns;
        }

        ret = tt_Node_poll(&node, -1);
    }

    print_summary(start_time);

    tt_Node_destroy(&node);

    return 0;
}
