#!/usr/bin/env bash
# shm_netns_collision.sh - two network namespaces on one host, each with a publisher and a subscriber of one topic, whose
# subscribers name the SAME shared-memory segment (2026-10-29). Both namespaces are lo-only, so both are 127.0.0.1; both
# subscribers take context id 20 and, through an LD_PRELOAD shim (shm_netns_collision_port.c), the same data port. The
# segment is named from exactly those three (segment_name(), src/tickle.c), and /dev/shm is shared, so a publisher in
# one namespace can attach to the other namespace's subscriber's ring and its samples cross over. Each publisher tags
# its values with its namespace; a subscriber counts samples with the other tag as foreign.
#
# Arms, each ROUNDS times, each round on fresh namespaces (B started 1 s after A, so B's subscriber builds its segment
# last and holds the name):
#   base-collide   BASE's core (default origin/main), colliding as above - expected to cross-deliver.
#   fix-collide    this tree's core, colliding.
#   fix-control    this tree's core, B at 127.0.0.2 (lo carries all of 127/8): the names differ, nothing collides, and
#                  each namespace's pair must still use shared memory.
#
# PRE-REGISTERED READING (implemented below, not only here):
#   VOID  a round without all four RESULT lines, or a colliding round whose two subscribers do not report the same port
#         (the shim did not apply: the treatment did not happen);
#         fix-control with a publisher at tx_shm=0 in any round (shm was not used where nothing collides, so a 0-foreign
#         fix-collide says nothing about the fix rather than about a dead transport);
#         base-collide with no cross-delivery in any round (the harness cannot produce the collision, so 0 in fix-collide
#         would prove nothing).
#   FAIL  any fix-collide or fix-control round in which a subscriber counted a foreign sample.
#   PASS  otherwise.
# The counts per arm (rounds with foreign > 0) are printed for the record whatever the verdict.
#
# Usage: [ROUNDS=20] [DUR_MS=5000] [BASE=origin/main] [OUT=prefix] shm_netns_collision.sh
#   Needs `sudo -n ip`. OUT defaults to ~/rig_results_safe/shm_netns_collision_<stamp> ($OUT.txt, $OUT.d/).
#   The summary always ends with "=== shm_netns_collision ended".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
ROUNDS=${ROUNDS:-20}
DUR_MS=${DUR_MS:-5000}
BASE=${BASE:-origin/main}
OUT=${OUT:-$HOME/rig_results_safe/shm_netns_collision_$(date +%Y%m%d-%H%M%S)}
CC=${CC:-gcc}
PORT=$((40000 + $$ % 20000))
SUB_ID=20
PUB_ID=21
mkdir -p "$OUT.d" || exit 1
SUM="$OUT.txt"
: > "$SUM"
say() { echo "$*" | tee -a "$SUM"; }

NS_A=shmcol-a-$$
NS_B=shmcol-b-$$
# shellcheck disable=SC2329 # invoked by the EXIT trap below
cleanup() { # the one EXIT trap
    sudo -n ip netns del "$NS_A" 2> /dev/null
    sudo -n ip netns del "$NS_B" 2> /dev/null
    # Only the files this run's subscribers and publishers can have made: by exact name, never by glob.
    for ip in 127.0.0.1 127.0.0.2; do
        rm -f "/dev/shm/tickle-seg-$ip-$PORT-$SUB_ID" "/dev/shm/tickle-seg-$ip-$PORT-$SUB_ID.bell"
    done
    say "=== shm_netns_collision ended"
}
trap cleanup EXIT

say "shm_netns_collision: $(date -Is) ROUNDS=$ROUNDS DUR_MS=$DUR_MS port=$PORT tree=$REPO" \
    "head=$(git -C "$REPO" rev-parse --short HEAD) base=$BASE($(git -C "$REPO" rev-parse --short "$BASE"))"
say "  dirty: $(git -C "$REPO" status --porcelain -- src include | wc -l) path(s) under src/ include/"

