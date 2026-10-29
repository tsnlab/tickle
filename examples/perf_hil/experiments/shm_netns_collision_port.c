/*
 * Copyright (c) 2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// LD_PRELOAD shim for shm_netns_collision.sh: the first IPv4 bind() to port 0 - the data socket's, which TickLE leaves
// to the kernel (tt_bind(), hal_linux.c) - binds port $SHM_COLLIDE_PORT instead. Two network namespaces running a
// context each under this shim at the same address and context id therefore name the same segment, which is the
// collision the script forces. Unset: nothing changes.
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming) - glibc feature-test macro, for RTLD_NEXT
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <netinet/in.h>
#include <sys/socket.h>

static int forced = 0;

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) - glibc's own names are reserved (__fd ...)
int bind(int sock_fd, const struct sockaddr* addr, socklen_t len) {
    int (*real)(int, const struct sockaddr*, socklen_t) =
        (int (*)(int, const struct sockaddr*, socklen_t))dlsym(RTLD_NEXT, "bind");
    const char* port = getenv("SHM_COLLIDE_PORT");
    if (!forced && port != NULL && addr != NULL && addr->sa_family == AF_INET && len >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in addr_in;
        memcpy(&addr_in, addr, sizeof(addr_in));
        if (addr_in.sin_port == 0) {
            forced = 1;
            addr_in.sin_port = htons((uint16_t)strtoul(port, NULL, 10)); // NOLINT(readability-magic-numbers) - base 10
            return real(sock_fd, (const struct sockaddr*)&addr_in, sizeof(addr_in));
        }
    }
    return real(sock_fd, addr, len);
}
