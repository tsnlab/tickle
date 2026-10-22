/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// bundle10_rss instrument: how many bytes of each table of an object are RESIDENT in this process - written, so backed
// by a page of its own - rather than its sizeof. Read from /proc/self/pagemap, page by page, over each field's address
// range; a field shares a page with its neighbours, so a field counts the bytes of itself that lie on resident pages.
//
// A page counts as resident when pagemap says present (bit 63) AND exclusively mapped (bit 56). A page only READ
// since its allocation maps the kernel's shared zero page: present but not exclusive. It costs no RSS and is reported
// apart as zero_mapped, so code that reads an untouched table (a scan finding nothing) does not read as one that
// filled it. A transparent huge page makes all 512 of its subpages present and exclusive at the first write anywhere
// in it, which is exactly what RSS then charges, so it is counted as it is charged.
//
// The layout comes from layout.py (gdb over the build's debug info): "TYPE <name> <sizeof>" and
// "FIELD <type> <path> <offset> <size>" lines, so no field list is written by hand and every field of the measured
// build's own layout is covered.
//
// Two ways to use it:
//   - linked into a host program (core_host.c): rt_load_layout(), rt_register(type, base), rt_snapshot(out, label).
//   - as an LD_PRELOAD (rss_tables_preload.c includes this file): objects are found by calloc() of exactly a TYPE's
//     sizeof - rmw_tickle zero_allocate()s its context implementation - and sampled every RSS_TABLES_PERIOD_MS.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RT_MAX_TYPES 8
#define RT_MAX_FIELDS 2048
#define RT_MAX_OBJS 32
#define RT_NAME 128
#define RT_PAGE 4096UL

struct rt_type {
    char name[RT_NAME];
    size_t size;
};
struct rt_field {
    int type;
    char path[RT_NAME];
    size_t off;
    size_t size;
};
struct rt_obj {
    int type; // -1: a plain range (rt_register_range)
    char name[RT_NAME];
    uintptr_t base;
    size_t size;
    int live;
    size_t* peak_res; // per field of its type (or [0] for a range)
    size_t* peak_zero;
    size_t* peak_nonzero;
};

static struct rt_type rt_types[RT_MAX_TYPES];
static int rt_ntypes;
static struct rt_field rt_fields[RT_MAX_FIELDS];
static int rt_nfields;
static struct rt_obj rt_objs[RT_MAX_OBJS];
static volatile int rt_nobjs;

static int rt_load_layout(const char* path) {
    FILE* f = fopen(path, "r");
    char line[512];
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char a[RT_NAME];
        char b[RT_NAME];
        size_t x = 0;
        size_t y = 0;
        if (sscanf(line, "TYPE %127s %zu", a, &x) == 2 && rt_ntypes < RT_MAX_TYPES) {
            snprintf(rt_types[rt_ntypes].name, RT_NAME, "%s", a);
            rt_types[rt_ntypes].size = x;
            rt_ntypes++;
        } else if (sscanf(line, "FIELD %127s %127s %zu %zu", a, b, &x, &y) == 4 && rt_nfields < RT_MAX_FIELDS) {
            for (int t = 0; t < rt_ntypes; t++) {
                if (strcmp(rt_types[t].name, a) == 0) {
                    rt_fields[rt_nfields].type = t;
                    snprintf(rt_fields[rt_nfields].path, RT_NAME, "%s", b);
                    rt_fields[rt_nfields].off = x;
                    rt_fields[rt_nfields].size = y;
                    rt_nfields++;
                    break;
                }
            }
        }
    }
    fclose(f);
    return rt_ntypes > 0 && rt_nfields > 0 ? 0 : -1;
}

static int rt_type_index(const char* name) {
    for (int t = 0; t < rt_ntypes; t++) {
        if (strcmp(rt_types[t].name, name) == 0) {
            return t;
        }
    }
    return -1;
}

