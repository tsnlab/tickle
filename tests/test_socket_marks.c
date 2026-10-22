/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Network profiles items D and E on the REAL Linux HAL (hal_linux.c "Traffic classes and send time"), built as
// rmw_tickle builds core (large samples: the UDP_SEGMENT send path). Every system call that could carry a class or a
// time - setsockopt(), sendto(), sendmsg(), sendmmsg() - is answered by this file, which records it and passes it on:
//   - the defaults set nothing and send as before: no IP_TOS, SO_PRIORITY or SO_TXTIME, a sendto() stays a sendto(),
//     no control message on any send;
//   - classes that agree go on the socket once (IP_TOS, then SO_PRIORITY, which IP_TOS would otherwise have set from
//     the TOS) and nothing per send;
//   - classes that differ: the DATA class on the socket, each other class's TOS and priority as control messages of
//     its own datagrams, by what the datagram carries (and while core resends); the TOS reaches a receiver
//     (IP_RECVTOS over loopback);
//   - a kernel without SO_PRIORITY per datagram (answered here) keeps the priority per socket and the TOS per
//     datagram; a priority the kernel refuses (7 without CAP_NET_ADMIN) is not sent;
//   - SO_TXTIME: asked only with the option, CLOCK_TAI, deadline mode off; a refusal leaves no time on any send; taken
//     (answered here), a time goes as SCM_TXTIME exactly while the context holds one;
//   - the checks refuse a DSCP above 63 and a txtime that is no switch.
// tests/mutants_socket_marks.py breaks each decision and expects this test, or test_tx_class.c, to fail.
//
// Loopback only, as test_init_options_socket.c: the broadcast is 127.255.255.255, the id registry is off, and the
// only datagrams are this test's own, to a socket of its own on 127.0.0.1.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // NOLINT(bugprone-reserved-identifier) - the real HAL needs ppoll(); RTLD_NEXT
#endif
#define tt_MAX_BUFFER_LENGTH 65507
#define tt_LARGE_SAMPLES 1
#define TT_HAL_LIBC_CALLS 1 // the HAL's sendto() and sendmsg() through the library, so that this file answers them

#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <linux/net_tstamp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <tickle/tickle.h>

#define TEST_COMMON_DEFINE_STORAGE
#ifndef HAL_LINUX_C
#define HAL_LINUX_C "../src/hal_linux.c"
#endif
#include HAL_LINUX_C // NOLINT(bugprone-suspicious-include) - the real HAL, deliberately; or a mutant of it
#ifndef TICKLE_C
#define TICKLE_C "../src/tickle.c"
#endif
#include TICKLE_C // NOLINT(bugprone-suspicious-include) -- whitebox; tests/mutants_socket_marks.py passes a mutant
#include "test_common.h"

#define PORT 47341
#define LOOPBACK_BROADCAST "127.255.255.255"
#define CONTEXT_ID 24
#define SEND_TIME 1761100000123456789ULL

static struct tt_Context ctx;

// ---- The recorder. One entry per call that set an option or sent, in order.
enum call_kind { CALL_SETSOCKOPT, CALL_SENDTO, CALL_SENDMSG, CALL_SENDMMSG };
#define MAX_CMSGS 4
struct cmsg_seen {
    int level;
    int type;
    uint64_t value; // the payload, widened: IP_TOS and SO_PRIORITY 4 bytes, SCM_TXTIME 8
};
struct call_seen {
    enum call_kind kind;
    int fd;
    int flags;
    int level; // setsockopt()
    int name;
    uint64_t value;
    int result;
    int cmsg_count; // a send's control messages (the first message's, for sendmmsg(): see per_message below)
    struct cmsg_seen cmsgs[MAX_CMSGS];
};
#define MAX_CALLS 64
static struct call_seen calls[MAX_CALLS];
static int call_count = 0;
// sendmmsg(): each message's control messages.
#define MAX_MESSAGES 8
static int mmsg_messages = 0;
static int per_message_count[MAX_MESSAGES];
static struct cmsg_seen per_message[MAX_MESSAGES][MAX_CMSGS];

// Answers this file gives instead of the kernel's.
static bool fake_txtime = false;       // SO_TXTIME taken; SCM_TXTIME then stripped before the kernel sees it
static bool fake_no_prio_cmsg = false; // SO_PRIORITY as a control message refused with EINVAL, as an older kernel does

