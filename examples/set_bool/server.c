#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#include "SetBool.h"

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

static int8_t set_bool_callback(struct tt_Server* server, struct SetBoolRequest* request,
                                struct SetBoolResponse* response) {
    (void)server;
    printf("  data: %d\n", request->data);

    if (request->data) {
        response->success = true;
        response->message = "Succeed";
    } else {
        response->success = false;
        response->message = "Failed";
    }

    return 0;
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

    struct tt_Server server;

    ret = tt_Node_create_server(&node, &server, &SetBoolService, "set_bool_server",
                                (tt_SERVER_CALLBACK)set_bool_callback);
    if (ret != 0) {
        printf("Cannot create server: %d\n", ret);
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
