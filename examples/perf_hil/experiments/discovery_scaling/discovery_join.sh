#!/usr/bin/env bash
# Discovery latency and convergence under loss, before and after the summary change (2026-09-26,
# rmw_tickle/DISCOVERY_PLAN.md M3 and M5). Same netns/bridge topology as discovery_scaling.sh, with every node
# given a discovery table (core and tool built with -Dtt_MAX_DISCOVERED_ENTITIES=512) and the number of alive
# entities it should end up holding, (N-1) x E.
#
#   join N E JOINS : N-1 nodes run in steady state; one more node is started JOINS times in turn, each for 8 s.
#                    Each start reports reached_ms: from its process start to holding every peer's endpoints.
#   loss N E REPS  : N nodes start together with LOSS% (default 5) netem loss on every interface, for 40 s.
#                    Each reports reached_ms, how often it later fell below the target (dips), when it last
#                    changed, and whether it was complete at exit.
#
# HOW TO READ IT (DISCOVERY_PLAN.md section 5, written before running):
#   M3 passes if the median reached_ms after the change is at most 5 ms above before.
#   M5 passes if every node reaches its target within 2 s and is complete at exit, as before the change.
#   CONTROL: in every mode each node's RESULT must show expected = (N-1) x E and a finite reached_ms before
#   anything else is read - a node that never reached it (-1) makes its row VOID, and is reported as such.
#
# Usage: SHA=<commit> discovery_join.sh join|loss <N> <E> <count>   Output: $OUT (default /tmp/discovery_join.txt)
set -u
MODE=${1:?usage: discovery_join.sh join|loss <N> <E> <count>}
N=${2:?}
E=${3:?}
COUNT=${4:?}
LOSS=${LOSS:-5}
OUT=${OUT:-/tmp/discovery_join.txt}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
BR=dscjbr0
DEFINE=-Dtt_MAX_DISCOVERED_ENTITIES=512

cleanup() {
    local i
    for i in $(seq 1 32); do sudo -n ip netns del "dscj-ns$i" 2>/dev/null; done
    sudo -n ip link del "$BR" 2>/dev/null
    return 0
}
trap cleanup EXIT
say() { echo "$*" | tee -a "$OUT"; }

SHA=${SHA:-$(git -C "$REPO" rev-parse origin/main)}
SHA=$(git -C "$REPO" rev-parse "$SHA") || exit 1
SRC=/tmp/discovery_src_$SHA
PREFIX=/tmp/discovery_core_${SHA}_d512
BIN=/tmp/discovery_join_node_${SHA:0:8}
if [ ! -f "$PREFIX/lib/libtickle.a" ]; then
    rm -rf "$SRC" && mkdir -p "$SRC" && git -C "$REPO" archive "$SHA" | tar -x -C "$SRC" || exit 1
    make -C "$SRC" clean > /dev/null 2>&1
    make -C "$SRC" install "PREFIX=$PREFIX" BUILD_TYPE=release "CPPFLAGS=$DEFINE" > /tmp/discovery_join_build.log 2>&1 \
        || { echo "core build failed, see /tmp/discovery_join_build.log"; exit 1; }
fi
SHAPE="$SRC/examples/perf_hil/tickle/common/p1"
# shellcheck disable=SC2046 # pkg-config output is a list of flags
gcc -O2 $DEFINE -o "$BIN" "$HERE/discovery_node.c" "$SHAPE/Bench.c" -I"$SHAPE" \
    $(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --cflags --libs tickle) -lm -lpthread || exit 1

topology() { # $1 node count
    local i
    cleanup
    sudo -n ip link add "$BR" type bridge && sudo -n ip link set "$BR" up || exit 1
    for i in $(seq 1 "$1"); do
        sudo -n ip netns add "dscj-ns$i"
        sudo -n ip link add "dscjv$i" type veth peer name "dscjb$i"
        sudo -n ip link set "dscjv$i" netns "dscj-ns$i"
        sudo -n ip link set "dscjb$i" master "$BR"
        sudo -n ip link set "dscjb$i" up
        sudo -n ip netns exec "dscj-ns$i" sysctl -qw net.ipv6.conf.all.disable_ipv6=1
        sudo -n ip -n "dscj-ns$i" addr add "192.168.21.$i/24" dev "dscjv$i"
        sudo -n ip -n "dscj-ns$i" link set lo up
        sudo -n ip -n "dscj-ns$i" link set "dscjv$i" up
        if [ "$MODE" = loss ]; then
            sudo -n ip netns exec "dscj-ns$i" tc qdisc add dev "dscjv$i" root netem loss "${LOSS}%" || exit 1
        fi
    done
}
node() { # $1 index, $2 seconds, $3 log
    # shellcheck disable=SC2024 # the log is meant to be written as this user, not root
    sudo -n ip netns exec "dscj-ns$1" "$BIN" "$1" "$E" "$2" 192.168.21.255 $(((N - 1) * E)) > "$3" 2>&1
}
row() { # $1 label, $2 log
    local r
    r=$(grep '^RESULT:' "$2")
    case "$r" in
    *"expected=$(((N - 1) * E)) reached_ms=-1"*) say "$1 | VOID(never reached) | ${r#RESULT: }" ;;
    *"expected=$(((N - 1) * E)) "*) say "$1 | ok | ${r#RESULT: }" ;;
    *) say "$1 | VOID(no RESULT) | $(tail -n 1 "$2")" ;;
    esac
}

: > "$OUT"
LOSSTAG=""; [ "$MODE" = loss ] && LOSSTAG=" loss=$LOSS%"
say "=== discovery $MODE, $(date -Is), core ${SHA:0:8}, N=$N E=$E count=$COUNT$LOSSTAG ==="
topology "$N"
if [ "$MODE" = join ]; then
    steady=$((5 + COUNT * 9 + 5))
    for i in $(seq 1 $((N - 1))); do node "$i" "$steady" "/tmp/dscj_$i.log" & done
    sleep 5
    for j in $(seq 1 "$COUNT"); do
        node "$N" 8 "/tmp/dscj_join.log"
        row "join $j" /tmp/dscj_join.log
        sleep 1
    done
    wait
else
    for rep in $(seq 1 "$COUNT"); do
        for i in $(seq 1 "$N"); do node "$i" 40 "/tmp/dscj_$i.log" & done
        wait
        for i in $(seq 1 "$N"); do row "rep$rep node$i" "/tmp/dscj_$i.log"; done
        sleep 2
    done
fi
rm -f /tmp/dscj_*.log
say "=== done ==="
