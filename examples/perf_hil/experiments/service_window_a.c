// Copyright (c) 2025-2026 TSN Lab, Inc.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3, as published by the Free
// Software Foundation. A proprietary license is also available on request - see README.md.

// S9 window A (RMW_GAPS S9, ROADMAP "Wired work"): a service datagram larger than a segment slot goes over UDP
// (segment_deliver(): UDP_BECAUSE_OVERSIZED, services do not fragment) while a smaller one to the same peer goes into
// the ring. The ring and the socket are two queues read in an order of the reader's choosing, so two datagrams sent
// one after the other on different paths can be taken in the other order. Only reachable when tt_MAX_BUFFER_LENGTH
// exceeds the slot (tt_SEGMENT_SLOT_BYTES = tt_CONTROL_MAX_LENGTH = 1472): the core default makes both 1472, so a
// service datagram always fits; rmw_tickle builds with 65507 (rosidl_typesupport_tickle_c), and there it does not.
// service_window_a.sh builds this with 65507, as rmw_tickle does.
//
// Two processes on one host. The child is one context with two clients, each of its own service and with its own
// request size, calling as fast as each call is answered (each client has at most one call outstanding, as core and
// rmw_tickle allow). Every request carries `g`, the child's send order across both clients; the parent is the server,
// answers each at once with a response of the request's size, and stamps every response with `r`, its send order.
//
// What is counted (the reading, service_window_a.sh, is pre-registered there and implemented in its summary):
//   cross   a request taken by the server after one sent LATER by the same context (g below the highest g taken so
//           far), and the same for responses at the client (r below the highest r taken). This is window A: two
//           streams of one context to one peer, delivered in an order other than they were sent.
//   stream  a request of one client taken after a later request of the SAME client (k below that client's highest
//           k), or a response to a call that is not the client's outstanding one. One outstanding call per client
//           makes this impossible by construction; it is counted so that "by construction" is checked.
// The arm prints how many datagrams of each path it actually sent (oversized_to_udp, tx_shm), so an arm that never
// took the two paths it is named for says so instead of reporting zero inversions about nothing.
//
// Usage: service_window_a <arm> <calls_per_client>   arm: mixed (3000 B + 64 B), shm (64 + 64), udp (3000 + 3000)
// Exit 0 ran (the verdict is the summary's), 3 setup failed.
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming) - for CPU affinity
#define _GNU_SOURCE
#include <inttypes.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/wait.h>
#include <tickle/config.h>
#include <tickle/hal.h>
#include <tickle/tickle.h>

#define SERVER_ID 71
#define CLIENT_ID 72
#define BROADCAST "127.255.255.255"
// One service per client. Until 2026-10-09 two clients of one service in one context sent identical requests (each
// counted its own seq_no from 0) and the server answered them as one; a call's seq_no now comes from the context
// (tt_Context.call_seq_no), and test_two_clients_one_service checks that shape. Two services are kept so the runs
// stay comparable with the ones published.
static const char* const service_names[] = {"window_a_0", "window_a_1"};
#define CLIENTS 2
#define LARGE_BYTES 3000U // past one slot (1472)
#define SMALL_BYTES 64U
#define HEADER_BYTES 24U                               // client, k, g/r, size: what every message carries first
#define SETUP_NS (10ULL * 1000ULL * 1000ULL * 1000ULL) // to the first answer: discovery and segment attach
#define RUN_NS (120ULL * 1000ULL * 1000ULL * 1000ULL)  // a backstop for a hang
#define REAP_TRIES 50
#define REAP_PAUSE_US 100000U
// Calls of each client before anything is counted. Until discovery and the segment attach, every datagram goes over
// UDP whatever its size (window B, RMW_GAPS S9), so the first calls take two paths in every arm, the controls included;
// the first run counted 3 cross inversions in each control from exactly that. Only client 0 calls until it has an
// answer, and inversions are counted between calls numbered above this on both sides.
#define WARMUP_K 50U
#define FILL_BYTE 0x5a                                     // the padding past the header
#define DRAIN_NS (200ULL * 1000ULL * 1000ULL)              // the server answers stragglers this long after the child
#define CHILD_WAIT_NS (5ULL * 1000ULL * 1000ULL * 1000ULL) // the child waits this long for the server to finish
#define NS_PER_MS 1000000ULL

enum run_exit { RUN_OK = 0, RUN_SETUP_FAILED = 3 };

struct message { // request and response alike
    uint32_t client;
    uint32_t k;     // this client's call number
    uint64_t order; // g (request: the child's send order) or r (response: the server's send order)
    uint32_t size;  // bytes on the wire for the payload, padding included
};