static void reset_calls(void) {
    call_count = 0;
    mmsg_messages = 0;
}

static struct call_seen* new_call(enum call_kind kind, int fd) {
    if (call_count >= MAX_CALLS) {
        return NULL;
    }
    struct call_seen* call = &calls[call_count++];
    memset(call, 0, sizeof(*call));
    call->kind = kind;
    call->fd = fd;
    return call;
}

static int read_cmsgs(const struct msghdr* msg, struct cmsg_seen* out) {
    int count = 0;
    if (msg->msg_control == NULL || msg->msg_controllen == 0) {
        return 0;
    }
    for (struct cmsghdr* c = CMSG_FIRSTHDR((struct msghdr*)msg); c != NULL && count < MAX_CMSGS;
         c = CMSG_NXTHDR((struct msghdr*)msg, c)) {
        out[count].level = c->cmsg_level;
        out[count].type = c->cmsg_type;
        out[count].value = 0;
        size_t len = c->cmsg_len - CMSG_LEN(0);
        memcpy(&out[count].value, CMSG_DATA(c), len < 8 ? len : 8);
        count++;
    }
    return count;
}

// A copy of `msg` without its SCM_TXTIME, for a socket that never took SO_TXTIME (fake_txtime).
static void strip_txtime(const struct msghdr* msg, struct msghdr* out, char* buffer, size_t size) {
    *out = *msg;
    size_t at = 0;
    if (msg->msg_control == NULL) {
        return;
    }
    for (struct cmsghdr* c = CMSG_FIRSTHDR((struct msghdr*)msg); c != NULL; c = CMSG_NXTHDR((struct msghdr*)msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TXTIME) {
            continue;
        }
        size_t space = CMSG_SPACE(c->cmsg_len - CMSG_LEN(0));
        if (at + space <= size) {
            memcpy(buffer + at, c, c->cmsg_len);
            at += space;
        }
    }
    out->msg_control = at != 0 ? buffer : NULL;
    out->msg_controllen = at;
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) - libc's prototype names them __fd and so on
int setsockopt(int fd, int level, int name, const void* value, socklen_t length) {
    struct call_seen* call = new_call(CALL_SETSOCKOPT, fd);
    if (call != NULL) {
        call->level = level;
        call->name = name;
        if (value != NULL) {
            memcpy(&call->value, value, length < 8 ? length : 8);
        }
    }
    if (fake_txtime && level == SOL_SOCKET && name == SO_TXTIME) {
        return 0;
    }
    int (*real)(int, int, int, const void*, socklen_t) =
        (int (*)(int, int, int, const void*, socklen_t))dlsym(RTLD_NEXT, "setsockopt");
    int result = real(fd, level, name, value, length);
    if (call != NULL) {
        call->result = result < 0 ? -errno : result;
    }
    return result;
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) - libc's prototype names them __fd and so on
ssize_t sendto(int fd, const void* buf, size_t len, int flags, const struct sockaddr* to, socklen_t to_len) {
    struct call_seen* call = new_call(CALL_SENDTO, fd);
    if (call != NULL) {
        call->flags = flags;
    }
    ssize_t (*real)(int, const void*, size_t, int, const struct sockaddr*, socklen_t) =
        (ssize_t(*)(int, const void*, size_t, int, const struct sockaddr*, socklen_t))dlsym(RTLD_NEXT, "sendto");
    return real(fd, buf, len, flags, to, to_len);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) - libc's prototype names them __fd and so on
ssize_t sendmsg(int fd, const struct msghdr* msg, int flags) {
    struct call_seen* call = new_call(CALL_SENDMSG, fd);
    if (call != NULL) {
        call->flags = flags;
        call->cmsg_count = read_cmsgs(msg, call->cmsgs);
    }
    if (fake_no_prio_cmsg) {
        struct cmsg_seen seen[MAX_CMSGS];
        int count = read_cmsgs(msg, seen);
        for (int i = 0; i < count; i++) {
            if (seen[i].level == SOL_SOCKET && seen[i].type == SO_PRIORITY) {
                errno = EINVAL;
                return -1;
            }
        }
    }
    ssize_t (*real)(int, const struct msghdr*, int) =
        (ssize_t(*)(int, const struct msghdr*, int))dlsym(RTLD_NEXT, "sendmsg");
    if (fake_txtime) {
        char buffer[256];
        struct msghdr stripped;
        strip_txtime(msg, &stripped, buffer, sizeof(buffer));
        return real(fd, &stripped, flags);
    }
    return real(fd, msg, flags);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) - libc's prototype names them __fd and so on