# build NAME TREE: the driver against TREE's headers and core, with this tree's UInt64 type.
build() {
    "$CC" -O2 -Wall -Wextra -I"$2/include" -I"$2/src" -I"$REPO/examples/uint64" -o "$OUT.d/$1" \
        "$HERE/shm_netns_collision.c" "$REPO/examples/uint64/UInt64.c" "$2/src/tickle.c" "$2/src/hal_linux.c" \
        "$2/src/encoding.c" "$2/src/log.c" -lm -lpthread 2> "$OUT.d/build_$1.log" || {
        cat "$OUT.d/build_$1.log" >&2
        return 1
    }
}
mkdir -p "$OUT.d/base_tree"
git -C "$REPO" archive "$BASE" src include | tar -x -C "$OUT.d/base_tree" || exit 1
build base "$OUT.d/base_tree" || exit 1
build fix "$REPO" || exit 1
"$CC" -shared -fPIC -O2 -o "$OUT.d/port_shim.so" "$HERE/shm_netns_collision_port.c" -ldl || exit 1
say "  built: base ($BASE), fix (this tree), port shim"

# netns_up: both namespaces fresh, lo-only, lo up with a default route (a lo-only namespace has none otherwise).
netns_up() {
    sudo -n ip netns del "$NS_A" 2> /dev/null
    sudo -n ip netns del "$NS_B" 2> /dev/null
    for ns in "$NS_A" "$NS_B"; do
        sudo -n ip netns add "$ns" || return 1
        sudo -n ip -n "$ns" link set lo up || return 1
        sudo -n ip -n "$ns" route add default dev lo || return 1
    done
}

# run_ns NS LABEL BIN ADDR TAG: the namespace's subscriber (forced port) and publisher, as this user.
run_ns() {
    local ns=$1 label=$2 bin=$3 addr=$4 tag=$5
    # shellcheck disable=SC2024 # the logs are this shell's
    sudo -n ip netns exec "$ns" setpriv --reuid="$(id -u)" --regid="$(id -g)" --init-groups \
        env HOME="$HOME" LD_PRELOAD="$OUT.d/port_shim.so" SHM_COLLIDE_PORT="$PORT" \
        "$OUT.d/$bin" "${label}_sub" sub "$addr" 127.255.255.255 "$SUB_ID" "$tag" $((DUR_MS + 1500)) \
        < /dev/null > "$ROUND_DIR/${label}_sub.log" 2>&1 &
    sleep 0.2
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$ns" setpriv --reuid="$(id -u)" --regid="$(id -g)" --init-groups env HOME="$HOME" \
        "$OUT.d/$bin" "${label}_pub" pub "$addr" 127.255.255.255 "$PUB_ID" "$tag" "$DUR_MS" \
        < /dev/null > "$ROUND_DIR/${label}_pub.log" 2>&1 &
}

field() { # LOG KEY: KEY's value on the RESULT line, whole; empty when absent
    grep '^RESULT ' "$1" | tail -1 | grep -o " $2=[0-9,]*" | cut -d= -f2
}

