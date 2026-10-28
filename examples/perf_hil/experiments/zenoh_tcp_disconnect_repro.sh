#!/usr/bin/env bash
# zenoh_tcp_disconnect_repro.sh - zenoh-pico's RELIABLE (TCP peer-to-peer) arm, reproduced off the rig: two private
# network namespaces joined by a veth, the pinned release (zenohpico/build.sh, 1.10.2), built with its own logging on.
#
# THE QUESTION. On the rig (2026-09-30, 1.10.1) the reliable arm died above p1: the subscriber logged
# "Connection compromised due to message processing error: -114" and "Dropping peer", about 7 s into a max-rate run,
# and the publisher went on counting successful puts into a session with no link (fd5d56c4). The user asked on
# 2026-10-28 that the disconnect be stated WITH its cause. This script answers: does it still happen on the pinned
# release, at which sizes and rates, and what the subscriber's own log says just before it drops the peer.
#
# READING RULES, written before the first run and implemented in summarise() below, not only here:
#   - A run DIED if the subscriber's log contains "Dropping peer" or "Connection compromised", or if the subscriber
#     received fewer than 90% of what the publisher reports sent (TCP is lossless: a shortfall means the link went).
#   - A run SURVIVED if neither, AND the subscriber received >= 1000 samples. Fewer than 1000 is VOID: a run that
#     moved nothing cannot show survival.
#   - A run whose subscriber log could not be read, or has no RESULT line, is VOID (no answer is not "survived").
#   - The cause is NOT hunted here (the user, 2026-10-28: state THAT it drops and when; the cause is recorded as
#     expected to come from zenoh-pico's internal structure). The script prints the subscriber's own error lines
#     (level 1 = errors only, a one-flag build) so the row can cite what zenoh-pico itself says.
#   - Control: p1 at the same rate. If p1 dies too, the netns setup (not the size) is the suspect and nothing about
#     p2+ may be concluded from the run.
#
# Usage: zenoh_tcp_disconnect_repro.sh            env: SIZES ("p1 p2 p4"), INTERVALS ("0.001 0") - 0 = max rate,
#        LEVEL (zenoh-pico log level, 1 = errors only), DUR (s, 8), NETEM ("" or e.g. "delay 1ms loss 1%"), OUT dir,
#        ZENOH_CMAKE_OPTS (a tuned arm; gets its own prefix), CPU (core to pin both processes to, default 15).
# Output: $OUT/runs.txt (one line per run + verdict), $OUT/<run>_{pub,sub}.log (each capped at 64 MB).
# Load: two processes pinned to ONE core, niced, for DUR seconds per run - a small repro, not a measurement.
# Needs passwordless `sudo -n ip`; the processes run inside the namespaces through `ip netns exec`.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZP="$HERE/../zenohpico"
SIZES=${SIZES:-"p1 p2 p4"}
INTERVALS=${INTERVALS:-"0.001 0"}
LEVEL=${LEVEL:-1}
DUR=${DUR:-8}
NETEM=${NETEM:-}
CPU=${CPU:-15}
# CPU_PUB / CPU_SUB put the two sides on separate cores (as on the rig, where each Pi has its own); both default to CPU.
CPU_SUB=${CPU_SUB:-$CPU}
CPU_PUB=${CPU_PUB:-$CPU}
REPS=${REPS:-1}
OUT=${OUT:-/home/semih/zenoh_fair_results/tcp_repro_$(date +%Y%m%d_%H%M%S)}
ZENOH_CMAKE_OPTS=${ZENOH_CMAKE_OPTS:-}
BASE=/home/semih/zenoh_fair_results/pc
CAP=$((64 * 1024 * 1024))
NS1=zfs1 NS2=zfs2 # subscriber (listens), publisher (connects)
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}
mkdir -p "$OUT" "$BASE"
RUNS="$OUT/runs.txt"
tag=$(printf '%s' "L${LEVEL}_${ZENOH_CMAKE_OPTS}" | tr -c 'A-Za-z0-9_=' '_')

netns_down() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
on_exit() { netns_down; }
trap on_exit EXIT

netns_up() {
    netns_down
    sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || return 1
    sudo -n ip link add zfv1 netns "$NS1" type veth peer name zfv2 netns "$NS2" || return 1
    sudo -n ip -n "$NS1" addr add 10.77.0.1/24 dev zfv1 && sudo -n ip -n "$NS2" addr add 10.77.0.2/24 dev zfv2 || return 1
    for ns in "$NS1" "$NS2"; do
        sudo -n ip -n "$ns" link set lo up
    done
    sudo -n ip -n "$NS1" link set zfv1 up && sudo -n ip -n "$NS2" link set zfv2 up || return 1
    if [ -n "$NETEM" ]; then
        # shellcheck disable=SC2086 # NETEM is a deliberate word list
        sudo -n ip netns exec "$NS2" tc qdisc add dev zfv2 root netem $NETEM || return 1
        sudo -n ip netns exec "$NS2" tc qdisc show dev zfv2 | grep -q netem || return 1
    fi
}

