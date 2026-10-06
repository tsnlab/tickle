// LD_PRELOAD treatment for p4_reorder_firsttouch.sh: before main(), populate every private writable mapping
// of the process (the executable's .data/.bss, where the benches keep their static reorder rings, and the
// libraries' own) with MADV_POPULATE_WRITE. The pages become present and writable without their contents
// changing, so the run that follows pays no first-touch page fault on them. Nothing else is altered: the
// shared-memory segment is mapped later, by core, and is not touched by this.
//
// It reports what it did on stderr as one line, "PREFAULT populated_bytes=N mappings=M failed=F minflt_after=K", so an
// arm that silently applied nothing (old kernel, no maps) cannot pass for a treated arm: the harness requires the line
// and a populated_bytes above the reorder ring's size.
//
// Build: cc -O2 -shared -fPIC -o prefault_rw.so prefault_rw.c
#define _GNU_SOURCE // NOLINT(bugprone-reserved-identifier,readability-identifier-naming) - madvise flags
#include <stdio.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/resource.h> // IWYU pragma: keep - struct rusage, getrusage()

#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23
#endif

enum { MAPS_LINE_BYTES = 512 };

__attribute__((constructor)) static void prefault_rw(void) {
    FILE* maps = fopen("/proc/self/maps", "r");
    if (maps == NULL) {
        fprintf(stderr, "PREFAULT populated_bytes=0 mappings=0 failed=1 (no /proc/self/maps)\n");
        return;
    }
    char line[MAPS_LINE_BYTES];
    unsigned long populated = 0;
    unsigned mappings = 0;
    unsigned failed = 0;
    while (fgets(line, sizeof(line), maps) != NULL) {
        unsigned long start = 0;
        unsigned long end = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3) {
            continue;
        }
        if (strcmp(perms, "rw-p") != 0 || strstr(line, "[stack]") != NULL || strstr(line, "[v") != NULL) {
            continue;
        }
        if (madvise((void*)start, end - start, MADV_POPULATE_WRITE) == 0) { // NOLINT(performance-no-int-to-ptr)
            populated += end - start;
            mappings++;
        } else {
            failed++;
        }
    }
    fclose(maps);
    // The faults the population itself took are counted against the process like any other, so the harness needs
    // the count at this point to subtract it: minflt_after is getrusage()'s figure once population is done.
    struct rusage usage; // NOLINT(misc-include-cleaner) - <sys/resource.h> declares it through a glibc bits header
    long minflt_after = getrusage(RUSAGE_SELF, &usage) == 0 ? usage.ru_minflt : -1;
    fprintf(stderr, "PREFAULT populated_bytes=%lu mappings=%u failed=%u minflt_after=%ld\n", populated, mappings,
            failed, minflt_after);
}
