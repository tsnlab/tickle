#!/usr/bin/env bash
# discovery_interval_netns.sh - two contexts with different discovery update intervals over a veth pair between two
# private network namespaces (2026-10-22, network-profiles item C: the summary carries its sender's interval and miss
# threshold, and a receiver judges that sender's silence by them).
#
# Side A runs update_interval 1 s (its own silence 3.5 s), side B 4 s (14 s); both default miss threshold, both for DUR
# seconds (default 30), started together. Two arms, one after the other, on the same namespaces:
#   change   this tree's core.
#   control  the same core with the receiver's use of the advertised value removed (check_node_silence() judges every
#            peer by its own silence again, as before the change) - built from a patched copy here, never in the tree.
# discovery_interval_netns.c reports each side's arrivals and deaths of its peer, and the peer's advertised silence.
#
# PRE-REGISTERED READING (implemented below, not only here):
#   VOID     a side without its RESULT line, or that never saw its peer (arrivals = 0), or polled less than DUR - 1 s;
#            or, in the change arm, A not told B's silence (peer_silence_ms != 14000) or B not told A's (!= 3500): the
#            treatment did not happen; or the control arm's A with no death of B: the harness cannot see a false death,
#            so a clean change arm would say nothing.
#   PASS     change arm: deaths = 0 on both sides and both peers alive at the end; and the control arm's A declared B
#            dead at least once.
#   FAIL     change arm: any death on either side (a false death - nothing is lost or killed here), or a peer not alive
#            at the end.
#
# Usage: [DUR=30] [OUT=prefix] discovery_interval_netns.sh     Needs `sudo -n ip`. OUT defaults to
#   ~/rig_results_safe/discovery_interval_netns_<stamp> ($OUT.txt, $OUT.d/). The summary always ends with
#   "=== discovery_interval_netns ended".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
DUR=${DUR:-30}
OUT=${OUT:-$HOME/rig_results_safe/discovery_interval_netns_$(date +%Y%m%d-%H%M%S)}
CC=${CC:-gcc}
mkdir -p "$OUT.d" || exit 1
SUM="$OUT.txt"
: > "$SUM"
say() { echo "$*" | tee -a "$SUM"; }

K=$(($$ % 250 + 2))
NET=10.127.$K
NS_A=dint-a-$$
NS_B=dint-b-$$
# shellcheck disable=SC2329 # invoked by the EXIT trap below
cleanup() { # the one EXIT trap
    sudo -n ip netns del "$NS_A" 2> /dev/null
    sudo -n ip netns del "$NS_B" 2> /dev/null
    say "=== discovery_interval_netns ended"
}
trap cleanup EXIT

say "discovery_interval_netns: $(date -Is) DUR=$DUR tree=$REPO head=$(git -C "$REPO" rev-parse --short HEAD)"
say "  dirty: $(git -C "$REPO" status --porcelain -- src include | wc -l) path(s) under src/ include/"

# build NAME TICKLE_C: the driver against this tree's headers, Linux HAL and the given core.
build() {
    "$CC" -O2 -Wall -Wextra -I"$REPO/include" -I"$REPO/src" -o "$OUT.d/$1" "$HERE/discovery_interval_netns.c" "$2" \
        "$REPO/src/hal_linux.c" "$REPO/src/encoding.c" "$REPO/src/log.c" -lm 2> "$OUT.d/build_$1.log" || {
        cat "$OUT.d/build_$1.log" >&2
        return 1
    }
}
cp "$REPO/src/tickle.c" "$OUT.d/control_tickle.c"
python3 - "$OUT.d/control_tickle.c" << 'EOF' || exit 1
import sys
p = sys.argv[1]
text = open(p).read()
old = "        uint64_t silence = node->peer_silence_ns[i] != 0 ? node->peer_silence_ns[i] : own_silence;"
if text.count(old) != 1:
    sys.exit(f"control anchor matched {text.count(old)} times, not 1")
