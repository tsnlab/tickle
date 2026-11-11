#!/usr/bin/env bash
# shellcheck disable=SC2024 # the redirects (>"$OUT/...") are meant to run as the invoking user; only the ip netns exec needs root
# T1 noise check: the same node (node_d100) run N times at p=0.006 with a longer run (DUR s), rel_small. Spread of p99 and retransmits.
set -u
T=${T1_DIR:-$HOME/tmp_build/wm/t1} # work dir: node binaries, run/
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd) # this directory (scripts)
export PHY_BASE=${PHY_BASE:-6}
P=${P:-0.006}
DUR=${DUR:-60}
N=${N:-6}
OUT=${RESULTS:-$HOME/rig_results_safe}/t1_noisepin/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
echo "T1_NOISE_START $(date -Is) out=$OUT p=$P dur=$DUR n=$N"
for r in $(seq 1 "$N"); do
    run=r$r
    "$S/t1_topo.sh" down >/dev/null 2>&1
    sleep 1
    BAD_P=$P LOSS=$P "$S/t1_topo.sh" up 1 "" >"$OUT/$run.topo" 2>&1
    grep -q "^T1_UP lanes" "$OUT/$run.topo" || { echo "VOID $run"; continue; }
    sudo -n ip netns exec t1_0_2 taskset -c 4 "$T/node_d100" echo -c 2 -b 10.80.0.255 -d $((DUR + 30)) -m rel -s 100 -r 100 -p wifi \
        >"$OUT/$run.echo" 2>&1 &
    sleep 1
    sudo -n ip netns exec t1_0_1 taskset -c 5 "$T/node_d100" ping -c 1 -b 10.80.0.255 -d "$DUR" -m rel -s 100 -r 100 -p wifi \
        >"$OUT/$run.ping" 2>&1
    sleep 4
    grep -m1 '^RESULT' "$OUT/$run.ping" >"$OUT/$run.result"
    echo "$run $(grep -o 'scored=[0-9]*\|retransmitted=[0-9]*\|p50_us=[0-9.]*\|p90_us=[0-9.]*\|p99_us=[0-9.]*' "$OUT/$run.result" | tr '\n' ' ')"
done
"$S/t1_topo.sh" down >/dev/null 2>&1
echo "T1_NOISE_END $(date -Is)"