int sendmmsg(int fd, struct mmsghdr* msgs, unsigned int count, int flags) {
    struct call_seen* call = new_call(CALL_SENDMMSG, fd);
    if (call != NULL) {
        call->flags = flags;
        call->value = count;
        call->cmsg_count = count > 0 ? read_cmsgs(&msgs[0].msg_hdr, call->cmsgs) : 0;
    }
    mmsg_messages = (int)(count < MAX_MESSAGES ? count : MAX_MESSAGES);
    for (int i = 0; i < mmsg_messages; i++) {
        per_message_count[i] = read_cmsgs(&msgs[i].msg_hdr, per_message[i]);
    }
    int (*real)(int, struct mmsghdr*, unsigned int, int) =
        (int (*)(int, struct mmsghdr*, unsigned int, int))dlsym(RTLD_NEXT, "sendmmsg");
    if (fake_txtime && count <= MAX_MESSAGES) {
        struct mmsghdr stripped[MAX_MESSAGES];
        char buffers[MAX_MESSAGES][256];
        for (unsigned int i = 0; i < count; i++) {
            stripped[i] = msgs[i];
            strip_txtime(&msgs[i].msg_hdr, &stripped[i].msg_hdr, buffers[i], sizeof(buffers[i]));
        }
        int result = real(fd, stripped, count, flags);
        for (int i = 0; i < result; i++) {
            msgs[i].msg_len = stripped[i].msg_len;
        }
        return result;
    }
    return real(fd, msgs, count, flags);
}

// ---- Looking things up in the record.
static int count_setsockopt(int fd, int level, int name) {
    int n = 0;
    for (int i = 0; i < call_count; i++) {
        n += calls[i].kind == CALL_SETSOCKOPT && calls[i].fd == fd && calls[i].level == level && calls[i].name == name;
    }
    return n;
}

static int index_of_setsockopt(int fd, int level, int name) {
    for (int i = 0; i < call_count; i++) {
        if (calls[i].kind == CALL_SETSOCKOPT && calls[i].fd == fd && calls[i].level == level && calls[i].name == name) {
            return i;
        }
    }
    return -1;
}

static const struct call_seen* last_send(void) {
    for (int i = call_count - 1; i >= 0; i--) {
        if (calls[i].kind != CALL_SETSOCKOPT) {
            return &calls[i];
        }
    }
    return NULL;
}

// A control message's value in a list, or -1 when it is not there.
static int64_t cmsg_value(const struct cmsg_seen* cmsgs, int count, int level, int type) {
    for (int i = 0; i < count; i++) {
        if (cmsgs[i].level == level && cmsgs[i].type == type) {
            return (int64_t)cmsgs[i].value;
        }
    }
    return -1;
}

// ---- The context, and a receiver that reads the TOS each datagram arrived with.
static struct tt_InitOptions defaults(void) {
    struct tt_InitOptions opts;
    tt_InitOptions_default(&opts);
    opts.net.broadcast = LOOPBACK_BROADCAST;
    opts.net.port = PORT;
    opts.net.context_id = CONTEXT_ID;
    return opts;
}

static tt_ret_t create(const struct tt_InitOptions* opts) {
    memset(&ctx, 0, sizeof(ctx));
    reset_calls();
    return tt_Context_create_with(&ctx, opts);
}

static void destroy(void) {
    EXPECT_EQ_INT(tt_RET_OK, tt_Context_destroy(&ctx));
}

static int receiver = -1;
static uint16_t receiver_port = 0;

static void open_receiver(void) {
    receiver = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = 0, .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
    int on = 1;
    if (receiver < 0 || bind(receiver, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
        setsockopt(receiver, IPPROTO_IP, IP_RECVTOS, &on, sizeof(on)) != 0) {
        fprintf(stderr, "receiver: %s\n", strerror(errno));
        test_failures++;
        return;
    }
    socklen_t length = sizeof(addr);
    (void)getsockname(receiver, (struct sockaddr*)&addr, &length);
    receiver_port = ntohs(addr.sin_port);
    struct timeval wait = {.tv_sec = 1, .tv_usec = 0};
    (void)setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
}

// The TOS byte of the next datagram the receiver gets, or -1 for none.
static int received_tos(void) {
    char data[2048];
    char control[64];
    struct iovec iov = {.iov_base = data, .iov_len = sizeof(data)};
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    if (recvmsg(receiver, &msg, 0) < 0) {
        return -1;
    }
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_TOS) {
            return *(uint8_t*)CMSG_DATA(c);
        }
    }
    return -1;
}

