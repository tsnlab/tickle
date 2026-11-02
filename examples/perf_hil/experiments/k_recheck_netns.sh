#!/usr/bin/env bash
# k_recheck_netns.sh - does K still gain anything on current main? (2026-11-02, before its first run)
#
# K = Karn's rule for a writer of large samples under the WIFI profile's repair timing (RESULTS `R4K`, `K5`; the commit
# 648821b1 on k5-base, cherry-picked onto main as ab/k-recheck). In October R4 (rel_large one-way p50 at ge10b4) failed
# on WIFI and K brought p50 100.8-104.1 -> 74.2-74.7 ms. On main 200eefc6 R4 passes without K (p50 57.79 ms at ge10b4,
# bound 74.24; ~/rig_results_safe/wifi_baseline/m1_200eefc6_20261101-2345). If K gains nothing now, the wire change W1
# (the repair echoes its request's serial, needed only to fix K's ~2x-resend reps, `K5`) is dropped.
#
# SHAPE: wifi_repair_ab.sh (the WB harness wifi_burst_loss_netns.sh, one instance per tree, run at once so the trees
# share the PC's load rep by rep), as ~/r4_results/k_rejudge used it. Three trees, each a detached worktree made here:
#   M  = BASE_SHA (main)          M2 = BASE_SHA again (the A/A)          K = K_SHA (main + K, nothing else)
# PROFILE=wifi, CELLS=rel_large, NODE_ENV=TT_UDP_OFFLOAD=0 (the WB reader voids large cells otherwise), LARGE_RSTATS=1.
# ARMS: ctl (no loss: the control - K changes only which recoveries a reader times, and with no loss there is no
# recovery, so K provably cannot touch it), ge1b2 (regressions), ge5b2, ge5b4, ge10b4.
#
# SAMPLE SIZE (worked out first). K's ~2x-resend reps at ge5b2: 1/3, 1/10, 2/10 in October -> p ~ 4/23 = 0.17. With
# REPS=20 a tree sees none of them with probability 0.83^20 = 0.024 (10 reps: 0.16). Per-rep p50 sd in k_rejudge: ctl
# 0.3-0.6 ms, ge5b2 3.3, ge5b4 1.2-1.4, ge10b4 14-20 ms -> with 20 reps a side the SE of a difference of means is ~0.15,
# ~1.0, ~0.4 and ~5.5 ms: K's October effect at ge10b4 (~30 ms) is ~5 SE. ~40 s a rep, 5 arms x 20 reps: ~70 min.
#
# PRE-REGISTERED - the reading is k_recheck_read.py (it, not this comment, decides; written and committed with this
# file before the run):
#   per tree and arm, scored reps = reps with no harness rep VOID (missing RESULT, send deficit) and the PC's 1-min
#   load <= nproc/2 at the rep's start; per scored rep its reader's p50 and p99 (RESULT role=sub), its writer's
#   resends and its reader's timer ACKNACKs.
#   VOID (in this order): a harness campaign VOID in any tree; the treatment not applied (K's tree still holds, or a
#     main tree lacks, the large-writer exception in repair_timing_times(), read from each tree's own src/tickle.c, and
#     the harness's own record of each tree's sha); the control: ctl with < 15 scored reps in any tree, any ctl writer
#     resend, or M2's or K's ctl mean per-rep p50 off M's by more than max(1.0 ms, 3 SE), or p99 by more than
#     max(5.0 ms, 3 SE); no lossy arm left to judge (an arm is not judged when any tree has < 15 scored reps on it or
#     a harness cell VOID there - listed).
#   Per judged arm and metric (mean per-rep p50, mean per-rep p99), K against M and against M2 (difference d, SE of
#     the difference from the per-rep spreads): K BETTER on it when d < -2 SE against both; K WORSE when d > +2 SE
#     against both. Delivered: K WORSE on an arm when its delivered % is more than 0.1 points below M's.
#   VERDICT: VOID as above; else WORSE if any judged arm has a WORSE; else GAIN if any judged arm has a BETTER; else
#     NO_GAIN.
#   Reported beside the verdict, not part of it: K's 2x-resend reps (a rep resending more than 2 x the median of
#   M's and M2's reps on that arm) per tree - the defect W1 was for; the A/A (M2 vs M beyond 2 SE) per arm; the
#   timer-ACKNACK median per tree (K's mechanism: it should ask by timer more often when it engages); R4 read with
#   the 200eefc6 baseline's base_rtt 6.88 ms (this run has no rel_small cell).
#   READING for W1: NO_GAIN or WORSE -> K has no room on main: W1 dropped. GAIN -> K still gains; W1 then matters only
#   if K's 2x-resend reps recur (main has transit_min since K5, RESULTS `RLC`, which may already stop them).
#
# WAITS FOR /home/semih/tmp_build/RELEASE_GATES_DONE (the lead's release gates load the PC; overlapping load voids an
# A/B): polls every 30 s for GATE_WAIT_S (default 5400 = 90 min), then aborts with no run.
#
# Usage: BASE_SHA=<sha> K_SHA=<sha> [REPS=20] [ARMS="ctl ge1b2 ge5b2 ge5b4 ge10b4"] [TAG=k_recheck] k_recheck_netns.sh
#   Output: ~/rig_results_safe/k_recheck/<TAG>_<stamp>/ ; driver.log there ends with "=== k_recheck ended".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
BASE_SHA=${BASE_SHA:?BASE_SHA=main sha}
K_SHA=${K_SHA:?K_SHA=main+K sha}
REPS=${REPS:-20}
ARMS=${ARMS:-"ctl ge1b2 ge5b2 ge5b4 ge10b4"}
TAG=${TAG:-k_recheck}
GATE_FILE=${GATE_FILE:-/home/semih/tmp_build/RELEASE_GATES_DONE}
GATE_WAIT_S=${GATE_WAIT_S:-5400}
STAMP=$(date +%Y%m%d-%H%M%S)
OUT=$HOME/rig_results_safe/k_recheck/${TAG}_$STAMP
TREES_DIR=$HOME/tmp_build/k_recheck_trees/$STAMP
mkdir -p "$OUT" "$TREES_DIR" || exit 1
LOG=$OUT/driver.log
T0=$(date +%s)
MADE=()
on_exit() {
    local rc=$? t
    for t in "${MADE[@]}"; do
        git -C "$REPO" worktree remove --force "$t" >>"$LOG" 2>&1
    done
    rmdir "$TREES_DIR" 2>/dev/null
    echo "=== k_recheck ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s: $OUT ===" >>"$LOG"
}
trap on_exit EXIT
say() { echo "$(date -Is) $*" >>"$LOG"; }
say "k_recheck start: driver tree $(git -C "$REPO" rev-parse --short HEAD) BASE_SHA=$BASE_SHA K_SHA=$K_SHA REPS=$REPS ARMS='$ARMS'"

