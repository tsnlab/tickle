#!/usr/bin/env bash
# storm_tv_ab.sh - the tv repair storm (2026-10-25, RESULTS `TVS`): main against a fix, pre-registered.
#
# FINDING. wifi_repair_ab.sh (PROFILE=wifi, TT_UDP_OFFLOAD=0, LARGE_RSTATS=1, rel_large: 1 MiB RELIABLE KEEP_LAST at
# 5 Hz) on main's own code: one tv rep in 20 resent 525,319 units against a median ~8,500 (and one of 10 on the K
# candidate 1,829,941). In those reps 82% / 89% of the resends were releases by the resend order's repair timeout, and
# the timeout stood under 256 us when they went (RSTATS hist=lro_timeout_value), on a path whose round trip is >= 4 ms.
#
# HYPOTHESIS (the mechanism, from storm_trace_read.py / storm_trace_scan.py over 45 traced main reps, storm_trace.patch
# = r4_trace.patch with Karn-sample, order-read and timeout-release hooks): the repair timeout is 2 x srtt over this reader's repairs, each timed from its resend to the
# first ACKNACK that shows it arrived. A unit the ACKNACK leaves out between its lowest and highest request counts as
# shown - but the reader leaves out repairs it reckons still on their way (repairs_in_flight()), so an ACKNACK it sent
# before the resend could reach it "shows" the resend microseconds after it went. Watermark samples were never under
# 1 ms in 45 reps; left-out samples were, in most reps (up to 244 a rep). A run of them takes srtt from ~40 ms to
# microseconds; the timeout then releases every held unit at every request, the resends fill netem's 10,000-packet
# queue (tc drops 407,389 and 1,680,378 against ~6,000 lost by the loss model in a calm rep), and the drops are named
# again. The loop: overflow -> named again -> released by a collapsed timeout -> overflow; the left-out samples it keeps
# producing hold srtt down.
#
# THE FIX (F; no wire change): a unit an ACKNACK leaves out is taken as arrived for the order (answered at once when
# named again, as before) but never timed into the timeout's srtt - only into RACK's round trip, whose rule releases a
# unit's first resend only - and its slot stays marked untimed until the watermark passes it (so a later copy arriving
# as the earlier one is not timed either).
#
# TREES: M = main + this harness, A = the same commit in a second checkout (the A/A control), F = M + the fix; optional
# K = M + 11908dea (WIFI repair timing gives large writers Karn's rule), FK = F + 11908dea. All run at once
# (wifi_repair_ab.sh), so they share the PC's load rep by rep.
#
# PHASES: PHASE=tv - ARMS=tv, 30 reps a tree (the storm); PHASE=arms - ge5b2 ge5b4 ge10b4 bc, 10 reps a tree (no
# WORSE). rel_small is not run: it builds without tt_LARGE_SAMPLES, where the change compiles out.
#
# SAMPLE SIZE (worked out before the run). Main's COLLAPSE rate (any release by a timeout under 1 ms) in the
# untraced r4_storm run: 6 of 20 tv reps (M 2, A 4); traced: 4 of 45. At 30 reps a tree, M + A = 60 against F's 30,
# one-sided Fisher with F at 0 needs 8 main COLLAPSE reps for p < 0.05 (7: 0.052, 8: 0.033). At the untraced 30% that
# is 18 expected (p < 0.001); at the traced 9% 5.3, short of it - then UNDECIDED, and the run is repeated before anything
# is read. STORM (10x) at 1 in 20 cannot be decided at this size (3 of 60 against 0 of 30: p = 0.29), so it is a
# falsifier, not the verdict.
#
# READING (implemented in storm_tv_read.py - its docstring is the rule set; summarised):
#   CONTROL: M against A, COLLAPSE and NEAR (> 2 x the main trees' median rep resends) reps, two-sided Fisher; p < 0.05
#     on either is VOID.
#   MECHANISM: SHOWN when F has 0 COLLAPSE reps and COLLAPSE (M + A) against F gives one-sided p < 0.05. FALSIFIED when
#     F has any COLLAPSE rep or any STORM rep (> 10 x that median). Else UNDECIDED.
#   OUTCOME: NEAR reps not driven by the reader's own timer collapse (timer ACKNACKs > 10 x the main median), one-sided
#     Fisher, SHOWN at p < 0.05.
#   NOT WORSE (tv and every arm): delivered (the harness's pooled %) >= min(M, A) - 0.1 point; median rep p50, p99 (and
#     on the arms median rep resends) <= max(M, A) + |M - A| + 5% (resends 10%) of max(M, A).
#   K / FK: COLLAPSE and STORM reps printed; FK with any is "still storms".
#
# AMENDED 2026-10-25 21:30, after the tv and arms phases (ab1) and before any run of this: the timeout's srtt took most
#   of its samples from left-out units, so the fix could cost the tail on ETHERNET, where LRT measured RACK and the
#   timeout (ge1b1 median rep p99 O 11.3 / P 19.6 / F 14.1 ms). Checked by large_repair_followups_netns.sh PHASE=tail
#   with TREES="O=A P=M F=F" (O is here main's second checkout, an A/A of P, not the order's parent), read by that
#   harness's own registered tail rule: FALSIFIED (the fix does not land as is) on a STORM rep or fewer samples
#   delivered than P at ge0.1b1 or ge1b1; COST at either arm when F's median rep p99 is above P's by more than N (the
#   ctl band); also read: O against P on the same rule, the A/A's spread.
#
# AMENDED 2026-10-25 22:20, after that check (tail1) and before its rerun (tail2): tail1 read ge1b1 COST as registered
#   (median rep p99 P 11.9, F 12.8 ms, N 0.16) - and the A/A O, main's own code, read 13.1 ms: by the same rule main
#   costs itself 1.2 ms, so N (the ctl band) is no band at ge1b1 with 4-5 reps. tail2 reruns it, 10 reps a tree, read by
#   storm_tail_read.py: BAND = max(N, |O - P|) on the same arm of the same run; FALSIFIED on a STORM rep of F or fewer
#   delivered than P; COST when F - P > BAND, GAIN when F - P < -BAND, else NO COST. tail1 stays as read (COST, with
#   the A/A failing the rule); tail2 is the verdict.
#
# Usage: TREES="M=dir A=dir F=dir [K=dir FK=dir]" TAG=name PHASE=tv|arms [REPS=n] storm_tv_ab.sh
#   Output: $BASE_OUT/<TAG>_<label>/, <TAG>.ab.txt (wifi_repair_ab.sh's own reading), <TAG>.storm.txt (this one's),
#   which ends with "=== storm_tv_ab ended".
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TREES=${TREES:?TREES=\"M=dir A=dir F=dir ...\"}
TAG=${TAG:?TAG names the output}
PHASE=${PHASE:?PHASE=tv or arms}
BASE_OUT=${BASE_OUT:-$HOME/storm_results}
mkdir -p "$BASE_OUT" || exit 1
OUT="$BASE_OUT/$TAG.storm.txt"
T0=$(date +%s)
on_exit() {
    local rc=$?
    echo "=== storm_tv_ab ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s ===" >>"$OUT"
}
trap on_exit EXIT
case "$PHASE" in
tv) ARMS=tv REPS=${REPS:-30} ;;
arms) ARMS="ge5b2 ge5b4 ge10b4 bc" REPS=${REPS:-10} ;;
*) echo "PHASE must be tv or arms" >&2; exit 2 ;;
esac
echo "=== storm_tv_ab $(date -Is) tag=$TAG PHASE=$PHASE REPS=$REPS TREES='$TREES' load $(cut -d' ' -f1-3 /proc/loadavg) ===" >"$OUT"
labels=()
for t in $TREES; do labels+=("${t%%=*}"); done
BASE_OUT=$BASE_OUT TAG=$TAG TREES=$TREES PROFILE=wifi REPS=$REPS ARMS=$ARMS CELLS=rel_large LARGE_RSTATS=1 \
    NODE_ENV=TT_UDP_OFFLOAD=0 "$HERE/wifi_repair_ab.sh" >>"$OUT" 2>&1
python3 "$HERE/storm_tv_read.py" "$BASE_OUT" "$TAG" "$PHASE" "${labels[@]}" >>"$OUT" 2>&1