// ---- Datagrams, in the single form core sends them in.
static uint8_t user_data[sizeof(struct tt_SingleHeader) + sizeof(struct tt_DataHeader) + 4];
static uint8_t heartbeat[sizeof(struct tt_SingleHeader) + sizeof(struct tt_HeartbeatHeader)];

static void build_datagrams(void) {
    struct tt_SingleHeader head = {tt_SINGLE_MARKER_LE, tt_VERSION, CONTEXT_ID, tt_SUBMESSAGE_TYPE_DATA};
    struct tt_DataHeader data = {.endpoint_id = 9, .seq_no = 1, .timestamp = 0, .entity_id = 0x10};
    memset(user_data, 0, sizeof(user_data));
    memcpy(user_data, &head, sizeof(head));
    memcpy(user_data + sizeof(head), &data, sizeof(data));
    head.type = tt_SUBMESSAGE_TYPE_HEARTBEAT;
    memset(heartbeat, 0, sizeof(heartbeat));
    memcpy(heartbeat, &head, sizeof(head));
}

static int32_t send_to_receiver(const void* buf, size_t len) {
    return tt_send_to(&ctx, buf, len, INADDR_LOOPBACK, receiver_port);
}

static void set_class(struct tt_InitOptions* opts, uint8_t tx_class, uint32_t priority, uint8_t dscp) {
    opts->socket.tx_class[tx_class].priority = priority;
    opts->socket.tx_class[tx_class].dscp = dscp;
}

static int socket_int(int level, int name) {
    int value = -1;
    socklen_t length = sizeof(value);
    (void)getsockopt(ctx.hal.data_sock, level, name, &value, &length);
    return value;
}

// ---- The defaults: nothing set, nothing per send.
static void test_defaults_set_nothing_and_send_as_before(void) {
    struct tt_InitOptions opts = defaults();
    for (uint32_t c = 0; c < tt_TX_CLASS_COUNT; c++) {
        EXPECT_EQ_U32(0, opts.socket.tx_class[c].priority);
        EXPECT_EQ_U32(0, opts.socket.tx_class[c].dscp);
    }
    EXPECT_EQ_U32(tt_SWITCH_OFF, opts.socket.txtime);
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    EXPECT_EQ_INT(0, ctx.hal.tx_marks);
    EXPECT_EQ_INT(0, count_setsockopt(ctx.hal.data_sock, IPPROTO_IP, IP_TOS));
    EXPECT_EQ_INT(0, count_setsockopt(ctx.hal.data_sock, SOL_SOCKET, SO_PRIORITY));
    EXPECT_EQ_INT(0, count_setsockopt(ctx.hal.data_sock, SOL_SOCKET, SO_TXTIME));
    for (int i = 0; i < call_count; i++) {
        EXPECT_TRUE(calls[i].kind == CALL_SETSOCKOPT); // no probe, no send
    }

    reset_calls();
    EXPECT_TRUE(send_to_receiver(heartbeat, sizeof(heartbeat)) > 0);
    const struct call_seen* sent = last_send();
    EXPECT_TRUE(sent != NULL && sent->kind == CALL_SENDTO);
    EXPECT_EQ_INT(0, received_tos());
    reset_calls();
    EXPECT_TRUE(tt_send_iov(&ctx, heartbeat, sizeof(heartbeat), "x", 1, INADDR_LOOPBACK, receiver_port) > 0);
    sent = last_send();
    EXPECT_TRUE(sent != NULL && sent->kind == CALL_SENDMSG && sent->cmsg_count == 0);
    (void)received_tos();
    reset_calls();
    struct tt_OutDatagram batch[2] = {
        {.head = user_data, .head_len = sizeof(user_data), .ip = INADDR_LOOPBACK, .port = receiver_port},
        {.head = heartbeat, .head_len = sizeof(heartbeat), .ip = INADDR_LOOPBACK, .port = receiver_port}};
    EXPECT_EQ_INT(2, tt_send_batch(&ctx, batch, 2));
    EXPECT_EQ_INT(2, mmsg_messages);
    EXPECT_EQ_INT(0, per_message_count[0]);
    EXPECT_EQ_INT(0, per_message_count[1]);
    (void)received_tos();
    (void)received_tos();
    // Core's mark of a resend changes nothing either: no class differs.
    ctx.tx_class = tt_TX_CLASS_REPAIR;
    ctx.tx_time_ns = SEND_TIME;
    reset_calls();
    EXPECT_TRUE(send_to_receiver(user_data, sizeof(user_data)) > 0);
    sent = last_send();
    EXPECT_TRUE(sent != NULL && sent->kind == CALL_SENDTO);
    (void)received_tos();
    destroy();
}

