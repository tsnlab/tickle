#!/usr/bin/env bash
# Does the dynamic retry interval (tt_RELIABLE_RETRY_INTERVAL=0, TickLE Dev 871129d8) make the
# healthy loss case worse? This is the disproof arm that was pre-registered before the change was
# built: the estimator exists to stop the collapsed P4 regime re-requesting three times per
# recovery, and the risk is that it does something unwanted where the fixed 1 ms was already fine.
#
# At c5 the measured healthy recovery is ~250 us, so srtt + 4*rttvar should land near the 250 us
# floor - SHORTER than the fixed 1 ms. Dynamic therefore retries *sooner* at c5, which is the
# opposite of what it does at P4, and is exactly why c5 rather than c6 is the gate.
#
# HOW TO READ IT, written before running:
#   PRIMARY - c5 (P1 + 5% loss, RELIABLE + KEEP_ALL):
#     dynamic must not be worse than fixed on either send_mbps (higher better) or
#     wire_bytes_per_sample (lower better), outside the spread of 3 repetitions. A throughput drop
#     or a bandwidth rise outside the spread means the default must NOT flip to 0.
#   CONTROL - c1 (P1, no shaping): no loss means no recoveries, so the estimator never learns and
#     the two builds must be indistinguishable. If they differ here, the change is touching
#     something outside the retry path and NEITHER c5 arm says anything.
#   IDENTITY - every run must report retry_interval_cfg_ns matching its own arm (1000000 fixed,
#     0 dynamic). The value comes from the library through tt_reliable_retry_interval_configured(),
#     not from the harness's view of config.h, so it reports what RAN. A mismatch voids that run:
#     the two builds share one output directory, so a rebuild that silently failed would otherwise
#     be measured as the other arm.
#   recovery_srtt_ns / recovery_rttvar_ns say what the estimator actually learned, so the interval
#   it implies can be checked rather than trusted.
set -uo pipefail
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
PIN="taskset -c 1-3"; REPS=${REPS:-3}
OUT=${OUT:-/tmp/retry_ab.txt}
SHA=$(git -C "$(dirname "${BASH_SOURCE[0]}")/../../.." rev-parse origin/main)
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$1" "${@:2}"; }
tc_set() { case "$1" in
    off) sh_ "$CLIENT" "sudo -n tc qdisc del dev eth0 root" >/dev/null 2>&1 || true ;;
    on)  sh_ "$CLIENT" "sudo -n tc qdisc replace dev eth0 root netem loss 5%" ;;
  esac; }
server_pids() {
    # shellcheck disable=SC2016  # $p must expand on the rig
    sh_ "$SERVER" 'for p in $(ls /proc | grep -E "^[0-9]+$"); do
        e=$(readlink /proc/$p/exe 2>/dev/null)
        case "$e" in */perf_hil/tickle/*/server) echo "$p";; esac; done'
}
kill_servers() { local p; p=$(server_pids); [ -z "$p" ] && return 0
    # shellcheck disable=SC2086
    sh_ "$SERVER" "kill -INT $p" >/dev/null 2>&1 || true; sleep 2; }
trap 'tc_set off; kill_servers' EXIT
: > "$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== dynamic vs fixed retry interval, $(date -Is), repo $SHA ==="

build() {   # $1 = 0 (fixed) | 1 (dynamic)
    local dyn="$1" pids=() bad=0
    say "--- building with TICKLE_DYNAMIC_RETRY=$dyn on both rpis ---"
    for h in "$CLIENT" "$SERVER"; do
        sh_ "$h" "set -e
cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdq
cd examples/perf_hil/tickle
TICKLE_DYNAMIC_RETRY=$dyn ./build.sh reliable_throughput p1 > /tmp/retryab_build.log 2>&1 \
  || { echo \"BUILD FAILED on \$(hostname)\"; tail -8 /tmp/retryab_build.log; exit 1; }
echo \"built on \$(hostname)\"" &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p" || bad=1; done
    [ "$bad" = 0 ] || { say "BUILD FAILED - stopping"; exit 1; }
}

run_cell() {   # $1 label  $2 net(on|off)  $3 expected cfg_ns
    local label="$1" net="$2" want="$3" r
    tc_set "$net"
    for r in $(seq 1 "$REPS"); do
        kill_servers
        [ -z "$(server_pids)" ] || { say "$label rep$r ABORT: server still up"; continue; }
        sh_ "$SERVER" "cd ~/tickle/examples/perf_hil/tickle/reliable_throughput_p1; nohup $PIN ./server -d 5 -Q > /tmp/retryab_srv.log 2>&1 < /dev/null &"
        sleep 3
        [ "$(server_pids | wc -l)" = 1 ] || { say "$label rep$r ABORT: not exactly one server"; continue; }
        local cl sv cfg
        cl=$(sh_ "$CLIENT" "cd ~/tickle/examples/perf_hil/tickle/reliable_throughput_p1 && $PIN ./client -d 5 -Q" | grep -m1 '^RESULT:')
        kill_servers
        sv=$(sh_ "$SERVER" "grep -m1 '^RESULT:' /tmp/retryab_srv.log")
        cfg=$(grep -oE 'retry_interval_cfg_ns=[0-9]+' <<<"$sv" | cut -d= -f2)
        if [ "$cfg" != "$want" ]; then
            say "$label rep$r IDENTITY FAIL: retry_interval_cfg_ns=$cfg, wanted $want - VOID"
            continue
        fi
        say "$label rep$r | $(grep -oE '(sent|send_mbps|wire_bytes_per_sample)=[0-9.]+' <<<"$cl" | tr '\n' ' ')| $(grep -oE '(recv|retry_interval_cfg_ns|recovery_srtt_ns|recovery_rttvar_ns)=[0-9]+' <<<"$sv" | tr '\n' ' ')"
    done
}

build 0
say ""; run_cell "A  c5 fixed  " on  1000000
say ""; run_cell "C  c1 fixed  " off 1000000
build 1
say ""; run_cell "B  c5 dynamic" on  0
say ""; run_cell "D  c1 dynamic" off 0
tc_set off
say ""
say "qdisc now: $(sh_ "$CLIENT" 'tc qdisc show dev eth0')"
say "=== done ==="