declare -A crossed void_rounds shm_unused
# round ARM BIN B_ADDR N
round() {
    local arm=$1 bin=$2 b_addr=$3 n=$4
    ROUND_DIR="$OUT.d/$arm/$n"
    mkdir -p "$ROUND_DIR"
    netns_up || {
        say "VOID  $arm round $n: namespace setup failed"
        void_rounds[$arm]=$((${void_rounds[$arm]:-0} + 1))
        return
    }
    run_ns "$NS_A" A "$bin" 127.0.0.1 1
    sleep 1
    run_ns "$NS_B" B "$bin" "$b_addr" 2
    wait
    local logs=("$ROUND_DIR/A_sub.log" "$ROUND_DIR/A_pub.log" "$ROUND_DIR/B_sub.log" "$ROUND_DIR/B_pub.log")
    local log missing=0
    for log in "${logs[@]}"; do
        grep -q '^RESULT .* attach=' "$log" || missing=1
    done
    local fa fb pa pb txa txb
    fa=$(field "$ROUND_DIR/A_sub.log" foreign)
    fb=$(field "$ROUND_DIR/B_sub.log" foreign)
    pa=$(field "$ROUND_DIR/A_sub.log" port)
    pb=$(field "$ROUND_DIR/B_sub.log" port)
    txa=$(field "$ROUND_DIR/A_pub.log" tx_shm)
    txb=$(field "$ROUND_DIR/B_pub.log" tx_shm)
    local line
    line="$arm round $n: A sub own=$(field "$ROUND_DIR/A_sub.log" own) foreign=${fa:-?} port=${pa:-?}"
    line+=" | B sub own=$(field "$ROUND_DIR/B_sub.log" own) foreign=${fb:-?} port=${pb:-?}"
    line+=" | A pub tx_shm=${txa:-?} attach=$(field "$ROUND_DIR/A_pub.log" attach)"
    line+=" | B pub tx_shm=${txb:-?} attach=$(field "$ROUND_DIR/B_pub.log" attach)"
    say "  $line"
    if [ "$missing" = 1 ]; then
        say "VOID  $arm round $n: a RESULT line is missing"
        void_rounds[$arm]=$((${void_rounds[$arm]:-0} + 1))
        return
    fi
    if [ "$arm" != fix-control ] && { [ "$pa" != "$PORT" ] || [ "$pb" != "$PORT" ]; }; then
        say "VOID  $arm round $n: the subscribers are not both at port $PORT ($pa, $pb) - no collision was forced"
        void_rounds[$arm]=$((${void_rounds[$arm]:-0} + 1))
        return
    fi
    if [ "$fa" -gt 0 ] || [ "$fb" -gt 0 ]; then
        crossed[$arm]=$((${crossed[$arm]:-0} + 1))
    fi
    if [ "$txa" -eq 0 ] || [ "$txb" -eq 0 ]; then
        shm_unused[$arm]=$((${shm_unused[$arm]:-0} + 1))
    fi
}

for n in $(seq 1 "$ROUNDS"); do
    round base-collide base 127.0.0.1 "$n"
    round fix-collide fix 127.0.0.1 "$n"
    round fix-control fix 127.0.0.2 "$n"
done

verdict=PASS
void() {
    say "VOID  $*"
    [ "$verdict" = FAIL ] || verdict=VOID
}
for arm in base-collide fix-collide fix-control; do
    say "ARM $arm: rounds=$ROUNDS cross-delivered=${crossed[$arm]:-0} void=${void_rounds[$arm]:-0}" \
        "rounds_with_a_publisher_at_tx_shm_0=${shm_unused[$arm]:-0}"
    [ "${void_rounds[$arm]:-0}" = 0 ] || void "$arm: ${void_rounds[$arm]} void round(s)"
done
[ "${shm_unused[fix-control]:-0}" = 0 ] || void "fix-control: shared memory unused in ${shm_unused[fix-control]} round(s)"
[ "${crossed[base-collide]:-0}" -gt 0 ] || void "base-collide: no cross-delivery in any round - no collision produced"
for arm in fix-collide fix-control; do
    if [ "${crossed[$arm]:-0}" -gt 0 ]; then
        say "FAIL  $arm: ${crossed[$arm]} round(s) cross-delivered"
        verdict=FAIL
    fi
done
say "VERDICT $verdict  base-collide ${crossed[base-collide]:-0}/$ROUNDS cross-delivered;" \
    "fix-collide ${crossed[fix-collide]:-0}/$ROUNDS; fix-control ${crossed[fix-control]:-0}/$ROUNDS" \
    "(shm unused in ${shm_unused[fix-control]:-0})"
[ "$verdict" = PASS ]
