#!/usr/bin/env bash
# segment_geometry_mismatch.sh - two contexts built with different segment geometry, talking to each other.
#
# WHAT THIS IS FOR. On 2026-10-02 Dev measured what a mismatched pair did when the attacher mapped ITS OWN
# length and then indexed with the OWNER's numbers: with a larger owner the writer filled the slots its short
# mapping could address, read a slot header it could not see, got a sequence that was not the index it claimed,
# and reported the ring full - permanently. 259 datagrams over shared memory, then shm_full_dropped=1535 of 2000
# messages discarded rather than rerouted, the subscriber receiving 536, and the publisher printing "sent 2,000
# message(s)" and exiting 0. c97fac8b refused any mismatch outright as the narrow fix available at the time.
#
# SHM_PLAN 6e replaced that: peer_segment() maps the header first, reads the owner's slots and slot_bytes, and
# maps the region at the owner's size. A different geometry is now something to handle rather than refuse. This
# is the measurement that says whether that is true, because the unit test exercises segment_header_check() and
# not two real processes with two real mappings.
#
# HOW TO READ IT, written before the run:
#   PASS - the mismatched pair's tx_shm is of the same order as the MATCHED control's, the reader receives every
#          sample, and shm_full_dropped is no worse than the control's. A different geometry costs nothing.
#   FAIL - tx_shm collapses to the order of the wedge (hundreds, against the control's millions), or the reader
#          receives fewer than were sent, or shm_full_dropped is far above the control's.
#
#   The control is not decoration and the first version of this script had none. It read "shm_full_dropped rises
#   at all -> FAIL" and duly failed a run that had carried 9.5 million datagrams through the segment and
#   delivered every one of 734,047 samples, because a 512-slot ring in front of a writer that fast fills
#   sometimes whatever its geometry. That criterion could not tell a ring that wedges for ever from one that is
#   briefly full, which is the whole question - so what normal looks like has to be measured in the same run
#   rather than assumed to be zero.
#   FAIL - tx_shm is 0 with the segment present: the pair fell back to UDP, which is c97fac8b's behaviour and
#          not this change's.
#   VOID - the two builds have the same geometry. Checked by a probe that prints the compiled-in numbers rather
#          than by trusting that -D reached the compiler, because a -D that is silently ignored makes both arms
#          the control and the run says "no difference" for the wrong reason (config.h's own #ifndef guards
#          existed before anything checked they were reachable).
#   VOID - either process did not start, or the reader received nothing at all.
#
# Two core processes in a private netns - never the default one, where a TickLE node reaches the rig over the
# 10.1.1.x management LAN.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
NS=tt-geomns-$$
OUT=${OUT:-$HOME/rig_results_safe/segment_geometry_mismatch}
BIG=${BIG:-1048576}   # the owner's tt_SEGMENT_BYTES; the default is 512 KiB, so this is twice the slots
rm -rf "$OUT"; mkdir -p "$OUT"
say() { echo "$*" | tee -a "$OUT/log.txt"; }
say "=== segment geometry mismatch $(date -Is) big=$BIG ==="

# A probe that prints what was actually compiled in, so "the -D took" is measured and not assumed.
cat > "$OUT/geom.c" <<'EOF'
#include <stdio.h>
#include <tickle/config.h>
int main(void) {
    printf("slots=%u slot_bytes=%u bytes=%u\n", (unsigned)tt_SEGMENT_SLOTS, (unsigned)tt_SEGMENT_SLOT_BYTES,
           (unsigned)tt_SEGMENT_BYTES);
    return 0;
}
EOF

build_arm() { # build_arm <name> [extra-cflags...]
    local name=$1
    shift
    # The extra flags come through as arguments rather than one string, so an arm with none passes none - a
    # quoted empty "$extra" would hand gcc an empty argument and an unquoted one is a splitting hazard.
    gcc -I"$REPO/include" "$@" -o "$OUT/geom_$name" "$OUT/geom.c" 2>>"$OUT/build_$name.log" || return 1
    make -C "$REPO/platform/linux" clean >/dev/null 2>&1
    make -C "$REPO/platform/linux" perf CFLAGS="-O2 -DNDEBUG -Wall -Wextra $*" \
        >>"$OUT/build_$name.log" 2>&1 || return 1
    cp "$REPO/platform/linux/perf_client" "$OUT/client_$name"
    cp "$REPO/platform/linux/perf_server" "$OUT/server_$name"
    say "  arm $name: $("$OUT/geom_$name")"
}

build_arm small || { say "VOID: the default build failed"; exit 1; }
build_arm big "-Dtt_SEGMENT_BYTES=$BIG" || { say "VOID: the $BIG build failed"; exit 1; }
if [ "$("$OUT/geom_small")" = "$("$OUT/geom_big")" ]; then
    say "VOID: both arms compiled to the same geometry - the -D did not reach the compiler, so there is no"
    say "      mismatch to test and 'no difference' would be the control agreeing with itself."
    exit 1
