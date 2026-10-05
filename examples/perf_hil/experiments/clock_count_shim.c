// clock_count_shim.c - counts clock_gettime() calls and where they come from, for perf_publisher_clock.sh.
//
// Why: perf on the rig (2026-10-05) put 24.8% of a same-host p3 publisher's user cycles in [vdso] - the clock - but
// DWARF unwinding stops at the vDSO, so the call graph could not say who reads the clock or how often. This counts
// instead of sampling: every clock_gettime() call goes through here (LD_PRELOAD) and is counted by the address it
// returns to AND the one above it - every core call comes through tt_get_ns(), so one level would name only that. The
// second level needs frame pointers: the client must be built with -fno-omit-frame-pointer. At exit the per-site counts
// are written to $CLOCK_COUNT_OUT as "<module> <offset1-hex> <module> <offset2-hex> <count>", resolved to function
// names afterwards with nm on the same binary.
//
// Build: cc -O2 -fno-omit-frame-pointer -shared -fPIC -o clock_count_shim.so clock_count_shim.c -ldl
#define _GNU_SOURCE // NOLINT(bugprone-reserved-identifier,readability-identifier-naming) - dladdr(), RTLD_NEXT
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SITES 64

static int (*real_clock_gettime)(clockid_t, struct timespec*); // NOLINT(misc-include-cleaner) - <time.h>
static void* site_addr[SITES];
static void* site_up[SITES];
static unsigned long long site_count[SITES];
static unsigned long long other_count; // calls from a site past the table's capacity

// glibc's own parameter names, which readability-inconsistent-declaration-parameter-name holds this to.
// NOLINTBEGIN(bugprone-reserved-identifier,readability-identifier-naming,misc-include-cleaner)
int clock_gettime(clockid_t __clock_id, struct timespec* __tp) {
    // NOLINTEND(bugprone-reserved-identifier,readability-identifier-naming,misc-include-cleaner)
    if (real_clock_gettime == NULL) {
        real_clock_gettime = (int (*)(clockid_t, struct timespec*))dlsym(RTLD_NEXT, "clock_gettime");
    }
    void* from = __builtin_return_address(0);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wframe-address"
    void* caller = __builtin_return_address(1); // valid only with frame pointers in the caller chain (see above)
#pragma GCC diagnostic pop
    int i = 0;
    for (; i < SITES && site_addr[i] != NULL; i++) {
        if (site_addr[i] == from && site_up[i] == caller) {
            break;
        }
    }
    if (i < SITES) {
        site_addr[i] = from; // single-threaded caller in this experiment; a race would only merge two sites
        site_up[i] = caller;
        site_count[i]++;
    } else {
        other_count++;
    }
    return real_clock_gettime(__clock_id, __tp);
}

__attribute__((destructor)) static void clock_count_report(void) {
    const char* path = getenv("CLOCK_COUNT_OUT");
    FILE* out = path != NULL ? fopen(path, "w") : stderr;
    if (out == NULL) {
        return;
    }
    for (int i = 0; i < SITES && site_addr[i] != NULL; i++) {
        void* level[2] = {site_addr[i], site_up[i]};
        for (int j = 0; j < 2; j++) {
            Dl_info info;
            if (level[j] != NULL && dladdr(level[j], &info) != 0 && info.dli_fname != NULL) {
                fprintf(out, "%s %lx ", info.dli_fname,
                        (unsigned long)((uintptr_t)level[j] - (uintptr_t)info.dli_fbase));
            } else {
                fprintf(out, "? %p ", level[j]);
            }
        }
        fprintf(out, "%llu\n", site_count[i]);
    }
    fprintf(out, "OTHER 0 OTHER 0 %llu\n", other_count);
    if (out != stderr) {
        fclose(out);
    }
}
