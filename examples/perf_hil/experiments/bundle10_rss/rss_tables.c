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
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming)
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

enum {
    RT_LAYOUT_LINE_BYTES = 512,
    RT_PROC_LINE_BYTES = 256,
    RT_PAGEMAP_PRESENT_BIT = 63,   // pagemap: page present
    RT_PAGEMAP_EXCLUSIVE_BIT = 56, // pagemap: page exclusively mapped
    RT_FLAG_RESIDENT = 1,          // rt_page_flags(): present + exclusive
    RT_FLAG_ZERO_MAPPED = 2,       // rt_page_flags(): present, shared (the zero page)
};

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
    FILE* file = fopen(path, "r");
    char line[RT_LAYOUT_LINE_BYTES];
    if (file == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char word1[RT_NAME]; // TYPE: its name; FIELD: its type
        char word2[RT_NAME]; // FIELD: its path
        size_t num1 = 0;     // TYPE: its sizeof; FIELD: its offset
        size_t num2 = 0;     // FIELD: its size
        if (sscanf(line, "TYPE %127s %zu", word1, &num1) == 2 && rt_ntypes < RT_MAX_TYPES) {
            snprintf(rt_types[rt_ntypes].name, RT_NAME, "%s", word1);
            rt_types[rt_ntypes].size = num1;
            rt_ntypes++;
        } else if (sscanf(line, "FIELD %127s %127s %zu %zu", word1, word2, &num1, &num2) == 4 &&
                   rt_nfields < RT_MAX_FIELDS) {
            for (int type_idx = 0; type_idx < rt_ntypes; type_idx++) {
                if (strcmp(rt_types[type_idx].name, word1) == 0) {
                    rt_fields[rt_nfields].type = type_idx;
                    snprintf(rt_fields[rt_nfields].path, RT_NAME, "%s", word2);
                    rt_fields[rt_nfields].off = num1;
                    rt_fields[rt_nfields].size = num2;
                    rt_nfields++;
                    break;
                }
            }
        }
    }
    fclose(file);
    return rt_ntypes > 0 && rt_nfields > 0 ? 0 : -1;
}

static int rt_type_index(const char* name) {
    for (int type_idx = 0; type_idx < rt_ntypes; type_idx++) {
        if (strcmp(rt_types[type_idx].name, name) == 0) {
            return type_idx;
        }
    }
    return -1;
}

static int rt_add(int type, const char* name, uintptr_t base, size_t size) {
    int i = rt_nobjs;
    if (i >= RT_MAX_OBJS) {
        return -1;
    }
    size_t count = 1;
    if (type >= 0) {
        count = (size_t)rt_nfields;
    }
    rt_objs[i].type = type;
    snprintf(rt_objs[i].name, RT_NAME, "%s#%d", name, i);
    rt_objs[i].base = base;
    rt_objs[i].size = size;
    rt_objs[i].peak_res = (size_t*)calloc(count, sizeof(size_t));
    rt_objs[i].peak_zero = (size_t*)calloc(count, sizeof(size_t));
    rt_objs[i].peak_nonzero = (size_t*)calloc(count, sizeof(size_t));
    rt_objs[i].live = 1;
    __atomic_store_n(&rt_nobjs, i + 1, __ATOMIC_RELEASE);
    return i;
}

// An object of a layout TYPE at base. Returns its index or -1.
__attribute__((unused)) static int rt_register(const char* type, const void* base) {
    int type_idx = rt_type_index(type);
    return type_idx < 0 ? -1 : rt_add(type_idx, type, (uintptr_t)base, rt_types[type_idx].size);
}

// A plain range (a publisher's storage, a server's cache), counted as one table.
__attribute__((unused)) static int rt_register_range(const char* name, const void* base, size_t size) {
    return rt_add(-1, name, (uintptr_t)base, size);
}

// pagemap flags of the pages covering [low, high): bit 0 resident (present + exclusive), bit 1 zero-mapped/shared.
// Recursive only as rss_tables_preload.c builds it, where its calloc()/free() are the interposed ones.
static unsigned char* rt_page_flags(uintptr_t low, uintptr_t high, uintptr_t* first_page) { // NOLINT(misc-no-recursion)
    uintptr_t first = low & ~(RT_PAGE - 1);
    size_t pages = (high - first + RT_PAGE - 1) / RT_PAGE;
    unsigned char* flags = (unsigned char*)calloc(pages, 1);
    uint64_t* ent = (uint64_t*)calloc(pages, sizeof(uint64_t));
    int pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
    *first_page = first;
    if (flags == NULL || ent == NULL || pagemap_fd < 0) {
        free(ent);
        if (pagemap_fd >= 0) {
            close(pagemap_fd);
        }
        free(flags);
        return NULL;
    }
    ssize_t got = pread(pagemap_fd, ent, pages * sizeof(uint64_t), (off_t)((first / RT_PAGE) * sizeof(uint64_t)));
    close(pagemap_fd);
    if (got != (ssize_t)(pages * sizeof(uint64_t))) {
        free(ent);
        free(flags);
        return NULL;
    }
    for (size_t i = 0; i < pages; i++) {
        int present = (int)((ent[i] >> RT_PAGEMAP_PRESENT_BIT) & 1U);
        int exclusive = (int)((ent[i] >> RT_PAGEMAP_EXCLUSIVE_BIT) & 1U);
        unsigned char flag = 0;
        if (present && exclusive) {
            flag = RT_FLAG_RESIDENT;
        } else if (present) {
            flag = RT_FLAG_ZERO_MAPPED;
        }
        flags[i] = flag;
    }
    free(ent);
    return flags;
}

