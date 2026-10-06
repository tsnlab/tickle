#!/usr/bin/env bash
# SHM_PLAN 6a item 11: the reader is killed mid-run, the writer keeps making progress, and the
# segment is reclaimed afterwards. A kill test, because MODULE_PLAN.md criterion 4 is not satisfiable
# by reading the code - and because until 2026-09-29 the writer's dead-reader rule counted its own
# refusals, which measures the writer and not the reader, so a live-but-slow peer was abandoned
# constantly and a genuinely dead one was indistinguishable from it. The rule is now time since we
# last managed to place anything in the ring, and this is what checks it against a real corpse.
#
# Two core processes in a private netns - never the default one, where a TickLE node reaches the rig
# over the 10.1.1.x management LAN.
#
# Pre-registered reading, written before the run:
#   VOID  - the reader never used the segment (rx_shm 0 before the kill), so the kill tested nothing.
#           Reported as VOID and not as a pass; this is the arm that stops the whole script from
#           being a check that cannot fail.
#   FAIL  - the writer stops making progress after the kill (no further "sent" lines), or dies.
#   FAIL  - the writer never gives the dead peer up: shm_gave_up stays at zero.
#   FAIL  - a fresh reader at the same address and id cannot take the segment over.
#   PASS  - progress continues, the writer falls back to UDP for that peer, and the successor reader
#           receives again.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
BIN="$REPO/platform/linux"
NS=tt-killns-$$
OUT=${OUT:-/tmp/segment_reader_kill}
rm -rf "$OUT"; mkdir -p "$OUT"

[ -x "$BIN/perf_client" ] && [ -x "$BIN/perf_server" ] || {
    echo "VOID: build first - make -C platform/linux perf"
    exit 1
}

sudo -n ip netns del "$NS" 2>/dev/null
sudo -n ip netns add "$NS" || { echo "VOID: netns add failed"; exit 1; }
cleanup() {
    sudo -n ip netns del "$NS" 2>/dev/null
    rm -f /dev/shm/tickle-seg-192.168.10.* 2>/dev/null
}
trap cleanup EXIT
sudo -n ip -n "$NS" link set lo up
sudo -n ip -n "$NS" link add dummy0 type dummy
sudo -n ip -n "$NS" addr add 192.168.10.1/24 dev dummy0
sudo -n ip -n "$NS" link set dummy0 up
sudo -n ip -n "$NS" route add default dev dummy0

in_ns() { sudo -n ip netns exec "$NS" setpriv --reuid="$(id -u)" --regid="$(id -g)" --init-groups env HOME="$HOME" "$@"; }

# The reader first, then the writer, then the kill eight seconds in - long enough for the segment to
# be attached and carrying traffic, short enough to leave twenty seconds of writer afterwards.
in_ns stdbuf -oL "$BIN/perf_server" -d 40 -I 20 >"$OUT/reader.log" 2>&1 &
reader_wrapper=$!
sleep 1
# stdbuf, because progress is the thing being measured: stdout to a file is block-buffered, so the
# writer's periodic lines appear in bursts long after they were produced and counting them mid-run
# reads zero on both sides of the kill. The first version of this script did exactly that.
in_ns stdbuf -oL "$BIN/perf_client" -d 30 -I 21 >"$OUT/writer.log" 2>&1 &
writer_wrapper=$!
sleep 8

# The reader's own pid inside the namespace, taken from what it reports rather than by pattern: a
# pgrep for a string this script itself contains matches the checking command as readily as the
# checked one.
reader_pid=$(in_ns pgrep -x perf_server | head -1)
if [ -z "$reader_pid" ]; then echo "VOID: the reader is not running - nothing to kill"; exit 1; fi

# `grep -c` prints 0 and EXITS 1 when nothing matches, so `|| echo 0` appended a second 0 and every
# comparison below received "0\n0" and errored - and the script then reported PASS anyway. A
# comparison that errors must never be read as a pass, which is why the verdict below starts at VOID.
reader_progress_at_kill=$(grep -c 'recv ' "$OUT/reader.log" 2>/dev/null; true)
writer_lines_at_kill=$(grep -c 'sent ' "$OUT/writer.log" 2>/dev/null; true)
# A glob rather than `ls | grep`: the names are ours and contain an address, so a glob is both
# correct and the thing shellcheck asks for.
printf '%s\n' /dev/shm/tickle-seg-* > "$OUT/segments_before.txt" 2>/dev/null || true

