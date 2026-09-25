#!/usr/bin/env bash
# Why does TickLE's latency client use ~30x the CPU of CycloneDDS's while having the lowest RTT?
# (OPTIMIZATION_PLAN.md section 9a, hypothesis B1: a fixed-rate poll independent of traffic.)
#
# The campaign's evidence is what does NOT move: TickLE's stime_s is 0.630, 0.631 and 0.622 s
# across an 18x payload difference and a 50x RTT difference. Neither per-byte work nor time spent
# waiting.
#
# HOW TO READ IT, written before running:
#   B1 predicts a syscall count that is LARGE and CONSTANT - order 10^5 for a ~5 s run, and
#   essentially the same at p1 and under +10 ms delay, because the loop's rate does not depend on
#   traffic. 100 round trips cannot account for it.
#     - count ~10^5 and flat across arms  -> B1 holds. The follow-up question is which call.
#     - count tracking the 100 samples    -> B1 is wrong; the cost is per-sample work.
#   CONTROL: CycloneDDS's own latency client under the identical strace. If it shows the same order
#   of magnitude, strace itself is generating the calls and NEITHER arm says anything. Its campaign
#   CPU is 30x lower, so it should be far lower here too.
#
# strace changes timing, so RTT and sample counts from these runs are NOT comparable with the
# campaign's. Only the call counts are being read.
set -uo pipefail
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
OUT=${OUT:-/tmp/b1_syscalls.txt}
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$1" "${@:2}"; }
server_pids() {
    # shellcheck disable=SC2016  # $p must expand on the rig
    sh_ "$SERVER" 'for p in $(ls /proc | grep -E "^[0-9]+$"); do
        e=$(readlink /proc/$p/exe 2>/dev/null)
        case "$e" in */perf_hil/*/server) echo "$p";; esac; done'
}
kill_servers() { local p; p=$(server_pids); [ -z "$p" ] && return 0
    # shellcheck disable=SC2086
    sh_ "$SERVER" "kill -INT $p" >/dev/null 2>&1 || true; sleep 2; }
tc_set() { case "$1" in
    off) sh_ "$CLIENT" "sudo -n tc qdisc del dev eth0 root" >/dev/null 2>&1 || true ;;
    d10) sh_ "$CLIENT" "sudo -n tc qdisc replace dev eth0 root netem delay 10ms 2ms" ;;
  esac; }
trap 'tc_set off; kill_servers' EXIT
: > "$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== B1 syscall count, $(date -Is) ==="

arm() {
    local label="$1" fw="$2" scen="$3" net="$4"
    local dir="tickle/examples/perf_hil/$fw/$scen"
    kill_servers; [ -z "$(server_pids)" ] || { say "$label ABORT: server still up"; return 1; }
    tc_set "$net"
    local env=""
    [ "$fw" = cyclonedds ] && env="export LD_LIBRARY_PATH=/opt/ros/jazzy/lib:\$LD_LIBRARY_PATH;"
    sh_ "$SERVER" "$env cd ~/$dir; nohup ./server -d 5 > /tmp/b1_server.log 2>&1 < /dev/null &"
    sleep 3
    [ "$(server_pids | wc -l)" = 1 ] || { say "$label ABORT: not exactly one server"; return 1; }
    local o; o=$(sh_ "$CLIENT" "$env cd ~/$dir && strace -c -f -o /tmp/b1_strace.txt ./client -d 5 2>&1; echo '--STRACE--'; cat /tmp/b1_strace.txt")
    kill_servers
    local res tot top
    res=$(grep -m1 '^RESULT:' <<<"$o" | grep -oE 'sent=[0-9]+ recv=[0-9]+|rtt_avg_ms=[0-9.]+' | tr '\n' ' ')
    tot=$(awk '/^ *[0-9]/{n+=$4} END{printf "%d", n}' <<<"${o#*--STRACE--}")
    top=$(awk '/^ *[0-9]/{printf "%s=%s ", $NF, $4}' <<<"${o#*--STRACE--}" | tr ' ' '\n' | sort -t= -k2 -rn | head -4 | tr '\n' ' ')
    say ""
    say "### $label"
    say "  result: $res"
    say "  total syscalls: $tot"
    say "  top calls: $top"
}

arm "A  TickLE     p1, no shaping" tickle     reliable_latency_p1 off
arm "B  TickLE     p1, +10ms delay" tickle    reliable_latency_p1 d10
arm "C  CONTROL CycloneDDS p1, no shaping" cyclonedds reliable_latency off
tc_set off
say ""
say "=== done ==="