static int32_t message_size(const struct message* msg) {
    return (int32_t)msg->size;
}
static int32_t message_encode(const struct message* msg, uint8_t* payload, const uint32_t len) {
    if (len < msg->size || msg->size < HEADER_BYTES) {
        return -1;
    }
    memset(payload, FILL_BYTE, msg->size);
    memcpy(payload, &msg->client, 4);
    memcpy(payload + 4, &msg->k, 4);
    memcpy(payload + 8, &msg->order, 8);
    memcpy(payload + 16, &msg->size, 4);
    return (int32_t)msg->size;
}
static int32_t message_decode(struct message* msg, const uint8_t* payload, const uint32_t len, bool native) {
    (void)native; // one host, one byte order
    if (len < HEADER_BYTES) {
        return -1;
    }
    memcpy(&msg->client, payload, 4);
    memcpy(&msg->k, payload + 4, 4);
    memcpy(&msg->order, payload + 8, 8);
    memcpy(&msg->size, payload + 16, 4);
    return (int32_t)len;
}
static void message_free(struct message* msg) {
    (void)msg;
}

static struct tt_Service window_a_service = {
    .name = "WindowA",
    .request_size = sizeof(struct message),
    .response_size = sizeof(struct message),
    .request_encode_size = (tt_REQUEST_ENCODE_SIZE)message_size,
    .request_encode = (tt_REQUEST_ENCODE)message_encode,
    .request_decode = (tt_REQUEST_DECODE)message_decode,
    .request_free = (tt_REQUEST_FREE)message_free,
    .response_encode_size = (tt_RESPONSE_ENCODE_SIZE)message_size,
    .response_encode = (tt_RESPONSE_ENCODE)message_encode,
    .response_decode = (tt_RESPONSE_DECODE)message_decode,
    .response_free = (tt_RESPONSE_FREE)message_free,
};

// Shared between the two processes; each side writes only its own half.
struct report {
    // client side
    uint64_t calls, answered, timeouts, refused_calls;
    uint64_t resp_cross, resp_stream, resp_max_r;
    uint64_t client_oversized_to_udp, client_tx_shm, client_tx_udp, client_rx_shm, client_rx_udp;
    uint64_t measured_ns; // from the first answer to the last
    uint32_t child_done;
    // server side
    uint64_t requests, req_cross, req_stream, req_duplicate;
    uint64_t server_oversized_to_udp, server_tx_shm, server_tx_udp, server_rx_shm, server_rx_udp;
    uint32_t server_done;
};
static struct report* g_report;

static uint32_t g_sizes[CLIENTS];
static uint32_t g_calls_per_client;

// ---- the server (parent) ----

static uint64_t s_max_g;
static uint64_t s_r;
static uint32_t s_last_k[CLIENTS];
static bool s_seen[CLIENTS];

static int8_t serve(struct tt_Server* server, struct message* request, struct message* response,
                    tt_RequestId request_id) {
    (void)server;
    (void)request_id;
    struct report* rep = g_report;
    rep->requests++;
    if (request->k > WARMUP_K) {
        if (request->order < s_max_g) {
            rep->req_cross++;
        } else {
            s_max_g = request->order;
        }
    }
    if (request->client < CLIENTS) {
        uint32_t idx = request->client;
        if (s_seen[idx] && request->k < s_last_k[idx]) {
            rep->req_stream++;
        } else if (s_seen[idx] && request->k == s_last_k[idx]) {
            rep->req_duplicate++; // a retry the cache did not answer, or answered again
        } else {
            s_last_k[idx] = request->k;
            s_seen[idx] = true;
        }
    }
    response->client = request->client;
    response->k = request->k;
    response->order = ++s_r;
    response->size = request->size;
    return 0;
}

static void note_counters(const struct tt_Context* node, bool server) {
    uint64_t oversized = node->segment_oversized_to_udp;
    uint64_t tx_shm = node->tx_datagrams_by_transport[tt_TRANSPORT_SHM];
    uint64_t tx_udp = node->tx_datagrams_by_transport[tt_TRANSPORT_UDP];
    uint64_t rx_shm = node->rx_datagrams_by_transport[tt_TRANSPORT_SHM];
    uint64_t rx_udp = node->rx_datagrams_by_transport[tt_TRANSPORT_UDP];
    if (server) {
        g_report->server_oversized_to_udp = oversized;
        g_report->server_tx_shm = tx_shm;
        g_report->server_tx_udp = tx_udp;
        g_report->server_rx_shm = rx_shm;
        g_report->server_rx_udp = rx_udp;
    } else {
        g_report->client_oversized_to_udp = oversized;
        g_report->client_tx_shm = tx_shm;
        g_report->client_tx_udp = tx_udp;
        g_report->client_rx_shm = rx_shm;
        g_report->client_rx_udp = rx_udp;
    }
}

