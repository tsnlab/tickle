#!/usr/bin/env bash
# shellcheck disable=SC1091,SC2029,SC2016,SC2086,SC2129,SC2012,SC2024,SC2034,SC2209  # experiment driver: ssh-side expansion, unquoted word lists and sudo redirects are deliberate
# Shared helpers of the tc-emulation runners (wtc_run.sh); sourced, not run.
# Needs before sourcing: S (this directory), REPO, SET (a name for the log), OUT (results directory, created here).
KEY=${PI_KEY:-$HOME/.ssh/tickle_ci_ed25519}
SSHO="-i $KEY -o StrictHostKeyChecking=no -o BatchMode=yes -o ControlMaster=auto -o ControlPath=/tmp/wtc_cm_%h -o ControlPersist=900"
CLI_H=${CLI_SSH:-ci@10.1.1.214}
SRV_H=${SRV_SSH:-ci@10.1.1.213}
CLI_ETH=192.168.10.3
SRV_ETH=192.168.10.2
T=wtc_scripts/wtc_topo.sh
BIN=t1pi_bin
cli() { ssh -n $SSHO "$CLI_H" "$@"; }
srv() { ssh -n $SSHO "$SRV_H" "$@"; }
cli_in() { ssh $SSHO "$CLI_H" "$@"; }
srv_in() { ssh $SSHO "$SRV_H" "$@"; }
log() { echo "$(date -Is) $*" | tee -a "$OUT/runner.log"; }

# The emulated link profiles (arms). tc netem on the egress of both taps, so every number is per direction; the round trip
# is twice the delay. Loss: "loss 1%" is independent per packet; the Gilbert-Elliott model is `gemodel p r 1-h 1-k`
# (p good->bad, r bad->good per packet, 1-h loss in the bad state, 1-k in the good state). `limit` is the packet queue (tail drop).
#   M1  30 Mbit/s, 5 ms,  1 % independent loss, limit 100
#   M2  30 Mbit/s, 20 ms, bursty loss (gemodel 1% 25% 70% 0.1%, about 2.7 % mean, bad runs of about 4 packets), limit 100
#   S1   4 Mbit/s, 5 ms,  1 % independent loss, limit 40
#   S2   4 Mbit/s, 20 ms, bursty loss (gemodel 2% 25% 70% 0.1%, about 5 % mean), limit 40
#   C1  30 Mbit/s, 5 ms, no loss, limit 100 (control, calibration only)
profile_netem() { # PROFILE -> NETEM
    case "$1" in
    M1) NETEM="limit 100 delay 5ms rate 30mbit loss 1%" ;;
    M2) NETEM="limit 100 delay 20ms rate 30mbit loss gemodel 1% 25% 70% 0.1%" ;;
    S1) NETEM="limit 40 delay 5ms rate 4mbit loss 1%" ;;
    S2) NETEM="limit 40 delay 20ms rate 4mbit loss gemodel 2% 25% 70% 0.1%" ;;
    C1) NETEM="limit 100 delay 5ms rate 30mbit" ;;
    *) echo "unknown profile $1"; return 1 ;;
    esac
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
    cli "WTC_BIN=\$HOME/$BIN $T cli down" >>"$OUT/down.log" 2>&1
    srv "WTC_BIN=\$HOME/$BIN $T srv down" >>"$OUT/down.log" 2>&1
}

# bring_up NAME PROFILE: fresh namespaces with the profile's netem on both taps (3 attempts), then wait for the first ping across the link; sets UP_READY
bring_up() {
    local name=$1 attempt ok=0 i
    profile_netem "$2" || return 1
    for attempt in 1 2 3; do
        down_both
        sleep 1
        srv "NETEM='$NETEM' LOCAL_IP=$SRV_ETH REMOTE_IP=$CLI_ETH $T srv up" >"$name.topo" 2>&1
        cli "NETEM='$NETEM' LOCAL_IP=$CLI_ETH REMOTE_IP=$SRV_ETH $T cli up" >>"$name.topo" 2>&1
        if grep -q '^WTC_SRV_UP' "$name.topo" && grep -q '^WTC_CLI_UP' "$name.topo"; then ok=1; break; fi
        log "HARNESS_FAULT $name attempt $attempt: topology did not come up"
    done
    [ $ok = 1 ] || { log "VOID $name: no topology"; down_both; return 1; }
    UP_READY=0
    for i in 1 2 3 4 5 6 7 8 9 10; do
        if cli "sudo -n ip netns exec wtc_sta ping -c 1 -W 1 10.90.0.3 >/dev/null 2>&1"; then UP_READY=1; break; fi
    done
    echo "ping_ready=$UP_READY tries=$i netem=$NETEM" >"$name.meta"
    return 0
}

session_start() {
    log "WTC_SESSION_START set=$SET out=$OUT tree=$(git -C "$REPO" rev-parse --short HEAD)"
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
    if grep -qE '^(wtc_|wtcg)' "$OUT/state_end.cli" "$OUT/state_end.srv"; then log "SESSION_END: LEFTOVERS"; else log "SESSION_END: clean (no wtc_* namespace or tap)"; fi
    log "WTC_SESSION_END"
}
