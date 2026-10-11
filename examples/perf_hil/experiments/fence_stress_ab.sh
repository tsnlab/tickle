#!/usr/bin/env bash
# fence_stress_ab.sh - test_thread_safety's stress, the place the UDP-to-segment switch loss was found (2026-10-11),
# run RUNS times per arm in PAR parallel loops: arm A (the core before the fence) against arm B (with it). PC only, no
# rig, no lock, no network: the test brings its own in-memory HAL, so nothing reaches a socket or /dev/shm.
#
# What is counted per run: the samples B's subscribers discarded as out of order (the harness: line), and, from the
# "segment fences:" line, the fences either context's ring met, passed or expired (arm B only).
#
# PRE-REGISTERED READING, enforced below:
#   VOID  arm A has no run with a discard: this load did not reproduce the defect, so B's zero would mean nothing.
#         (Measured before the fence: 9 of 240 runs at 4x parallel load. At p = 9/240 the chance that 600 runs see
#         none is (1 - p)^600 ~ 1e-10, so RUNS=600 cannot void by chance.)
#   VOID  arm B has no run in which a ring met a fence: the switch the fence exists for never happened there.
#         (Run 1, 2026-10-11, read this from a log line test_thread_safety never prints, and voided correctly.)
#   FAIL  arm B has any run with a discard.
#   PASS  otherwise.
# Test failures (no "all tests passed") are counted per arm and printed; they do not decide, since the stress has other
# assertions (timers, calls) this change does not touch - but an arm-B count above arm A's is reported as a warning.
#
# Usage: A=<commit|@dir> B=<commit|@dir> [RUNS=600] [PAR=4] [TIMEOUT=60] [OUT=prefix] fence_stress_ab.sh
#   @dir builds that tree as it is on disk (uncommitted changes included). OUT defaults to
#   ~/rig_results_safe/fence_stress_ab_<stamp> ($OUT.txt, $OUT.d/). Ends with "=== fence_stress_ab ended".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
A=${A:?A=<commit or @dir>}
B=${B:?B=<commit or @dir>}
RUNS=${RUNS:-600}
PAR=${PAR:-4}
TIMEOUT=${TIMEOUT:-60}
OUT=${OUT:-$HOME/rig_results_safe/fence_stress_ab_$(date +%Y%m%d-%H%M%S)}
T0=$(date +%s)
mkdir -p "$OUT.d" || exit 1
SUM="$OUT.txt"
: >"$SUM"
say() { echo "$*" | tee -a "$SUM"; }
WORK=$(mktemp -d /tmp/fence_stress_ab.XXXXXX) || exit 1
on_exit() {
    local rc=$?
    rm -rf "$WORK"
    say "=== fence_stress_ab ended rc=$rc $(date -Is) after $(($(date +%s) - T0)) s: $OUT ==="
}
trap on_exit EXIT
say "=== fence_stress_ab $(date -Is): A=$A B=$B RUNS=$RUNS PAR=$PAR harness=$(git -C "$REPO" rev-parse --short HEAD) ==="

build_arm() { # $1 label, $2 commit or @dir
    local label=$1 src=$2 tree="$WORK/$1"
    mkdir -p "$tree"
    if [ "${src:0:1}" = @ ]; then
        (cd "${src:1}" && git ls-files -z --cached --others --exclude-standard | tar --null -T - -cf -) | tar -x -C "$tree"
        say "    $label: tree ${src:1} at $(git -C "${src:1}" rev-parse --short HEAD) + diff $(git -C "${src:1}" diff HEAD | sha256sum | cut -c1-12)"
    else
        git -C "$REPO" archive "$src" | tar -x -C "$tree"
        say "    $label: commit $(git -C "$REPO" rev-parse "$src")"
    fi
    # The command platform/linux/Makefile uses for this test, with the debug build's flags.
    cc -I"$tree/include" -I"$tree/src" -O0 -g -Wall -Wextra -o "$WORK/tts_$label" "$tree/tests/test_thread_safety.c" \
        "$tree/src/tickle.c" "$tree/src/encoding.c" "$tree/src/log.c" -lpthread -lm >"$OUT.d/build_$label.log" 2>&1
}
build_arm A "$A" || { say "FATAL: build A failed ($OUT.d/build_A.log)"; exit 1; }
build_arm B "$B" || { say "FATAL: build B failed ($OUT.d/build_B.log)"; exit 1; }

