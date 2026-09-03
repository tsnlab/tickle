#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "UInt64.h"

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

static void uint64_data_callback(struct tt_Subscriber* sub, uint64_t timestamp, uint16_t seq_no,
                                 struct UInt64Data* data) {
    (void)sub;
    printf("  timestamp: %ld\n", timestamp);
    printf("  seq_no: %d\n", seq_no);
    printf("  data->data: %lx\n", data->data);
}

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s [-b broadcast] [-p port] [-a bind_addr] [-d duration_seconds]\n", prog);
    fprintf(stderr, "  -b  broadcast address (default 192.168.10.255)\n");
    fprintf(stderr, "  -p  UDP port (default: compiled-in tt_NODE_PORT)\n");
    fprintf(stderr, "  -a  bind address (default: compiled-in tt_NODE_ADDRESS)\n");
    fprintf(stderr, "  -d  exit automatically after this many seconds (default 0 = run until Ctrl+C)\n");
}

int main(int argc, char** argv) {
    char* broadcast = "192.168.10.255";
    int port = 0;            // 0 = keep the compiled-in default
    char* bind_addr = NULL;  // NULL = keep the compiled-in default
    double duration_s = 0.0; // 0 = run until Ctrl+C

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            broadcast = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            bind_addr = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_s = strtod(argv[++i], NULL);
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    _tt_CONFIG.broadcast = broadcast;
    if (port != 0) {
        _tt_CONFIG.port = port;
    }
    if (bind_addr != NULL) {
        _tt_CONFIG.addr = bind_addr;
    }

    signal(SIGINT, handle_sigint);

    struct tt_Node node;
    tt_ret_t ret = tt_Node_create(&node);
    if (ret != 0) {
        printf("Cannot create node: %d\n", ret);
        return ret;
    }

    printf("Node created(#%d)\n", node.id);

    struct tt_Subscriber sub;

    ret = tt_Node_create_subscriber(&node, &sub, &UInt64Topic, "uint64_topic",
                                    (tt_SUBSCRIBER_CALLBACK)uint64_data_callback);
    if (ret != 0) {
        printf("Cannot create subscriber: %d\n", ret);
        return ret;
    }

    if (duration_s > 0.0) {
        tt_Node_schedule(&node, tt_get_ns() + (uint64_t)(duration_s * (double)tt_SECOND), handle_duration_elapsed,
                         NULL);
    }

    ret = tt_RET_OK;
    while (!g_interrupted && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }

    tt_Node_destroy(&node);
    printf("Node destroyed(#%d): %d\n", node.id, ret);

    return 0;
}
