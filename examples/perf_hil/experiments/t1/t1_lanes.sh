#!/usr/bin/env bash
# shellcheck disable=SC2024 # the redirects (>"$OUT/...") are meant to run as the invoking user; only the ip netns exec needs root
# T1 test 7 (lane calibration, rule L1 in ~/claude_reports/T1_PLAN.md): the same be_small cell on N lanes at once, N from
# LANE_LIST, REPS reps each; N=1 appears twice (alone, alone again = the noise floor). Steady link error P (0.012 = about 5%
# realized loss). Lane L: phys PHY_BASE+3L.., cores 4+3L.. (taskset). Simulation only.
set -u
T=${T1_DIR:-$HOME/tmp_build/wm/t1} # work dir: node binaries, run/
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd) # this directory (scripts)
export PHY_BASE=${PHY_BASE:-6}
P=${P:-0.012}
REPS=${REPS:-3}
LANE_LIST=${LANE_LIST:-"1 1 2 3"}
OUT=${RESULTS:-$HOME/rig_results_safe}/${OUTDIR:-t1_lanes}/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
echo "T1_LANES_START $(date -Is) out=$OUT p=$P reps=$REPS list='$LANE_LIST'"
idx=0
for N in $LANE_LIST; do
    idx=$((idx + 1))
    for rep in $(seq 1 "$REPS"); do
        run=n${N}_c${idx}_r$rep
        "$S/t1_topo.sh" down >/dev/null 2>&1
        sleep 1
        BAD_P=$P LOSS=$P "$S/t1_topo.sh" up "$N" "" >"$OUT/$run.topo" 2>&1
        if ! grep -q "^T1_UP lanes" "$OUT/$run.topo"; then echo "VOID $run: no topology"; continue; fi
        echo "load=$(cut -d' ' -f1 /proc/loadavg) start=$(date -Is)" >"$OUT/$run.meta"
        for ((L = 0; L < N; L++)); do
            c=$((4 + 3 * L))
            sudo -n ip netns exec "t1_${L}_2" taskset -c "$c" "$T/node" echo -c 2 -b "10.8$L.0.255" -d 50 -m be -s 100 -r 100 -p wifi \
                -l "$OUT/$run.l$L.owd" >"$OUT/$run.l$L.echo" 2>&1 &
        done
        sleep 1
        for ((L = 0; L < N; L++)); do
            c=$((5 + 3 * L))
            sudo -n ip netns exec "t1_${L}_1" taskset -c "$c" "$T/node" ping -c 1 -b "10.8$L.0.255" -d 20 -m be -s 100 -r 100 -p wifi \
                -l "$OUT/$run.l$L.rtt" >"$OUT/$run.l$L.ping" 2>&1 &
        done
        sleep 26
        for ((L = 0; L < N; L++)); do
            grep -m1 '^RESULT' "$OUT/$run.l$L.ping" >"$OUT/$run.l$L.result"
            echo "$run lane$L $(grep -o 'published=[0-9]*\|scored=[0-9]*\|p50_us=[0-9.]*\|p99_us=[0-9.]*' "$OUT/$run.l$L.result" | tr '\n' ' ')"
        done
    done
done
"$S/t1_topo.sh" down >/dev/null 2>&1
echo "T1_LANES_END $(date -Is)"
