#!/usr/bin/env bash
# eth_storm_census.sh - the ETHERNET amp writer storm on main, traced (RESULTS `EWS`, 2026-10-26).
#
# BACKGROUND. In the reader-floor A/B (`RLF`, ~/rlf_results/rlf1_e5_A/runs/ge0.1b1-rel_large-6) one rep of the A tree
# (origin/main a5b52eee, which has the TVS storm fix 8d0d2eed, untraced) on ETHERNET's amp cell (1 MiB RELIABLE at
# 30 Hz, BANDS=1, netem "limit 100000 delay 100us rate 1gbit" + gemodel 0.1% burst 1) resent 4,572,542 units against
# a calm 2,600-7,300. Its writer released 4,551,938 of them by the repair timeout, every one with the timeout at 1-2 ms
# (calm reps: 0-1,640 such releases, 2-4 ms); netem's egress queue dropped 1,903,943 (calm ~650) - overflow, as TVS's
# storms; the reader got 47 of 476 samples, p99 1.74 s; the writer published 476 of 900 (its CPU 34.7 s in 30 s).
# Unlike TVS the timeout had not collapsed below a real round trip: 1-2 ms is what calm reps run with. On disk the
# storm is 1 of 505 reps with main's writer on this cell (a5b52eee 400, 81daa880 60, 8d0d2eed 45; 0 of 200 more on
# RLF's F, whose writer is main's) - so p ~ 0.2%, with a wide interval.
#
# HYPOTHESIS H (from the code, large_resend_due() / large_order_arrived()): the repair timeout is 2 x srtt with no
#   back-off, and srtt takes Karn samples only from units resent once. Once a burst of repairs puts more in netem's
#   queue than the timeout covers (100,000 packets at 1 Gbit/s hold ~1.2 s, against a ~2 ms timeout), every named
#   unit is released again at each ACKNACK, every unit soon went more than once, no Karn sample can be taken, and the
#   timeout stays at its calm value while the queue it is too short for is fed by the resends themselves.
#   H predicts, in a storm rep's writer trace (onset t0 = the first 100 ms with >= 200 timeout releases):
#     P2 at most 5 Karn samples in [t0, t0 + 2 s] - the timeout frozen;
#     P3 the median one-way time of the repairs resent in [t0 + 0.5 s, t0 + 2.5 s] (writer S -> the reader's next
#        U/D of that unit) >= 10 x the median timeout released with there - repairs released long before they could
#        have arrived.
#   FALSIFIED (for that rep) by either failing: P2 failing reads "the estimate moves and still storms", P3 failing
#   reads "the releases were not premature" (real loss, the reader).
#   S1 (classified, not part of H): what the timeout stood at when it began. The storm rep's timeouts were 1-2 ms
#   (srtt ~0.5-1 ms) while a calm traced smoke rep's srtt ran 3.5-7.5 ms, so srtt may have been low, not merely
#   frozen. srtt at t0 (the last Karn sample's srtt before it) is COLLAPSED under 128 us (TVS-LIKE: a short sample
#   leaking in), LOW under the 1st percentile of every srtt the round-1 (calm) traces took, CALM otherwise; the last
#   20 Karn samples before t0 are printed (what took it there). The trigger before t0 is descriptive only (Y
#   path-failed mark, the reader's timer ACKNACKs, first sends, ACKNACK sizes in the 2 s before).
#
# THIS RUN: one tree, this checkout = origin/main + eth_storm_trace.patch applied UNCOMMITTED to src/tickle.c (the R4
#   trace hooks of ~/rlf_trees/MT plus EWS's writer events K/T/W/k/Y and a dump filter; experiment only, compiled only
#   with -DR4_TRACE). The script refuses unless `git diff -- src` equals that patch. INSTANCES (default 8)
#   wifi_burst_loss_netns.sh at once, each its own namespaces, ROUND_REPS (10) reps a round, PROFILE=ethernet
#   ARMS=ge0.1b1 CELLS=rel_large LARGE_HZ=30 BANDS=1 NETEM_BASE as RLF's E rounds, NODE_ENV=TT_UDP_OFFLOAD=0,
#   LARGE_RSTATS=1 LARGE_TRACE=1. Round 1 keeps every trace (R4_TRACE_KEEP=1: the trace control's reps); later rounds
#   keep a trace only where the rep resent, timed out, duplicated or retried far beyond a calm rep (the patch's filter).
# STOPPING (fixed before the run, by the clock): no round starts after LAST_START (default 20:30 KST today); at most
#   MAX_ROUNDS (30).
# SAMPLE SIZE (before the run): ~45 s a rep, a round ~8 min with 8 at once: 17:30-20:30 ~22 rounds x 80 = ~1,760 reps.
#   At p = 1/505, P(no storm) = (1 - 1/505)^1760 = 3%; at p = 1/1000, 17%. 0 storms in N reps bounds p < 3/N (95%).
# READING: eth_storm_read.py (its docstring is the rule set, applied in its code): STORM, the TRACE CONTROL, the rate,
#   and H's P1-P3 per storm rep.
# Usage: [INSTANCES=8] [ROUND_REPS=10] [LAST_START=epoch] [MAX_ROUNDS=30] [TAG=ews1] [OUT=~/ews_results]
#        eth_storm_census.sh
#   Output: $OUT/<TAG>_r<round>_i<k>/ and $OUT/<TAG>.read.txt; this script's log ends with "=== eth_storm_census ended".
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
INSTANCES=${INSTANCES:-8}
ROUND_REPS=${ROUND_REPS:-10}
LAST_START=${LAST_START:-$(date -d 'today 20:30' +%s)}
MAX_ROUNDS=${MAX_ROUNDS:-30}
TAG=${TAG:-ews1}
OUT=${OUT:-$HOME/ews_results}
mkdir -p "$OUT" || exit 1
T0=$(date +%s)
on_exit() {
    local rc=$?
    echo "=== eth_storm_census ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s ==="
}
trap on_exit EXIT
if ! git -C "$REPO" diff -- src | cmp -s - "$HERE/eth_storm_trace.patch"; then
    echo "REFUSED: src/ of $REPO is not HEAD + eth_storm_trace.patch"
    exit 2
