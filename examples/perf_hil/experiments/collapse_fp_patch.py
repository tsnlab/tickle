#!/usr/bin/env python3
"""Builds the collapse-false-positive sweep tree (2026-10-17): a copy of <tree>'s src/, include/ and tests/ under
<out>/tree with three changes, none of them in the source tree itself.

1. tests/test_hybrid_send.c: the link's loss draws seeded from $CFP_SEED (sim_rng, 11 in the test); main() runs only the
   test functions named in $CFP_TESTS (comma list; all when unset) and prints "CFP_T <name> <failures> <ms>" after
   each; two scenario functions are added, cfp_plain() and cfp_squeeze() (below), run by naming them in $CFP_TESTS.
2. src/tickle.c: hybrid_collapsed() and hybrid_clean() take an option from $CFP_OPT (0 = main's test, unchanged), and
   hybrid_control() prints one "CFP_D" line per decision with what it was decided on.
3. Nothing else: the law, the estimators and the sim are main's.

Options ($CFP_OPT):
  0  main: p_hi the 95% Poisson upper bound of p_mtu, z >= 3.09, ratio 2, at least 4 lost of 16 runs.
  1  bound999: p_hi the 99.9% bound in the collapse test only (the clean test keeps 95%).
  2  minev: as 0, and no collapse while p_mtu rests on fewer than 3 events.
  3  z4: as 0 with z >= 4.0.
  4  confirm2: as 0, and a collapse only on the second positive window in a row (a fresh window each).
  5  bound999 + confirm2.
  6  bound999 in both the collapse and the clean test.
  7  z4.5.
  8  main + "far" read on the survival side too: X / R >= min(2 L0, (1 + L0) / 2), i.e. q <= q0 / 2 where 2 L0 > 1.
  9  bound999 + 8.
 10  bound999 + 8 + z4.
 11  z at the 99.9% bound, "far" (with 8's survival side) at the 95% one.
 12  11 + z4.

CFP_KEEP_LAW=1 keeps the tree's own hybrid_collapsed() and hybrid_clean() (traced, z and L0 printed as 0).

Reading rules: see collapse_fp_read.py. The patch asserts every anchor it replaces occurs exactly once.
"""
import os
import re
import shutil
import sys

tree, out = sys.argv[1], sys.argv[2]
dst = os.path.join(out, "tree")
if os.path.exists(dst):
    shutil.rmtree(dst)
for d in ("src", "include", "tests"):
    shutil.copytree(os.path.join(tree, d), os.path.join(dst, d))


def replace_once(text, old, new):
    assert text.count(old) == 1, "anchor not found once: " + old[:80]
    return text.replace(old, new)