waited=0
until [ -e "$GATE_FILE" ]; do
    if [ "$waited" -ge "$GATE_WAIT_S" ]; then
        say "ABORT: $GATE_FILE did not appear within $GATE_WAIT_S s - no run"
        echo "VERDICT: NOT RUN - release gates not done within $GATE_WAIT_S s" >"$OUT/verdict.txt"
        exit 3
    fi
    sleep 30
    waited=$((waited + 30))
done
say "gate file present after ${waited} s: $(ls -l --time-style=full-iso "$GATE_FILE")"
say "load at start: $(cat /proc/loadavg) nproc $(nproc)"

for spec in "M=$BASE_SHA" "M2=$BASE_SHA" "K=$K_SHA"; do
    label=${spec%%=*}
    sha=${spec#*=}
    if ! git -C "$REPO" worktree add --detach "$TREES_DIR/$label" "$sha" >>"$LOG" 2>&1; then
        say "ABORT: worktree $label at $sha failed"
        exit 2
    fi
    MADE+=("$TREES_DIR/$label")
done

TREES="M=$TREES_DIR/M M2=$TREES_DIR/M2 K=$TREES_DIR/K" TAG=$TAG BASE_OUT=$OUT PROFILE=wifi REPS=$REPS \
    ARMS="$ARMS" CELLS=rel_large NODE_ENV=TT_UDP_OFFLOAD=0 LARGE_RSTATS=1 \
    "$HERE/wifi_repair_ab.sh" >>"$LOG" 2>&1
say "wifi_repair_ab rc=$? (its own table: $OUT/$TAG.ab.txt)"

python3 "$HERE/k_recheck_read.py" "$OUT" "$TAG" "$TREES_DIR" >"$OUT/verdict.txt" 2>&1
say "reader rc=$?"
grep '^VERDICT:' "$OUT/verdict.txt" >>"$LOG"
