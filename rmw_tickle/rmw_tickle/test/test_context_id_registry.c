/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// g8 (RMW_GAPS_PLAN.md): the host registry behind tt_claim_context_id() (hal_linux.c), on a file of its own. A first
// claim gets the preferred id and a second a different one; a dead pid's id is taken back; a pid that cannot be
// checked (pid 1: EPERM for an ordinary user) counts as alive; a release frees the id; `avoid` and `salt` steer the
// choice; no registry at all falls back to the preferred id. Mutant, killed here: the registry treating dead pids as
// alive.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <tickle/config.h>
#include <tickle/hal_linux.h>

#define PREFERRED 5
#define DEAD_ID 7
#define INIT_ID 9
#define ONLY_FREE 100
#define ID_BYTES (tt_MAX_CONTEXT_IDS / 8)

// A pid that has just exited, so nothing holds it.
static int32_t dead_pid(void) {
    pid_t child = fork();
    if (0 == child) {
        _exit(0);
    }
    assert(child > 0);
    int status = 0;
    assert(child == waitpid(child, &status, 0));
    return (int32_t)child;
}

static void hold(const char* path, uint8_t id, int32_t pid) {
    FILE* file = fopen(path, "r+b");
    assert(NULL != file);
    assert(0 == fseek(file, (long)id * (long)sizeof(int32_t), SEEK_SET));
    assert(1 == fwrite(&pid, sizeof(pid), 1, file));
    assert(0 == fclose(file));
}

int main(void) {
    if (0 == getuid()) {
        printf("test_context_id_registry: SKIP (as root, pid 1 is checkable, so the EPERM case does not arise)\n");
        return 0;
    }
    char path[] = "/tmp/tickle_id_registry_XXXXXX";
    int descriptor = mkstemp(path);
    assert(descriptor >= 0);
    assert(0 == close(descriptor));
    int32_t self = (int32_t)getpid();

    // First and second claim: the preferred id, then the highest free one.
    assert(PREFERRED == tt_id_registry_claim(path, PREFERRED, NULL, 0, self));
    assert(tt_CONTEXT_ID_BROADCAST - 1 == tt_id_registry_claim(path, PREFERRED, NULL, 0, self));

    // A release frees it.
    tt_id_registry_release(path, PREFERRED, self);
    assert(PREFERRED == tt_id_registry_claim(path, PREFERRED, NULL, 0, self));

    // A dead holder holds nothing; one that cannot be checked holds its id.
    hold(path, DEAD_ID, dead_pid());
    assert(DEAD_ID == tt_id_registry_claim(path, DEAD_ID, NULL, 0, self));
    hold(path, INIT_ID, 1);
    assert(INIT_ID != tt_id_registry_claim(path, INIT_ID, NULL, 0, self));

    // `avoid` leaves one id; `salt` picks among the free ones, and two salts pick differently.
    uint8_t avoid[ID_BYTES];
    memset(avoid, 0xff, sizeof(avoid));
    avoid[ONLY_FREE / 8] &= (uint8_t)~(1U << (ONLY_FREE % 8));
    assert(ONLY_FREE == tt_id_registry_claim(path, tt_CONTEXT_ID_INVALID, avoid, 0, self));
    uint8_t first = tt_id_registry_claim(path, tt_CONTEXT_ID_INVALID, NULL, 1, self);
    uint8_t second = tt_id_registry_claim(path, tt_CONTEXT_ID_INVALID, NULL, 2, self);
    assert(first != second && tt_CONTEXT_ID_INVALID != first && tt_CONTEXT_ID_INVALID != second);

    // Every id avoided: none.
    memset(avoid, 0xff, sizeof(avoid));
    assert(tt_CONTEXT_ID_INVALID == tt_id_registry_claim(path, PREFERRED, avoid, 0, self));

    // No registry: the preferred id, as a lone context has always had.
    assert(PREFERRED == tt_id_registry_claim(NULL, PREFERRED, NULL, 0, self));

    assert(0 == unlink(path));
    printf("test_context_id_registry: PASS\n");
    return 0;
}
