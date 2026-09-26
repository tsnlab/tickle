#!/usr/bin/env bash
# What discovery costs on the wire as nodes and endpoints grow (2026-09-26, rmw_tickle/DISCOVERY_PLAN.md
# M1 before the change, M2 after it). N idle nodes of E publishers each (discovery_node.c), each in its own
# netns on one bridge. After a warm-up, every node's interface byte and packet counters are read over a
# steady-state window. Nothing but discovery (and one-time ARP) crosses the bridge, so the rate is
# discovery's cost. A byte count, not a timing, so the PC is the right place for it.
#
# HOW TO READ IT (DISCOVERY_PLAN.md section 1, written before running): today every node broadcasts its
# whole list every second, so bytes received per node should grow as (N - 1) x E. If it does not, the
# plan's motivation is weaker than stated and goes back to the user.
#
# Usage: [SHA=<commit>] discovery_scaling.sh "<N values>" "<E values>" [reps]   Output: $OUT (default /tmp/discovery_scaling.txt)
set -u
NS=${1:?usage: discovery_scaling.sh \"<N values>\" \"<E values>\" [reps]}
ES=${2:?usage: discovery_scaling.sh \"<N values>\" \"<E values>\" [reps]}
REPS=${3:-2}
OUT=${OUT:-/tmp/discovery_scaling.txt}
WARM_S=5
WINDOW_S=30
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
BIN=/tmp/discovery_node
BR=dscbr0

cleanup() {
    local i
    for i in $(seq 1 32); do sudo -n ip netns del "dsc-ns$i" 2>/dev/null; done
    sudo -n ip link del "$BR" 2>/dev/null
    return 0
}
trap cleanup EXIT
say() { echo "$*" | tee -a "$OUT"; }

# The core is built from a pinned commit (SHA, default origin/main), unpacked from git into /tmp, never from the
# working tree: Plan and Dev share one checkout, and M1 must measure today's code even while the change is being
# written in it. Release build, as the harness uses. discovery_node.c is compiled against that install.
SHA=${SHA:-$(git -C "$REPO" rev-parse origin/main)}
SHA=$(git -C "$REPO" rev-parse "$SHA") || exit 1
SRC=/tmp/discovery_src_$SHA
PREFIX=/tmp/discovery_core_$SHA
if [ ! -f "$PREFIX/lib/libtickle.a" ]; then
    rm -rf "$SRC" && mkdir -p "$SRC" && git -C "$REPO" archive "$SHA" | tar -x -C "$SRC" || exit 1
    make -C "$SRC" install "PREFIX=$PREFIX" BUILD_TYPE=release > /tmp/discovery_build.log 2>&1 \
        || { echo "core build failed, see /tmp/discovery_build.log"; exit 1; }
fi
SHAPE="$SRC/examples/perf_hil/tickle/common/p1"
# shellcheck disable=SC2046 # pkg-config output is a list of flags
gcc -O2 -o "$BIN" "$HERE/discovery_node.c" "$SHAPE/Bench.c" -I"$SHAPE" \
    $(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --cflags --libs tickle) -lm -lpthread || exit 1

: > "$OUT"
say "=== discovery scaling, $(date -Is), core ${SHA:0:8}, warm-up ${WARM_S}s, window ${WINDOW_S}s ==="

counters() { # $1 ns index -> "rx_bytes rx_packets tx_bytes tx_packets"
    sudo -n ip netns exec "dsc-ns$1" sh -c "cd /sys/class/net/dscv$1/statistics && echo \$(cat rx_bytes rx_packets tx_bytes tx_packets)"
}

for rep in $(seq 1 "$REPS"); do
  for n in $NS; do
    for e in $ES; do
        cleanup
        sudo -n ip link add "$BR" type bridge && sudo -n ip link set "$BR" up || exit 1
        for i in $(seq 1 "$n"); do
            sudo -n ip netns add "dsc-ns$i"
            sudo -n ip link add "dscv$i" type veth peer name "dscb$i"
            sudo -n ip link set "dscv$i" netns "dsc-ns$i"
            sudo -n ip link set "dscb$i" master "$BR"
            sudo -n ip link set "dscb$i" up
            sudo -n ip netns exec "dsc-ns$i" sysctl -qw net.ipv6.conf.all.disable_ipv6=1
            sudo -n ip -n "dsc-ns$i" addr add "192.168.20.$i/24" dev "dscv$i"
            sudo -n ip -n "dsc-ns$i" link set lo up
            sudo -n ip -n "dsc-ns$i" link set "dscv$i" up
        done
        for i in $(seq 1 "$n"); do
            # shellcheck disable=SC2024 # the log is meant to be written as this user, not root
            sudo -n ip netns exec "dsc-ns$i" "$BIN" "$i" "$e" $((WARM_S + WINDOW_S + 3)) 192.168.20.255 \
                > "/tmp/discovery_node_$i.log" 2>&1 &
        done
        sleep "$WARM_S"
        declare -A before
        for i in $(seq 1 "$n"); do before[$i]=$(counters "$i"); done
        sleep "$WINDOW_S"
        rxb=0; rxp=0; txb=0; txp=0
        for i in $(seq 1 "$n"); do
            read -r b0 p0 t0 q0 <<< "${before[$i]}"
            read -r b1 p1 t1 q1 <<< "$(counters "$i")"
            rxb=$((rxb + b1 - b0)); rxp=$((rxp + p1 - p0)); txb=$((txb + t1 - t0)); txp=$((txp + q1 - q0))
        done
        unset before
        wait
        ok=$(grep -l "^RESULT:" /tmp/discovery_node_*.log 2>/dev/null | wc -l)
        verdict=ok; [ "$ok" -ge "$n" ] || verdict="VOID(only $ok of $n nodes finished)"
        say "N=$n E=$e rep$rep | $verdict | rx_bytes_per_node_s=$((rxb / n / WINDOW_S)) rx_pkts_per_node_s=$((rxp / n / WINDOW_S)) tx_bytes_per_node_s=$((txb / n / WINDOW_S)) tx_pkts_per_node_s=$((txp / n / WINDOW_S))"
        rm -f /tmp/discovery_node_*.log
    done
  done
done
say "=== done ==="