# Plain kill, not sudo: the reader runs as this user, `ip netns exec` does not create a PID
# namespace, and the sudoers rule here covers `ip netns` and nothing else - so `sudo -n kill` failed
# for want of a password, the failure went into 2>/dev/null, and the first three runs of this script
# reported on a reader that was never killed and ran its full forty seconds. An action that silently
# does not happen reads exactly like one that did.
#
# So the kill is checked against /proc, and the identity of the target is checked before it: a PID
# taken from pgrep is a PID until something confirms what it is.
if [ ! -r "/proc/$reader_pid/exe" ] || [ "$(readlink -f "/proc/$reader_pid/exe")" != "$(readlink -f "$BIN/perf_server")" ]; then
    echo "VOID: pid $reader_pid is not $BIN/perf_server - refusing to kill it"
    exit 1
fi
kill -9 "$reader_pid"
sleep 1
if [ -e "/proc/$reader_pid" ]; then
    echo "VOID: pid $reader_pid is still there after kill -9 - the reader was not killed"
    exit 1
fi
echo "killed reader pid $reader_pid at $(date -Is), /proc entry gone"
sleep 12

writer_lines_after=$(grep -c 'sent ' "$OUT/writer.log" 2>/dev/null; true)
writer_alive=0
in_ns pgrep -x perf_client >/dev/null 2>&1 && writer_alive=1

# A successor at the same address and id: it must be able to take the name over, which is what
# "reclaimed" means. tt_segment_create() unlinks before it creates, so the corpse's file is replaced.
in_ns "$BIN/perf_server" -d 8 -I 20 >"$OUT/reader2.log" 2>&1 || true

wait "$writer_wrapper" 2>/dev/null || true
kill "$reader_wrapper" 2>/dev/null || true

traffic=$(grep -o 'traffic: .*' "$OUT/writer.log" | tail -1)
# NOT the reader's rx_shm: a process killed with -9 runs no teardown and prints no traffic line, so
# that field is absent by construction here and asking for it would fail every run for the wrong
# reason. The writer's tx_shm and the reader's own progress lines carry the same fact.
# NOT tx_udp_unattached: it rises for every ordinary reason a peer has no segment - a name that
# cannot be formed, a negative cache still in date, a revalidation in flight - so a non-zero value
# says nothing about whether this writer judged a reader dead. Checked rather than assumed: a build
# with the dead-reader threshold raised to an hour, which can never abandon anyone, produced
# tx_udp_unattached=196,962 against the real build's 203,318. The assertion could not fail.
#
# shm_gave_up rises only in the one branch that abandons a peer, which is what this test is about.
gave_up=$(echo "$traffic" | grep -o 'shm_gave_up=[0-9]*' | cut -d= -f2)
tx_shm=$(echo "$traffic" | grep -o 'tx_shm=[0-9]*' | cut -d= -f2)
recv2=$(grep -oE 'RESULT: recv=[0-9,]+' "$OUT/reader2.log" | tail -1 | tr -d ',' | cut -d= -f2)

echo "--- writer traffic: ${traffic:-none}"
echo "--- reader progress lines before the kill: ${reader_progress_at_kill:-none}"
echo "--- writer 'sent' lines: $writer_lines_at_kill at the kill, $writer_lines_after after, alive=$writer_alive"
echo "--- successor reader recv: ${recv2:-none}"

# Starts at VOID and is only lowered to PASS once every question has been answered with a number.
# The first version started at PASS and fell through to it whenever a comparison errored, which is
# how it reported PASS on a run whose progress counters were both the string "0\n0".
verdict=VOID
is_num() { case "${1:-}" in '' | *[!0-9]*) return 1 ;; *) return 0 ;; esac; }
if ! is_num "${tx_shm:-}" || [ "${tx_shm:-0}" -eq 0 ]; then
    echo "VOID: the writer never used the segment, so killing the reader tested nothing"
elif ! is_num "${reader_progress_at_kill:-}" || [ "$reader_progress_at_kill" -eq 0 ]; then
    echo "VOID: the reader showed no progress before the kill - it was not a live reader"
elif ! is_num "$writer_lines_at_kill" || ! is_num "$writer_lines_after"; then
    echo "VOID: could not count the writer's progress lines ('$writer_lines_at_kill' -> '$writer_lines_after')"
elif [ "$writer_lines_after" -le "$writer_lines_at_kill" ] || [ "$writer_alive" = 0 ]; then
    echo "FAIL: the writer stopped making progress after the reader was killed"
    verdict=FAIL
elif ! is_num "${gave_up:-}" || [ "$gave_up" -eq 0 ]; then
    echo "FAIL: the writer never gave the dead peer up (shm_gave_up=${gave_up:-absent})"
    verdict=FAIL
elif ! is_num "${recv2:-}" || [ "$recv2" -lt 1 ]; then
    echo "FAIL: a fresh reader could not take the segment over (recv=${recv2:-none})"
    verdict=FAIL
else
    verdict=PASS
fi
echo "=== $verdict === logs in $OUT"
[ "$verdict" = PASS ]