static int rt_add(int type, const char* name, uintptr_t base, size_t size) {
    int i = rt_nobjs;
    if (i >= RT_MAX_OBJS) {
        return -1;
    }
    size_t n = 1;
    if (type >= 0) {
        n = (size_t)rt_nfields;
    }
    rt_objs[i].type = type;
    snprintf(rt_objs[i].name, RT_NAME, "%s#%d", name, i);
    rt_objs[i].base = base;
    rt_objs[i].size = size;
    rt_objs[i].peak_res = (size_t*)calloc(n, sizeof(size_t));
    rt_objs[i].peak_zero = (size_t*)calloc(n, sizeof(size_t));
    rt_objs[i].peak_nonzero = (size_t*)calloc(n, sizeof(size_t));
    rt_objs[i].live = 1;
    __atomic_store_n(&rt_nobjs, i + 1, __ATOMIC_RELEASE);
    return i;
}

// An object of a layout TYPE at base. Returns its index or -1.
__attribute__((unused)) static int rt_register(const char* type, const void* base) {
    int t = rt_type_index(type);
    return t < 0 ? -1 : rt_add(t, type, (uintptr_t)base, rt_types[t].size);
}

// A plain range (a publisher's storage, a server's cache), counted as one table.
__attribute__((unused)) static int rt_register_range(const char* name, const void* base, size_t size) {
    return rt_add(-1, name, (uintptr_t)base, size);
}

// pagemap flags of the pages covering [lo, hi): bit 0 resident (present + exclusive), bit 1 zero-mapped/shared.
static unsigned char* rt_page_flags(uintptr_t lo, uintptr_t hi, uintptr_t* first_page) {
    uintptr_t p0 = lo & ~(RT_PAGE - 1);
    size_t pages = (hi - p0 + RT_PAGE - 1) / RT_PAGE;
    unsigned char* flags = (unsigned char*)calloc(pages, 1);
    uint64_t* ent = (uint64_t*)calloc(pages, sizeof(uint64_t));
    int fd = open("/proc/self/pagemap", O_RDONLY);
    *first_page = p0;
    if (flags == NULL || ent == NULL || fd < 0) {
        free(ent);
        if (fd >= 0) {
            close(fd);
        }
        free(flags);
        return NULL;
    }
    ssize_t got = pread(fd, ent, pages * sizeof(uint64_t), (off_t)((p0 / RT_PAGE) * sizeof(uint64_t)));
    close(fd);
    if (got != (ssize_t)(pages * sizeof(uint64_t))) {
        free(ent);
        free(flags);
        return NULL;
    }
    for (size_t i = 0; i < pages; i++) {
        int present = (int)((ent[i] >> 63) & 1U);
        int exclusive = (int)((ent[i] >> 56) & 1U);
        flags[i] = (unsigned char)(present && exclusive ? 1 : (present ? 2 : 0));
    }
    free(ent);
    return flags;
}

// Bytes of [lo, hi) on pages with flag bit `bit`.
static size_t rt_count(const unsigned char* flags, uintptr_t p0, uintptr_t lo, uintptr_t hi, int bit) {
    size_t total = 0;
    for (uintptr_t a = lo; a < hi;) {
        uintptr_t page_end = (a & ~(RT_PAGE - 1)) + RT_PAGE;
        uintptr_t e = page_end < hi ? page_end : hi;
        if (flags[(a - p0) / RT_PAGE] & bit) {
            total += e - a;
        }
        a = e;
    }
    return total;
}

// Nonzero bytes of [lo, hi) on resident pages only: reading a page that is not resident would map it (the zero page)
// and change the next reading. A lower bound on how much of a table holds anything - an entry may be all zero - and a
// table that is resident with almost no nonzero byte was written (zeroed) rather than used.
static size_t rt_nonzero(const unsigned char* flags, uintptr_t p0, uintptr_t lo, uintptr_t hi) {
    size_t total = 0;
    for (uintptr_t a = lo; a < hi;) {
        uintptr_t page_end = (a & ~(RT_PAGE - 1)) + RT_PAGE;
        uintptr_t e = page_end < hi ? page_end : hi;
        if (flags[(a - p0) / RT_PAGE] & 1) {
            for (const unsigned char* b = (const unsigned char*)a; b < (const unsigned char*)e; b++) {
                total += *b != 0;
            }
        }
        a = e;
    }
    return total;
}

