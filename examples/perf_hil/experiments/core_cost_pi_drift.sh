#!/usr/bin/env bash
# The Pi bench's own drift (2026-09-27): one core build measured against itself on several occasions, to give every
# later Pi CPU criterion its noise floor. On the PC the same binary moved ~1.7 ns between runs, beyond its within-run
# 2 x SE (CONTEXT_NODE_PLAN 4a), so a within-run 2 x SE alone overstates what a difference means.
# Each occasion is core_cost_pi.sh with the same ref twice (REF and REF^0 resolve to one binary), default and -R,
# 10 rounds; occasions are GAP_S apart. Output: $OUT.<n>.{default,R}.txt, then a summary line per occasion.
# Reading: the spread of the per-occasion A/A differences (send and recv) is the floor; a later A/B difference inside
# it is not evidence of a change, whatever its within-run t.
cd /home/semih/tickle || exit 1
REF=${REF:?REF=<sha>}; N=${N:-4}; GAP_S=${GAP_S:-900}; OUT=${OUT:-/tmp/core_cost_pi_drift_$(date +%Y%m%d)}
export RIG_LOCK_WAIT=10800
X=./examples/perf_hil/experiments
for n in $(seq 1 "$N"); do
    OUT=$OUT.$n.default.txt $X/core_cost_pi.sh -r 10 "$REF" "$REF^0" > "$OUT.$n.default.log" 2>&1
    BENCH_ARGS=-R OUT=$OUT.$n.R.txt $X/core_cost_pi.sh -r 10 "$REF" "$REF^0" > "$OUT.$n.R.log" 2>&1
    echo "occasion $n $(date -Is): default $(grep 'vs ' "$OUT.$n.default.log" | tail -1) | -R $(grep 'vs ' "$OUT.$n.R.log" | tail -1)" >> "$OUT.summary"
    [ "$n" -lt "$N" ] && sleep "$GAP_S"
done
echo "=== done $(date -Is) ===" >> "$OUT.summary"