# ---- src/tickle.c -------------------------------------------------------------------------------------------------
p = os.path.join(dst, "src", "tickle.c")
s = open(p).read()
start = s.index("static bool hybrid_collapsed(const struct tt_RunWindow* window, const struct tt_LossEstimate* mtu) {")
end = s.index("\n}\n", s.index("static bool hybrid_clean(", start)) + 3
law = r'''
static const char* cfp_test = "-";
static int cfp_opt_value = -1;
static double cfp_z;
static double cfp_l0;
static double cfp_phi;
static int cfp_opt(void) {
    if (cfp_opt_value < 0) {
        const char* e = getenv("CFP_OPT");
        cfp_opt_value = e != NULL ? atoi(e) : 0;
    }
    return cfp_opt_value;
}
static double cfp_upper999(uint32_t k) {
    static const double exact[11] = {6.908, 9.233, 11.229, 13.062, 14.794, 16.455, 18.062, 19.626, 21.156, 22.657,
                                     24.134};
    if (k < 11U) {
        return exact[k];
    }
    double sh = (double)k + 1.0;
    double c = 1.0 - (1.0 / (9.0 * sh)) + (3.0902 / (3.0 * hybrid_sqrt(sh)));
    return sh * c * c * c;
}
static double cfp_baseline(const struct tt_RunWindow* window, const struct tt_LossEstimate* mtu, bool strict) {
    double size = window->sent != 0 ? (double)window->frames / (double)window->sent : 1.0;
    double p_hi = loss_estimate_p(mtu);
    if (strict) {
        p_hi = mtu->frames == 0 ? 1.0 : cfp_upper999(mtu->events) / (double)mtu->frames;
        p_hi = p_hi < 1.0 ? p_hi : 1.0;
    }
    cfp_phi = p_hi;
    double clean = 1.0;
    for (uint32_t i = 0; i < (uint32_t)size; i++) {
        clean *= 1.0 - p_hi;
    }
    double part = size - (double)(uint32_t)size;
    clean *= 1.0 - (p_hi * part);
    return 1.0 - clean;
}
static bool hybrid_collapsed(const struct tt_RunWindow* window, const struct tt_LossEstimate* mtu) {
    int o = cfp_opt();
    cfp_z = 0.0;
    cfp_l0 = cfp_baseline(window, mtu, o == 1 || o == 5 || o == 6 || o == 9 || o == 10 || o == 11 || o == 12);
    if (window->sent < tt_HYBRID_COLLAPSE_MIN_RUNS || window->lost < tt_HYBRID_COLLAPSE_MIN_LOST) {
        return false;
    }
    double runs = (double)window->sent;
    double lost = (double)window->lost;
    double baseline = cfp_l0;
    double variance = runs * baseline * (1.0 - baseline);
    if (variance <= 0.0) {
        return false;
    }
    double score = (lost - (runs * baseline) - HYBRID_HALF) / hybrid_sqrt(variance);
    cfp_z = score;
    if (o == 2 && mtu->events < 3U) {
        return false;
    }
    double zc = o == 3 || o == 10 || o == 12 ? 4.0 : (o == 7 ? 4.5 : tt_HYBRID_COLLAPSE_Z);
    if (o == 11 || o == 12) { // significance at the 99.9% bound, the size of the excess at the 95% one
        baseline = cfp_baseline(window, mtu, false);
    }
    double far = tt_HYBRID_COLLAPSE_RATIO * baseline;
    if (o >= 8) { // "far below (1 - p)^n" on the survival side too: q at most half the q0 predicted
        double half_survival = 1.0 - ((1.0 - baseline) / tt_HYBRID_COLLAPSE_RATIO);
        far = far < half_survival ? far : half_survival;
    }
    return score >= zc && lost >= far * runs;
}

static bool hybrid_clean(const struct tt_RunWindow* window, const struct tt_LossEstimate* mtu) {
    return window->sent >= tt_HYBRID_COLLAPSE_MIN_RUNS &&
           (double)window->lost <= cfp_baseline(window, mtu, cfp_opt() == 6) * (double)window->sent;
}

static const void* cfp_pending_of[16];
static bool cfp_pending(const void* h, int set) { // set: -1 read, 0 clear, 1 set
    int free_slot = -1;
    for (int i = 0; i < 16; i++) {
        if (cfp_pending_of[i] == h) {
            if (set == 0) {
                cfp_pending_of[i] = NULL;
            }
            return true;
        }
        if (cfp_pending_of[i] == NULL && free_slot < 0) {
            free_slot = i;
        }
    }
    if (set == 1 && free_slot >= 0) {
        cfp_pending_of[free_slot] = h;
    }
    return false;
}
'''
if os.environ.get("CFP_KEEP_LAW"):  # the tree's own collapse test, traced (the after-sweep of an implementation)
    a = law.index("static bool hybrid_collapsed(")
    b = law.index("static const void* cfp_pending_of[16];")
    s = s[:start] + law[:a] + law[b:] + s[start:]
else:
    s = s[:start] + law + s[end:]