// ---- One mark for all three classes: on the socket, once, and nothing per send.
static void test_classes_that_agree_go_on_the_socket(void) {
    struct tt_InitOptions opts = defaults();
    for (uint8_t c = 0; c < tt_TX_CLASS_COUNT; c++) {
        set_class(&opts, c, 3, 10);
    }
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    EXPECT_EQ_INT(0, ctx.hal.tx_marks);
    int tos_at = index_of_setsockopt(ctx.hal.data_sock, IPPROTO_IP, IP_TOS);
    int priority_at = index_of_setsockopt(ctx.hal.data_sock, SOL_SOCKET, SO_PRIORITY);
    EXPECT_TRUE(tos_at >= 0 && priority_at > tos_at); // the priority after the TOS, which would have set its own
    EXPECT_EQ_INT(40, socket_int(IPPROTO_IP, IP_TOS));
    EXPECT_EQ_INT(3, socket_int(SOL_SOCKET, SO_PRIORITY));
    EXPECT_EQ_INT(0, count_setsockopt(ctx.hal.sock, IPPROTO_IP, IP_TOS)); // the well-known socket sends nothing
    reset_calls();
    EXPECT_TRUE(send_to_receiver(heartbeat, sizeof(heartbeat)) > 0);
    const struct call_seen* sent = last_send();
    EXPECT_TRUE(sent != NULL && sent->kind == CALL_SENDTO);
    EXPECT_EQ_INT(40, received_tos());
    destroy();

    // A DSCP and no priority: the priority stays 0, not the kernel's from the TOS (EF, 46, would make it 4).
    opts = defaults();
    for (uint8_t c = 0; c < tt_TX_CLASS_COUNT; c++) {
        set_class(&opts, c, 0, 46);
    }
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    EXPECT_EQ_INT(184, socket_int(IPPROTO_IP, IP_TOS));
    EXPECT_EQ_INT(0, socket_int(SOL_SOCKET, SO_PRIORITY));
    destroy();

    // A priority and no DSCP: no IP_TOS.
    opts = defaults();
    for (uint8_t c = 0; c < tt_TX_CLASS_COUNT; c++) {
        set_class(&opts, c, 5, 0);
    }
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    EXPECT_EQ_INT(0, count_setsockopt(ctx.hal.data_sock, IPPROTO_IP, IP_TOS));
    EXPECT_EQ_INT(5, socket_int(SOL_SOCKET, SO_PRIORITY));
    EXPECT_EQ_INT(0, ctx.hal.tx_marks);
    destroy();
}

// ---- Classes that differ: DATA on the socket, the others per datagram.
static void expect_marks(const struct cmsg_seen* cmsgs, int count, int tos, int priority) {
    EXPECT_EQ_INT(tos, (int)cmsg_value(cmsgs, count, IPPROTO_IP, IP_TOS));
    EXPECT_EQ_INT(priority, (int)cmsg_value(cmsgs, count, SOL_SOCKET, SO_PRIORITY));
}