fi
echo "=== eth_storm_census $(date -Is) tag=$TAG tree=$(git -C "$REPO" rev-parse --short HEAD) + eth_storm_trace.patch" \
    "INSTANCES=$INSTANCES ROUND_REPS=$ROUND_REPS LAST_START=$(date -d "@$LAST_START" -Is) MAX_ROUNDS=$MAX_ROUNDS" \
    "load $(cut -d' ' -f1-3 /proc/loadavg) ==="
round=0
while [ "$round" -lt "$MAX_ROUNDS" ] && [ "$(date +%s)" -lt "$LAST_START" ]; do
    round=$((round + 1))
    keep=""
    [ "$round" = 1 ] && keep=" R4_TRACE_KEEP=1"
    pids=()
    for k in $(seq 1 "$INSTANCES"); do
        env OUT="$OUT/${TAG}_r${round}_i$k" REPS="$ROUND_REPS" DUR=30 PROFILE=ethernet ARMS="ge0.1b1" CELLS="rel_large" \
            LARGE_HZ=30 BANDS=1 NETEM_BASE="limit 100000 delay 100us rate 1gbit" NODE_ENV="TT_UDP_OFFLOAD=0$keep" \
            LARGE_RSTATS=1 LARGE_TRACE=1 "$HERE/wifi_burst_loss_netns.sh" >"$OUT/${TAG}_r${round}_i$k.log" 2>&1 &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p"; done
    echo "round $round done $(date -Is) load $(cut -d' ' -f1-3 /proc/loadavg)"
done
echo "rounds done: $round $(date -Is)"
python3 -I "$HERE/eth_storm_read.py" "$OUT" "$TAG" >"$OUT/$TAG.read.txt" 2>&1
echo "read rc=$?"
grep -E '^(REPS|STORM|RATE|TRACE CONTROL|H |VERDICT)' "$OUT/$TAG.read.txt"
