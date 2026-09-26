#!/usr/bin/env bash
# L3 of rmw_tickle/LIVELINESS_PLAN.md (2026-09-26): no false deaths. The liveliness_loss_detection pair runs over a
# veth pair on this PC with netem loss on both directions for 120 s, and nothing is killed. The server exits
# with departed=1 on a (false) departure, or at its safety cap with departed=0. Two cases:
#   data  lease 2 s, the client publishing at 10 Hz
#   idle  lease 1 s, the client publishing once an hour (-i 3600): only the node's own summaries keep the lease
# each on the core before the change (the control) and after it.
#
# HOW TO READ IT (LIVELINESS_PLAN section 4 and amendment 1, written before running):
#   PASS if the after core shows departed=0 in both cases, every repetition.
#   The before core is expected to fail the idle case (a 1 s lease against 1 s summaries flaps, amendment 1), and
#   is the control that the test can fail at all.
# A count of departures, not a timing, so the PC is the right place for it.
#
# The two cores are built from git worktrees of the two commits, each under its own HOME so that build.sh's
# install prefixes cannot collide, with this checkout's harness files overlaid (the -i flag is newer than
# the before commit). Processes are stopped by netns membership (ip netns pids), never by name.
#
# Usage: BEFORE=<sha> AFTER=<sha> liveliness_l3.sh [reps] [seconds]   Output: $OUT (default /tmp/liveliness_l3.txt)
set -u
REPS=${1:-2}
SECS=${2:-120}
LOSS=${LOSS:-5}
OUT=${OUT:-/tmp/liveliness_l3.txt}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
BEFORE=$(git -C "$REPO" rev-parse "${BEFORE:?BEFORE=sha}") || exit 1
AFTER=$(git -C "$REPO" rev-parse "${AFTER:?AFTER=sha}") || exit 1
NS1=l3-ns1
NS2=l3-ns2
kill_ns() { local p; for p in $(sudo -n ip netns pids "$1" 2>/dev/null); do sudo -n ip netns exec "$1" kill -TERM "$p" 2>/dev/null; done; }
cleanup() {
    kill_ns "$NS1"; kill_ns "$NS2"
    sudo -n ip netns del "$NS1" 2>/dev/null; sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT
: > "$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== liveliness L3, $(date -Is), before ${BEFORE:0:8}, after ${AFTER:0:8}, $REPS reps x ${SECS}s, loss ${LOSS}% both ways ==="

build() { # $1 sha -> prints the scenario dir
    local sha=$1 wt=/tmp/l3_wt_${1:0:8} home=/tmp/l3_home_${1:0:8}
    if [ ! -d "$wt" ]; then git -C "$REPO" worktree add -q --detach "$wt" "$sha" || return 1; fi
    cp "$REPO"/examples/perf_hil/tickle/liveliness_loss_detection/*.c "$wt/examples/perf_hil/tickle/liveliness_loss_detection/"
    mkdir -p "$home"
    (cd "$wt/examples/perf_hil/tickle" && HOME=$home ./build.sh liveliness_loss_detection p1 > "/tmp/l3_build_${sha:0:8}.log" 2>&1) || return 1
    echo "$wt/examples/perf_hil/tickle/liveliness_loss_detection_p1"
}
DB=$(build "$BEFORE") || { say "BUILD FAILED (before), see /tmp/l3_build_${BEFORE:0:8}.log"; exit 1; }
DA=$(build "$AFTER") || { say "BUILD FAILED (after), see /tmp/l3_build_${AFTER:0:8}.log"; exit 1; }

run() { # $1 arm, $2 dir, $3 case, $4 rep
    local arm=$1 dir=$2 kind=$3 rep=$4 lease=2.0 cargs="" res
    [ "$kind" = idle ] && { lease=1.0; cargs="-i 3600"; }
    cleanup
    sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
    sudo -n ip link add l3v1 type veth peer name l3v2
    sudo -n ip link set l3v1 netns "$NS1"; sudo -n ip link set l3v2 netns "$NS2"
    sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev l3v1; sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev l3v2
    for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
    sudo -n ip -n "$NS1" link set l3v1 up; sudo -n ip -n "$NS2" link set l3v2 up
    sudo -n ip netns exec "$NS1" tc qdisc add dev l3v1 root netem loss "${LOSS}%" || exit 1
    sudo -n ip netns exec "$NS2" tc qdisc add dev l3v2 root netem loss "${LOSS}%" || exit 1
    # shellcheck disable=SC2024 # the logs are meant to be written as this user
    (cd "$dir" && sudo -n ip netns exec "$NS2" ./server -T "$lease" -d "$SECS" > /tmp/l3_server.log 2>&1) &
    local spid=$!
    # shellcheck disable=SC2024,SC2086 # as above; cargs is a list of flags
    (cd "$dir" && sudo -n ip netns exec "$NS1" ./client -T "$lease" $cargs > /tmp/l3_client.log 2>&1) &
    wait "$spid"
    kill_ns "$NS1"
    wait
    res=$(grep '^RESULT:' /tmp/l3_server.log | head -1)
    local verdict=ok
    [ -n "$res" ] || verdict="VOID(no RESULT)"
    say "$arm $kind lease=$lease rep$rep | $verdict | ${res#RESULT: }"
}
for rep in $(seq 1 "$REPS"); do
    for kind in data idle; do
        run before "$DB" "$kind" "$rep"
        run after "$DA" "$kind" "$rep"
    done
done
say "=== done ==="
