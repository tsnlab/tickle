#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/log.h>
#include <tickle/tickle.h>

#include "UInt64.h"

static volatile sig_atomic_t g_interrupted = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static uint32_t target_count = 0; // 0 = unlimited
static uint32_t transmitted = 0;
static uint64_t publish_interval_ns = 0;

static void publish(struct tt_Node* node, uint64_t time, void* param) {
    struct tt_Publisher* pub = param;

    const uint64_t example_data = 0xdeadbeef;
    struct UInt64Data data = {.data = example_data + transmitted};
    tt_ret_t ret = tt_Publisher_publish(pub, (struct tt_Data*)&data);
    if (ret == tt_RET_OK) {
        transmitted++;
    } else {
        printf("Cannot publish: %d\n", ret);
    }

    if (target_count == 0 || transmitted < target_count) {
        tt_Node_schedule(node, time + publish_interval_ns, publish, pub);
    } else {
        g_interrupted = 1;
    }
}

static bool parse_log_level(const char* str, tt_LogLevel* level) {
    if (strcmp(str, "debug") == 0) {
        *level = TT_LOG_DEBUG;
    } else if (strcmp(str, "info") == 0) {
        *level = TT_LOG_INFO;
    } else if (strcmp(str, "warning") == 0) {
        *level = TT_LOG_WARNING;
    } else if (strcmp(str, "error") == 0) {
        *level = TT_LOG_ERROR;
    } else if (strcmp(str, "none") == 0) {
        *level = TT_LOG_NONE;
    } else {
        return false;
    }
    return true;
}

static void print_usage(const char* prog) {
    fprintf(stderr,
            "Usage: %s [-b broadcast] [-p port] [-a bind_addr] [-c count] [-i interval_seconds]\n"
            "          [-n topic_name] [-l log_level]\n",
            prog);
    fprintf(stderr, "  -b  broadcast address (default 192.168.10.255)\n");
    fprintf(stderr, "  -p  UDP port (default: compiled-in tt_NODE_PORT)\n");
    fprintf(stderr, "  -a  bind address (default: compiled-in tt_NODE_ADDRESS)\n");
    fprintf(stderr, "  -c  stop after publishing this many messages (default 0 = run until Ctrl+C)\n");
    fprintf(stderr, "  -i  seconds between publishes (default 1)\n");
    fprintf(stderr, "  -n  topic name to publish on (default uint64_topic)\n");
    fprintf(stderr, "  -l  log level: debug|info|warning|error|none (default info)\n");
}

struct cli_options {
    char* broadcast;
    int port;        // 0 = keep the compiled-in default
    char* bind_addr; // NULL = keep the compiled-in default
    double interval_s;
    char* topic_name;
    tt_LogLevel log_level;
    bool log_level_set;
};

// Returns 0 on success, non-zero if argv held an unrecognized/incomplete option.
static int parse_args(int argc, char** argv, struct cli_options* opts) {
    opts->broadcast = "192.168.10.255";
    opts->port = 0;
    opts->bind_addr = NULL;
    opts->interval_s = 1.0;
    opts->topic_name = "uint64_topic";
    opts->log_level = TT_LOG_INFO;
    opts->log_level_set = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            opts->broadcast = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            opts->port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            opts->bind_addr = argv[++i];
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            target_count = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            opts->interval_s = strtod(argv[++i], NULL);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            opts->topic_name = argv[++i];
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            if (!parse_log_level(argv[++i], &opts->log_level)) {
                return 1;
            }
            opts->log_level_set = true;
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

    publish_interval_ns = (uint64_t)(opts.interval_s * (double)tt_SECOND);

    _tt_CONFIG.broadcast = opts.broadcast;
    if (opts.port != 0) {
        _tt_CONFIG.port = opts.port;
    }
    if (opts.bind_addr != NULL) {
        _tt_CONFIG.addr = opts.bind_addr;
    }
    if (opts.log_level_set) {
        tt_log_set_level(opts.log_level);
    }

    // sigaction (not signal()) so SA_RESTART is off: an interrupted blocking recv
    // returns immediately instead of silently restarting with the same wait.
    struct sigaction sigint_action = {0};
    sigint_action.sa_handler = handle_sigint;
    sigaction(SIGINT, &sigint_action, NULL);

    struct tt_Node node;
    tt_ret_t ret = tt_Node_create(&node);
    if (ret != 0) {
        printf("Cannot create node: %d\n", ret);
        return ret;
    }

    printf("Node created(#%d)\n", node.id);

    struct tt_Publisher pub;

    ret = tt_Node_create_publisher(&node, &pub, &UInt64Topic, opts.topic_name);
    if (ret != 0) {
        printf("Cannot create server: %d\n", ret);
        return ret;
    }

    tt_Node_schedule(&node, tt_get_ns(), publish, &pub);

    ret = tt_RET_OK;
    while (!g_interrupted && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }

    tt_Node_destroy(&node);
    printf("Node destroyed(#%d): %d\n", node.id, ret);

    return 0;
}
