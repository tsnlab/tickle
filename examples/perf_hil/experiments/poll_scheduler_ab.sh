#!/usr/bin/env bash
# Does the scheduler-driven poll timeout (69e63a34) cut ppoll calls without costing latency?
#
# Clean A/B: 9a230a1b has the KEEP_ALL byte budget and NOT the poll change; 69e63a34 adds only the
# poll change. Nothing else differs.
#
# HOW TO READ IT, written before running:
#   PRIMARY, c10 (reliable_latency p1): ppoll calls should fall to within a small multiple of the
#     datagrams the run actually handled. The criterion is self-calibrating on purpose - a ratio
#     against the old value only asks "did it get better", which is how a 7.7x reduction from a
#     working change nearly read as a failure when node_flush was still ticking at 1 ms. The excess
#     over the packet count IS the measurement of any remaining cadence, and its size says how fast
#     that cadence ticks.
#   DECISIVE CONTROL, c10 rtt_avg_ms: must NOT get worse outside the spread of 3 repetitions. The
#     100 us spin is the plausible reason TickLE has the lowest RTT of the three (0.204 ms against
#     0.358 and 0.282), so a version that cuts CPU and loses the latency is a loss, not a win. Both
#     rig hosts report cpuidle current_driver=none, so a regression here cannot be a sleeping core
#     and is the change itself.
#   c1 (reliable_throughput p1): throughput must stay inside its spread. Its ppoll count is NOT a
#     pass criterion - at max rate the publisher's own self-rescheduling entry is always due, so the
#     busy-node cap returns control every 100 us of work by design (TickLE Dev). Those are entries
#     falling due, not idle wakes, and reading c1 as idle would report a third cadence that is not
#     one.
#   IDENTITY: each arm records the rig's checked-out HEAD. Weaker than a library-reported value like
#     retry_interval_cfg_ns - it proves the checkout, not the binary - so the build step failing
#     loudly is what carries the rest.
#
# strace distorts timing, so RTT and throughput come from un-straced runs and only the call counts
# come from the straced ones.
set -uo pipefail
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
PIN="taskset -c 1-3"; REPS=${REPS:-3}
OUT=${OUT:-/tmp/poll_ab.txt}
BEFORE=9a230a1b; AFTER=69e63a34
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
say "=== scheduler-driven poll A/B, $(date -Is) ==="
say "before=$BEFORE (KEEP_ALL budget, no poll change)   after=$AFTER (adds the poll change)"

build() {
    local sha="$1" pids=() bad=0
    say ""; say "--- building $sha on both rpis ---"
    for h in "$CLIENT" "$SERVER"; do
        sh_ "$h" "set -e
cd ~/tickle && git fetch -q origin && git reset -q --hard $sha && git clean -fdqx -e install -e build -e log
cd examples/perf_hil/tickle
for s in reliable_latency reliable_throughput; do
  ./build.sh \$s p1 > /tmp/pollab_build.log 2>&1 || { echo \"BUILD FAILED \$s on \$(hostname)\"; tail -8 /tmp/pollab_build.log; exit 1; }
done
echo \"\$(hostname) at \$(git -C ~/tickle rev-parse --short HEAD)\"" &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p" || bad=1; done
    [ "$bad" = 0 ] || { say "BUILD FAILED - stopping"; exit 1; }
    local head; head=$(sh_ "$CLIENT" 'git -C ~/tickle rev-parse --short HEAD')
    say "  identity: client HEAD=$head (wanted $sha)"
    [ "$head" = "$sha" ] || { say "  IDENTITY FAIL - stopping"; exit 1; }
}

one_run() {   # $1 scenario_dir  $2 use_strace(0|1)
    local dir="tickle/examples/perf_hil/tickle/$1" st="$2"
    kill_servers
    [ -z "$(server_pids)" ] || { echo "ABORT-server"; return; }
    sh_ "$SERVER" "cd ~/$dir; nohup $PIN ./server -d 5 -Q > /tmp/pollab_srv.log 2>&1 < /dev/null &"
    sleep 3
    [ "$(server_pids | wc -l)" = 1 ] || { echo "ABORT-nserver"; return; }
    local cmd="cd ~/$dir && $PIN ./client -d 5 -Q"
    [ "$st" = 1 ] && cmd="cd ~/$dir && strace -c -f -o /tmp/pollab_st.txt $PIN ./client -d 5 -Q"
    local o; o=$(sh_ "$CLIENT" "$cmd" 2>&1 | grep -m1 '^RESULT:')
    local pp=""
    [ "$st" = 1 ] && pp=$(sh_ "$CLIENT" "awk '/ ppoll\$/{print \$4}' /tmp/pollab_st.txt")
    kill_servers
    echo "$o|ppoll=${pp:-na}"
}

arm() {
    local tag="$1" r line
    say ""
    line=$(one_run reliable_latency_p1 1)
    say "$tag c10 straced | $(grep -oE '(sent|recv|rtt_avg_ms|wire_rx_packets|wire_tx_packets)=[0-9.]+' <<<"$line" | tr '\n' ' ')ppoll=${line##*|ppoll=}"
    for r in $(seq 1 "$REPS"); do
        line=$(one_run reliable_latency_p1 0)
        say "$tag c10 rep$r     | $(grep -oE '(sent|recv|rtt_avg_ms|rtt_max_ms)=[0-9.]+' <<<"$line" | tr '\n' ' ')"
    done
    line=$(one_run reliable_throughput_p1 1)
    say "$tag c1  straced | $(grep -oE '(sent|send_mbps|wire_tx_packets)=[0-9.]+' <<<"$line" | tr '\n' ' ')ppoll=${line##*|ppoll=}"
    for r in $(seq 1 "$REPS"); do
        line=$(one_run reliable_throughput_p1 0)
        say "$tag c1  rep$r     | $(grep -oE '(sent|send_mbps)=[0-9.]+' <<<"$line" | tr '\n' ' ')"
    done
}

build "$BEFORE"; arm "BEFORE"
build "$AFTER";  arm "AFTER "
say ""; say "=== done ==="
