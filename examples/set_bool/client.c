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

static uint32_t target_count = 0; // 0 = unlimited
static uint32_t transmitted = 0;
static uint32_t completed = 0; // calls whose outcome (response or error) is known
static uint64_t call_interval_ns = 0;
static bool next_data = true; // alternates the request payload each call, like the original demo

static void note_completed(void) {
    completed++;
    if (target_count > 0 && completed >= target_count) {
        g_interrupted = 1;
    }
}

static void set_bool_callback(struct tt_Client* client, int8_t return_code, struct SetBoolResponse* response) {
    (void)client;
    if (return_code == 0 && response == NULL) {
        printf("  Server not found\n");
    } else if (return_code != 0) {
        printf("  Error, return_code: %d\n", return_code);
    } else {
        printf("  return_code: %d\n", return_code);
        printf("  response: %d, %s\n", response->success, response->message);
    }
    note_completed();
}

static void call(struct tt_Node* node, uint64_t time, void* param) {
    struct tt_Client* client = param;

    printf("Call #%u\n", transmitted);
    struct SetBoolRequest request = {.data = next_data};
    tt_ret_t ret = tt_Client_call(client, (struct tt_Request*)&request);
    if (ret == tt_RET_OK) {
        transmitted++;
        next_data = !next_data;
    } else if (ret == tt_RET_ILLEGAL_STATUS) {
        printf("Previous call still awaiting a response, skipping this interval\n");
    } else {
        printf("Cannot call: %d\n", ret);
    }

    if (target_count == 0 || transmitted < target_count) {
        tt_Node_schedule(node, time + call_interval_ns, call, client);
    }
}

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s [-b broadcast] [-p port] [-a bind_addr] [-c count] [-i interval_seconds]\n", prog);
    fprintf(stderr, "  -b  broadcast address (default 192.168.10.255)\n");
    fprintf(stderr, "  -p  UDP port (default: compiled-in tt_NODE_PORT)\n");
    fprintf(stderr, "  -a  bind address (default: compiled-in tt_NODE_ADDRESS)\n");
    fprintf(stderr, "  -c  stop after this many calls (default 0 = run until Ctrl+C)\n");
    fprintf(stderr, "  -i  seconds between calls (default 1)\n");
}

int main(int argc, char** argv) {
    char* broadcast = "192.168.10.255";
    int port = 0;           // 0 = keep the compiled-in default
    char* bind_addr = NULL; // NULL = keep the compiled-in default
    double interval_s = 1.0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            broadcast = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            bind_addr = argv[++i];
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            target_count = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval_s = strtod(argv[++i], NULL);
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    call_interval_ns = (uint64_t)(interval_s * (double)tt_SECOND);

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

    struct tt_Client client;

    ret = tt_Node_create_client(&node, &client, &SetBoolService, "set_bool_server",
                                (tt_CLIENT_CALLBACK)set_bool_callback);
    if (ret != 0) {
        printf("Cannot create server: %d\n", ret);
        return ret;
    }

    tt_Node_schedule(&node, tt_get_ns(), call, &client);

    ret = tt_RET_OK;
    while (!g_interrupted && (ret == tt_RET_OK || ret == tt_RET_TIMEOUT)) {
        ret = tt_Node_poll(&node, -1);
    }

    tt_Node_destroy(&node);
    printf("Node destroyed(#%d): %d\n", node.id, ret);

    return 0;
}