# One loop: runs its share and writes one line per run: arm run rc passed discards fences.
loop() { # $1 arm, $2 loop index
    local arm=$1 k=$2 run out rc passed disc fences
    for run in $(seq "$k" "$PAR" "$RUNS"); do
        out=$(cd "$WORK" && timeout "$TIMEOUT" "$WORK/tts_$arm" 2>&1)
        rc=$?
        passed=$(grep -c 'test_thread_safety: all tests passed' <<<"$out")
        disc=$(grep -a '^harness:' <<<"$out" | sed -n 's/.*discarded as out of order \([0-9]*\).*/\1/p')
        # Fences met by either context's ring (both publish to the other), from the summary test_thread_safety prints
        # in a build that has them (arm B only).
        fences=$(grep -a '^segment fences:' <<<"$out" |
            sed -n 's/.*a passed=\([0-9]*\) expired=\([0-9]*\), b passed=\([0-9]*\) expired=\([0-9]*\).*/\1 \2 \3 \4/p' |
            awk '{print $1 + $2 + $3 + $4}')
        echo "$arm $run $rc $passed ${disc:-NA} ${fences:-NA}"
    done
}
for arm in A B; do
    say "--- arm $arm $(date -Is) ---"
    pids=()
    for k in $(seq 1 "$PAR"); do
        loop "$arm" "$k" >"$OUT.d/runs_${arm}_$k.txt" &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p"; done
done
cat "$OUT.d"/runs_*.txt >"$OUT.d/runs.txt"

python3 - "$OUT.d/runs.txt" "$RUNS" <<'EOF' | tee -a "$SUM"
import sys
rows = [line.split() for line in open(sys.argv[1]) if line.strip()]
runs = int(sys.argv[2])
stat = {}
for arm in ("A", "B"):
    r = [x for x in rows if x[0] == arm]
    n = len(r)
    unread = sum(1 for x in r if x[4] == "NA")
    disc_runs = sum(1 for x in r if x[4] != "NA" and int(x[4]) > 0)
    disc_total = sum(int(x[4]) for x in r if x[4] != "NA")
    failed = sum(1 for x in r if x[3] != "1")
    fence_runs = sum(1 for x in r if x[5] not in ("NA",) and int(x[5]) > 0)
    stat[arm] = dict(n=n, unread=unread, disc_runs=disc_runs, disc_total=disc_total, failed=failed, fence_runs=fence_runs)
    print(f"arm {arm}: {n} runs ({unread} without a harness line), {disc_runs} with a discard ({disc_total} samples), "
          f"{fence_runs} where the reader met a fence, {failed} not passing")
a, b = stat["A"], stat["B"]
if a["n"] != runs or b["n"] != runs:
    print(f"VERDICT VOID: runs recorded A={a['n']} B={b['n']}, planned {runs} each")
elif a["unread"] or b["unread"]:
    print("VERDICT VOID: a run printed no harness line, so its discards could not be read")
elif a["disc_runs"] == 0:
    print("VERDICT VOID: arm A never discarded - this load did not reproduce the defect")
elif b["fence_runs"] == 0:
    print("VERDICT VOID: arm B's reader never met a fence - the switch did not happen there")
elif b["disc_runs"] > 0:
    print(f"VERDICT FAIL: arm B discarded in {b['disc_runs']} of {b['n']} runs")
else:
    print(f"VERDICT PASS: A discarded in {a['disc_runs']}/{a['n']} runs, B in 0/{b['n']} "
          f"(B met a fence in {b['fence_runs']})")
if b["failed"] > a["failed"]:
    print(f"WARNING: arm B failed {b['failed']} runs against arm A's {a['failed']}")
EOF
