#!/usr/bin/env bash
# One TickLE build against another on the rig's native campaign cells, as interleaved A B B A blocks (2026-09-27,
# WIRE_PLAN.md 8.1: the first v10 campaign ran its arms one after the other, so a drift over the session could not be
# told from the change). Each block is campaign_sweep.sh at one SHA, TickLE only, REPS repetitions (default 3, so 6
# per arm), and takes the rig lock itself. Outputs: /tmp/${TAG}_b<n>_<sha8>.txt, then
#   ab_compare.py campaign <(cat A blocks) <(cat B blocks)
# reads the arms at 2 x SE. The reading rule is the caller's pre-registration (e.g. WIRE_PLAN 6: nothing WORSE).
cd /home/semih/tickle || exit 1
A=${A:?A=<sha> the parent}; B=${B:?B=<sha> the change}; TAG=${TAG:?TAG=<name>}; REPS=${REPS:-3}
export RIG_LOCK_WAIT=36000
X=./examples/perf_hil/experiments
n=0
for sha in "$A" "$B" "$B" "$A"; do
    n=$((n + 1))
    SHA=$sha FWS=tickle REPS=$REPS OUT=/tmp/${TAG}_b${n}_${sha:0:8}.txt $X/campaign_sweep.sh > "/tmp/${TAG}_b${n}_${sha:0:8}.log" 2>&1
done
cat "/tmp/${TAG}_b1_${A:0:8}.txt" "/tmp/${TAG}_b4_${A:0:8}.txt" > "/tmp/${TAG}_A.txt"
cat "/tmp/${TAG}_b2_${B:0:8}.txt" "/tmp/${TAG}_b3_${B:0:8}.txt" > "/tmp/${TAG}_B.txt"
python3 $X/ab_compare.py campaign "/tmp/${TAG}_A.txt" "/tmp/${TAG}_B.txt" > "/tmp/${TAG}_compare.txt"
echo "=== chain done $(date -Is) ===" > "/tmp/${TAG}.done"