static void test_classes_that_differ_go_per_datagram(void) {
    struct tt_InitOptions opts = defaults();
    set_class(&opts, tt_TX_CLASS_REPAIR, 3, 16);
    set_class(&opts, tt_TX_CLASS_CONTROL, 2, 8);
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    EXPECT_EQ_INT(TT_TX_MARK_CLASS, ctx.hal.tx_marks);
    EXPECT_TRUE(ctx.hal.tx_priority_cmsg); // this kernel: SO_PRIORITY per datagram (the probe below said so)
    EXPECT_EQ_INT(0, count_setsockopt(ctx.hal.data_sock, IPPROTO_IP, IP_TOS)); // DATA is all 0
    EXPECT_EQ_INT(0, count_setsockopt(ctx.hal.data_sock, SOL_SOCKET, SO_PRIORITY));
    bool probed = false;
    for (int i = 0; i < call_count; i++) {
        probed = probed || (calls[i].kind == CALL_SENDMSG && (calls[i].flags & TT_MSG_PROBE) != 0);
    }
    EXPECT_TRUE(probed);

    // A user's sample: nothing of its own. A HEARTBEAT: CONTROL's. A user's sample while core resends: REPAIR's.
    reset_calls();
    EXPECT_TRUE(send_to_receiver(user_data, sizeof(user_data)) > 0);
    const struct call_seen* sent = last_send();
    EXPECT_TRUE(sent != NULL && sent->kind == CALL_SENDMSG && sent->cmsg_count == 0);
    EXPECT_EQ_INT(0, received_tos());
    reset_calls();
    EXPECT_TRUE(send_to_receiver(heartbeat, sizeof(heartbeat)) > 0);
    sent = last_send();
    EXPECT_TRUE(sent != NULL && sent->kind == CALL_SENDMSG);
    if (sent != NULL) {
        expect_marks(sent->cmsgs, sent->cmsg_count, 32, 2);
    }
    EXPECT_EQ_INT(32, received_tos());
    ctx.tx_class = tt_TX_CLASS_REPAIR;
    reset_calls();
    EXPECT_TRUE(send_to_receiver(user_data, sizeof(user_data)) > 0);
    sent = last_send();
    if (sent != NULL) {
        expect_marks(sent->cmsgs, sent->cmsg_count, 64, 3);
    }
    EXPECT_EQ_INT(64, received_tos());
    ctx.tx_class = tt_TX_CLASS_DATA;

    // tt_send() (the broadcast) and tt_send_iov(): the same.
    reset_calls();
    EXPECT_TRUE(tt_send(&ctx, heartbeat, sizeof(heartbeat)) > 0);
    sent = last_send();
    EXPECT_TRUE(sent != NULL && sent->kind == CALL_SENDMSG);
    if (sent != NULL) {
        expect_marks(sent->cmsgs, sent->cmsg_count, 32, 2);
    }
    reset_calls();
    EXPECT_TRUE(tt_send_iov(&ctx, heartbeat, sizeof(heartbeat), NULL, 0, INADDR_LOOPBACK, receiver_port) > 0);
    sent = last_send();
    if (sent != NULL) {
        expect_marks(sent->cmsgs, sent->cmsg_count, 32, 2);
    }
    EXPECT_EQ_INT(32, received_tos());

    // A batch: each message its own.
    reset_calls();
    struct tt_OutDatagram batch[2] = {
        {.head = user_data, .head_len = sizeof(user_data), .ip = INADDR_LOOPBACK, .port = receiver_port},
        {.head = heartbeat, .head_len = sizeof(heartbeat), .ip = INADDR_LOOPBACK, .port = receiver_port}};
    EXPECT_EQ_INT(2, tt_send_batch(&ctx, batch, 2));
    EXPECT_EQ_INT(2, mmsg_messages);
    EXPECT_EQ_INT(0, per_message_count[0]);
    expect_marks(per_message[1], per_message_count[1], 32, 2);
    EXPECT_EQ_INT(0, received_tos());
    EXPECT_EQ_INT(32, received_tos());
    EXPECT_EQ_U64(3, ctx.hal.tx_class_datagrams[tt_TX_CLASS_DATA] + ctx.hal.tx_class_datagrams[tt_TX_CLASS_REPAIR]);
    EXPECT_EQ_U64(4, ctx.hal.tx_class_datagrams[tt_TX_CLASS_CONTROL]);
    destroy();

    // DATA set too, and CONTROL left at 0: the socket carries DATA's, CONTROL's own 0s go with its datagrams.
    opts = defaults();
    set_class(&opts, tt_TX_CLASS_DATA, 5, 46);
    set_class(&opts, tt_TX_CLASS_REPAIR, 5, 46);
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    EXPECT_EQ_INT(184, socket_int(IPPROTO_IP, IP_TOS));
    EXPECT_EQ_INT(5, socket_int(SOL_SOCKET, SO_PRIORITY));
    reset_calls();
    EXPECT_TRUE(send_to_receiver(heartbeat, sizeof(heartbeat)) > 0);
    sent = last_send();
    if (sent != NULL) {
        expect_marks(sent->cmsgs, sent->cmsg_count, 0, 0);
    }
    EXPECT_EQ_INT(0, received_tos());
    ctx.tx_class = tt_TX_CLASS_REPAIR; // REPAIR as DATA: nothing of its own
    reset_calls();
    EXPECT_TRUE(send_to_receiver(user_data, sizeof(user_data)) > 0);
    sent = last_send();
    EXPECT_TRUE(sent != NULL && sent->cmsg_count == 0);
    EXPECT_EQ_INT(184, received_tos());
    destroy();

    // A class that differs only in its DSCP, and one only in its priority.
    opts = defaults();
    set_class(&opts, tt_TX_CLASS_CONTROL, 0, 8);
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    EXPECT_EQ_INT(TT_TX_MARK_CLASS, ctx.hal.tx_marks);
    reset_calls();
    EXPECT_TRUE(send_to_receiver(heartbeat, sizeof(heartbeat)) > 0);
    EXPECT_EQ_INT(32, received_tos());
    destroy();
    opts = defaults();
    set_class(&opts, tt_TX_CLASS_CONTROL, 2, 0);
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    EXPECT_EQ_INT(TT_TX_MARK_CLASS, ctx.hal.tx_marks);
    reset_calls();
    EXPECT_TRUE(send_to_receiver(heartbeat, sizeof(heartbeat)) > 0);
    sent = last_send();
    if (sent != NULL) {
        expect_marks(sent->cmsgs, sent->cmsg_count, 0, 2);
    }
    (void)received_tos();
    destroy();
}