open(p, "w").write(text.replace(old, "        uint64_t silence = own_silence;"))
EOF
# The control's tickle.c is a copy outside src/: its includes ("log.h", "consts.h" ...) resolve through -I src.
build change "$REPO/src/tickle.c" || exit 1
build control "$OUT.d/control_tickle.c" || exit 1
say "  built: change, control (judge line patched in a copy)"

sudo -n ip netns add "$NS_A" || exit 1
sudo -n ip netns add "$NS_B" || exit 1
sudo -n ip link add dint-a netns "$NS_A" type veth peer name dint-b netns "$NS_B" || exit 1
sudo -n ip -n "$NS_A" addr add "$NET.1/24" dev dint-a
sudo -n ip -n "$NS_B" addr add "$NET.2/24" dev dint-b
for ns in "$NS_A" "$NS_B"; do
    sudo -n ip -n "$ns" link set lo up
done
sudo -n ip -n "$NS_A" link set dint-a up
sudo -n ip -n "$NS_B" link set dint-b up
if ! sudo -n ip netns exec "$NS_A" ping -c 1 -W 1 "$NET.2" > /dev/null 2>&1; then
    say "VOID  setup: $NS_A cannot reach $NS_B"
    exit 1
fi
say "  namespaces $NS_A ($NET.1) / $NS_B ($NET.2)"

# arm NAME: both sides at once, DUR seconds; their outputs in $OUT.d/NAME_{a,b}.log.
arm() {
    local name=$1
    # shellcheck disable=SC2024 # the logs are this shell's
    sudo -n ip netns exec "$NS_A" "$OUT.d/$name" A "$NET.255" 1000 "$DUR" < /dev/null > "$OUT.d/${name}_a.log" 2>&1 &
    local pid_a=$!
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS_B" "$OUT.d/$name" B "$NET.255" 4000 "$DUR" < /dev/null > "$OUT.d/${name}_b.log" 2>&1 &
    local pid_b=$!
    wait "$pid_a" "$pid_b"
}

field() { # LOG KEY: KEY's value on the RESULT line, whole; empty when absent
    grep '^RESULT ' "$1" | tail -1 | grep -o " $2=[0-9-]*" | cut -d= -f2
}

arm change
arm control

verdict=PASS
void() {
    say "VOID  $*"
    verdict=VOID
}
for name in change control; do
    for side in a b; do
        log="$OUT.d/${name}_${side}.log"
        say "  $name $side: $(grep '^RESULT ' "$log" | tail -1)"
        grep -h '^DEATH ' "$log" | sed 's/^/      /' | tee -a "$SUM"
        if ! grep -q '^RESULT .*arrivals=' "$log"; then
            void "$name $side: no RESULT line"
            continue
        fi
        [ "$(field "$log" arrivals)" -ge 1 ] || void "$name $side: never saw its peer"
        [ "$(field "$log" polled_s)" -ge $((DUR - 1)) ] || void "$name $side: polled $(field "$log" polled_s) s of $DUR"
    done
done
[ "$(field "$OUT.d/change_a.log" peer_silence_ms)" = 14000 ] || void "change A: not told B's 14 s"
[ "$(field "$OUT.d/change_b.log" peer_silence_ms)" = 3500 ] || void "change B: not told A's 3.5 s"
control_deaths=$(field "$OUT.d/control_a.log" deaths)
[ "${control_deaths:-0}" -ge 1 ] || void "control A: no death of B - the harness cannot see a false death"
for side in a b; do
    log="$OUT.d/change_${side}.log"
    deaths=$(field "$log" deaths)
    alive=$(field "$log" alive_at_end)
    if [ "${deaths:-1}" != 0 ] || [ "${alive:-0}" != 1 ]; then
        say "FAIL  change $side: deaths=${deaths:-?} alive_at_end=${alive:-?}"
        [ "$verdict" = VOID ] || verdict=FAIL
    fi
done
say "VERDICT $verdict  change: A deaths=$(field "$OUT.d/change_a.log" deaths) B deaths=$(field "$OUT.d/change_b.log" deaths);" \
    "control: A deaths=${control_deaths:-?} B deaths=$(field "$OUT.d/control_b.log" deaths)"
[ "$verdict" = PASS ]
