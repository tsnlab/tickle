// Checks BenchStats.h's per-thread CPU instrument (sched_cpu_s, sched_by_thread=,
// sched_unattributed_s) against two cases it must tell apart. Added 2026-09-28 for WIRE_PLAN section
// 10's open p1 client CPU question: getrusage's utime/stime are tick-accounted, so a 0.5-0.7%
// difference is inside the tick, and the campaign needed an instrument that resolves it and says
// which thread the time went to.
//
// Both arms and what each outcome means are in bench_sched_check.sh, which runs them. The worker
// names itself with prctl() rather than pthread_setname_np(), which would need _GNU_SOURCE and so a
// define the project's own lint does not pass.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/prctl.h>

#include "BenchStats.h" // NOLINT(misc-include-cleaner) - the header under test; -I its directory

// The worker burns this much CPU before it starts waiting to be stopped, and main burns this much
// before it takes the end snapshot. Both are far above the accounting tick and short enough that the
// check costs a second: the arms are about which thread the time lands on, not how much there is.
#define WORKER_BURN_S 0.30
#define MAIN_BURN_S 0.60
#define IDLE_POLL_BURN_S 0.01
// Arithmetic per timing check, chosen so the clock is read often enough to stop on time and rarely
// enough not to dominate what is being measured.
#define BURN_ITERATIONS 100000
#define BURN_MULTIPLIER 1.000001
#define NS_PER_S 1e9
// A plausible campaign row, so the per-Msample fields are exercised rather than divided by zero.
#define CHECK_SAMPLES 1000000
#define CHECK_SAMPLE_BYTES 64

static char g_fields[BENCH_STATS_FIELDS_MAX];
static volatile double g_sink = 0.0;
static volatile int g_stop = 0;

// Spends `seconds` of CPU in userspace. Not a sleep: the point is to put measurable, attributable
// time on this thread and no other.
static void burn(double seconds) {
    struct timespec started;
    struct timespec now;
    double elapsed = 0.0;

    clock_gettime(CLOCK_MONOTONIC, &started); // NOLINT(misc-include-cleaner) - time.h, above
    do {
        for (int i = 0; i < BURN_ITERATIONS; i++) {
            g_sink += (double)i * BURN_MULTIPLIER;
        }
        clock_gettime(CLOCK_MONOTONIC, &now); // NOLINT(misc-include-cleaner)
        elapsed = (double)(now.tv_sec - started.tv_sec) + ((double)(now.tv_nsec - started.tv_nsec) / NS_PER_S);
    } while (elapsed < seconds);
}

static void* worker(void* arg) {              // NOLINT(misc-include-cleaner) - pthread.h, above
    prctl(PR_SET_NAME, "bs_worker", 0, 0, 0); // NOLINT(misc-include-cleaner) - sys/prctl.h, above
    burn(*(const double*)arg);
    while (g_stop == 0) {
        burn(IDLE_POLL_BURN_S); // the `live` arm keeps this thread on the CPU until main says stop
    }
    return NULL;
}

int main(void) {
    struct BenchStats stats;
    pthread_t worker_thread; // NOLINT(misc-include-cleaner)
    double worker_burn_s = WORKER_BURN_S;
    const char* arm = (getenv("ARM") != NULL) ? getenv("ARM") : "live";
    const int live = (strcmp(arm, "live") == 0) ? 1 : 0;
    const int startup = (strcmp(arm, "startup") == 0) ? 1 : 0;

    if (startup != 0) {
        burn(MAIN_BURN_S); // before the window opens, so no counter inside it may account for this
    }
    bench_stats_begin(&stats);
    if (pthread_create(&worker_thread, NULL, worker, &worker_burn_s) != 0) {
        printf("ARM=%s RESULT: failed reason=pthread_create\n", arm);
        return 1;
    }
    burn(startup != 0 ? IDLE_POLL_BURN_S : MAIN_BURN_S);
    if (live == 0 && startup == 0) {
        // The worker's whole CPU time belongs to a thread neither snapshot can name.
        g_stop = 1;
        pthread_join(worker_thread, NULL);
    }
    bench_stats_end(&stats);
    printf("ARM=%s RESULT: %s\n", arm,
           bench_stats_fields(&stats, BENCH_ROLE_SENDER, CHECK_SAMPLES, CHECK_SAMPLE_BYTES, g_fields, sizeof g_fields));
    if (live != 0 || startup != 0) {
        g_stop = 1;
        pthread_join(worker_thread, NULL);
    }
    return 0;
}
