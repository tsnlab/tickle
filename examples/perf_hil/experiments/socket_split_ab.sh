#!/usr/bin/env bash
# A/B: did the same-host socket work (82a6a02d's per-node data socket, 2d72282c's receive
# alternation) cost throughput under heavy injected loss?
#
# Why this regime and not the latency benchmark: at 20-50% tc loss the RELIABLE path is doing
# continuous ACKNACK/retransmit work, so drain_rx() - and therefore tt_try_receive(), which now
# makes up to two recvfrom() calls where it used to make one - runs hot. A same-host latency
# benchmark barely exercises it, which is why it showed nothing. Prompted by COMPARISON.MD §3b's
# own re-measurement coming back at 55 Mbps at 20% and 22 at 50% against published 89.3 and 56.5.
#
# ABBA over the two arms so a drift in the rig across the run cannot be read as an arm difference.
# Each rep records cpu_mhz_mean too: this rig's governor is `ondemand`, so a blocked publisher
# downclocks, and the clock is an effect of the workload rather than a confound - but it is
# recorded so that claim can be checked rather than believed.
#
# How to read it: the arms are separated only if the two A blocks agree with each other and the two
# B blocks agree with each other. If A1 and A2 differ by as much as A differs from B, the rig moved
# during the run and this says nothing.
set -euo pipefail

SSH_KEY="$HOME/.ssh/tickle_ci_ed25519"
RPI_CLIENT="10.1.1.214"
RPI_SERVER="10.1.1.213"
PERF_LINK_BROADCAST="192.168.10.255"
BEFORE="${BEFORE:-0be8c33f}" # parent of 82a6a02d - last commit before the socket split
AFTER="${AFTER:-$(git rev-parse HEAD)}"
REPS="${REPS:-5}"
OUT="${OUT:-/tmp/tickle_socket_ab.txt}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ssh_c() { ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new "ci@$RPI_CLIENT" "$@"; }

CLIENT_IFACE="$(ssh_c "ip route get $PERF_LINK_BROADCAST" 2>/dev/null | grep -oP 'dev \K\S+' | head -1 || true)"
[ -n "$CLIENT_IFACE" ] || { echo "cannot resolve rpi#1's test-link interface - aborting" >&2; exit 1; }

set_loss() {
    local pct="$1"
    if [ "$pct" = "0" ]; then
        ssh_c "sudo -n tc qdisc del dev $CLIENT_IFACE root" >/dev/null 2>&1 || true
    else
        ssh_c "sudo -n tc qdisc replace dev $CLIENT_IFACE root netem loss ${pct}%"
    fi
}
trap 'set_loss 0' EXIT

deploy() {
    local sha="$1" host
    for host in "$RPI_CLIENT" "$RPI_SERVER"; do
        ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=8 "ci@$host" "set -e
cd ~/tickle
git fetch --quiet origin
git reset --hard --quiet $sha
git clean -fdq
rm -rf ~/tickle_local_install
cd ~/tickle/examples/perf_hil/tickle && ./build.sh reliable_throughput >/tmp/ab_build.log 2>&1 || { tail -20 /tmp/ab_build.log; exit 1; }" &
    done
    wait
    # Verify what the rig is actually at, remotely - never by echoing a local expansion.
    ssh_c 'cd ~/tickle && git rev-parse --short HEAD'
}

: > "$OUT"
say() { echo "$*" | tee -a "$OUT"; }
say "=== socket-split A/B, $(date -Is) ==="
say "A = $BEFORE (before the socket split), B = $AFTER (HEAD)"

run_block() {
    local label="$1" sha="$2"
    local at; at="$(deploy "$sha")"
    say ""
    say "--- block $label: requested $sha, rig reports $at ---"
    for pct in 20 50; do
        set_loss "$pct"
        for rep in $(seq 1 "$REPS"); do
            line="$("$HERE/../tickle/run_scenario.sh" reliable_throughput -Q -d 5 2>/dev/null | grep '^RESULT:' | grep 'role=client' || true)"
            [ -n "$line" ] || { say "$label tc${pct}% rep $rep: NO RESULT LINE"; continue; }
            say "$label tc${pct}% rep $rep: mbps=$(echo "$line" | grep -oP 'send_mbps=\K[0-9.]+') mhz=$(echo "$line" | grep -oP 'cpu_mhz_mean=\K[0-9.]+') migrations=$(echo "$line" | grep -oP 'cpu_migrations=\K[0-9]+')"
        done
    done
    set_loss 0
}

run_block A1 "$BEFORE"
run_block B1 "$AFTER"
run_block B2 "$AFTER"
run_block A2 "$BEFORE"

deploy "$AFTER" >/dev/null # leave the rig on HEAD, not on a historical commit
say ""
say "=== done $(date -Is); tc restored, rig left at HEAD ==="
