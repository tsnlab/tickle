#!/usr/bin/env bash
# C7 sim sweep: slow_probe.c built against BASE (default 73e7a38c, the parent) and against this checkout, over a grid of
# slow readers (service time x socket size), slow readers with path loss, and the control - a fast reader with path
# loss only, which the repair window must not touch (window_held 0, resends equal to BASE's).
# Output: one "arm R ..." / "arm X ..." pair per cell in $OUT (default /home/semih/tmp_build/c7/sweep.txt).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
base=${BASE:-73e7a38c}
work=${WORK:-/home/semih/tmp_build/c7}
out=${OUT:-$work/sweep.txt}
mkdir -p "$work/base_tree/src" "$work/base_tree/include"
cp -r "$root/src/." "$work/base_tree/src/"
cp -r "$root/include/." "$work/base_tree/include/"
git -C "$root" show "$base:src/tickle.c" > "$work/base_tree/src/tickle.c"
git -C "$root" show "$base:include/tickle/tickle.h" > "$work/base_tree/include/tickle/tickle.h"
# The base probe: this checkout's sim (tests/, the probe) against BASE's core; what the sim reads of the repair window
# (tt_PeerAck.repair_window, which BASE has not) reads 0 there.
rm -f "$work/probe_base" "$work/probe_head"
mkdir -p "$work/base_tree/tests" "$work/base_tree/examples/perf_hil/experiments/c7"
cp "$root/tests/"*.h "$work/base_tree/tests/"
sed 's/window->repair_window/0U/g' "$root/tests/test_large_sample.c" > "$work/base_tree/tests/test_large_sample.c"
cp "$here/slow_probe.c" "$work/base_tree/examples/perf_hil/experiments/c7/"
cc -I"$work/base_tree/include" -I"$work/base_tree/src" -DTICKLE_C="\"$work/base_tree/src/tickle.c\"" -O2 -w \
    -o "$work/probe_base" "$work/base_tree/examples/perf_hil/experiments/c7/slow_probe.c" "$root/src/encoding.c" \
    "$root/src/log.c" -lpthread -lm
cc -DPROBE_WINDOW -I"$root/include" -I"$root/src" -O2 -w -o "$work/probe_head" "$here/slow_probe.c" \
    "$root/src/encoding.c" "$root/src/log.c" -lpthread -lm
[ -x "$work/probe_base" ] && [ -x "$work/probe_head" ] || { echo "build failed" >&2; exit 1; }
: > "$out"
cell() { # samples svc cap loss seed paced
    for arm in base head; do
        timeout 120 "$work/probe_$arm" "$1" "$2" "$3" "$4" "$5" 0 30 "$6" | sed "s/^/$arm /" >> "$out" ||
            echo "$arm FAILED $*" >> "$out"
    done
}
for svc in 15 20 30 36 40 44 46 50; do
    for cap in 100 200 400 1000; do
        cell 30 "$svc" "$cap" 0 7 1
    done
done
cell 10 20 400 0 7 0 # resent_through_a_queue()'s bunched clock
for seed in 1 2 3 4 5 6 7 8 9 10; do
    cell 30 20 200 10 "$seed" 1 # slow reader, 1% path loss
    cell 30 40 400 10 "$seed" 1
    cell 30 0 0 10 "$seed" 1 # control: fast reader, 1% path loss
    cell 30 0 0 50 "$seed" 1 # control: fast reader, 5% path loss
done
echo "SWEEP_DONE" >> "$out"
