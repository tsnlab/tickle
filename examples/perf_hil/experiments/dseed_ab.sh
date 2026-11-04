#!/usr/bin/env bash
# dseed_ab.sh - the pre-registered A/B of the discovery-seeded retry interval (branch ab/discovery-seed), between
# private network namespaces on this PC, through each tree's own time_varying_netns.sh. 2026-11-04. No rig, no
# default namespace.
#
# WHAT IS TESTED. A RELIABLE reader's recovery estimate is 0 until a requested sample is actually recovered. At 50 and
# 100 ms one-way delay with KEEP_LAST 10 at 100 Hz the writer evicts a lost sample before the request reaches it, so
# the estimate never seeds and the reader re-requests at retry_initial (1 ms) for a whole round trip: X1 rel_small
# TIMER_PER_GAP 24-33 at 50 ms and 25-56 at 100 ms (base b6cffab5, 2026-11-04). The treatment seeds the retry interval
# from a discovery round trip (announce -> unicast reply) while the recovery estimate is 0; real samples replace it.
# Seed interval = srtt + 4 rttvar with rttvar = srtt / 2, i.e. ~3 round trips (capped by max_srtt_multiple).
#
# ARMS. Three trees, given as paths; each invocation runs the tree's OWN copy of time_varying_netns.sh (and so builds
# that tree's core and bench nodes):
#   A  = <base_tree>  the base.
#   B  = <new_tree>   the treatment.
#   A2 = <base_tree>  again: the A/A control. It is the same tree as A, so the change provably cannot touch it; its
#                     distance from A is the noise every A/B delta is compared against.
# Each round runs A, B and A2 once, the order rotated by one per round (round 1 A B A2, round 2 B A2 A, round 3 A2 A
# B, ...), so a drift over the session lands on every arm at a different position. order.txt records what ran.
#
# ONE INVOCATION: OUT/<arm>-r<round>/ (a complete time_varying OUT, its own summary and s_l0 control), with
#   REPS=1 NODE_PIN="2 4 6" PROFILE=ethernet EST_TRACE=1 CELLS="rel_small rel_large" ARMS="<the points below>"
# (X1=1 is not set: it would refuse ARMS). time_varying_netns.sh adds s_l0 itself.
#   Treatment points (where the estimate never seeds): s_x50l1 s_x50l2 s_x50l5 s_x50l10 s_x100l1 s_x100l2 s_x100l5
#     s_x100l10.
#   Control points (short delay: real samples seed the estimate, the seed should not matter): s_x1l1 s_x1l10 s_x5l1
#     s_x5l10 s_x20l1 s_x20l10.
#
# PRE-REGISTERED (written 2026-11-04, before any run). dseed_ab_read.py implements every rule IN CODE; it, not this
# comment, decides:
#   Sample counts first: per arm, point and cell, the usable reps (time_varying_read.py's own per-rep VOIDs: RESULT
#     missing, realized loss outside [0.5 L, 1.5 L], delay not seen, ...; an invocation whose s_l0 control lost
#     anything voids all of its runs, one whose s_l0 rel_large resent voids its rel_large runs). A point with under 2
#     usable reps in any arm is VOID.
#   P1 (the treatment works), rel_small, the 8 treatment points: B's median TIMER_PER_GAP <= 2.0 at every point ->
#     PASS. FAIL (falsified) if any treatment point stays > 5.0 while B's trace shows a non-zero seed on the data
#     reader. Otherwise between 2.0 and 5.0 -> PARTIAL. A point > 5.0 with no seed in B's trace: the treatment is not
#     seen -> VOID.
#   P2 (no cost), every point (treatment and control), rel_small and rel_large: OWD p99 and RTT p99 (rel_small) are
#     WORSE when median(B) - median(A) > max(|median(A2) - median(A)|, 10% of median(A)); RELIABLE DELIVERY is WORSE
#     when B < A by more than max(|A2 - A|, 0.1 point). Any WORSE -> P2 FAIL, each printed with its numbers.
#   Control check: |A2 - A| over 10% of A on a metric at a point voids that metric at that point (noise), printed.
#   Trace evidence (EST_TRACE=1): per arm and treatment point, the share of the data reader's ESTTRACE lines (echo's
#     obj=wproxy for writer_ctx=1: its view of the data writer) with recovery_srtt_ns=0 and a non-zero seed field
#     (dseed_ab_read.py SEED_FIELD; base arms lack the field and read "absent").
#   VERDICT P1=<PASS|PARTIAL|FAIL|VOID> P2=<PASS|FAIL|VOID>, and a HYPOTHESIS line.
#
# TIME (time_varying_netns.sh's own figures, measured 2026-11-03 at DUR=30: a small run ~37 s, a rel_large run ~40 s,
# a build 12 s): one invocation = (14 points + s_l0) x (37 + 40) s + 12 s = 1167 s (~19.5 min); a round is 3
# invocations, ~58 min; the default ROUNDS=3 is ~2.9 h. The script prints its own estimate for the ROUNDS given.
#
# Usage: [ROUNDS=3] [OUT=dir] [RIG_LOCK_WAIT=36000] [DRY_RUN=1] dseed_ab.sh <base_tree> <new_tree>
#   Detach it yourself (setsid nohup ... > log 2>&1 < /dev/null &). The last lines are always
#     DSEED_AB_END rc=<n> out=<dir>
#   followed by the reading (dseed_ab_read.py OUT, also in OUT/read.txt). Each invocation takes the box lock
#   (rig_lock.sh, RIG_LOCK_SCOPE=box: this PC) around itself. OUT defaults to ~/rig_results_safe/dseed_ab/<stamp>;
#   provenance.txt records each arm's tree, SHA and dirty state. DRY_RUN=1 prints the order and the estimate and exits.
#   Needs `sudo -n ip`.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"