old_control = '''    bool decided = true;
    if (hybrid_collapsed(&hybrid->window, &hybrid->mtu_loss)) {
        hybrid_collapse(hybrid, now, units_per_s);
    } else if (hybrid_clean(&hybrid->window, &hybrid->mtu_loss) || hybrid->cap < 2U) {
        // A cap of one unit allows no run, so no window can ever be clean there: time alone moves it on - to its probe.
        hybrid_grow(hybrid, now, n_max);
    } else {
        decided = hybrid->window.sent >= tt_HYBRID_WINDOW_MAX_RUNS; // undecided: gather more, up to a bound
    }
'''
new_control = '''    bool decided = true;
    char verdict = 'U';
    uint32_t cap_before = hybrid->cap;
    struct tt_RunWindow seen = hybrid->window;
    struct tt_LossEstimate mtu_seen = hybrid->mtu_loss;
    uint32_t wn = hybrid->window_n;
    bool confirm = cfp_opt() == 4 || cfp_opt() == 5;
    if (hybrid_collapsed(&hybrid->window, &hybrid->mtu_loss)) {
        if (confirm && !cfp_pending(hybrid, -1)) {
            (void)cfp_pending(hybrid, 1);
            verdict = 'P';
        } else {
            (void)cfp_pending(hybrid, 0);
            hybrid_collapse(hybrid, now, units_per_s);
            verdict = 'C';
        }
    } else if (hybrid_clean(&hybrid->window, &hybrid->mtu_loss) || hybrid->cap < 2U) {
        // A cap of one unit allows no run, so no window can ever be clean there: time alone moves it on - to its probe.
        (void)cfp_pending(hybrid, 0);
        hybrid_grow(hybrid, now, n_max);
        verdict = 'G';
    } else {
        decided = hybrid->window.sent >= tt_HYBRID_WINDOW_MAX_RUNS; // undecided: gather more, up to a bound
        verdict = decided ? 'X' : 'U';
    }
    if (seen.sent != 0 || verdict == 'C') {
        fprintf(stderr, "CFP_D %s %.3f cap=%u->%u wn=%u sent=%u lost=%u fr=%u ev=%u mf=%u phi=%.4g L0=%.4g z=%.2f v=%c\\n",
                cfp_test, (double)now / 1e9, (unsigned)cap_before, (unsigned)hybrid->cap, (unsigned)wn,
                (unsigned)seen.sent, (unsigned)seen.lost, (unsigned)seen.frames, (unsigned)mtu_seen.events,
                (unsigned)mtu_seen.frames, cfp_phi, cfp_l0, cfp_z, verdict);
    }
'''
s = replace_once(s, old_control, new_control)
open(p, "w").write(s)

# ---- tests/test_hybrid_send.c -------------------------------------------------------------------------------------
p = os.path.join(dst, "tests", "test_hybrid_send.c")
s = open(p).read()
s = replace_once(s, "    sim_rng = 11;\n", "    sim_rng = getenv(\"CFP_SEED\") != NULL ? (uint32_t)atoi(getenv(\"CFP_SEED\")) : 11U;\n")
old_deliver = """static void sim_deliver(struct sim_datagram* d) {
    if (!sim_lost(d)) {"""
new_deliver = r"""static void sim_deliver(struct sim_datagram* d) {
    bool cfp_lost = sim_lost(d);
    if (cfp_lost && d->from != SENDER_ID) { // a reader's datagram lost: what the writer never heard
        fprintf(stderr, "CFP_L %.3f from=%u len=%u\n", (double)test_mock_now / 1e9, (unsigned)d->from, (unsigned)d->len);
    }
    if (!cfp_lost) {"""
