#!/usr/bin/env bash
# i4be_close_gap.sh - the candidates for the last two LOSE rows against Fast DDS as shipped in rmw large messages,
# I4 best effort (Array4m, BEST_EFFORT KEEP_LAST 5, 15 Hz, cross-host): subscriber 2.19 against 2.10 ms/MB, publisher
# 3.72 against 3.62 (docs/RESULTS.md "Every LOSE"; ~/claude_reports/I4BE_CPU_GAP_2026-10-17.md). Written 2026-10-18
# before any of its runs. Each candidate is an A/B switch of branch ab/i4be-cpu-candidates, off by default, so every
# arm runs ONE binary (tickle@head's) and differs only in its environment (rmw_keepall_rig.sh, "Added 2026-10-18"):
#   tickle@head     control (every switch unset)       tickle@aa  the same again: the A/A floor
#   tickle@thp      RMW_TICKLE_LARGE_HUGEPAGE=1        (candidate 1: huge pages for the large-buffer pool)
#   tickle@ntcopy   RMW_TICKLE_TAKE_COPY=nt            (candidate 2: non-temporal copy-out in rmw_take)
#   tickle@revcopy  RMW_TICKLE_TAKE_COPY=rev           (candidate 2b: copy-out from the buffer's end to its start)
#   tickle@ntrev    RMW_TICKLE_TAKE_COPY=ntrev         (candidate 2c: both)
#   fastdds         the vendor the rows lose to; the control no switch can touch
# Interleaved, the order rotated per rep (rmw_keepall_rig.sh). perf stat --per-thread on each perf_test over a steady
# window (PERF_ATTACH), unpinned - the headline condition.
#
# HOW TO READ IT: i4be_close_gap_read.py, whose header holds the rules (VOID on an unrealized treatment, the A/A floor,
# the primary metric per candidate, no-harm on latency and delivery). Not a headline figure in either mode.
#
# Modes:
#   PC=1   PC preflight (rmw_keepall_rig.sh PC_PREFLIGHT=1): two private network namespaces and a veth, the publisher's
#          egress shaped to 1 Gbit/s (PF_SHAPE tbf, the rig's link), rmw_tickle rebuilt asking for a 212,992 B socket
#          buffer as the Pis grant (large_block_pc.sh's BUFFERS=rig). x86 Xeon with a 42 MB L3: a 4 MB buffer fits, so
#          the cache question of candidate 2 cannot show here; the PC checks that each switch is realized, harmless
#          and costs nothing it should not.
#   rig    (default) takes the rig lock inside this process; HEAD_SHA must be pushed (the Pis fetch it).
# ESTIMATE: PC 3 reps x 7 arms x ~55 s = ~20 min (+5 min for a rig-buffer build); rig: build 2-5 min + 5 reps x 7 arms
# x ~85 s = ~50 min - analyse an overrun at 1.5 x. The last line is always "I4BE_CLOSE_GAP rc=<n> ...".
# Usage:
#   PC=1 setsid nohup examples/perf_hil/experiments/i4be_close_gap.sh > ~/largemsg_pc/i4be_close/pc.log 2>&1 < /dev/null &
#   HEAD_SHA=<pushed sha of ab/i4be-cpu-candidates> setsid nohup examples/perf_hil/experiments/i4be_close_gap.sh \
#     > ~/rig_results_safe/i4be_close_gap_queue.log 2>&1 < /dev/null &
set -u
X="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$X/../../.." && pwd)"
PC=${PC:-0}
ARMS=${ARMS:-"tickle@head tickle@aa tickle@thp tickle@ntcopy tickle@revcopy tickle@ntrev fastdds"}
say() { echo "$(date -Is) $*"; }
if [ "$PC" != 1 ] && [ "${RIG_LOCK_HELD_HIL:-0}" != 1 ]; then
    : "${HEAD_SHA:?set HEAD_SHA to a pushed commit}"
    say "=== I4 BE close-gap candidates: head $HEAD_SHA, harness $X; taking the rig lock (hil) ==="
    export RIG_LOCK_SCOPE=hil RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-36000} TMPDIR=/home/semih/tmp_build
    exec "$X/../rig_lock.sh" "$0" "$@"
