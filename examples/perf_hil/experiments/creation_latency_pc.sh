#!/usr/bin/env bash
# creation_latency_pc.sh - the PC dry run of creation-time latency (creation_latency_summary.py; the user's decision of
# 2026-10-17: latency = arrival - the scheduled publish time). Three rmw_keepall_rig.sh PC_PREFLIGHT invocations, two
# private network namespaces joined by a veth, never the default namespace, no load generator:
#   SAT   a link-saturating cell, scaled down from the rig's I4s so the PC carries a fifth of its traffic: Array1m
#         BEST_EFFORT KEEP_LAST 5 at 25 Hz through the publisher's eth0 shaped to tbf 200 Mbit/s (offered 222 Mb/s on
#         the wire, 1.11x the link; I4s is 1.07x)
#   FREE  the same topic and link at 10 Hz (0.44x): nothing has to wait
#   CTL   the ping-pong control: Array1k, RELIABLE KEEP_LAST 10, 50 Hz, perf_test --roundtrip_mode Main/Relay
# Arms tickle@head, tickle@block (RMW_TICKLE_LARGE_PUBLISH_BLOCK_MS), fastdds, cyclonedds; CTL tickle@head, fastdds,
# cyclonedds. The perf_test is a copy of this PC's performance_test source with perf_test_creation_trace.py applied,
# built against the PC preflight build of HEAD (pc_preflight_build.sh --find) into $CTL_BUILD.
#
# NOT a rig figure: a veth and tbf are not the rig's NIC. It shows the instrument does what it must.
#
# HOW TO READ IT, written 2026-10-17 before the first run and enforced at the end (PC_CHECK lines):
#   P1 SAT: every arm whose publish blocks (fastdds, cyclonedds, tickle@block) shows a median realized gap above one
#      period (40 ms) in every usable rep - the creation time includes the sender's wait - and its median creation
#      latency exceeds its median stamp latency by more than one period - the stamp hides it.
#   P2 SAT: tickle@head (publish does not block) shows a median gap below 1 ms: its creation and stamp latencies are
#      the same quantity there, and its wait is inside both.
#   P3 FREE: every arm's median gap below 1 ms and none GROWING: with nothing to wait for, the two definitions agree.
#   P4 CTL: creation_latency_summary.py's control (R4) PASSES.
#   Any VOID run (identity, trace, grid, instrument fault) fails the dry run. A rep count under 2 is NOT READ, a fail.
# Usage: creation_latency_pc.sh [REPS] [DUR]   (defaults 2, 30)   env: OUT (default
#   /home/semih/tmp_build/ctl/pc_<ts>), CTL_BUILD (default /home/semih/tmp_build/ctl/perf; built here when it has no
#   CTL_OK), PERF_TEST_SRC (~/rmw_perf_ws/src/performance_test), PF_BUILD. Detached:
#   setsid nohup examples/perf_hil/experiments/creation_latency_pc.sh > /path/outside/the/session.log 2>&1 < /dev/null &
# Estimated 25 min (3 cells x up to 8 runs x ~40 s, plus a 2 min perf_test build when missing). The last line is
# always "CREATION_LATENCY_PC rc=<n> ...".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
REPS=${1:-2}
DUR=${2:-30}
OUT=${OUT:-/home/semih/tmp_build/ctl/pc_$(date +%Y%m%d-%H%M%S)}
CTL_BUILD=${CTL_BUILD:-/home/semih/tmp_build/ctl/perf}
PERF_TEST_SRC=${PERF_TEST_SRC:-$HOME/rmw_perf_ws/src/performance_test}
rc=1
trap 'echo "CREATION_LATENCY_PC rc=$rc out=$OUT $(date -Is)"' EXIT
mkdir -p "$OUT" || exit
d=${PF_BUILD:-}
if [ -z "$d" ]; then
    d=$("$HERE/pc_preflight_build.sh" --find) || { echo "no PC preflight build for HEAD: run pc_preflight_build.sh"; exit; }
fi
echo "preflight build $d; perf_test with the creation trace in $CTL_BUILD"
if [ ! -f "$CTL_BUILD/CTL_OK" ] || ! grep -qx "pf_build=$d" "$CTL_BUILD/CTL_OK"; then
    rm -rf "$CTL_BUILD" && mkdir -p "$CTL_BUILD" || exit
    cp -a "$PERF_TEST_SRC" "$CTL_BUILD/src" && rm -rf "$CTL_BUILD/src/.git" || exit
    python3 "$HERE/perf_test_creation_trace.py" apply "$CTL_BUILD/src/performance_test" || exit
    (
        set +u
        # shellcheck disable=SC1091
        source /opt/ros/lyrical/setup.bash && source "$d/install/local_setup.bash"
        set -u
        cd "$CTL_BUILD" && colcon build --base-paths src --build-base build --install-base install \
            --packages-select performance_test --cmake-args -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
            -DPERFORMANCE_TEST_RT_ENABLED=ON -DPERFORMANCE_TEST_CALLBACK_EXECUTOR_ENABLED=ON
    ) > "$CTL_BUILD.build.log" 2>&1 || { echo "perf_test build failed: $CTL_BUILD.build.log"; exit; }
    printf 'pf_build=%s\nbuilt=%s\n' "$d" "$(date -Is)" > "$CTL_BUILD/CTL_OK"