s = replace_once(s, old_deliver, new_deliver)
m = re.search(r"int main\(void\) \{\n((?:    test_\w+\(\);\n)+)", s)
assert m, "main's test list not found"
names = re.findall(r"    (test_\w+)\(\);\n", m.group(1))
scen = r'''
#include <time.h>
static uint64_t cfp_wall_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static uint32_t cfp_env(const char* name, uint32_t fallback) {
    const char* e = getenv(name);
    return e != NULL ? (uint32_t)strtoul(e, NULL, 10) : fallback;
}

// Plain frame loss: $CFP_LOSS ppm, $CFP_SIZE-byte best-effort samples at $CFP_HZ for $CFP_SECS s, no squeeze.
// The cap must never move: every collapse is false.
static void cfp_plain(void) {
    init_one(cfp_env("CFP_SIZE", ONE_MB), FULL_CEILING, false);
    sim_start(cfp_env("CFP_LOSS", 1000));
    tick_readers(test_mock_now);
    uint8_t seed = 1;
    struct tt_HybridState* hybrid = &readers[0].ctx.hybrid;
    uint32_t secs = cfp_env("CFP_SECS", 120);
    for (uint32_t i = 0; i < secs; i++) {
        (void)sim_stream(1, cfp_env("CFP_HZ", 30), &seed);
    }
    printf("CFP_R plain loss=%u size=%u hz=%u secs=%u collapses=%lu probes=%lu cap=%u ticks=%lu path_failures=%lu "
           "refused=%u delivered=%d\n",
           cfp_env("CFP_LOSS", 1000), cfp_env("CFP_SIZE", ONE_MB), cfp_env("CFP_HZ", 30), secs,
           (unsigned long)hybrid->collapses, (unsigned long)hybrid->probes, (unsigned)hybrid->cap,
           (unsigned long)hybrid->ticks, (unsigned long)hybrid->path_failures, sim_reasm_refused,
           readers[0].delivered);
}

// A real collapse: test_a_reassembly_squeeze_end_to_end's link (1 MB best effort at 30 Hz, 4 MB reassembly memory,
// $CFP_LIFE s lifetime) at $CFP_LOSS ppm. Reports the second the memory first refused a run and the second the
// collapse was called (0: not within $CFP_SECS).
static void cfp_squeeze(void) {
    init_one(ONE_MB, FULL_CEILING, false);
    sim_start(cfp_env("CFP_LOSS", 1000));
    sim_reasm_threshold = 4194304U;
    sim_reasm_lifetime = (uint64_t)cfp_env("CFP_LIFE", 30) * tt_SECOND;
    struct tt_HybridState* hybrid = &readers[0].ctx.hybrid;
    tick_readers(test_mock_now);
    uint8_t seed = 1;
    uint32_t secs = cfp_env("CFP_SECS", 60);
    uint32_t refused_at = 0;
    uint32_t collapsed_at = 0;
    uint32_t refused_when_collapsed = 0;
    for (uint32_t i = 1; i <= secs && collapsed_at == 0; i++) {
        (void)sim_stream(1, 30, &seed);
        if (refused_at == 0 && sim_reasm_refused > 0) {
            refused_at = i;
        }
        if (collapsed_at == 0 && hybrid->collapses > 0) {
            collapsed_at = i;
            refused_when_collapsed = sim_reasm_refused;
        }
    }
    printf("CFP_R squeeze loss=%u life=%u refused_at=%u collapsed_at=%u refused_then=%u cap=%u n_fail=%u "
           "path_failures=%lu\n",
           cfp_env("CFP_LOSS", 1000), cfp_env("CFP_LIFE", 30), refused_at, collapsed_at, refused_when_collapsed,
           (unsigned)hybrid->cap, (unsigned)hybrid->n_fail, (unsigned long)hybrid->path_failures);
}

int main(void) {
    const char* want = getenv("CFP_TESTS");
    static const struct {
        const char* name;
        void (*fn)(void);
    } table[] = {
''' + "".join('        {"%s", %s},\n' % (n, n) for n in names) + r'''        {"cfp_plain", cfp_plain},
        {"cfp_squeeze", cfp_squeeze},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (want != NULL) {
            const char* hit = strstr(want, table[i].name);
            size_t len = strlen(table[i].name);
            if (hit == NULL || (hit != want && hit[-1] != ',') || (hit[len] != '\0' && hit[len] != ',')) {
                continue;
            }
        } else if (strncmp(table[i].name, "cfp_", 4) == 0) {
            continue;
        }
        int before = test_failures;
        uint64_t t0 = cfp_wall_ns();
        cfp_test = table[i].name;
        table[i].fn();
        fflush(stdout);
        fprintf(stderr, "CFP_T %s %d %llu\n", table[i].name, test_failures - before,
                (unsigned long long)((cfp_wall_ns() - t0) / 1000000U));
    }
    free(sim_queue);
    return test_result() != 0 ? 1 : 0;
}

int cfp_unused_main(void) {
'''
s = s[:m.start()] + scen + s[m.end():]
open(p, "w").write(s)
print("CFP_PATCHED", len(names), "tests")