TREAT_POINTS="s_x50l1 s_x50l2 s_x50l5 s_x50l10 s_x100l1 s_x100l2 s_x100l5 s_x100l10"
CONTROL_POINTS="s_x1l1 s_x1l10 s_x5l1 s_x5l10 s_x20l1 s_x20l10"
TV_ARMS="$TREAT_POINTS $CONTROL_POINTS"
TV_CELLS="rel_small rel_large"
TV_NODE_PIN="2 4 6"
RUN_S_SMALL=37 # time_varying_netns.sh header, measured 2026-11-03 at DUR=30
RUN_S_LARGE=40
BUILD_S=12
ROUNDS=${ROUNDS:-3}
DRY_RUN=${DRY_RUN:-0}
OUT=${OUT:-$HOME/rig_results_safe/dseed_ab/$(date +%Y%m%d-%H%M%S)}

T0=$(date +%s)
RC=0
CHILD_PID=""
# The one EXIT trap: stop a running invocation (its own trap removes its namespaces), print the END line, then read.
# shellcheck disable=SC2329 # invoked by the EXIT trap
on_exit() {
    local rc=$?
    [ "$RC" = 0 ] && RC=$rc
    if [ -n "$CHILD_PID" ]; then
        kill "$CHILD_PID" 2>/dev/null
        wait "$CHILD_PID" 2>/dev/null
    fi
    echo "DSEED_AB_END rc=$RC out=$OUT after $(($(date +%s) - T0)) s"
    if [ "$DRY_RUN" != 1 ] && [ -d "$OUT" ]; then
        echo "--- reading (dseed_ab_read.py)"
        python3 "$HERE/dseed_ab_read.py" "$OUT" 2>&1 | tee "$OUT/read.txt"
    fi
}
trap on_exit EXIT
trap 'RC=143; exit 143' TERM INT HUP