fi
export PF_TICKLE_INSTALL=$d/install PF_PERF_WS=$CTL_BUILD/install
export PC_PREFLIGHT=1 CLOCK_PROBE=1 CREATION_TRACE=1 HEAD_SHA
HEAD_SHA=$(git -C "$REPO" rev-parse --short=8 HEAD)
cell() { # tag: one rmw_keepall_rig.sh invocation; its summary is largemsg_rig_summary.py's, creation's comes after
    echo "--- $1 $(date -Is) ---"
    REPS=$REPS DUR=$DUR SUMMARY="$HERE/largemsg_rig_summary.py" OUT="$OUT/$1" "$HERE/rmw_keepall_rig.sh" \
        > "$OUT/$1.launch.log" 2>&1
    echo "$1 rmw_keepall_rig.sh rc=$? $(date -Is)"
}
# latency 500ms: tbf's queue limit (rate x latency, 12.5 MB) must exceed the PC's 8 MiB socket send buffer, so a full
# link pushes back on the sender's socket as the rig's NIC does instead of dropping in the qdisc. At 100 ms (2.5 MB)
# the first run (2026-10-17 09:11) dropped 2.2% of rmw_tickle's datagrams there and delivered 8 of 625 samples.
SHAPE="tbf rate 200mbit burst 64kb latency 500ms"
PF_SHAPE=$SHAPE CELLS="Array1m:0" QOS_ARGS="--keep_last --history_depth 5" PUB_RATE=25 \
    ARMS="tickle@head tickle@block fastdds cyclonedds" cell sat
PF_SHAPE=$SHAPE CELLS="Array1m:0" QOS_ARGS="--keep_last --history_depth 5" PUB_RATE=10 \
    ARMS="tickle@head tickle@block fastdds cyclonedds" cell free
ROUNDTRIP=1 CELLS="Array1k:0" QOS_ARGS="--reliable --keep_last --history_depth 10" PUB_RATE=50 \
    ARMS="tickle@head fastdds cyclonedds" cell ctl

S=$HERE/creation_latency_summary.py
python3 "$S" "$OUT/sat.runs" "$DUR" --control "$OUT/ctl.runs" > "$OUT/sat.creation.txt" 2>&1
python3 "$S" "$OUT/free.runs" "$DUR" > "$OUT/free.creation.txt" 2>&1
cat "$OUT/sat.creation.txt" "$OUT/free.creation.txt"
python3 - "$HERE" "$OUT" <<'PY'
import statistics as st
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
import creation_latency_summary as c  # noqa: E402
out = Path(sys.argv[2])
bad = []
for tag in ("sat", "free", "ctl"):
    meta, rate, rt, recs, mbps = c.load(out / f"{tag}.runs")
    for r in recs:
        if r["void"]:
            bad.append(f"{tag} VOID {r['arm']} r{r['rep']}: {r['void']}")
    per = 1e3 / rate
    arms = sorted({r["arm"] for r in recs})
    for arm in arms:
        g = [r for r in recs if r["arm"] == arm and not r["void"]]
        if len(g) < 2:
            bad.append(f"{tag} {arm}: {len(g)} usable reps (NOT READ)")
            continue
        gap = [r["gap_med"] for r in g]
        diff = [r["crt_med_deliv"] - r["stamp_med"] for r in g]
        line = (f"PC_CHECK {tag} {arm}: gap med {[round(x, 3) for x in gap]} ms, creation - stamp median "
                f"{[round(x, 2) for x in diff]} ms, creation (offered) {[round(r['crt_med'], 2) for r in g]}, stamp "
                f"{[round(r['stamp_med'], 2) for r in g]}, growth {[round(r['growth_ms'], 1) for r in g]} ms")
        print(line)
        if tag == "sat" and arm.startswith(c.BLOCKING):
            if not all(x > per for x in gap) or not all(x > per for x in diff):
                bad.append(f"P1 {arm}: gap {gap} / creation - stamp {diff} not above one period {per:.1f} ms")
        if tag == "sat" and arm == "tickle@head" and not all(x < 1.0 for x in gap):
            bad.append(f"P2 tickle@head: gap {gap} not below 1 ms")
        if tag == "free" and (not all(x < 1.0 for x in gap) or
                              any(r["growth_ms"] > max(1.0, 0.1 * r["crt_med_deliv"]) for r in g)):
            bad.append(f"P3 {arm}: gap {gap} or growth {[r['growth_ms'] for r in g]}")
    if tag == "ctl":
        ok, lines = c.control(recs)
        print("\n".join(lines))
        if not ok:
            bad.append("P4 control FAILED")
print("PC_CHECK " + ("PASS" if not bad else "FAIL:\n  " + "\n  ".join(bad)))
sys.exit(1 if bad else 0)
PY
rc=$?