// ---- A kernel without SO_PRIORITY per datagram; a priority the kernel refuses.
static void test_what_the_kernel_will_not_take(void) {
    struct tt_InitOptions opts = defaults();
    set_class(&opts, tt_TX_CLASS_CONTROL, 2, 8);
    fake_no_prio_cmsg = true;
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    fake_no_prio_cmsg = false;
    EXPECT_TRUE(!ctx.hal.tx_priority_cmsg);
    EXPECT_EQ_INT(TT_TX_MARK_CLASS, ctx.hal.tx_marks); // the TOS still differs
    reset_calls();
    EXPECT_TRUE(send_to_receiver(heartbeat, sizeof(heartbeat)) > 0);
    const struct call_seen* sent = last_send();
    if (sent != NULL) {
        expect_marks(sent->cmsgs, sent->cmsg_count, 32, -1);
    }
    EXPECT_EQ_INT(32, received_tos());
    destroy();

    // Priority only, and the kernel without it per datagram: nothing differs any more.
    opts = defaults();
    set_class(&opts, tt_TX_CLASS_CONTROL, 2, 0);
    fake_no_prio_cmsg = true;
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    fake_no_prio_cmsg = false;
    EXPECT_EQ_INT(0, ctx.hal.tx_marks);
    destroy();

    if (geteuid() == 0) {
        printf("SKIP priority 7: running as root, which may set it\n");
        return;
    }
    opts = defaults();
    set_class(&opts, tt_TX_CLASS_CONTROL, 7, 0);
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    EXPECT_EQ_INT(0, ctx.hal.tx_marks); // refused (EPERM): the class keeps the socket's
    EXPECT_EQ_U32(0, ctx.hal.tx_priority[tt_TX_CLASS_CONTROL]);
    destroy();
    opts = defaults();
    set_class(&opts, tt_TX_CLASS_CONTROL, 7, 8);
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    EXPECT_EQ_INT(TT_TX_MARK_CLASS, ctx.hal.tx_marks);
    reset_calls();
    EXPECT_TRUE(send_to_receiver(heartbeat, sizeof(heartbeat)) > 0); // goes: 7 is not asked of the kernel
    EXPECT_EQ_INT(32, received_tos());
    destroy();
}