if [ $# -ne 2 ]; then
    echo "usage: [ROUNDS=3] [OUT=dir] dseed_ab.sh <base_tree> <new_tree>" >&2
    RC=2
    exit 2
fi
[[ $ROUNDS =~ ^[1-9][0-9]*$ ]] || { echo "ROUNDS is a positive integer" >&2; RC=2; exit 2; }
declare -A TREE
for i in 1 2; do
    t=$(cd "${!i}" 2>/dev/null && pwd) || { echo "no such tree: ${!i}" >&2; RC=2; exit 2; }
    [ -f "$t/examples/perf_hil/experiments/time_varying_netns.sh" ] ||
        { echo "$t has no examples/perf_hil/experiments/time_varying_netns.sh" >&2; RC=2; exit 2; }
    [ "$i" = 1 ] && TREE[A]=$t && TREE[A2]=$t
    [ "$i" = 2 ] && TREE[B]=$t
done
[ "${TREE[A]}" = "${TREE[B]}" ] && { echo "base and new tree are the same path" >&2; RC=2; exit 2; }

# order ROUND: the arms in the order that round runs them (rotated by one per round).
ARM_LIST=(A B A2)
order() {
    local r=$1 k out=()
    for k in 0 1 2; do out+=("${ARM_LIST[$(((r - 1 + k) % 3))]}"); done
    echo "${out[*]}"
}
n_points=$(($(wc -w <<<"$TV_ARMS") + 1)) # + s_l0, which time_varying_netns.sh adds
inv_s=$((n_points * (RUN_S_SMALL + RUN_S_LARGE) + BUILD_S))
total_s=$((ROUNDS * 3 * inv_s))
echo "dseed_ab $(date -Is): ROUNDS=$ROUNDS, 3 invocations a round of $n_points points x ($TV_CELLS):" \
    "estimate $inv_s s an invocation, $total_s s ($((total_s / 60)) min) in all; OUT=$OUT"
for r in $(seq 1 "$ROUNDS"); do echo "  round $r: $(order "$r")"; done
if [ "$DRY_RUN" = 1 ]; then
    exit 0
fi

mkdir -p "$OUT" || { RC=1; exit 1; }
# provenance: each arm's tree, its HEAD and its dirty state, recorded before the first run and checked again at its
# every invocation (a tree edited during the campaign is no longer the tree its arm names).
tree_state() { # tree_state DIR: "sha=<full> dirty_paths=<n> dirty_core=<n>"
    local sha all core
    sha=$(git -C "$1" rev-parse HEAD 2>/dev/null || echo none)
    all=$(git -C "$1" status --porcelain 2>/dev/null | wc -l)
    core=$(git -C "$1" status --porcelain -- src include platform 2>/dev/null | wc -l)
    echo "sha=$sha dirty_paths=$all dirty_core=$core"
}
{
    echo "dseed_ab started=$(date -Is) rounds=$ROUNDS host=$(hostname) harness=$(git -C "$REPO" rev-parse HEAD)"
    echo "tv_arms='$TV_ARMS' cells='$TV_CELLS' node_pin='$TV_NODE_PIN' profile=ethernet est_trace=1 reps=1"
    for arm in A B A2; do
        echo "arm=$arm tree=${TREE[$arm]} $(tree_state "${TREE[$arm]}")" \
            "tv_sha256=$(sha256sum "${TREE[$arm]}/examples/perf_hil/experiments/time_varying_netns.sh" | cut -c1-16)"
    done
} >"$OUT/provenance.txt"
cat "$OUT/provenance.txt"
: >"$OUT/order.txt"

for r in $(seq 1 "$ROUNDS"); do
    for arm in $(order "$r"); do
        tree=${TREE[$arm]}
        run_out="$OUT/$arm-r$r"
        echo "round=$r arm=$arm start=$(date -Is) $(tree_state "$tree")" | tee -a "$OUT/order.txt"
        # The box lock is taken around each invocation, inside this (detached) process. The owner names the
        # campaign's OUT, set apart from the invocation's own OUT below.
        owner="dseed_ab.sh pid=$$ arm=$arm round=$r out=$OUT $(date -Is)"
        RIG_LOCK_SCOPE=box RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-36000} \
            RIG_LOCK_OWNER="$owner" \
            REPS=1 NODE_PIN="$TV_NODE_PIN" PROFILE=ethernet EST_TRACE=1 CELLS="$TV_CELLS" ARMS="$TV_ARMS" \
            OUT="$run_out" "$REPO/examples/perf_hil/rig_lock.sh" \
            "$tree/examples/perf_hil/experiments/time_varying_netns.sh" >"$OUT/$arm-r$r.log" 2>&1 </dev/null &
        CHILD_PID=$!
        wait "$CHILD_PID"
        inv_rc=$?
        CHILD_PID=""
        echo "round=$r arm=$arm end=$(date -Is) rc=$inv_rc runs=$(find "$run_out/runs" -mindepth 1 -maxdepth 1 \
            -type d 2>/dev/null | wc -l)" | tee -a "$OUT/order.txt"
        [ "$inv_rc" = 0 ] || RC=$inv_rc
    done
done
exit "$RC"