// Bytes of [low, high) on pages with flag bit `bit`.
static size_t rt_count(const unsigned char* flags, uintptr_t first_page, uintptr_t low, uintptr_t high, int bit) {
    size_t total = 0;
    for (uintptr_t addr = low; addr < high;) {
        uintptr_t page_end = (addr & ~(RT_PAGE - 1)) + RT_PAGE;
        uintptr_t end = page_end < high ? page_end : high;
        if (flags[(addr - first_page) / RT_PAGE] & bit) {
            total += end - addr;
        }
        addr = end;
    }
    return total;
}

// Nonzero bytes of [low, high) on resident pages only: reading a page that is not resident would map it (the zero page)
// and change the next reading. A lower bound on how much of a table holds anything - an entry may be all zero - and a
// table that is resident with almost no nonzero byte was written (zeroed) rather than used.
static size_t rt_nonzero(const unsigned char* flags, uintptr_t first_page, uintptr_t low, uintptr_t high) {
    size_t total = 0;
    for (uintptr_t addr = low; addr < high;) {
        uintptr_t page_end = (addr & ~(RT_PAGE - 1)) + RT_PAGE;
        uintptr_t end = page_end < high ? page_end : high;
        if (flags[(addr - first_page) / RT_PAGE] & RT_FLAG_RESIDENT) {
            // The addresses are a registered object's own: reading its bytes back through them is the measurement.
            const unsigned char* byte = (const unsigned char*)addr; // NOLINT(performance-no-int-to-ptr)
            const unsigned char* stop = (const unsigned char*)end;  // NOLINT(performance-no-int-to-ptr)
            for (; byte < stop; byte++) {
                total += *byte != 0;
            }
        }
        addr = end;
    }
    return total;
}

static void rt_proc_kb(size_t* rss, size_t* anon, size_t* hwm) {
    char line[RT_PROC_LINE_BYTES];
    *rss = *anon = *hwm = 0;
    FILE* file = fopen("/proc/self/smaps_rollup", "r");
    if (file != NULL) {
        while (fgets(line, sizeof(line), file) != NULL) {
            sscanf(line, "Rss: %zu", rss);
            sscanf(line, "Anonymous: %zu", anon);
        }
        fclose(file);
    }
    file = fopen("/proc/self/status", "r");
    if (file != NULL) {
        while (fgets(line, sizeof(line), file) != NULL) {
            sscanf(line, "VmHWM: %zu", hwm);
        }
        fclose(file);
    }
}

// How many field slots an object has: its type's fields (rt_nfields, filtered by type), or one for a plain range.
static int rt_field_slots(const struct rt_obj* obj) {
    return obj->type < 0 ? 1 : rt_nfields;
}

// A freed object: its peaks stand as the reading.
static void rt_print_freed(FILE* out, const char* label, const struct rt_obj* obj) {
    for (int fld = 0; fld < rt_field_slots(obj); fld++) {
        if (obj->type >= 0 && rt_fields[fld].type != obj->type) {
            continue;
        }
        fprintf(out,
                "RT label=%s obj=%s field=%s off=%zu size=%zu resident=freed zero_mapped=freed "
                "peak_resident=%zu nonzero=freed peak_nonzero=%zu\n",
                label, obj->name, obj->type < 0 ? "(range)" : rt_fields[fld].path,
                obj->type < 0 ? 0 : rt_fields[fld].off, obj->type < 0 ? obj->size : rt_fields[fld].size,
                obj->peak_res[fld], obj->peak_nonzero[fld]);
    }
}

// A live object: reads each field now, raises its peaks and, when out is not NULL, prints the reading.
static void rt_read_live(FILE* out, const char* label, struct rt_obj* obj) {
    uintptr_t first_page = 0;
    unsigned char* flags = rt_page_flags(obj->base, obj->base + obj->size, &first_page);
    if (flags == NULL) {
        return;
    }
    for (int fld = 0; fld < rt_field_slots(obj); fld++) {
        size_t off = 0;
        size_t size = obj->size;
        const char* path = "(range)";
        if (obj->type >= 0) {
            if (rt_fields[fld].type != obj->type) {
                continue;
            }
            off = rt_fields[fld].off;
            size = rt_fields[fld].size;
            path = rt_fields[fld].path;
        }
        uintptr_t low = obj->base + off;
        size_t res = rt_count(flags, first_page, low, low + size, RT_FLAG_RESIDENT);
        size_t zero = rt_count(flags, first_page, low, low + size, RT_FLAG_ZERO_MAPPED);
        size_t nonzero = rt_nonzero(flags, first_page, low, low + size);
        if (nonzero > obj->peak_nonzero[fld]) {
            obj->peak_nonzero[fld] = nonzero;
        }
        if (res > obj->peak_res[fld]) {
            obj->peak_res[fld] = res;
        }
        if (zero > obj->peak_zero[fld]) {
            obj->peak_zero[fld] = zero;
        }
        if (out != NULL) {
            fprintf(out,
                    "RT label=%s obj=%s field=%s off=%zu size=%zu resident=%zu zero_mapped=%zu "
                    "peak_resident=%zu nonzero=%zu peak_nonzero=%zu\n",
                    label, obj->name, path, off, size, res, zero, obj->peak_res[fld], nonzero, obj->peak_nonzero[fld]);
        }
    }
    free(flags);
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
    int nobjs = __atomic_load_n(&rt_nobjs, __ATOMIC_ACQUIRE);
    for (int i = 0; i < nobjs; i++) {
        struct rt_obj* obj = &rt_objs[i];
        if (!obj->live) {
            if (out != NULL) {
                rt_print_freed(out, label, obj);
            }
            continue;
        }
        rt_read_live(out, label, obj);
    }
    if (out != NULL) {
        fflush(out);
    }
}
