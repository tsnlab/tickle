#!/usr/bin/env bash
# T1 smoke of the slow-envelope loss model fitted to the PSU traces (profiles from ge2_fit.py). One lane, CELL be|rel.
# Profile format: name:bad_p:good_ms,bad_ms,good_p,quiet_s,busy_s,busy_k . Simulation only.
set -u
T=${T1_DIR:-$HOME/tmp_build/wm/t1} # work dir: node binaries, run/
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd) # this directory (scripts)
export PHY_BASE=${PHY_BASE:-6}
# Per-attempt MAC retries (plan 2026-11-07: the single-draw model is not the validated one). RETRY unset = 1; RETRY= (empty) turns it off.
export RETRY=${RETRY-1}
REPS=${REPS:-2}
CELL=${CELL:-be}
DUR=${DUR:-60}
PROFILES=${PROFILES:-"ug:0.36:733,2.2,0.01,2.1,1.3,30.5 library:0.31:116,6.3,0.005,4.8,2.1,22.8 psu_cs:0.44:504,28,0.01,9.7,1.0,46.5 pioneer:0.35:37,9.8,0.01,22.9,14.2,13.6"}
OUT=${RESULTS:-$HOME/rig_results_safe}/${OUTDIR:-t1_ge2}/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
echo "T1_GE2_START $(date -Is) out=$OUT cell=$CELL dur=$DUR"
echo "T1_GE2_MODEL type=prob retry=${RETRY:-off} seed=${SEED:-none} trace=${TRACE:-none} pin_wm=${PIN_WM:-none} pin_echo=${PIN_ECHO:-none} phy_base=$PHY_BASE node=${NODE:-node_d100} profiles=$PROFILES"
for prof in $PROFILES; do
    name=${prof%%:*}
    rest=${prof#*:}
    badp=${rest%%:*}
    ge=${rest#*:}
    for rep in $(seq 1 "$REPS"); do
        run=${name}_r$rep
        "$S/t1_topo.sh" down >/dev/null 2>&1
        sleep 1
        BAD_P=$badp "$S/t1_topo.sh" up 1 "$ge" >"$OUT/$run.topo" 2>&1
        grep -q "^T1_UP lanes" "$OUT/$run.topo" || { echo "VOID $run"; continue; }
        ${PIN_ECHO:+taskset -c $PIN_ECHO} sudo -n ip netns exec t1_0_2 "$T/${NODE:-node_d100}" echo -c 2 -b 10.80.0.255 -d $((DUR + 30)) -m "$CELL" -s 100 -r 100 -p wifi \
            >"$OUT/$run.echo" 2>&1 &
        sleep 1
        ${PIN_PING:+taskset -c $PIN_PING} sudo -n ip netns exec t1_0_1 "$T/${NODE:-node_d100}" ping -c 1 -b 10.80.0.255 -d "$DUR" -m "$CELL" -s 100 -r 100 -p wifi \
            >"$OUT/$run.ping" 2>&1
        sleep 4
        grep -m1 '^RESULT' "$OUT/$run.ping" >"$OUT/$run.result"
        echo "$run $(grep -o 'scored=[0-9]*\|retransmitted=[0-9]*\|p50_us=[0-9.]*\|p90_us=[0-9.]*\|p99_us=[0-9.]*' "$OUT/$run.result" | tr '\n' ' ')"
    done
done
"$S/t1_topo.sh" down >/dev/null 2>&1
echo "T1_GE2_END $(date -Is)"
