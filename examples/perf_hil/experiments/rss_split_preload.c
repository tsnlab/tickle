/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Where a bench process's resident memory is, printed at exit: an LD_PRELOAD instrument for stage1_payg.sh
// (RSS_SPLIT=1), so a peak-RSS difference between two builds can be split into the program's own code (the
// executable's file-backed mappings), its stack (where the bench keeps struct tt_Context), and the rest - without
// changing the bench or its RESULT line. Peak RSS alone cannot say whether a difference is code that was faulted in or
// data that was written, and the two have different remedies.
//
//   gcc -O2 -shared -fPIC -o rss_split_preload.so rss_split_preload.c
//   LD_PRELOAD=$PWD/rss_split_preload.so ./client ...   -> one "RSS_SPLIT: ..." line on stderr at exit
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum {
    STATUS_LINE_BYTES = 256,
    EXE_PATH_BYTES = 512,
    SMAPS_LINE_BYTES = 768,
};

static unsigned long status_kb(const char* key) {
    FILE* file = fopen("/proc/self/status", "r");
    char line[STATUS_LINE_BYTES];
    unsigned long kib = 0;
    size_t key_len = strlen(key);
    if (file == NULL) {
        return 0;
    }
    while (fgets(line, (int)sizeof(line), file) != NULL) {
        if (strncmp(line, key, key_len) == 0 && sscanf(line + key_len, "%lu", &kib) == 1) {
            break;
        }
    }
    (void)fclose(file);
    return kib;
}

__attribute__((destructor)) static void rss_split_report(void) {
    char exe[EXE_PATH_BYTES];
    ssize_t exe_len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    exe[exe_len > 0 ? exe_len : 0] = '\0';
    FILE* file = fopen("/proc/self/smaps", "r");
    char line[SMAPS_LINE_BYTES];
    unsigned long exe_kb = 0;
    unsigned long stack_kb = 0;
    int in_exe = 0;
    int in_stack = 0;
    if (file != NULL) {
        while (fgets(line, (int)sizeof(line), file) != NULL) {
            unsigned long kib = 0;
            // A mapping header, "start-end perms offset dev inode [path]", starts with a lower-case hex address; every
            // field line ("Rss:", "VmFlags:") starts with an upper-case letter.
            if ((line[0] >= '0' && line[0] <= '9') || (line[0] >= 'a' && line[0] <= 'f')) {
                in_exe = exe[0] != '\0' && strstr(line, exe) != NULL;
                in_stack = strstr(line, "[stack]") != NULL;
                continue;
            }
            if (strncmp(line, "Rss:", 4) == 0 && sscanf(line + 4, "%lu", &kib) == 1) {
                exe_kb += in_exe ? kib : 0;
                stack_kb += in_stack ? kib : 0;
            }
        }
        (void)fclose(file);
    }
    fprintf(stderr, "RSS_SPLIT: hwm_kb=%lu rss_kb=%lu anon_kb=%lu file_kb=%lu exe_kb=%lu stack_kb=%lu\n",
            status_kb("VmHWM:"), status_kb("VmRSS:"), status_kb("RssAnon:"), status_kb("RssFile:"), exe_kb, stack_kb);
}
