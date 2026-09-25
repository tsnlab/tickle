#!/usr/bin/env bash
# Re-run after 6f01de6f, which removes the per-entry clock_gettime() that cost c1 1.53%.
# Two questions in one session:
#   (a) is c1 throughput back inside the pre-change spread?
#   (b) is c10's +2.14% RTT mean drift real, at 10 repetitions instead of 3?
#
# HOW TO READ IT, written before running:
#   (a) c1: BEFORE (9a230a1b, no poll change) and AFTER (6f01de6f) ranges must OVERLAP over 3 reps.
#       Rebuilt in the same session rather than compared against yesterday's numbers, because rig
#       conditions drift and the comparison that matters is same-session.
#   (b) c10 RTT at n=10 per side. "Overlap" is the WRONG test at n=10 - more repetitions widen the
#       observed range, so overlap gets easier as evidence accumulates, which is backwards. The
#       criterion is instead: the drift is REAL if |mean_after - mean_before| exceeds 2x the
#       combined standard error, and NOT RESOLVED otherwise. Pre-registered here so it cannot be
#       chosen after seeing which way it falls.
#   (c) c10 ppoll must stay order-tens against ~14 rx datagrams - the 1133x must survive the fix,
#       which only touches the busy path.
#   IDENTITY: each arm asserts the rig's checked-out HEAD.
set -uo pipefail
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
PIN="taskset -c 1-3"
OUT=${OUT:-/tmp/poll_fix.txt}
BEFORE=9a230a1b; AFTER=$(git -C "$(dirname "${BASH_SOURCE[0]}")/../../.." rev-parse --short origin/main)
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$1" "${@:2}"; }
server_pids() {
    # shellcheck disable=SC2016  # $p must expand on the rig
    sh_ "$SERVER" 'for p in $(ls /proc | grep -E "^[0-9]+$"); do
        e=$(readlink /proc/$p/exe 2>/dev/null)
        case "$e" in */perf_hil/tickle/*/server) echo "$p";; esac; done'
}
kill_servers() { local p; p=$(server_pids); [ -z "$p" ] && return 0
    # shellcheck disable=SC2086
    sh_ "$SERVER" "kill -INT $p" >/dev/null 2>&1 || true; sleep 2; }
trap kill_servers EXIT
: > "$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== poll fix verification, $(date -Is) ==="
say "before=$BEFORE (no poll change)   after=$AFTER (poll change + per-entry clock fix)"

build() {
    local sha="$1" pids=() bad=0
    say ""; say "--- building $sha ---"
    for h in "$CLIENT" "$SERVER"; do
        sh_ "$h" "set -e
cd ~/tickle && git fetch -q origin && git reset -q --hard $sha && git clean -fdq
cd examples/perf_hil/tickle
for s in reliable_latency reliable_throughput; do
  ./build.sh \$s p1 > /tmp/pfv_build.log 2>&1 || { echo \"BUILD FAILED \$s on \$(hostname)\"; tail -8 /tmp/pfv_build.log; exit 1; }
done" &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p" || bad=1; done
    [ "$bad" = 0 ] || { say "BUILD FAILED - stopping"; exit 1; }
    local head; head=$(sh_ "$CLIENT" 'git -C ~/tickle rev-parse --short HEAD')
    say "  identity: HEAD=$head (wanted $sha)"
    [ "$head" = "$sha" ] || { say "  IDENTITY FAIL - stopping"; exit 1; }
}
one_run() {
    local dir="tickle/examples/perf_hil/tickle/$1" st="$2"
    kill_servers; [ -z "$(server_pids)" ] || { echo "ABORT-server"; return; }
    sh_ "$SERVER" "cd ~/$dir; nohup $PIN ./server -d 5 -Q > /tmp/pfv_srv.log 2>&1 < /dev/null &"
    sleep 3
    [ "$(server_pids | wc -l)" = 1 ] || { echo "ABORT-nserver"; return; }
    local cmd="cd ~/$dir && $PIN ./client -d 5 -Q"
    [ "$st" = 1 ] && cmd="cd ~/$dir && strace -c -f -o /tmp/pfv_st.txt $PIN ./client -d 5 -Q"
    local o; o=$(sh_ "$CLIENT" "$cmd" 2>&1 | grep -m1 '^RESULT:')
    local pp=""; [ "$st" = 1 ] && pp=$(sh_ "$CLIENT" "awk '/ ppoll\$/{print \$4}' /tmp/pfv_st.txt")
    kill_servers
    echo "$o|ppoll=${pp:-na}"
}
arm() {
    local tag="$1" r line
    say ""
    for r in $(seq 1 3); do
        line=$(one_run reliable_throughput_p1 0)
        say "$tag c1  rep$r  | $(grep -oE '(sent|send_mbps)=[0-9.]+' <<<"$line" | tr '\n' ' ')"
    done
    for r in $(seq 1 10); do
        line=$(one_run reliable_latency_p1 0)
        say "$tag c10 rep$r | $(grep -oE '(sent|recv|rtt_avg_ms|rtt_max_ms)=[0-9.]+' <<<"$line" | tr '\n' ' ')"
    done
    line=$(one_run reliable_latency_p1 1)
    say "$tag c10 strace| $(grep -oE '(wire_rx_packets|rtt_avg_ms)=[0-9.]+' <<<"$line" | tr '\n' ' ')ppoll=${line##*|ppoll=}"
}
build "$BEFORE"; arm "BEFORE"
build "$AFTER";  arm "AFTER "
say ""; say "=== done ==="
