/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

#include <netdb.h>   // getaddrinfo() - a static peer given by name (g6)
#include <pthread.h> // NOLINT(misc-include-cleaner) - see rmw_tickle.h's own <pthread.h> comment
#include <stdatomic.h>
#include <stdint.h> // uint32_t - the trace dump below, with -DRMW_TICKLE_TRACE=ON
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>  // inet_ntop()
#include <netinet/in.h> // struct sockaddr_in
#include <sys/socket.h> // AF_INET
#include <tickle/config.h>
#include <tickle/tickle.h> // struct tt_LockStats
#ifdef tt_TRACE
#include <tickle/trace.h> // tt_trace_read(), with -DRMW_TICKLE_TRACE=ON
#endif

#include "rcutils/allocator.h"
#include "rcutils/error_handling.h"
#include "rcutils/logging_macros.h"
#include "rcutils/macros.h" // RCUTILS_STRINGIFY
#include "rcutils/strdup.h"
#include "rmw/discovery_options.h" // rmw_discovery_options_t, RMW_AUTOMATIC_DISCOVERY_RANGE_* (g6)
#include "rmw/domain_id.h"         // RMW_DEFAULT_DOMAIN_ID
#include "rmw/error_handling.h"
#include "rmw/init.h"             // rmw_context_t, rmw_context_impl_t
#include "rmw/init_options.h"     // rmw_init_options_t
#include "rmw/ret_types.h"        // rmw_ret_t, RMW_RET_*
#include "rmw/rmw.h"              // rmw_init/_shutdown/_context_fini, rmw_get_implementation_identifier, ...
#include "rmw/security_options.h" // rmw_security_options_t
#include "rmw_tickle_c/rmw_tickle.h"

static void configure_discovery(rmw_tickle_context_impl_t* impl, const rmw_discovery_options_t* discovery);

const char* const rmw_tickle_identifier = RMW_TICKLE_IDENTIFIER;
const char* const rmw_tickle_serialization_format = RMW_TICKLE_SERIALIZATION_FORMAT;

const char* rmw_get_implementation_identifier(void) {
    return RMW_TICKLE_IDENTIFIER;
}

const char* rmw_get_serialization_format(void) {
    return RMW_TICKLE_SERIALIZATION_FORMAT;
}

rmw_init_options_t rmw_get_zero_initialized_init_options(void) {
    rmw_init_options_t init_options;
    memset(&init_options, 0, sizeof(init_options));
    return init_options;
}

// Missing from the #20-era scaffold, same as rmw_get_serialization_format() was (Milestone 2) -
// rmw_init()'s own real contract (rmw/init.h) requires the caller to have zero-initialized
// `context` this way first; never actually exercised until Milestone 10's own rmw_init()-calling
// tests surfaced the gap.
rmw_context_t rmw_get_zero_initialized_context(void) {
    rmw_context_t context;
    memset(&context, 0, sizeof(context));
    return context;
}