static void rt_proc_kb(size_t* rss, size_t* anon, size_t* hwm) {
    char line[256];
    *rss = *anon = *hwm = 0;
    FILE* f = fopen("/proc/self/smaps_rollup", "r");
    if (f != NULL) {
        while (fgets(line, sizeof(line), f) != NULL) {
            sscanf(line, "Rss: %zu", rss);
            sscanf(line, "Anonymous: %zu", anon);
        }
        fclose(f);
    }
    f = fopen("/proc/self/status", "r");
    if (f != NULL) {
        while (fgets(line, sizeof(line), f) != NULL) {
            sscanf(line, "VmHWM: %zu", hwm);
        }
        fclose(f);
    }
}

// Reads every live object's fields now, raises their peaks, and - when out is not NULL - prints the reading:
//   RT_PROC label=L rss_kb=.. anon_kb=.. hwm_kb=..
//   RT label=L obj=NAME field=PATH off=.. size=.. resident=.. zero_mapped=.. peak_resident=..
static void rt_snapshot(FILE* out, const char* label) {
    size_t rss = 0;
    size_t anon = 0;
    size_t hwm = 0;
    rt_proc_kb(&rss, &anon, &hwm);
    if (out != NULL) {
        fprintf(out, "RT_PROC label=%s pid=%d rss_kb=%zu anon_kb=%zu hwm_kb=%zu\n", label, (int)getpid(), rss, anon,
                hwm);
    }
    int n = __atomic_load_n(&rt_nobjs, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        struct rt_obj* o = &rt_objs[i];
        if (!o->live) {
            if (out != NULL) { // freed: its peaks stand as the reading
                for (int f = 0; f < (o->type < 0 ? 1 : rt_nfields); f++) {
                    if (o->type >= 0 && rt_fields[f].type != o->type) {
                        continue;
                    }
                    fprintf(out,
                            "RT label=%s obj=%s field=%s off=%zu size=%zu resident=freed zero_mapped=freed "
                            "peak_resident=%zu nonzero=freed peak_nonzero=%zu\n",
                            label, o->name, o->type < 0 ? "(range)" : rt_fields[f].path,
                            o->type < 0 ? 0 : rt_fields[f].off, o->type < 0 ? o->size : rt_fields[f].size,
                            o->peak_res[f], o->peak_nonzero[f]);
                }
            }
            continue;
        }
        uintptr_t p0 = 0;
        unsigned char* flags = rt_page_flags(o->base, o->base + o->size, &p0);
        if (flags == NULL) {
            continue;
        }
        for (int f = 0; f < (o->type < 0 ? 1 : rt_nfields); f++) {
            size_t off = 0;
            size_t size = o->size;
            const char* path = "(range)";
            if (o->type >= 0) {
                if (rt_fields[f].type != o->type) {
                    continue;
                }
                off = rt_fields[f].off;
                size = rt_fields[f].size;
                path = rt_fields[f].path;
            }
            uintptr_t lo = o->base + off;
            size_t res = rt_count(flags, p0, lo, lo + size, 1);
            size_t zero = rt_count(flags, p0, lo, lo + size, 2);
            size_t nonzero = rt_nonzero(flags, p0, lo, lo + size);
            if (nonzero > o->peak_nonzero[f]) {
                o->peak_nonzero[f] = nonzero;
            }
            if (res > o->peak_res[f]) {
                o->peak_res[f] = res;
            }
            if (zero > o->peak_zero[f]) {
                o->peak_zero[f] = zero;
            }
            if (out != NULL) {
                fprintf(out,
                        "RT label=%s obj=%s field=%s off=%zu size=%zu resident=%zu zero_mapped=%zu "
                        "peak_resident=%zu nonzero=%zu peak_nonzero=%zu\n",
                        label, o->name, path, off, size, res, zero, o->peak_res[f], nonzero, o->peak_nonzero[f]);
            }
        }
        free(flags);
    }
    if (out != NULL) {
        fflush(out);
    }
}
