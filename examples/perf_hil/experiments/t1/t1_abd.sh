#!/usr/bin/env bash
# shellcheck disable=SC2024 # the redirects (>"$OUT/...") are meant to run as the invoking user; only the ip netns exec needs root
# T1 candidate A/B (ab/evict-seed): A = node_d100, A2 = node_base again (noise), B = node_evict_d100, rotated per round.
# Cell from CELL (be|rel), steady link error P. Rules are in ~/claude_reports/T1_PLAN.md (written before this run).
set -u
T=${T1_DIR:-$HOME/tmp_build/wm/t1} # work dir: node binaries, run/
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd) # this directory (scripts)
export PHY_BASE=${PHY_BASE:-6}
P=${P:-0.012}
ROUNDS=${ROUNDS:-3}
CELL=${CELL:-be}
OUT=${RESULTS:-$HOME/rig_results_safe}/${OUTDIR:-t1_abd}/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
echo "T1_AB_START $(date -Is) out=$OUT cell=$CELL p=$P rounds=$ROUNDS"
ORDERS=("A A2 B" "A2 B A" "B A A2")
for r in $(seq 1 "$ROUNDS"); do
    for arm in ${ORDERS[$(( (r - 1) % 3 ))]}; do
        case $arm in B) N=$T/node_evict_d100 ;; *) N=$T/node_d100 ;; esac
        run=${arm}_r$r
        "$S/t1_topo.sh" down >/dev/null 2>&1
        sleep 1
        BAD_P=$P LOSS=$P "$S/t1_topo.sh" up 1 "" >"$OUT/$run.topo" 2>&1
        if ! grep -q "^T1_UP lanes" "$OUT/$run.topo"; then echo "VOID $run: no topology"; continue; fi
        sudo -n ip netns exec t1_0_2 "$N" echo -c 2 -b 10.80.0.255 -d 50 -m "$CELL" -s 100 -r 100 -p wifi \
            -l "$OUT/$run.owd" >"$OUT/$run.echo" 2>&1 &
        sleep 1
        sudo -n ip netns exec t1_0_1 "$N" ping -c 1 -b 10.80.0.255 -d 20 -m "$CELL" -s 100 -r 100 -p wifi \
            -l "$OUT/$run.rtt" >"$OUT/$run.ping" 2>&1
        sleep 4
        grep -m1 '^RESULT' "$OUT/$run.ping" >"$OUT/$run.result"
        echo "$run $(grep -o 'scored=[0-9]*\|retransmitted=[0-9]*\|p50_us=[0-9.]*\|p99_us=[0-9.]*' "$OUT/$run.result" | tr '\n' ' ')"
    done
done
"$S/t1_topo.sh" down >/dev/null 2>&1
echo "T1_AB_END $(date -Is)"
