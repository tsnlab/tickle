#!/usr/bin/env python3
"""p_mtu's bias from samples lost whole (2026-10-17): adds one scenario, cfp_pmtu, to a sweep tree that
collapse_fp_patch.py (branch ab/collapse-test-999) has built from a tree with test_p_mtu_counts_samples_lost_whole's
helpers (pmtu_stream(), the link counters). Run it by naming it in $CFP_TESTS, seeded by $CFP_SEED.

  pmtu_bias_patch.py <sweep_tree>     (the <out>/tree collapse_fp_patch.py wrote)

cfp_pmtu streams $PMTU_LEN-byte best-effort samples at 100 Hz for $PMTU_SECS s through $PMTU_PPM frame loss, the writer
in forced IP at n <= $PMTU_BOUND (0: the ceiling's) and the reader's cap off (pmtu_stream()), or with $PMTU_AUTO=1 the
writer in AUTO and the reader's cap on (what a deployment runs). It prints one line:

  PMTU_R ppm=.. len=.. bound=.. auto=.. secs=.. seed=.. n=<largest n sent> reader=<events>/<frames>
         link=<lost>/<sent> runs_reader=<lost>/<sent> runs_link=<lost>/<sent> missed=<samples counted lost whole>
         collapses=..

Reading rule (pmtu_bias_read.py): the bias of p_mtu is reader rate / link rate, pooled over seeds (sum of events over
sum of frames, each side). The link's own count is the truth of the same draws, so the ratio has no sampling error of
the link's: it is exactly 1 for an estimator that counts every MTU unit sent, and below 1 by the share of tails lost
with their runs for one that cannot see a sample lost whole. Pre-registered: the fix passes if every cell's ratio is
within 0.5% of 1 (the stream's first and last samples are the only ones it may miss) and main's one-run cells are below
1 by about L(n) (0.1%: ~0.99, 1%: ~0.90, 5%: ~0.57). A fix cell outside 0.5% falsifies "the inference counts every
sample lost whole".
"""
import os
import sys

tree = sys.argv[1]
p = os.path.join(tree, "tests", "test_hybrid_send.c")
s = open(p).read()


def replace_once(text, old, new):
    assert text.count(old) == 1, "anchor not found once: " + old[:80]
    return text.replace(old, new)


assert "static struct pmtu_result pmtu_stream(" in s, "the tree has no pmtu_stream(): build it from the fix branch's tests"
scenario = r'''
static void cfp_pmtu(void) {
    struct pmtu_arm arm = {cfp_env("PMTU_LEN", 16384), cfp_env("PMTU_BOUND", 0), cfp_env("PMTU_PPM", 10000), 0,
                           cfp_env("PMTU_SECS", 60)};
    uint32_t largest = 0;
    struct pmtu_result got;
    if (cfp_env("PMTU_AUTO", 0) != 0) {
        init_one(arm.len, FULL_CEILING, false);
        sim_start(arm.ppm);
        tick_readers(test_mock_now);
        uint8_t seed = 1;
        largest = sim_stream(arm.seconds, 100, &seed);
        const struct tt_HybridState* h = &readers[0].ctx.hybrid;
        got = (struct pmtu_result) {h->mtu_loss.frames, h->mtu_loss.events, h->rx_writers[0].runs.sent,
                                    h->rx_writers[0].runs.lost, largest};
    } else {
        got = pmtu_stream(&arm);
        largest = got.largest;
    }
    const struct tt_HybridState* hybrid = &readers[0].ctx.hybrid;
#ifdef PMTU_HAS_MISSED
    unsigned long missed = (unsigned long)hybrid->missed_samples;
#else
    unsigned long missed = 0;
#endif
    printf("PMTU_R ppm=%u len=%u bound=%u auto=%u secs=%u seed=%s n=%u reader=%u/%u link=%u/%u runs_reader=%u/%u "
           "runs_link=%u/%u missed=%lu collapses=%lu\n",
           arm.ppm, arm.len, arm.bound, cfp_env("PMTU_AUTO", 0), arm.seconds,
           getenv("CFP_SEED") != NULL ? getenv("CFP_SEED") : "11", largest, got.events, got.frames, link.mtu_lost,
           link.mtu, got.runs_lost, got.runs, link.runs_lost, link.runs, missed,
           (unsigned long)hybrid->collapses);
}

int main(void) {
    const char* want = getenv("CFP_TESTS");'''
s = replace_once(s, '''
int main(void) {
    const char* want = getenv("CFP_TESTS");''', scenario)
s = replace_once(s, '''        {"cfp_squeeze", cfp_squeeze},
''', '''        {"cfp_squeeze", cfp_squeeze},
        {"cfp_pmtu", cfp_pmtu},
''')
if "missed_samples" in open(os.path.join(tree, "include", "tickle", "tickle.h")).read():
    s = "#define PMTU_HAS_MISSED 1\n" + s
open(p, "w").write(s)
print("PMTU_PATCHED")