rmw_ret_t rmw_init_options_init(rmw_init_options_t* const init_options, rcutils_allocator_t allocator) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(init_options, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(allocator.allocate, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(allocator.deallocate, RMW_RET_INVALID_ARGUMENT);
    // Initializing an already-initialized init_options (implementation_identifier already set by
    // a prior rmw_init_options_init() call that was never rmw_init_options_fini()'d) is a real
    // caller mistake rmw's own contract rejects (test_rmw_implementation's own test_init_options.
    // cpp's "Initializing twice fails"), not something to silently re-stamp over.
    if (NULL != init_options->implementation_identifier) {
        RMW_SET_ERROR_MSG("init_options is already initialized");
        return RMW_RET_INVALID_ARGUMENT;
    }

    init_options->instance_id = 0;
    // RMW_DEFAULT_DOMAIN_ID, not 0: it means "nobody has chosen one", which is what lets rcl fill
    // in ROS_DOMAIN_ID. Setting 0 here reads to rcl as a deliberate choice of domain 0, so a node
    // whose stack does not set the domain itself lands there whatever ROS_DOMAIN_ID says - and one
    // whose stack does (rclpy) lands on the right one, so the two silently stop hearing each other.
    // Found 2026-09-28 through the bag acceptance: its rclpy talker opened port 8373 (domain 91)
    // while `ros2 bag record`, driven by rclcpp, opened 8282 (domain 0) and recorded nothing.
    init_options->domain_id = RMW_DEFAULT_DOMAIN_ID;
    // Initialize security options to zero
    memset(&init_options->security_options, 0, sizeof(rmw_security_options_t));
    init_options->enclave = NULL;
    init_options->allocator = allocator;
    init_options->impl = NULL;
    // (g6) As the DDS vendors do: rmw's own defaults, LOCALHOST and no static peers. rcl then sets what
    // ROS_AUTOMATIC_DISCOVERY_RANGE says (SUBNET when unset). Left zeroed until g6, which read as NOT_SET.
    init_options->discovery_options = rmw_get_zero_initialized_discovery_options();
    rmw_ret_t discovery_ret = rmw_discovery_options_init(&init_options->discovery_options, 0, &allocator);
    if (RMW_RET_OK != discovery_ret) {
        return discovery_ret;
    }

    // Set the implementation identifier
    init_options->implementation_identifier = RMW_TICKLE_IDENTIFIER;

    return RMW_RET_OK;
}

rmw_ret_t rmw_init_options_fini(rmw_init_options_t* init_options) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(init_options, RMW_RET_INVALID_ARGUMENT);
    rcutils_allocator_t* allocator = &init_options->allocator;
    RCUTILS_CHECK_ALLOCATOR(allocator, return RMW_RET_INVALID_ARGUMENT);
    // implementation_identifier is NULL both on a zero-initialized (never rmw_init_options_init()'d)
    // init_options, and - now that this function itself clears it below - on an already-finalized
    // one (test_rmw_implementation's own test_init_options.cpp's "Finalizing twice fails"). Either
    // way that's a plain invalid argument, not "wrong rmw" (which needs a real, non-null, just
    // incorrect identifier to mean anything) - and not something a bare strcmp() can safely be
    // handed regardless.
    if (NULL == init_options->implementation_identifier) {
        RMW_SET_ERROR_MSG("init_options has already been finalized, or was never initialized");
        return RMW_RET_INVALID_ARGUMENT;
    }
    if (strcmp(init_options->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Implementation identifiers does not match");
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    // rmw_init_options_copy() below is the only place that ever heap-allocates enclave (via
    // rcutils_strdup()) - free it here so a copied (not just a directly-init()'d) init_options
    // doesn't leak it.
    if (init_options->enclave != NULL) {
        allocator->deallocate(init_options->enclave, allocator->state);
        init_options->enclave = NULL;
    }
    rmw_ret_t discovery_ret = rmw_discovery_options_fini(&init_options->discovery_options); // (g6)
    if (RMW_RET_OK != discovery_ret) {
        return discovery_ret;
    }
    init_options->implementation_identifier = NULL;
    return RMW_RET_OK;
}

rmw_ret_t rmw_init_options_copy(const rmw_init_options_t* src, rmw_init_options_t* const dst) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(src, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(dst, RMW_RET_INVALID_ARGUMENT);

    // src->implementation_identifier being NULL means src itself was never initialized - a plain
    // invalid argument (test_rmw_implementation's test_init_options.cpp's copy_with_bad_arguments
    // exercises exactly this, distinct from a real-but-wrong identifier below) - and, same as
    // everywhere else in this file, not safe to hand a bare strcmp() regardless.
    if (NULL == src->implementation_identifier) {
        RMW_SET_ERROR_MSG("src has not been initialized");
        return RMW_RET_INVALID_ARGUMENT;
    }
    if (strcmp(src->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Implementation identifiers does not match");
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }

    if (NULL != dst->implementation_identifier) {
        RMW_SET_ERROR_MSG("expected zero-initialized dst");
        return RMW_RET_INVALID_ARGUMENT;
    }

    // Copy the basic structure
    memcpy(dst, src, sizeof(rmw_init_options_t));
    // (g6) Its own static peers, not src's: fini frees each copy's.
    dst->discovery_options = rmw_get_zero_initialized_discovery_options();
    rcutils_allocator_t discovery_allocator = src->allocator;
    rmw_ret_t discovery_ret =
        rmw_discovery_options_copy(&src->discovery_options, &discovery_allocator, &dst->discovery_options);
    if (RMW_RET_OK != discovery_ret) {
        dst->implementation_identifier = NULL;
        return discovery_ret;
    }

    // Copy the enclave string if it exists
    if (src->enclave != NULL) {
        dst->enclave = rcutils_strdup(src->enclave, src->allocator);
        if (NULL == dst->enclave) {
            return RMW_RET_BAD_ALLOC;
        }
    } else {
        dst->enclave = NULL;
    }

    return RMW_RET_OK;
}

rmw_ret_t rmw_init(const rmw_init_options_t* options, rmw_context_t* const context) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(options, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);

    // options->implementation_identifier is NULL if `options` was never passed through
    // rmw_init_options_init() (still zero-initialized) - rmw's own contract (test_rmw_
    // implementation's test_init_shutdown.cpp) treats that as a plain invalid argument, distinct
    // from a non-null identifier that just names the wrong rmw (INCORRECT_RMW_IMPLEMENTATION).
    if (NULL == options->implementation_identifier) {
        RMW_SET_ERROR_MSG("options has not been initialized (implementation_identifier is null)");
        return RMW_RET_INVALID_ARGUMENT;
    }
    if (strcmp(options->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    if (NULL == options->enclave) {
        RMW_SET_ERROR_MSG("options->enclave is null");
        return RMW_RET_INVALID_ARGUMENT;
    }
    if (NULL != context->impl) {
        RMW_SET_ERROR_MSG("context has already been initialized");
        return RMW_RET_INVALID_ARGUMENT;
    }
    // (g7, CONTEXT_NODE_PLAN.md roadmap, 2026-09-27) rmw_tickle implements no ROS 2 security: nothing is
    // authenticated, access-controlled or encrypted. It used to ignore security_options, so with
    // ROS_SECURITY_ENFORCEMENT=Enforce it started unsecured without a word. Enforce now refuses to start, as the
    // DDS vendors refuse when they cannot apply security; security enabled but permissive starts, and says once
    // that it is not applied.
    if (RMW_SECURITY_ENFORCEMENT_ENFORCE == options->security_options.enforce_security) {
        RMW_SET_ERROR_MSG("rmw_tickle does not implement ROS 2 security (SROS2), and ROS_SECURITY_ENFORCEMENT=Enforce "
                          "requires it - refusing to start unsecured");
        return RMW_RET_ERROR;
    }
    if (NULL != options->security_options.security_root_path) {
        static atomic_bool security_notice_logged = false;
        if (!atomic_exchange(&security_notice_logged, true)) {
            RCUTILS_LOG_WARN_NAMED("rmw_tickle",
                                   "ROS 2 security is enabled (keystore %s) but rmw_tickle does not implement it: "
                                   "this process runs unauthenticated and unencrypted",
                                   options->security_options.security_root_path);
        }
    }
    // ROS_DOMAIN_ID (2026-09-27, DDS parity): each domain listens on its own well-known port, _tt_CONTEXT_PORT +
    // the domain id, so two domains on one network never discover each other - as DDS keeps domains apart
    // by port. Until then every rmw_tickle on a network was in one domain, and a PC's unit tests (domain 0)
    // were heard by the rig's nodes (domain 73) over a shared management LAN. rcl passes the domain it
    // resolved from ROS_DOMAIN_ID; RMW_DEFAULT_DOMAIN_ID (a direct rmw caller) reads the variable itself.
    size_t domain_id = options->domain_id;
    if (RMW_DEFAULT_DOMAIN_ID == domain_id) {
        const char* from_env = getenv("ROS_DOMAIN_ID");
        domain_id = NULL != from_env && '\0' != from_env[0] ? strtoul(from_env, NULL, 10) : 0;
    }
    if (domain_id > RMW_TICKLE_MAX_DOMAIN_ID) {
        RMW_SET_ERROR_MSG("domain id above " RCUTILS_STRINGIFY(RMW_TICKLE_MAX_DOMAIN_ID) " (the DDS limit)");
        return RMW_RET_INVALID_ARGUMENT;
    }

    context->instance_id = 0;
    context->implementation_identifier = RMW_TICKLE_IDENTIFIER;
    context->options = *options;

    // Allocate and initialize context implementation
    rmw_tickle_context_impl_t* impl = (rmw_tickle_context_impl_t*)options->allocator.allocate(
        sizeof(rmw_tickle_context_impl_t), options->allocator.state);
    if (impl == NULL) {
        RMW_SET_ERROR_MSG("Failed to allocate context implementation");
        return RMW_RET_BAD_ALLOC;
    }

    // Initialize the context implementation
    memset(impl, 0, sizeof(rmw_tickle_context_impl_t));
    impl->allocator = options->allocator;

    if (pthread_mutex_init(&impl->wait_mutex, NULL) != 0) {
        RMW_SET_ERROR_MSG("failed to initialize context wait_mutex");
        options->allocator.deallocate(impl, options->allocator.state);
        return RMW_RET_ERROR;
    }
    if (pthread_cond_init(&impl->wait_cond, NULL) != 0) {
        RMW_SET_ERROR_MSG("failed to initialize context wait_cond");
        pthread_mutex_destroy(&impl->wait_mutex);
        options->allocator.deallocate(impl, options->allocator.state);
        return RMW_RET_ERROR;
    }
    // Milestone 34 - guards node_count/nodes[] (rmw_tickle_context_impl_t's own doc comment,
    // rmw_tickle.h) - a separate, dedicated mutex from wait_mutex above (a different lock-nesting
    // contract, guarding a different concern, not worth conflating just because both happen to be
    // context-level).
    if (pthread_cond_init(&impl->handover_cond, NULL) != 0) {
        RMW_SET_ERROR_MSG("failed to initialize context handover_cond");
        pthread_cond_destroy(&impl->wait_cond);
        pthread_mutex_destroy(&impl->wait_mutex);
        options->allocator.deallocate(impl, options->allocator.state);
        return RMW_RET_ERROR;
    }
    // Executor-driven receive, on by default since its rig A/B passed (RMW_PERF_PLAN.md 8.4, 2026-09-27: block
    // RTT -7 to -13 us, pong CPU not up); RMW_TICKLE_EXECUTOR_POLL=0 turns it off, for A/B runs.
    const char* executor_poll = getenv("RMW_TICKLE_EXECUTOR_POLL");
    impl->executor_poll_enabled = NULL == executor_poll || '0' != executor_poll[0];
    if (pthread_mutex_init(&impl->registry_mutex, NULL) != 0) {
        RMW_SET_ERROR_MSG("failed to initialize context registry_mutex");
        pthread_cond_destroy(&impl->handover_cond);
        pthread_cond_destroy(&impl->wait_cond);
        pthread_mutex_destroy(&impl->wait_mutex);
        options->allocator.deallocate(impl, options->allocator.state);
        return RMW_RET_ERROR;
    }

    // rmw_tickle/PLAN.md's Milestone 5: safely waitable from the start (rmw_wait() never crashes
    // seeing it in a wait set); actually triggered on a real graph change is Milestone 6, not yet
    // wired up - see rmw_tickle_context_impl_t's own doc comment.
    impl->graph_guard_condition.rmw_guard_condition.implementation_identifier = RMW_TICKLE_IDENTIFIER;
    impl->graph_guard_condition.rmw_guard_condition.data = &impl->graph_guard_condition;
    impl->graph_guard_condition.rmw_guard_condition.context = context;
    impl->graph_guard_condition.context_impl = impl;
    atomic_init(&impl->graph_guard_condition.has_triggered, false);
    impl->graph_guard_condition.allocator = options->allocator;

    context->impl = (rmw_context_impl_t*)impl;

    // _tt_CONFIG is one process-wide TickLE HAL config (include/tickle/config.h), not something
    // rmw_init_options_t carries a slot for - a hardcoded subnet here would silently break every
    // ROS 2 graph not on that exact network. TICKLE_BROADCAST_ADDR lets a deployment override the
    // HAL's own compiled-in default (see src/hal_linux.c / src/hal_freertos.c) the same way
    // examples/linux's drivers let `-b` do it for a CLI-driven process; unset, this leaves that
    // default untouched instead of guessing.
    _tt_CONFIG.port = _tt_CONTEXT_PORT + (int)domain_id; // see the domain check at the top

    char* broadcast_addr = getenv("TICKLE_BROADCAST_ADDR");
    if (broadcast_addr != NULL) {
        _tt_CONFIG.broadcast = broadcast_addr;
    }

    // Same reasoning as TICKLE_BROADCAST_ADDR just above, for _tt_CONFIG.context_id (config.h's own
    // doc comment on that field spells out exactly this scenario): tt_get_node_id()'s auto-detect
    // derives a node's id from the last octet of its own address on the broadcast subnet, which
    // silently collides whenever two rmw_tickle processes share one host/interface (e.g.
    // buildfarm_perf_tests' own "two_process" test topology, both sides on the same CI runner) -
    // each then discards the other's every packet as "self sent" (process_datagram(), tickle.c).
    // Real separate hosts need no override at all; examples/linux/*'s own standalone binaries
    // already expose this as a `-I` CLI flag for exactly the same reason - an rmw plugin has no
    // CLI of its own to extend, so this is the equivalent for anything that launches multiple
    // rmw_tickle nodes on one host (e.g. rmw-perf.yml's own launch template).
    char* node_id = getenv("TICKLE_NODE_ID");
    if (node_id != NULL) {
        _tt_CONFIG.context_id = atoi(node_id);
    }

    configure_discovery(impl, &options->discovery_options);
    return RMW_RET_OK;
}

// ---- (g6, RMW_GAPS_PLAN.md) ROS_AUTOMATIC_DISCOVERY_RANGE and ROS_STATIC_PEERS. Our own implementation of the
// behaviour rmw_cyclonedds shows (read and probed 2026-09-28): an unset range refuses node creation; SUBNET is the
// links plus the static peers; SYSTEM_DEFAULT the links alone; LOCALHOST loopback plus the static peers, others
// dropped; OFF nothing at all. _tt_CONFIG is one per process, so the last rmw_init's options are the ones every
// context uses (rmw_cyclonedds refuses a second domain with different ones; a process here that mixes them gets the
// last).

#define PEER_TEXT_LENGTH 64
static char peer_text[tt_MAX_LINK_COUNT][PEER_TEXT_LENGTH]; // what _tt_CONFIG.links[i].broadcast points at

// One static peer as core's peer link wants it: "a.b.c.d" or "a.b.c.d/nn". A hostname is looked up now, IPv4 only.
static bool peer_as_ipv4(const char* peer, char* out, size_t size) {
    struct in_addr literal;
    const char* slash = strchr(peer, '/');
    if (NULL != strchr(peer, ':')) {
        return false; // IPv6: TickLE is IPv4
    }
    if (NULL != slash) { // a subnet: its address must be literal
        char address[PEER_TEXT_LENGTH];
        size_t length = (size_t)(slash - peer);
        if (length == 0 || length >= sizeof(address)) {
            return false;
        }
        memcpy(address, peer, length);
        address[length] = '\0';
        return inet_pton(AF_INET, address, &literal) == 1 && (size_t)snprintf(out, size, "%s", peer) < size;
    }
    if (inet_pton(AF_INET, peer, &literal) == 1) {
        return (size_t)snprintf(out, size, "%s", peer) < size;
    }
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    struct addrinfo* found = NULL;
    if (getaddrinfo(peer, NULL, &hints, &found) != 0 || NULL == found) {
        return false;
    }
    bool converted = NULL != inet_ntop(AF_INET, &((struct sockaddr_in*)found->ai_addr)->sin_addr, out, (socklen_t)size);
    freeaddrinfo(found);
    return converted;
}

// The static peers as links 1.., after link 0 - the one the scalar addr/broadcast settings describe.
static void configure_static_peers(const rmw_discovery_options_t* discovery) {
    _tt_CONFIG.link_count = 0;
    if (0 == discovery->static_peers_count) {
        return; // no link table: core builds link 0 from the scalar settings, as always
    }
    _tt_CONFIG.links[0].broadcast = _tt_CONFIG.broadcast;
    _tt_CONFIG.links[0].addr = _tt_CONFIG.addr;
    _tt_CONFIG.links[0].unicast_threshold = tt_UNICAST_PEER_THRESHOLD;
    _tt_CONFIG.links[0].peer = false;
    uint8_t count = 1;
    for (size_t i = 0; i < discovery->static_peers_count; i++) {
        const char* peer = discovery->static_peers[i].peer_address;
        if (count >= tt_MAX_LINK_COUNT) {
            RCUTILS_LOG_WARN_NAMED("rmw_tickle", "static peer %s ignored: rmw_tickle holds %d static peers at most",
                                   peer, tt_MAX_LINK_COUNT - 1);
            continue;
        }
        if (!peer_as_ipv4(peer, peer_text[count], sizeof(peer_text[count]))) {
            RCUTILS_LOG_WARN_NAMED("rmw_tickle",
                                   "static peer %s ignored: not an IPv4 address, subnet or IPv4 host name", peer);
            continue;
        }
        _tt_CONFIG.links[count].broadcast = peer_text[count];
        _tt_CONFIG.links[count].addr = NULL;
        _tt_CONFIG.links[count].unicast_threshold = tt_UNICAST_PEER_THRESHOLD;
        _tt_CONFIG.links[count].peer = true;
        count++;
    }
    _tt_CONFIG.link_count = count > 1 ? count : 0;
}

static void ignore_static_peers(const rmw_discovery_options_t* discovery, const char* why) {
    if (discovery->static_peers_count > 0) {
        RCUTILS_LOG_WARN_NAMED("rmw_tickle", "%zu static peers were given, but discovery is %s, so they are ignored",
                               discovery->static_peers_count, why);
    }
}

static void configure_discovery(rmw_tickle_context_impl_t* impl, const rmw_discovery_options_t* discovery) {
    impl->discovery_range_unset = false;
    _tt_CONFIG.discovery_range = tt_DISCOVERY_RANGE_SUBNET;
    _tt_CONFIG.link_count = 0;
    switch (discovery->automatic_discovery_range) {
    case RMW_AUTOMATIC_DISCOVERY_RANGE_NOT_SET:
        impl->discovery_range_unset = true; // rmw_create_node() refuses, as rmw_cyclonedds does (observed 2026-09-28)
        break;
    case RMW_AUTOMATIC_DISCOVERY_RANGE_SUBNET:
        configure_static_peers(discovery);
        break;
    case RMW_AUTOMATIC_DISCOVERY_RANGE_SYSTEM_DEFAULT:
        ignore_static_peers(discovery, "SYSTEM_DEFAULT");
        break;
    case RMW_AUTOMATIC_DISCOVERY_RANGE_LOCALHOST:
        if (NULL != getenv("TICKLE_BROADCAST_ADDR")) {
            RCUTILS_LOG_WARN_NAMED("rmw_tickle",
                                   "ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST: TICKLE_BROADCAST_ADDR=%s is "
                                   "overridden, as nothing may leave this host",
                                   getenv("TICKLE_BROADCAST_ADDR"));
        }
        _tt_CONFIG.discovery_range = tt_DISCOVERY_RANGE_LOCALHOST;
        _tt_CONFIG.broadcast = "127.255.255.255";
        // Bound to loopback, unless a static peer must be able to reach this context's data socket.
        _tt_CONFIG.addr = discovery->static_peers_count > 0 ? "0.0.0.0" : "127.0.0.1";
        configure_static_peers(discovery);
        break;
    case RMW_AUTOMATIC_DISCOVERY_RANGE_OFF:
        ignore_static_peers(discovery, "OFF");
        _tt_CONFIG.discovery_range = tt_DISCOVERY_RANGE_OFF;
        _tt_CONFIG.broadcast = "127.255.255.255";
        _tt_CONFIG.addr = "127.0.0.1";
        RCUTILS_LOG_INFO_NAMED("rmw_tickle", "ROS_AUTOMATIC_DISCOVERY_RANGE=OFF: this process sends nothing and hears "
                                             "nothing; its own nodes still talk to each other");
        break;
    default:
        break;
    }
}

// RMW_TICKLE_TRACE_FILE=<path> (2026-09-26, rmw_tickle/RMW_PERF_PLAN.md H1/H2): at the first shutdown,
// write this context's node state-lock counters to <path> - who waited for the lock, and how long - and,
// in a build with -DRMW_TICKLE_TRACE=ON, every latency stamp still in core's ring (tickle/trace.h).
// Measurement only: unset, it does nothing, and a file that cannot be opened is skipped silently.
//
// Format, one record a line:
//   lock acquisitions=A contended=C wait_ns=W poller_contended=PC poller_wait_ns=PW
//   points 1=rx_wake 2=rx_datagram 3=deliver 4=signaled 5=exec_wake 6=taken 7=publish 8=tx_done
//   stamp <ns> <thread> <point>      (oldest first; ns is CLOCK_MONOTONIC)
// The counters are read while the poll thread may still run, so they can be one update stale.
static void dump_measurements(rmw_tickle_context_impl_t* impl) {
    const char* path = getenv("RMW_TICKLE_TRACE_FILE");
    if (NULL == path || '\0' == path[0]) {
        return;
    }
    FILE* out = fopen(path, "w");
    if (NULL == out) {
        return;
    }
    const struct tt_LockStats* lock = &impl->tickle_context.state_lock_stats;
    fprintf(out, "lock acquisitions=%llu contended=%llu wait_ns=%llu poller_contended=%llu poller_wait_ns=%llu\n",
            (unsigned long long)lock->acquisitions, (unsigned long long)lock->contended,
            (unsigned long long)lock->wait_ns, (unsigned long long)lock->poller_contended,
            (unsigned long long)lock->poller_wait_ns);
    fprintf(out, "points 1=rx_wake 2=rx_datagram 3=deliver 4=signaled 5=exec_wake 6=taken 7=publish 8=tx_done\n");
#ifdef tt_TRACE
    static struct tt_TraceStamp stamps[tt_TRACE_CAPACITY];
    uint32_t count = tt_trace_read(stamps, tt_TRACE_CAPACITY);
    for (uint32_t i = 0; i < count; i++) {
        fprintf(out, "stamp %llu %llu %u\n", (unsigned long long)stamps[i].ns, (unsigned long long)stamps[i].thread,
                stamps[i].point);
    }
#endif
    fclose(out);
}

rmw_ret_t rmw_shutdown(rmw_context_t* context) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);

    // context->implementation_identifier is NULL on a zero-initialized (never rmw_init()'d)
    // context - see rmw_init()'s own comment on the same NULL-vs-wrong-identifier distinction.
    if (NULL == context->implementation_identifier) {
        RMW_SET_ERROR_MSG("context has not been initialized");
        return RMW_RET_INVALID_ARGUMENT;
    }
    if (strcmp(context->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    // NULL here means either never-initialized-past-the-identifier-check (shouldn't happen - the
    // identifier is only ever set together with impl in rmw_init()) or already-finalized
    // (rmw_context_fini() nulls it) - either way, nothing left to shut down.
    if (NULL == context->impl) {
        RMW_SET_ERROR_MSG("context has already been finalized");
        return RMW_RET_INVALID_ARGUMENT;
    }

    // Idempotent by rmw's own contract (test_init_shutdown.cpp's "Shutdown twice should succeed")
    // - just (re-)mark it, no different work needed the second time.
    rmw_tickle_context_impl_t* impl = (rmw_tickle_context_impl_t*)context->impl;
    if (!impl->shutdown) {
        dump_measurements(impl);
    }
    impl->shutdown = true;

    return RMW_RET_OK;
}

rmw_ret_t rmw_context_fini(rmw_context_t* const context) {
    RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);

    // See rmw_init()'s own comment on the same NULL-vs-wrong-identifier distinction -
    // context->implementation_identifier is NULL on a zero-initialized (never rmw_init()'d)
    // context.
    if (NULL == context->implementation_identifier) {
        RMW_SET_ERROR_MSG("context has not been initialized");
        return RMW_RET_INVALID_ARGUMENT;
    }
    if (strcmp(context->implementation_identifier, RMW_TICKLE_IDENTIFIER) != 0) {
        RMW_SET_ERROR_MSG("Expected implementation identifier to be " RMW_TICKLE_IDENTIFIER);
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    // NULL here means already finalized (this function itself is what nulls it, below) - rmw's
    // own contract requires a second finalization to fail, not silently double-free/no-op
    // (test_init_shutdown.cpp's "Finalization twice should fail").
    if (NULL == context->impl) {
        RMW_SET_ERROR_MSG("context has already been finalized");
        return RMW_RET_INVALID_ARGUMENT;
    }
    // rmw's own contract: shutdown must precede finalization (test_init_shutdown.cpp's
    // "Finalization w/o shutdown should fail") - shutdown itself doesn't null context->impl (it's
    // idempotent and this function is still the one that owns freeing it), so this needs its own
    // flag rather than reusing the impl-null check above.
    if (!((rmw_tickle_context_impl_t*)context->impl)->shutdown) {
        RMW_SET_ERROR_MSG("context must be shut down (rmw_shutdown()) before finalization");
        return RMW_RET_INVALID_ARGUMENT;
    }

    // Free the context implementation. nodes[] itself (Milestone 34) is expected to already be
    // empty/NULL here - the real rmw/rcl lifecycle contract requires every node (and every
    // publisher/subscription/etc. on it) to be destroyed before its own context is finalized, the
    // same pre-existing assumption every other per-entity teardown in this package already relies
    // on - so this just frees whatever allocation nodes[] itself still holds (the array, not its
    // now-empty contents), not a defensive walk-and-destroy of anything still registered.
    rmw_tickle_context_impl_t* impl = (rmw_tickle_context_impl_t*)context->impl;
    int leaked = rmw_tickle_stop_leaked_nodes(impl);
    if (leaked > 0) {
        RCUTILS_LOG_WARN_NAMED("rmw_tickle",
                               "context finalized with %d node(s) never destroyed; stopped the TickLE node they "
                               "shared so nothing runs on a freed context",
                               leaked);
    }
    if (impl->nodes != NULL) {
        impl->allocator.deallocate((void*)impl->nodes, impl->allocator.state);
    }
    pthread_mutex_destroy(&impl->registry_mutex);
    pthread_cond_destroy(&impl->handover_cond);
    pthread_cond_destroy(&impl->wait_cond);
    pthread_mutex_destroy(&impl->wait_mutex);
    context->options.allocator.deallocate(context->impl, context->options.allocator.state);
    context->impl = NULL;

    // context->options is only ever a shallow copy (`context->options = *options;`, rmw_init()
    // above) of whatever the caller's own rmw_init_options_t still is - it does NOT own enclave
    // (or anything else heap-allocated in it), so finalizing it here is not this function's job at
    // all: the caller who originally rmw_init_options_init()'d that struct is the one who must
    // rmw_init_options_fini() it, separately, whenever *they're* done with it (which may be well
    // after this context itself is finalized - test_rmw_implementation's own test_init_shutdown.cpp
    // does exactly that in its fixture's TearDown()). This function calling rmw_init_options_fini()
    // on its own borrowed copy used to look harmless only because that function was a no-op past
    // its own identifier check - once it started actually freeing enclave, doing so here as well
    // produced a real double-free the moment a caller (correctly) finalized their own options too.
    context->implementation_identifier = NULL;

    return RMW_RET_OK;
}