# Build once per size into a directory of this run's own, so the level and options can never be mixed up.
build_size() {
    local size=$1 bdir="$BASE/bin_${tag}_$1"
    if [ ! -x "$bdir/client" ]; then
        ( cd "$ZP" && ZENOH_DEBUG="$LEVEL" ZENOH_BUILD_JOBS=2 ZENOH_CMAKE_OPTS="$ZENOH_CMAKE_OPTS" \
            ZENOH_PICO_PREFIX="$BASE/install_${tag}" ZENOH_PICO_SRC="$BASE/src_${tag}" \
            nice -n 19 ./build.sh reliable_throughput "$size" ) >"$OUT/build_$size.log" 2>&1 || {
            echo "BUILD FAILED $size, see $OUT/build_$size.log"; return 1; }
        mkdir -p "$bdir" && cp "$ZP/reliable_throughput_$size/client" "$ZP/reliable_throughput_$size/server" "$bdir/"
    fi
    echo "$bdir"
}

run_one() {
    local size=$1 iv=$2 bdir=$3 name="${1}_i${2}_r${4}" sp pp
    netns_up || { echo "run=$name VOID netns" | tee -a "$RUNS"; return; }
    sudo -n ip netns exec "$NS1" env BENCH_ZENOH_LISTEN=tcp/10.77.0.1:7447 nice -n 19 taskset -c "$CPU_SUB" \
        stdbuf -oL "$bdir/server" -d $((DUR + 8)) 2>&1 | head -c "$CAP" >"$OUT/${name}_sub.log" &
    sp=$!
    sleep 1
    sudo -n ip netns exec "$NS2" env BENCH_ZENOH_CONNECT=tcp/10.77.0.1:7447 nice -n 19 taskset -c "$CPU_PUB" \
        stdbuf -oL "$bdir/client" -d "$DUR" -i "$iv" 2>&1 | head -c "$CAP" >"$OUT/${name}_pub.log"
    pp=$?
    wait "$sp"
    summarise "$name" "$pp"
}

summarise() {
    local name=$1 sub="$OUT/${1}_sub.log" pub="$OUT/${1}_pub.log" sent recv drop verdict first
    sent=$(grep -o 'role=client sent=[0-9]*' "$pub" 2>/dev/null | grep -o '[0-9]*$')
    recv=$(grep -o 'role=server received=[0-9]*' "$sub" 2>/dev/null | grep -o '[0-9]*$')
    drop=$(grep -c 'Dropping peer\|Connection compromised' "$sub" 2>/dev/null)
    if [ -z "$recv" ] || [ -z "$sent" ]; then
        verdict="VOID(no RESULT: sub='${recv}' pub='${sent}')"
    elif [ "${drop:-0}" -gt 0 ] || [ "$recv" -lt $((sent * 9 / 10)) ]; then
        verdict=DIED
    elif [ "$recv" -lt 1000 ]; then
        verdict="VOID(received<1000)"
    else
        verdict=SURVIVED
    fi
    echo "run=$name level=$LEVEL opts='$ZENOH_CMAKE_OPTS' netem='$NETEM' cpu=$CPU_PUB/$CPU_SUB sent=$sent received=$recv drop_lines=$drop" \
        "pub_exit=$2 verdict=$verdict" | tee -a "$RUNS"
    first=$(grep -n "Dropping peer\|Connection compromised" "$sub" | head -1 | cut -d: -f1)
    if [ -n "$first" ]; then
        echo "  last 40 subscriber lines before the first drop line (line $first):" | tee -a "$RUNS"
        sed -n "$((first > 40 ? first - 40 : 1)),${first}p" "$sub" | sed 's/^/    | /' | tee -a "$RUNS"
    fi
    grep -E '_Z_ERR|ERROR|WARN' "$pub" | head -5 | sed 's/^/  pub| /' | tee -a "$RUNS"
}

echo "=== zenoh_tcp_disconnect_repro $(date -Is) sizes='$SIZES' intervals='$INTERVALS' level=$LEVEL dur=$DUR" \
    "netem='$NETEM' opts='$ZENOH_CMAKE_OPTS' ===" | tee -a "$RUNS"
for size in $SIZES; do
    bdir=$(build_size "$size" | tail -1) || exit 1
    [ -x "$bdir/client" ] || { echo "no binaries for $size: $bdir" | tee -a "$RUNS"; exit 1; }
    for iv in $INTERVALS; do
        for r in $(seq 1 "$REPS"); do run_one "$size" "$iv" "$bdir" "$r"; done
    done
done
echo "=== done $(date -Is) ===" | tee -a "$RUNS"