// ---- SO_TXTIME.
static void test_txtime(void) {
    // Asked of the kernel only with the option.
    struct tt_InitOptions opts = defaults();
    opts.socket.txtime = tt_SWITCH_ON;
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    int at = index_of_setsockopt(ctx.hal.data_sock, SOL_SOCKET, SO_TXTIME);
    EXPECT_TRUE(at >= 0);
    if (at >= 0) {
        struct sock_txtime asked;
        memcpy(&asked, &calls[at].value, sizeof(asked));
        EXPECT_EQ_INT(CLOCK_TAI, asked.clockid);
        EXPECT_EQ_U32(0, asked.flags); // deadline mode off, no error reports (nothing reads the error queue)
        if (calls[at].result != 0) {
            // Refused here (EPERM: no CAP_NET_ADMIN): no time on any send.
            EXPECT_EQ_INT(0, ctx.hal.tx_marks);
            ctx.tx_time_ns = SEND_TIME;
            reset_calls();
            EXPECT_TRUE(send_to_receiver(user_data, sizeof(user_data)) > 0);
            const struct call_seen* sent = last_send();
            EXPECT_TRUE(sent != NULL && sent->kind == CALL_SENDTO);
            (void)received_tos();
        }
    }
    destroy();

    // Taken (answered here): the time goes exactly while the context holds one.
    fake_txtime = true;
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    EXPECT_EQ_INT(TT_TX_MARK_TIME, ctx.hal.tx_marks);
    ctx.tx_time_ns = SEND_TIME;
    reset_calls();
    EXPECT_TRUE(send_to_receiver(user_data, sizeof(user_data)) > 0);
    const struct call_seen* sent = last_send();
    EXPECT_TRUE(sent != NULL && sent->kind == CALL_SENDMSG);
    if (sent != NULL) {
        EXPECT_EQ_U64(SEND_TIME, (uint64_t)cmsg_value(sent->cmsgs, sent->cmsg_count, SOL_SOCKET, SCM_TXTIME));
        EXPECT_EQ_INT(-1, (int)cmsg_value(sent->cmsgs, sent->cmsg_count, IPPROTO_IP, IP_TOS)); // no class differs
    }
    (void)received_tos();
    reset_calls();
    struct tt_OutDatagram batch[2] = {
        {.head = user_data, .head_len = sizeof(user_data), .ip = INADDR_LOOPBACK, .port = receiver_port},
        {.head = heartbeat, .head_len = sizeof(heartbeat), .ip = INADDR_LOOPBACK, .port = receiver_port}};
    EXPECT_EQ_INT(2, tt_send_batch(&ctx, batch, 2));
    EXPECT_EQ_U64(SEND_TIME, (uint64_t)cmsg_value(per_message[0], per_message_count[0], SOL_SOCKET, SCM_TXTIME));
    EXPECT_EQ_U64(SEND_TIME, (uint64_t)cmsg_value(per_message[1], per_message_count[1], SOL_SOCKET, SCM_TXTIME));
    (void)received_tos();
    (void)received_tos();
    ctx.tx_time_ns = 0;
    reset_calls();
    EXPECT_TRUE(send_to_receiver(user_data, sizeof(user_data)) > 0);
    sent = last_send();
    EXPECT_TRUE(sent != NULL && sent->cmsg_count == 0);
    (void)received_tos();
    EXPECT_EQ_U64(3, ctx.hal.tx_timed_datagrams);
    destroy();

    // Off (the default, and tt_SWITCH_OFF): not asked.
    opts.socket.txtime = tt_SWITCH_OFF;
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    EXPECT_EQ_INT(0, count_setsockopt(ctx.hal.data_sock, SOL_SOCKET, SO_TXTIME));
    EXPECT_EQ_INT(0, ctx.hal.tx_marks);
    destroy();
    fake_txtime = false;
}

// ---- The checks.
static void test_validation(void) {
    struct tt_InitOptions opts = defaults();
    set_class(&opts, tt_TX_CLASS_REPAIR, 0, 64);
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, create(&opts));
    set_class(&opts, tt_TX_CLASS_REPAIR, 0, 63); // the boundary beside it
    EXPECT_EQ_INT(tt_RET_OK, create(&opts));
    destroy();
    opts = defaults();
    opts.socket.txtime = 3;
    EXPECT_EQ_INT(tt_RET_INVALID_ARGUMENT, create(&opts));
}

int main(void) {
    tt_current_log_level = TT_LOG_ERROR;
    (void)setenv("TICKLE_ID_REGISTRY", "off", 1);
    open_receiver();
    build_datagrams();
    test_defaults_set_nothing_and_send_as_before();
    test_classes_that_agree_go_on_the_socket();
    test_classes_that_differ_go_per_datagram();
    test_what_the_kernel_will_not_take();
    test_txtime();
    test_validation();
    if (test_result() != 0) {
        return 1;
    }
    printf("test_socket_marks: all tests passed\n");
    return 0;
}
