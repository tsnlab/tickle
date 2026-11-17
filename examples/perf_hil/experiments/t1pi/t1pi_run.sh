#!/usr/bin/env bash
# T1-Pi runner (plan: PLAN.md). Runs on the dev PC, drives both Pis over ssh. SIMULATION ONLY (hwsim + wmediumd).
#   t1pi_run.sh SET        SET = v1 | calib | noise | one
#   one: CELL=be|rel DUR=60 PROFILE=ctl|o60|o200|p0.1 NAME=x REPS=1
# Env: OUTDIR (under $RESULTS, default ~/rig_results_safe), RESULTS, REPS, DUR.
# The rig lock is taken here, inside the (detached) process: flock /tmp/rig.lock + rig_lock.sh (CI's hil lock).
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$S/../../../.." && pwd)
if [ -z "${T1PI_LOCKED:-}" ]; then
    T1PI_LOCKED=1 RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-600} exec flock /tmp/rig.lock "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi
SET=${1:-}
OUT=${RESULTS:-$HOME/rig_results_safe}/${OUTDIR:-t1pi_$SET}/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
# shellcheck source=t1pi_lib.sh
. "$S/t1pi_lib.sh"
NODE=${NODE:-node_d100}

# run_one NAME CELL(be|rel) DUR PROFILE
run_one() {
    local name=$1 cell=$2 dur=$3 prof=$4 i
    bring_up "$OUT/$name" "$prof" || return 1
    up=$UP_READY
    { echo "start=$(date -Is)"; echo "cli $(cpu_state cli)"; echo "srv $(cpu_state srv)"; } >>"$OUT/$name.meta"
    local stop=/home/ci/t1pi_run/stop_$name
    srv "rm -f $stop; setsid nohup sudo -n ip netns exec t1p_srv \$HOME/$BIN/$NODE echo -c 2 -b 10.90.0.255 -d $((dur + 40)) -m $cell -s 100 -r 100 -p wifi -q $stop >\$HOME/t1pi_run/$name.echo 2>&1 </dev/null & echo \$!" >"$OUT/$name.echo.pid"
    sleep 1
    cli "sudo -n ip netns exec t1p_sta \$HOME/$BIN/$NODE ping -c 1 -b 10.90.0.255 -d $dur -m $cell -s 100 -r 100 -p wifi -l \$HOME/t1pi_run/$name.rtt" >"$OUT/$name.ping" 2>&1
    sleep 2
    srv "touch $stop"
    for i in $(seq 1 20); do
        srv "[ -d /proc/$(cat "$OUT/$name.echo.pid") ]" || break
        sleep 1
    done
    srv "cat \$HOME/t1pi_run/$name.echo; rm -f \$HOME/t1pi_run/$name.echo $stop" >"$OUT/$name.echo" 2>&1
    cli "cat \$HOME/t1pi_run/$name.rtt; rm -f \$HOME/t1pi_run/$name.rtt" >"$OUT/$name.rtt" 2>&1
    { echo "end=$(date -Is)"; echo "cli $(cpu_state cli)"; echo "srv $(cpu_state srv)"; } >>"$OUT/$name.meta"
    cli "sudo -n cat \$HOME/t1pi_run/run/wm.log" >"$OUT/$name.wm.log" 2>&1
    down_both
    grep -m1 '^RESULT' "$OUT/$name.ping" >"$OUT/$name.result"
    log "$name $(grep -o 'published=[0-9]*\|scored=[0-9]*\|retransmitted=[0-9]*\|p50_us=[0-9.]*\|p99_us=[0-9.]*' "$OUT/$name.result" | tr '\n' ' ')ping_ready=$up"
    check_state "$name" || { log "ABORT: wlan0/eth0/modules state changed"; return 2; }
    return 0
}

trap session_end EXIT

run_list() { # lines: name cell dur profile
    local name cell dur prof rc
    while read -r name cell dur prof; do
        [ -n "$name" ] || continue
        run_one "$name" "$cell" "$dur" "$prof"; rc=$?
        [ $rc = 2 ] && return 2
    done
}

session_start
case "$SET" in
v1) # V1: p = 0, be_small 20 s, 3 reps
    for r in 1 2 3; do echo "p0_r$r be 20 p0.0"; done | run_list ;;
calib) # V2 steady error and the profiles, one-hop realized loss
    {
        for p in 0.1 0.3; do for r in 1 2 3; do echo "p${p}_r$r be 20 p$p"; done; done
        for prof in o60 o200; do for cell in be rel; do for r in 1 2; do echo "${prof}_${cell}_r$r $cell 60 $prof"; done; done; done
    } | run_list ;;
noise) # A/A: 4 sets x REPS (default 5), 120 s, copies alternate A/A2
    {
        for prof in o60 o200; do for cell in be rel; do
            for r in $(seq 1 "${REPS:-5}"); do c=A; [ $((r % 2)) = 0 ] && c=A2; echo "${prof}_${cell}_${c}_r$r $cell ${DUR:-120} $prof"; done
        done; done
    } | run_list ;;
one)
    for r in $(seq 1 "${REPS:-1}"); do echo "${NAME:-one}_r$r ${CELL:-be} ${DUR:-20} ${PROFILE:-ctl}"; done | run_list ;;
*) echo "usage: $0 v1|calib|noise|one"; exit 2 ;;
esac