fi

sudo -n ip netns del "$NS" 2>/dev/null
sudo -n ip netns add "$NS" || { say "VOID: netns add failed"; exit 1; }
cleanup() {
    sudo -n ip netns del "$NS" 2>/dev/null
    rm -f /dev/shm/tickle-seg-192.168.10.* 2>/dev/null
    make -C "$REPO/platform/linux" clean >/dev/null 2>&1
}
trap cleanup EXIT
sudo -n ip -n "$NS" link set lo up
sudo -n ip -n "$NS" link add dummy0 type dummy
sudo -n ip -n "$NS" addr add 192.168.10.1/24 dev dummy0
sudo -n ip -n "$NS" link set dummy0 up
sudo -n ip -n "$NS" route add default dev dummy0
in_ns() { sudo -n ip netns exec "$NS" setpriv --reuid="$(id -u)" --regid="$(id -g)" --init-groups env HOME="$HOME" "$@"; }

# The OWNER is the reader and is the big one: it creates the segment the writer attaches to, so the owner's
# geometry is the larger of the two. That is the direction that wedged - a smaller owner was already refused
# cleanly by tt_segment_attach()'s own fstat, measured the same day.
run_pair() { # run_pair <tag> <server-arm> <client-arm>
    rm -f /dev/shm/tickle-seg-192.168.10.* 2>/dev/null
    in_ns stdbuf -oL "$OUT/server_$2" -d 25 -I 20 >"$OUT/reader_$1.log" 2>&1 &
    sleep 1
    in_ns stdbuf -oL "$OUT/client_$3" -d 15 -I 21 >"$OUT/writer_$1.log" 2>&1 &
    wait
}
# The control first: both sides the SAME geometry, which is what every deployment has today, so its
# shm_full_dropped is what a full ring costs at this rate with nothing mismatched.
say "--- control: matched geometry (big owner, big writer) ---"
run_pair control big big
# Then the arm. The OWNER is the reader and is the big one: it creates the segment the writer attaches to, so
# the owner's geometry is the larger of the two. That is the direction that wedged - a smaller owner was already
# refused cleanly by tt_segment_attach()'s own fstat, measured the same day.
say "--- arm: mismatched (big owner, small writer) ---"
run_pair arm big small
say "=== done $(date -Is) ==="

field() { grep -oE "$2=[0-9]+" "$1" 2>/dev/null | tail -1 | cut -d= -f2; }
read_tag() { # read_tag <tag> -> "shm udp full sent recv"
    echo "$(field "$OUT/writer_$1.log" tx_shm) $(field "$OUT/writer_$1.log" tx_udp)" \
         "$(field "$OUT/writer_$1.log" shm_full_dropped) $(field "$OUT/writer_$1.log" sent)" \
         "$(field "$OUT/reader_$1.log" received)"
}
read -r c_shm c_udp c_full c_sent c_recv <<<"$(read_tag control)"
read -r a_shm a_udp a_full a_sent a_recv <<<"$(read_tag arm)"
say ""
say "  control (matched)    tx_shm=${c_shm:-?} tx_udp=${c_udp:-?} shm_full_dropped=${c_full:-?} sent=${c_sent:-?} received=${c_recv:-?}"
say "  arm     (mismatched) tx_shm=${a_shm:-?} tx_udp=${a_udp:-?} shm_full_dropped=${a_full:-?} sent=${a_sent:-?} received=${a_recv:-?}"
say ""
if [ -z "${a_shm:-}" ] || [ -z "${a_recv:-}" ] || [ -z "${c_shm:-}" ]; then
    say "VOID: a process produced no RESULT line - nothing was measured."
elif [ "${c_recv:-0}" -eq 0 ] || [ "${a_recv:-0}" -eq 0 ]; then
    say "VOID: a reader received nothing, so that pair never talked and the geometry was not exercised."
elif [ "${a_recv:-0}" -ne "${a_sent:-1}" ]; then
    say "FAIL: the mismatched reader received $a_recv of $a_sent. Samples were lost, which is the wedge's"
    say "      signature - it discarded 1,535 of 2,000 before the two-step attach."
elif [ "$((a_shm))" -lt "$((c_shm / 100))" ]; then
    say "FAIL: the mismatched arm carried $a_shm datagrams against the control's $c_shm - two orders down, which"
    say "      is the ring ceasing to accept rather than being briefly full."
elif [ "$((a_full))" -gt "$((c_full * 4 + 1000))" ]; then
    say "FAIL: shm_full_dropped $a_full against the control's $c_full. Far beyond what a full ring costs when"
    say "      nothing is mismatched, so the geometry is costing something this change was meant to remove."
else
    say "PASS: $a_shm datagrams through a segment whose geometry is not ours against the control's $c_shm, every"
    say "      one of $a_sent samples delivered, and shm_full_dropped $a_full against the control's $c_full - a"
    say "      full ring at this rate, not a wedged one. It stopped at 259 and lost 1,535 before this."
fi
