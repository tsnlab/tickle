#!/usr/bin/env bash
# What the LIVELINESS change costs ordinary traffic (2026-09-26, rmw_tickle/LIVELINESS_PLAN.md amendment 4):
# every datagram now tests tt_Node.liveliness_flags[source] (a lapsed entity to revive), and every DATA tests
# it again for a MANUAL_BY_TOPIC writer to look up. Both are one byte for a node with no such entity, which
# is every node in these scenarios (no discovery table, AUTOMATIC only). Plan asked for it measured on the
# PC before it is claimed.
#
# ARMS: before = origin/main (b90a047d, no liveliness change), after = the working tree. p1
# reliable_throughput (RELIABLE + KEEP_ALL, -N 2048) and p1 reliable_latency, on a veth pair between two
# private netns, interleaved, 3 reps.
#
# HOW TO READ IT, written before running:
#   The claim holds if after is inside before's rep-to-rep spread on server cpu_s_per_Msample (the receive
#   path's cost per sample), recv (throughput) and rtt_avg_ms. A byte test per datagram is ~1 ns against a
#   few us of receive path, far below this PC's noise, so a difference outside the spread would not be the
#   gate - it would say something else in the change reached the per-datagram path (the timer, a scan).
#   CONTROL: before's own spread across its reps is the noise floor the comparison is read against.
#
# SEEN (x86 veth, 2026-09-26; means, sd, per arm):
#   first version, 5 reps: sent 742K (27K) -> 694K (19K), -6.5%, ~3.3 SE; cpu_s_per_Msample 2.42 -> 2.51;
#     rtt_avg 0.0428 -> 0.0414. The throughput cost was the publish path, not the receive gate: every
#     tt_Publisher_publish() read the clock to stamp liveliness_asserted_ns.
#   with that stamp only for a leased Publisher, 5 reps: sent 756K (19K) -> 747K (36K), -1.1%, 0.5 SE;
#     cpu_s_per_Msample 2.39 -> 2.43, 0.7 SE; rtt_avg 0.0396 -> 0.0394. Inside the noise on all three.
#   Arm identity checked by the death log string each binary carries (old "no announce for" / new "silent for").
#
# Usage: veth_liveliness_gate.sh <reps>     Results: $OUT (default /tmp/veth_liveliness_gate.txt)
set -u
REPS=${1:?usage: veth_liveliness_gate.sh <reps>}
OUT=${OUT:-/tmp/veth_liveliness_gate.txt}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PERF="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$PERF/../.." && pwd)"
ARMS_DIR=${ARMS_DIR:-/tmp/veth_liveliness_gate_arms}
BEFORE_WT=/tmp/veth_liveliness_gate_before_wt
NS1=lvg-ns1
NS2=lvg-ns2
CLIENT_LOG=/tmp/veth_liveliness_gate_client.log
SERVER_LOG=/tmp/veth_liveliness_gate_server.log

cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT

build_arm() { # <name> <perf dir> <env...>
    local name=$1 perf=$2
    shift 2
    for scenario in reliable_throughput reliable_latency; do
        env "$@" "$perf/tickle/build.sh" "$scenario" p1 >>"$ARMS_DIR/$name.build.log" 2>&1 ||
            { echo "build $name $scenario failed - $ARMS_DIR/$name.build.log" >>"$OUT"; exit 1; }
        mkdir -p "$ARMS_DIR/$name/${scenario}_p1"
        cp "$perf/tickle/${scenario}_p1/client" "$perf/tickle/${scenario}_p1/server" "$ARMS_DIR/$name/${scenario}_p1/"
    done
}

setup_ns() {
    cleanup
    sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
    sudo -n ip link add lvg1 type veth peer name lvg2 || exit 1
    sudo -n ip link set lvg1 netns "$NS1"
    sudo -n ip link set lvg2 netns "$NS2"
    sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev lvg1
    sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev lvg2
    for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
    sudo -n ip -n "$NS1" link set lvg1 up
    sudo -n ip -n "$NS2" link set lvg2 up
}

run_throughput() { # <arm>
    local arm=$1 dir="$ARMS_DIR/$1/reliable_throughput_p1"
    setup_ns
    # shellcheck disable=SC2024 # the log is this shell's
    sudo -n ip netns exec "$NS2" "$dir/server" -d 8 -N 2048 >"$SERVER_LOG" 2>&1 &
    local server_pid=$!
    sleep 2
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" "$dir/client" -d 3 -Q -N 2048 >"$CLIENT_LOG" 2>&1
    wait "$server_pid"
    echo "kind=throughput arm=$arm $(grep -h '^RESULT' "$CLIENT_LOG" | tr ' ' '\n' | grep -E '^(sent)=' | paste -sd' ') $(grep -h '^RESULT' "$SERVER_LOG" | tr ' ' '\n' | grep -E '^(recv|lost|cpu_s_per_Msample)=' | paste -sd' ')" >>"$OUT"
}

run_latency() { # <arm>
    local arm=$1 dir="$ARMS_DIR/$1/reliable_latency_p1"
    setup_ns
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS2" "$dir/server" -d 8 >"$SERVER_LOG" 2>&1 &
    local server_pid=$!
    sleep 2
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" "$dir/client" -d 3 -i 0.001 >"$CLIENT_LOG" 2>&1
    wait "$server_pid"
    echo "kind=latency arm=$arm $(grep -h '^RESULT' "$CLIENT_LOG" | tr ' ' '\n' | grep -E '^(sent|recv|rtt_min_ms|rtt_avg_ms|rtt_max_ms)=' | paste -sd' ')" >>"$OUT"
}

mkdir -p "$ARMS_DIR"
: >"$OUT"
git -C "$REPO" worktree remove --force "$BEFORE_WT" 2>/dev/null
git -C "$REPO" worktree add --detach "$BEFORE_WT" origin/main >>"$ARMS_DIR/before.build.log" 2>&1 || { echo "worktree failed" >>"$OUT"; exit 1; }
mkdir -p /tmp/veth_liveliness_gate_home
build_arm before "$BEFORE_WT/examples/perf_hil" HOME=/tmp/veth_liveliness_gate_home
git -C "$REPO" worktree remove --force "$BEFORE_WT"
build_arm after "$PERF"
echo "before=$(git -C "$REPO" rev-parse --short origin/main) after=working tree on $(git -C "$REPO" rev-parse --short HEAD)" >>"$OUT"
for _ in $(seq 1 "$REPS"); do
    for arm in before after; do run_throughput "$arm"; done
    for arm in after before; do run_latency "$arm"; done
done
echo "DONE" >>"$OUT"
