#!/usr/bin/env bash
# Shared helpers of the T1-Pi runners (t1pi_run.sh, t1pi_fw.sh); sourced, not run. SIMULATION ONLY (hwsim + wmediumd).
# Needs before sourcing: S (this directory), REPO, SET (a name for the log), OUT (results directory, created here).
KEY=${PI_KEY:-$HOME/.ssh/tickle_ci_ed25519}
SSHO="-i $KEY -o StrictHostKeyChecking=no -o BatchMode=yes -o ControlMaster=auto -o ControlPath=/tmp/t1pi_cm_%h -o ControlPersist=900"
CLI_H=${CLI_SSH:-ci@10.1.1.214}
SRV_H=${SRV_SSH:-ci@10.1.1.213}
CLI_ETH=192.168.10.3
SRV_ETH=192.168.10.2
T=t1pi_scripts/t1pi_topo.sh
BIN=t1pi_bin
# shellcheck disable=SC2086
cli() { ssh -n $SSHO "$CLI_H" "$@"; }
# shellcheck disable=SC2086
srv() { ssh -n $SSHO "$SRV_H" "$@"; }
# with stdin (file contents)
# shellcheck disable=SC2086
cli_in() { ssh $SSHO "$CLI_H" "$@"; }
# shellcheck disable=SC2086
srv_in() { ssh $SSHO "$SRV_H" "$@"; }
log() { echo "$(date -Is) $*" | tee -a "$OUT/runner.log"; }

topo_cli_env() { # PROFILE -> ENVS (env assignments for `cli up`) and GE_ARG
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

cpu_state() { # cli|srv: governor, freq, temperature, load
    "$1" 'echo "gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null) cur_khz=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null) temp_mC=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null) load=$(cut -d" " -f1-3 /proc/loadavg)"'
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

# bring_up NAME PROFILE: fresh topology (3 attempts), then wait for the first ping across the Wi-Fi hop; sets UP_READY
bring_up() {
    local name=$1 attempt ok=0 i
    topo_cli_env "$2" || return 1
    for attempt in 1 2 3; do
        down_both
        sleep 1
        srv "LOCAL_IP=$SRV_ETH REMOTE_IP=$CLI_ETH $T srv up" >"$name.topo" 2>&1
        # shellcheck disable=SC2086
        cli "$ENVS LOCAL_IP=$CLI_ETH REMOTE_IP=$SRV_ETH $T cli up '$GE_ARG'" >>"$name.topo" 2>&1
        if grep -q '^T1PI_SRV_UP' "$name.topo" && grep -q '^T1PI_CLI_UP' "$name.topo"; then ok=1; break; fi
        log "HARNESS_FAULT $name attempt $attempt: topology did not come up"
    done
    [ $ok = 1 ] || { log "VOID $name: no topology"; down_both; return 1; }
    UP_READY=0
    for i in 1 2 3 4 5 6 7 8 9 10; do
        if cli "sudo -n ip netns exec t1p_sta ping -c 1 -W 1 10.90.0.3 >/dev/null 2>&1"; then UP_READY=1; break; fi
    done
    echo "ping_ready=$UP_READY tries=$i" >"$name.meta"
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
    if grep -qE '^(mac80211_hwsim|t1p_|t1pg)' "$OUT/state_end.cli" "$OUT/state_end.srv"; then log "SESSION_END: LEFTOVERS"; else log "SESSION_END: clean (no hwsim, no t1p_*)"; fi
    log "T1PI_SESSION_END"
}