static struct tt_Context s_node;
static struct tt_Server s_servers[CLIENTS];

static int server_main(pid_t child) {
    _tt_CONFIG.broadcast = BROADCAST;
    _tt_CONFIG.context_id = SERVER_ID;
    bool created = tt_Context_create(&s_node) == tt_RET_OK;
    for (int i = 0; i < CLIENTS && created; i++) {
        created = tt_Context_create_server(&s_node, &s_servers[i], &window_a_service, service_names[i],
                                           (tt_SERVER_CALLBACK)serve) == tt_RET_OK;
    }
    if (!created) {
        (void)kill(child, SIGKILL);
        (void)waitpid(child, NULL, 0);
        return RUN_SETUP_FAILED;
    }
    uint64_t start = tt_get_ns();
    while (!__atomic_load_n(&g_report->child_done, __ATOMIC_ACQUIRE) && tt_get_ns() - start < RUN_NS) {
        (void)tt_Context_poll(&s_node, -1);
    }
    // Answer whatever is still in flight for a moment, so the child's last call is not left to time out.
    uint64_t drain_until = tt_get_ns() + DRAIN_NS;
    while (tt_get_ns() < drain_until) {
        (void)tt_Context_poll(&s_node, 0);
    }
    note_counters(&s_node, true);
    __atomic_store_n(&g_report->server_done, 1U, __ATOMIC_RELEASE);
    int status = 0;
    pid_t reaped = 0;
    for (int tries = 0; tries < REAP_TRIES && reaped == 0; tries++) {
        reaped = waitpid(child, &status, WNOHANG);
        if (reaped == 0) {
            (void)usleep(REAP_PAUSE_US);
        }
    }
    if (reaped == 0) {
        (void)kill(child, SIGKILL); // the pid is ours: it is reaped here and nowhere else
        (void)waitpid(child, &status, 0);
        printf("service_window_a: the child did not stop and was killed\n");
    }
    tt_Context_destroy(&s_node);
    return (reaped == child && WIFEXITED(status)) ? WEXITSTATUS(status) : RUN_SETUP_FAILED;
}

// ---- the clients (child) ----

static struct tt_Context c_node;
static struct tt_Client c_clients[CLIENTS];
static bool c_outstanding[CLIENTS];
static uint32_t c_k[CLIENTS];
static uint32_t c_answered[CLIENTS];
static uint64_t c_g;
static uint64_t c_first_answer_ns;

static int client_index(const struct tt_Client* client) {
    for (int i = 0; i < CLIENTS; i++) {
        if (client == &c_clients[i]) {
            return i;
        }
    }
    return -1;
}

static void answered(struct tt_Client* client, int8_t return_code, struct message* response) {
    int i = client_index(client);
    if (i < 0) {
        return;
    }
    c_outstanding[i] = false;
    if (return_code == tt_CALL_TIMEOUT || response == NULL) {
        g_report->timeouts++;
        return;
    }
    if (c_first_answer_ns == 0) {
        c_first_answer_ns = tt_get_ns();
    }
    g_report->answered++;
    c_answered[i]++;
    if (response->k > WARMUP_K) {
        if (response->order < g_report->resp_max_r) {
            g_report->resp_cross++;
        } else {
            g_report->resp_max_r = response->order;
        }
    }
    if (response->client != (uint32_t)i || response->k != c_k[i]) {
        g_report->resp_stream++; // an answer to a call that is not this client's outstanding one
    }
}

static void call_if_idle(int i) {
    if (c_outstanding[i] || c_k[i] >= g_calls_per_client) {
        return;
    }
    struct message request = {.client = (uint32_t)i, .k = c_k[i] + 1U, .order = c_g + 1U, .size = g_sizes[i]};
    tt_ret_t ret = tt_Client_call(&c_clients[i], (struct tt_Request*)&request);
    if (ret == tt_RET_OK) {
        c_k[i]++;
        c_g++;
        c_outstanding[i] = true;
        g_report->calls++;
    } else {
        g_report->refused_calls++;
    }
}

