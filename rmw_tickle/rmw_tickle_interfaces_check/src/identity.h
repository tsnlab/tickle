/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Which TickLE libraries this process actually has mapped, printed by the process itself.
//
// check_ros2_interfaces.sh used to read /proc/<pid>/maps from outside, which it can only do while
// that process happens to still be alive. On 2026-09-30 that window closed: in -r mode the
// publisher's identity poll ran its full five seconds first, and by then the subscriber had
// round-tripped and exited, so its identity read "not seen" - which is also exactly what the
// library shadowing this check exists to catch looks like. A guard whose false alarm is
// indistinguishable from its true finding is not a guard, so the question is asked of the process
// under test instead, while it is certainly running, and the answer outlives it in the log.
//
// Every mapped path containing "tickle" is printed, one per line, duplicates and all - the reader
// sorts them. That covers librmw_tickle.so, libtickle.so and every ..._typesupport_tickle_* the
// process pulled in, without this header having to know which of them a given check cares about.
//
// Shared-memory segments go on their own "segment-self" lines rather than among the libraries. They
// are not part of the identity question, and mixing them in made the answer to it unreadable - but
// they are worth printing, because which segments a process has mapped is the one direct, race-free
// way to see the shared-memory module's lifecycle from outside: a build that creates its segment
// only once a same-host peer appears has none of these lines before that happens.

#ifndef RMW_TICKLE_INTERFACES_CHECK_IDENTITY_H
#define RMW_TICKLE_INTERFACES_CHECK_IDENTITY_H

#include <stdio.h>
#include <string.h>

// Call once the entities exist: rmw_tickle is dlopen'd during init and a typesupport library is
// mapped when its type is first used, so calling this before that reports a half-loaded process.
static inline void report_identity(const char* who) {
    FILE* maps = fopen("/proc/self/maps", "re");
    if (NULL == maps) {
        printf("identity-self %s: /proc/self/maps could not be read\n", who);
        fflush(stdout);
        return;
    }
    char line[1024];
    while (NULL != fgets(line, sizeof(line), maps)) {
        // A maps line's fields hold no '/' - the address range uses '-', the device uses ':' - so
        // the first one starts the path, and an anonymous mapping has none.
        char* path = strchr(line, '/');
        if (NULL == path) {
            continue;
        }
        char* newline = strchr(path, '\n');
        if (NULL != newline) {
            *newline = '\0';
        }
        if (NULL == strstr(path, "tickle")) {
            continue;
        }
        printf("%s %s: %s\n", NULL != strstr(path, "/dev/shm/") ? "segment-self" : "identity-self", who, path);
    }
    (void)fclose(maps);
    fflush(stdout);
}

#endif // RMW_TICKLE_INTERFACES_CHECK_IDENTITY_H
