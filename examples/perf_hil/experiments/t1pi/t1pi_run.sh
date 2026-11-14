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
KEY=${PI_KEY:-$HOME/.ssh/tickle_ci_ed25519}
SSHO="-i $KEY -o StrictHostKeyChecking=no -o BatchMode=yes -o ControlMaster=auto -o ControlPath=/tmp/t1pi_cm_%h -o ControlPersist=900"
CLI_H=${CLI_SSH:-ci@10.1.1.214}
SRV_H=${SRV_SSH:-ci@10.1.1.213}
CLI_ETH=192.168.10.3
SRV_ETH=192.168.10.2
T=t1pi_scripts/t1pi_topo.sh
BIN=t1pi_bin
NODE=${NODE:-node_d100}
# shellcheck disable=SC2086
cli() { ssh -n $SSHO "$CLI_H" "$@"; }
# shellcheck disable=SC2086
srv() { ssh -n $SSHO "$SRV_H" "$@"; }
OUT=${RESULTS:-$HOME/rig_results_safe}/${OUTDIR:-t1pi_$SET}/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
log() { echo "$(date -Is) $*" | tee -a "$OUT/runner.log"; }

topo_cli_env() { # PROFILE -> env string for `cli up` and the GE argument in $GE_ARG
    GE_ARG=""
    case "$1" in
    ctl) ENVS="BAD_P=0.0"; GE_ARG="1000000,1,0.0" ;;
    o60) ENVS="BAD_P=0.9"; GE_ARG="1140,60,0.01" ;;
    o200) ENVS="BAD_P=0.9"; GE_ARG="3800,200,0.01" ;;
    p*) ENVS="LOSS=${1#p}" ;;
    *) echo "unknown profile $1"; return 1 ;;
    esac
    ENVS="$ENVS RETRY=1 SEED=${SEED:-7} PIN_WM=${PIN_WM:-2}"
}

cpu_state() { # label host-cmd: governor, freq, temperature, throttle flags
    "$@" 'echo "gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null) cur_khz=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null) temp_mC=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null) throttled=$(vcgencmd get_throttled 2>/dev/null) load=$(cut -d" " -f1-3 /proc/loadavg)"'
}

snap_both() { # file prefix
    cli "$T snap" >"$1.cli" 2>&1
    srv "$T snap" >"$1.srv" 2>&1
}

check_state() { # label; compares against the session baseline
    snap_both "$OUT/state_now"
    if ! diff -q "$OUT/state_base.cli" "$OUT/state_now.cli" >/dev/null || ! diff -q "$OUT/state_base.srv" "$OUT/state_now.srv" >/dev/null; then
        log "STATE_CHANGED after $1"
        diff "$OUT/state_base.cli" "$OUT/state_now.cli" >"$OUT/state_diff_$1.cli"
        diff "$OUT/state_base.srv" "$OUT/state_now.srv" >"$OUT/state_diff_$1.srv"
        return 1
    fi
    return 0
}

down_both() {
    cli "T1P_BIN=\$HOME/$BIN $T cli down" >>"$OUT/down.log" 2>&1
    srv "T1P_BIN=\$HOME/$BIN $T srv down" >>"$OUT/down.log" 2>&1
}

# run_one NAME CELL(be|rel) DUR PROFILE
run_one() {
    local name=$1 cell=$2 dur=$3 prof=$4 attempt ok=0
    topo_cli_env "$prof" || return 1
    for attempt in 1 2 3; do
        down_both
        sleep 1
        srv "LOCAL_IP=$SRV_ETH REMOTE_IP=$CLI_ETH $T srv up" >"$OUT/$name.topo" 2>&1
        # shellcheck disable=SC2086
        cli "$ENVS LOCAL_IP=$CLI_ETH REMOTE_IP=$SRV_ETH $T cli up '$GE_ARG'" >>"$OUT/$name.topo" 2>&1
        if grep -q '^T1PI_SRV_UP' "$OUT/$name.topo" && grep -q '^T1PI_CLI_UP' "$OUT/$name.topo"; then ok=1; break; fi
        log "HARNESS_FAULT $name attempt $attempt: topology did not come up"
    done
    [ $ok = 1 ] || { log "VOID $name: no topology"; down_both; return 1; }
    local i up=0
    for i in 1 2 3 4 5 6 7 8 9 10; do
        if cli "sudo -n ip netns exec t1p_sta ping -c 1 -W 1 10.90.0.3 >/dev/null 2>&1"; then up=1; break; fi
    done
    echo "ping_ready=$up tries=$i" >"$OUT/$name.meta"
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

session_start() {
    log "T1PI_SESSION_START set=$SET out=$OUT tree=$(git -C "$REPO" rev-parse --short HEAD) seed=${SEED:-7}"
    down_both
    cli "$T modules load" >>"$OUT/runner.log" 2>&1
    srv "$T modules load" >>"$OUT/runner.log" 2>&1
    snap_both "$OUT/state_base"
}

session_end() {
    down_both
    cli "$T modules unload" >>"$OUT/runner.log" 2>&1
    srv "$T modules unload" >>"$OUT/runner.log" 2>&1
    snap_both "$OUT/state_end"
    # the base snapshot was taken with the modules loaded; compare against the one a clean Pi shows
    if grep -qE '^(mac80211_hwsim|t1p_|t1pg)' "$OUT/state_end.cli" "$OUT/state_end.srv"; then log "SESSION_END: LEFTOVERS"; else log "SESSION_END: clean (no hwsim, no t1p_*)"; fi
    log "T1PI_SESSION_END"
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