fi
rc=1
trap 'echo "I4BE_CLOSE_GAP rc=$rc $(date -Is)"' EXIT
TS=$(date +%Y%m%d-%H%M%S)
QOS="--keep_last --history_depth 5"
if [ "$PC" = 1 ]; then
    REPS=${REPS:-3} DUR=${DUR:-40} ATTACH=${ATTACH:-8_25}
    OUTDIR=${OUTDIR:-$HOME/largemsg_pc/i4be_close}
    mkdir -p "$OUTDIR" || exit
    OUT=$OUTDIR/i4be_close_pc_$TS
    d=${PF_BUILD:-}
    [ -n "$d" ] || d=$("$X/pc_preflight_build.sh" --find) || { say "no PC preflight build for HEAD"; exit; }
    sb=$d-sb212992 # large_block_pc.sh's BUFFERS=rig: the build's own sources, rmw_tickle rebuilt with the Pis' buffer
    if [ ! -f "$sb/SB_OK" ]; then
        rm -rf "$sb" && mkdir -p "$sb" && cp -a "$d/src" "$d/install" "$sb/" || exit
        sed -i 's/tt_SOCKET_BUFFER_SIZE=4194304/tt_SOCKET_BUFFER_SIZE=212992/' "$sb/src/rmw_tickle/rmw_tickle/CMakeLists.txt"
        grep -q 'tt_SOCKET_BUFFER_SIZE=212992' "$sb/src/rmw_tickle/rmw_tickle/CMakeLists.txt" ||
            { say "cannot set the socket buffer in $sb"; exit; }
        (
            set +u
            # shellcheck disable=SC1091
            source /opt/ros/lyrical/setup.bash && source "$sb/install/local_setup.bash"
            set -u
            export PYTHONPATH=$sb/src/tools/typesupport${PYTHONPATH:+:$PYTHONPATH}
            cd "$sb" && MAKEFLAGS=-j4 colcon build --base-paths src/rmw_tickle --build-base build --install-base install \
                --packages-select rmw_tickle --cmake-args -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=None \
                '-DCMAKE_C_FLAGS=-g -O2' '-DCMAKE_CXX_FLAGS=-g -O2' -DBUILD_TESTING=OFF
        ) > "$sb.build.log" 2>&1 || { say "the rig-buffer build failed: $sb.build.log"; exit; }
        touch "$sb/SB_OK"
    fi
    say "=== PC: build $sb, $REPS reps x ${DUR} s, attach $ATTACH, arms: $ARMS, load $(cut -d' ' -f1-3 /proc/loadavg) ==="
    # x86 has no exc_svc/exc_irq (Arm PMU exception counters): the rest of the rig's events.
    env PC_PREFLIGHT=1 PF_TICKLE_INSTALL="$sb/install" PF_PERF_WS="$d/perf/install" \
        PERF_EVENTS=task-clock,context-switches,cpu-migrations,instructions:u,instructions:k,cycles:u,cycles:k \
        PF_SHAPE="tbf rate 1gbit burst 64kb latency 100ms" HEAD_SHA="$(git -C "$REPO" rev-parse --short=8 HEAD)" \
        PERF_ATTACH="$ATTACH" REPS="$REPS" DUR="$DUR" PUB_RATE=15 QOS_ARGS="$QOS" CELLS="Array4m:0" ARMS="$ARMS" \
        SUMMARY="$X/largemsg_rig_summary.py" OUT="$OUT" "$X/rmw_keepall_rig.sh" > "$OUT.launch.log" 2>&1
    say "  rmw_keepall_rig.sh rc=$? -> $OUT.txt"
else
    say "--- rig lock held: $(cat /tmp/tickle-hil.lock 2>/dev/null) ---"
    OUT=$HOME/rig_results_safe/i4be_close_rig_$TS
    env KCOUNTERS=1 CLOCK_PROBE=1 HEAD_SHA="$HEAD_SHA" PERF_ATTACH=10_40 SKIP_BUILD=0 REPS="${REPS:-5}" DUR=60 \
        PUB_RATE=15 QOS_ARGS="$QOS" CELLS="Array4m:0" ARMS="$ARMS" SUMMARY="$X/largemsg_rig_summary.py" OUT="$OUT" \
        "$X/rmw_keepall_rig.sh" > "$OUT.launch.log" 2>&1
    say "  rmw_keepall_rig.sh rc=$? -> $OUT.txt"
fi
python3 "$X/i4be_close_gap_read.py" "$OUT.runs" > "$OUT.read.txt" 2>&1
read_rc=$?
say "  reading rc=$read_rc -> $OUT.read.txt"
[ "$read_rc" = 0 ] && rc=0
say "=== done ==="