static int client_main(void) {
    _tt_CONFIG.broadcast = BROADCAST;
    _tt_CONFIG.context_id = CLIENT_ID;
    if (tt_Context_create(&c_node) != tt_RET_OK) {
        return RUN_SETUP_FAILED;
    }
    for (int i = 0; i < CLIENTS; i++) {
        if (tt_Context_create_client(&c_node, &c_clients[i], &window_a_service, service_names[i],
                                     (tt_CLIENT_CALLBACK)answered) != tt_RET_OK) {
            return RUN_SETUP_FAILED;
        }
    }
    uint64_t start = tt_get_ns();
    for (;;) {
        uint64_t now = tt_get_ns();
        if (now - start > RUN_NS || (c_first_answer_ns == 0 && now - start > SETUP_NS)) {
            break;
        }
        bool all_done = true;
        for (int i = 0; i < CLIENTS; i++) {
            if (i == 0 || c_first_answer_ns != 0) {
                call_if_idle(i);
            }
            all_done = all_done && c_k[i] >= g_calls_per_client && !c_outstanding[i];
        }
        if (all_done) {
            break;
        }
        (void)tt_Context_poll(&c_node, 0); // busy: each answer is followed by the next call at once
    }
    g_report->measured_ns = c_first_answer_ns != 0 ? tt_get_ns() - c_first_answer_ns : 0;
    note_counters(&c_node, false);
    __atomic_store_n(&g_report->child_done, 1U, __ATOMIC_RELEASE);
    uint64_t wait_start = tt_get_ns();
    while (!__atomic_load_n(&g_report->server_done, __ATOMIC_ACQUIRE) && tt_get_ns() - wait_start < CHILD_WAIT_NS) {
        (void)tt_Context_poll(&c_node, 0);
    }
    tt_Context_destroy(&c_node);
    return c_first_answer_ns == 0 ? RUN_SETUP_FAILED : RUN_OK;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s mixed|shm|udp calls_per_client\n", argv[0]);
        return RUN_SETUP_FAILED;
    }
    const char* arm = argv[1];
    if (strcmp(arm, "mixed") == 0) {
        g_sizes[0] = LARGE_BYTES;
        g_sizes[1] = SMALL_BYTES;
    } else if (strcmp(arm, "shm") == 0) {
        g_sizes[0] = SMALL_BYTES;
        g_sizes[1] = SMALL_BYTES;
    } else if (strcmp(arm, "udp") == 0) {
        g_sizes[0] = LARGE_BYTES;
        g_sizes[1] = LARGE_BYTES;
    } else {
        fprintf(stderr, "unknown arm %s\n", arm);
        return RUN_SETUP_FAILED;
    }
    g_calls_per_client = (uint32_t)strtoul(argv[2], NULL, 10);
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_report = mmap(NULL, sizeof *g_report, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_report == MAP_FAILED) {
        perror("mmap");
        return RUN_SETUP_FAILED;
    }
    memset(g_report, 0, sizeof *g_report);
    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return RUN_SETUP_FAILED;
    }
    if (child == 0) {
        _exit(client_main());
    }
    int result = server_main(child);
    const struct report* r = g_report;
    printf("RESULT arm=%s sizes=%u,%u max_buffer=%u slot=%u calls=%" PRIu64 " answered=%" PRIu64 " timeouts=%" PRIu64
           " refused_calls=%" PRIu64 " requests=%" PRIu64 " req_cross=%" PRIu64 " req_stream=%" PRIu64
           " req_duplicate=%" PRIu64 " resp_cross=%" PRIu64 " resp_stream=%" PRIu64 " client_oversized_to_udp=%" PRIu64
           " client_tx_shm=%" PRIu64 " client_tx_udp=%" PRIu64 " server_oversized_to_udp=%" PRIu64
           " server_tx_shm=%" PRIu64 " server_tx_udp=%" PRIu64 " server_rx_shm=%" PRIu64 " server_rx_udp=%" PRIu64
           " client_rx_shm=%" PRIu64 " client_rx_udp=%" PRIu64 " measured_ms=%" PRIu64 " exit=%d\n",
           arm, g_sizes[0], g_sizes[1], (unsigned)tt_MAX_BUFFER_LENGTH, (unsigned)tt_SEGMENT_SLOT_BYTES, r->calls,
           r->answered, r->timeouts, r->refused_calls, r->requests, r->req_cross, r->req_stream, r->req_duplicate,
           r->resp_cross, r->resp_stream, r->client_oversized_to_udp, r->client_tx_shm, r->client_tx_udp,
           r->server_oversized_to_udp, r->server_tx_shm, r->server_tx_udp, r->server_rx_shm, r->server_rx_udp,
           r->client_rx_shm, r->client_rx_udp, (uint64_t)(r->measured_ns / NS_PER_MS), result);
    return result;
}
