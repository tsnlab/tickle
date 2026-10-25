#!/usr/bin/env bash
# k5_floor_ab.sh - the K candidate's 2x-resend reps (RESULTS `R4K`, "K at 10 reps") and the transit floor that is to
# stop them, PC netns. Written and committed 2026-10-26 before the floor's first netns run.
#
# MECHANISM (traced before this harness, ~/k5_results/trace1_*, k5_ambig.py / k5_transit_min.py): a reader's probe on
# its watermark starts at the first request naming the watermark, but a 1 MiB sample's missing unit has nearly always
# been named before, while it lay above the watermark, and its repair may already be on its way. Every recovery Karn's
# rule timed for a large writer was such a probe; some end 0.04-3 ms after their request, against a least repair
# transit of 4.1-4.7 ms. Main's default rule also times retried (long) recoveries, which outweigh them; Karn's rule (K)
# keeps only the short ones, and when they come first they hold the interval down: timer ACKNACKs 6,000-12,000 a rep
# against ~200, ~2x the resends (the writer's RACK and timeout release resends for units still on their way).
# THE FIX (KF, branch k5-floor): such a recovery is folded as no faster than the least repair transit the reader has
# timed (path_recovery_sample(), tt_WriterProxy.transit_min_ns) - large writers on repair_timing links only.
#
# Trees, all at once (wifi_repair_ab.sh): M = main, A = main in a second checkout (the A/A control), K = main + K,
# KF = K + the floor. PROFILE=wifi TT_UDP_OFFLOAD=0 LARGE_RSTATS=1, as in R4K.
# Phases: P1 rel_large ge5b2 ge5b4, REPS1 (20) reps - the 2x reps; P2 rel_small+rel_large ctl ge10b4, REPS2 (6) -
# R4 (the rel_small ctl gives base_rtt; without it R4 is VOID); P3 rel_large bc tv, REPS3 (5) - no WORSE.
#
# PRE-REGISTERED READING (k5_floor_read.py implements it; P1 pools ge5b2 and ge5b4, per tree 2 x REPS1 reps):
#   HIGH rep: writer resends > 1.5 x the median of M's reps on that arm (the 2x reps were 1.9-2.3 x).
#   COLLAPSE rep: the reader's timer ACKNACKs > 5 x the median of M's reps on that arm (the mechanism's own count).
#   STORM rep: writer resends > 10 x M's median.
#   CONTROL: A vs M, HIGH and COLLAPSE counts, Fisher two-sided p >= 0.05 -> PASS; else the run is VOID.
#   MECHANISM: KF COLLAPSE < K COLLAPSE, Fisher one-sided p < 0.05 -> SHOWN; KF COLLAPSE >= K COLLAPSE with K >= 3 ->
#     FALSIFIED (the floor does not hold the estimate up); otherwise UNDECIDED (K collapsed too rarely in this run).
#   OUTCOME (the 2x-rep rate): KF HIGH < K HIGH, one-sided p < 0.05, and KF HIGH <= max(M, A) + 1 -> FIXED;
#     KF HIGH > max(M, A) + 1 -> NOT FIXED; otherwise UNDECIDED. Expected power: K ~ 4/23 = 17% a rep on ge5b2
#     (history), so ~4-7 HIGH in 40 reps; 0 for KF would give p ~ 0.006-0.06. The history (K 4/23, M+A 0/46) is
#     printed beside it, never pooled into the verdict.
#   R4: KF's ge10b4 rel_large pooled one-way p50 <= the harness's R4 limit (ctl p50 + 4 x base_rtt, its R4 line) ->
#     PASS; and KF's median per-rep p50 - K's <= max(2 ms, |A - M|) -> KEEPS K's GAIN; else LOSES IT (FALSIFIED: the
#     floor costs what K bought).
#   NO WORSE: wifi_repair_ab.sh's VERDICT KF vs M on every arm and cell NOT WORSE (a WORSE that A vs M shows too is
#     noise, listed); KF's median writer resends on each rel_large arm <= 1.10 x max(M, A); KF no STORM rep.
#   LAND the floor with K only if CONTROL PASS, MECHANISM SHOWN, OUTCOME FIXED, R4 PASS and KEEPS, NO WORSE.
#   VOID: a tree with fewer than 0.8 x the planned reps on an arm.
#
# EXTENSION (registered 2026-10-26 04:30, after P1 of run k5ab and before the extension's run): P1 read K HIGH 3/40 and
#   COLLAPSE 4/40 against KF 0/40 and 0/40 - the power the header expected at its low end (one-sided p 0.12 / 0.06).
#   P1X = P1 again, 20 more reps a tree on both arms, the same four trees at once (EXTEND=1: P1 only, tag <TAG>x).
#   k5_floor_read.py with K5_P1_EXTRA=<TAG>x re-reads MECHANISM and OUTCOME on the pooled 80 reps a tree by the same
#   rules (references from M's pooled reps). The 40-rep reading stays the registered one and is reported beside it;
#   the pooled one is a second look (optional stopping: read its p as nominal).
#
# THE DISCARD VARIANT (registered 2026-10-26 04:53, before its run; tag k5d, KF= the k5-discard tree, the other three as
#   before). Seen in k5ab, not registered there: the clamp gave back K's tail - median per-rep p99 at ge5b2 M 254, K 181,
#   KF 247 ms (ge5b4 256 / 182 / 256, bc 99 / 77 / 102). Traced (trace2_KFT, k5_sample_lat.py, k5_floor_traj.py): its
#   slow samples are the rep's first ~20; the seed probe ends at ~0.04 s, inside the first sample's drain, when the
#   least transit is still 20-36 ms (~5 ms by the next probe), and the seed raised to it held the interval long for
#   ~2 s. The variant leaves a sub-floor recovery untimed instead (path_sample_too_short()): the seed is the first
#   recovery no faster than any repair, here the next probe (5.7-6.8 ms at ~0.3 s).
#   Every rule above applies to it unchanged, plus TAIL (k5_floor_read.py): on rel_large ge5b2, ge5b4 (P1) and bc, tv
#   (P3), KF's median per-rep p99 <= K's + (10% of K's + |A - M|) -> KEEPS, else LOSES; LAND also needs TAIL KEEPS on
#   every arm. FALSIFIED if KF COLLAPSE >= K COLLAPSE with K >= 3 (as before), or TAIL LOSES at ge5b2 or ge5b4 (the
#   start-up was not the cause).
#
# Usage: M=dir A=dir K=dir KF=dir [REPS1=20] [REPS2=6] [REPS3=5] [OUT=~/k5_results] TAG=name k5_floor_ab.sh
#   Output: $OUT/<TAG>_p{1,2,3}_* (wifi_repair_ab.sh) and $OUT/<TAG>.verdict.txt, which ends "=== k5_floor_ab ended".
set -uo pipefail
TAG=${TAG:?TAG names the output}
OUT=${OUT:-$HOME/k5_results}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT" || exit 1
VERDICT="$OUT/$TAG.verdict.txt"
on_exit() {
    local rc=$?
    echo "=== k5_floor_ab ended rc=$rc $(date -Is) ===" >>"$VERDICT"
}
trap on_exit EXIT
echo "=== k5_floor_ab $(date -Is) tag=$TAG M=$M A=$A K=$K KF=$KF" >"$VERDICT"
TREES="M=$M A=$A K=$K KF=$KF"
export PROFILE=wifi NODE_ENV=TT_UDP_OFFLOAD=0 LARGE_RSTATS=1 BASE_OUT="$OUT"
TREES=$TREES TAG=${TAG}_p1 REPS=${REPS1:-20} CELLS=rel_large ARMS="ge5b2 ge5b4" "$HERE/wifi_repair_ab.sh"
echo "p1 done $(date -Is)" >>"$VERDICT"
[ "${EXTEND:-0}" = 1 ] && exit 0 # the P1 extension (header): P1 only
TREES=$TREES TAG=${TAG}_p2 REPS=${REPS2:-6} CELLS="rel_small rel_large" ARMS="ctl ge10b4" "$HERE/wifi_repair_ab.sh"
echo "p2 done $(date -Is)" >>"$VERDICT"
TREES=$TREES TAG=${TAG}_p3 REPS=${REPS3:-5} CELLS=rel_large ARMS="bc tv" "$HERE/wifi_repair_ab.sh"
echo "p3 done $(date -Is)" >>"$VERDICT"
python3 "$HERE/k5_floor_read.py" "$OUT" "$TAG" "${REPS1:-20}" "${REPS2:-6}" "${REPS3:-5}" >>"$VERDICT" 2>&1
