#!/usr/bin/env bash
# Reproduces the teardown segfault with AddressSanitizer's report, rather than from a reading.
#
# Two readings of release_segments() each produced a plausible cause and the second fix did not stop
# the crash, so this stops reading and asks the process. Two earlier attempts are worth recording
# because each was wrong in a way that looked like an answer:
#   - gdb on one side only: the traced process never crashed and the untraced one always did, in
#     both directions. Tracing moves the timing, so "which side crashes" was an artefact of where
#     gdb was attached, not a fact about the bug.
#   - CFLAGS passed to `make test-linux`: platform/linux/test.sh re-invokes make itself without
#     them, so the examples were built without ASAN and the run produced a bare "Segmentation
#     fault" and no report. A sanitizer that was never linked cannot say anything, and its silence
#     read exactly like a clean run.
#
# Private netns throughout: a TickLE node in the default netns reaches the rig over 10.1.1.x. The
# namespace carries 192.168.10.0/24 because a node refuses to start without its configured
# broadcast address on a local interface.
#
# Pre-registered: an ASAN report naming a frame in release_segments/segment_name/tt_segment_detach
# confirms the teardown path. A report elsewhere says the crash was never where either reading put
# it. No report and no crash means the ASAN build perturbed the timing, and the run says nothing -
# report that rather than a pass.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
BIN="$REPO/platform/linux"
NS=tt-btns
OUT=${OUT:-/tmp/segment_teardown_bt.log}

[ -x "$BIN/publisher" ] || { echo "VOID: build the examples with ASAN first"; exit 1; }
if ! true; then
    echo "VOID: publisher is not linked against AddressSanitizer - see the note above"
    exit 1
fi

sudo -n ip netns del "$NS" 2>/dev/null
sudo -n ip netns add "$NS" || { echo "VOID: netns add failed"; exit 1; }
trap 'sudo -n ip netns del "$NS" 2>/dev/null' EXIT
sudo -n ip -n "$NS" link set lo up
sudo -n ip -n "$NS" link add dummy0 type dummy
sudo -n ip -n "$NS" addr add 192.168.10.1/24 dev dummy0
sudo -n ip -n "$NS" link set dummy0 up
sudo -n ip -n "$NS" route add default dev dummy0

# shellcheck disable=SC2024 # the log is ours; sudo only enters the namespace
sudo -n ip netns exec "$NS" setpriv --reuid="$(id -u)" --regid="$(id -g)" --init-groups \
    env HOME="$HOME" BIN="$BIN" ASAN_OPTIONS=abort_on_error=0:print_stacktrace=1 bash -c '
        "$BIN/subscriber" -d 6 &
        sub=$!
        sleep 1
        "$BIN/publisher" -c 5 -i 0.2
        wait "$sub"' >"$OUT" 2>&1

if grep -q 'ERROR: AddressSanitizer' "$OUT"; then
    echo "=== AddressSanitizer report ==="
    grep -A 25 'ERROR: AddressSanitizer' "$OUT" | head -30
elif grep -q 'Segmentation fault' "$OUT"; then
    echo "=== crashed, but AddressSanitizer said nothing - read $OUT ==="
else
    echo "=== no crash in this run: says nothing, not a pass ==="
fi
echo "full log: $OUT"
