#!/usr/bin/env bash
# large_block_pc.sh - the PC check of RMW_TICKLE_LARGE_PUBLISH_BLOCK_MS (docs/RMW.md "Large samples: blocking publish")
# on the I4s cell: perf_test Array4m, BEST_EFFORT KEEP_LAST 5 at 30 Hz, through rmw_keepall_rig.sh's PC preflight (two
# private network namespaces, a veth) with the publisher's egress shaped to the rig's 1 Gbit/s (PF_SHAPE tbf), so the
# 4 MB stream saturates the link as on the rig. Arms: tickle@head (option unset, the control), tickle@block (the same
# binary with the option), fastdds, cyclonedds.
#
# NOT a rig figure. It answers whether the option moves the cell the way the design predicts.
#
# The socket buffers (BUFFERS): rmw_tickle asks for 4 MiB; the Pis grant 425,984 bytes (their net.core.[rw]mem_max is
# the default 212,992), this PC 8 MiB, which holds two 4 MB samples in the sender's socket and adds their wire time to
# every latency. BUFFERS=rig (the default) runs a copy of the PC build whose rmw_tickle asks for 212,992 - granted
# 425,984 here as on the Pis - built next to it as <build>-sb212992; BUFFERS=pc runs the build as it is. Measured
# 2026-10-16 (the first version of the option, waiting before the hand-off): tickle@head's median latency 53.8 ms
# with rig buffers - the rig's I4s row reads 55.0 - and 81.0 with the PC's.
#
# HOW TO READ IT, written before the first measured run (2026-10-16; amended after the 6 s harness dry run, which
# showed the publisher's teardown abandoning the one sample in flight when perf_test stops, in either arm):
#   TREATMENT  every tickle@block publisher log shows large_block_waits > 0, every tickle@head one large_block_waits=0
#              (rmw_tickle's shutdown line); else the run is VOID for this question.
#   SATURATED  tickle@head abandons at the sender (core's large: line) more than the one sample teardown cuts, in
#              every run; a run where it does not is NOT SATURATED and says nothing about the option.
#   PREDICTED  tickle@block: large_block_timeouts 0 and tail_abandoned at most 1 (the teardown's) in every run, else
#              FALSIFIED. Against tickle@head, medians over reps: fewer samples published, as a DDS publisher's are;
#              delivered-of-offered (30 Hz x the window) within 2 points, else a COST; median latency lower, toward
#              the vendors', else FALSIFIED (the option does not do what it is for).
# The summary is largemsg_rig_summary.py's (offered_pct, latency per arm), with this script's checks after it.
#
# Usage: large_block_pc.sh [REPS] [DUR]   (defaults 3, 30)   env: OUT, BLOCK_MS (1000), BUFFERS (rig), PF_SHAPE,
#   PF_BUILD (pc_preflight_build.sh's build directory; found for HEAD when unset). Detached, as CLAUDE.md says:
#   setsid nohup examples/perf_hil/experiments/large_block_pc.sh > /path/large_block_pc.log 2>&1 < /dev/null &
# The last line is always "LARGE_BLOCK_PC rc=<n> ...".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
REPS=${1:-3}
DUR=${2:-30}
OUT=${OUT:-$HOME/largemsg_pc/large_block/large_block_pc_$(date +%Y%m%d-%H%M%S)}
rc=1
trap 'echo "LARGE_BLOCK_PC rc=$rc out=$OUT $(date -Is)"' EXIT
mkdir -p "$(dirname "$OUT")" || exit
d=${PF_BUILD:-}
if [ -z "$d" ]; then
    d=$("$HERE/pc_preflight_build.sh" --find) || { echo "no PC preflight build for HEAD: run pc_preflight_build.sh"; exit; }
fi
PF_PERF_WS=$d/perf/install
case "${BUFFERS:-rig}" in
    pc) PF_TICKLE_INSTALL=$d/install ;;
    rig) # the build's own sources, rmw_tickle alone rebuilt into a copy of its install
        sb=$d-sb212992
        if [ ! -f "$sb/SB_OK" ]; then
            rm -rf "$sb" && mkdir -p "$sb" && cp -a "$d/src" "$d/install" "$sb/" || exit
            sed -i 's/tt_SOCKET_BUFFER_SIZE=4194304/tt_SOCKET_BUFFER_SIZE=212992/' "$sb/src/rmw_tickle/rmw_tickle/CMakeLists.txt"
            grep -q 'tt_SOCKET_BUFFER_SIZE=212992' "$sb/src/rmw_tickle/rmw_tickle/CMakeLists.txt" ||
                { echo "cannot set the socket buffer in $sb"; exit; }
            (
                set +u
                # shellcheck disable=SC1091
                source /opt/ros/lyrical/setup.bash && source "$sb/install/local_setup.bash"
                set -u
                export PYTHONPATH=$sb/src/tools/typesupport${PYTHONPATH:+:$PYTHONPATH}
                cd "$sb" && colcon build --base-paths src/rmw_tickle --build-base build --install-base install \
                    --packages-select rmw_tickle --cmake-args -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=None \
                    '-DCMAKE_C_FLAGS=-g -O2' '-DCMAKE_CXX_FLAGS=-g -O2' -DBUILD_TESTING=OFF
            ) > "$sb.build.log" 2>&1 || { echo "the rig-buffer build failed: $sb.build.log"; exit; }
            touch "$sb/SB_OK"
        fi
        PF_TICKLE_INSTALL=$sb/install ;;
    *) echo "BUFFERS is rig or pc, not '$BUFFERS'"; exit ;;
esac
echo "buffers ${BUFFERS:-rig}: $PF_TICKLE_INSTALL"
export PF_TICKLE_INSTALL PF_PERF_WS
export PF_SHAPE=${PF_SHAPE:-tbf rate 1gbit burst 64kb latency 100ms}
export BLOCK_MS=${BLOCK_MS:-1000}
PC_PREFLIGHT=1 CLOCK_PROBE=1 HEAD_SHA=$(git -C "$REPO" rev-parse --short=8 HEAD) REPS=$REPS DUR=$DUR CELLS="Array4m:0" \
    QOS_ARGS="--keep_last --history_depth 5" PUB_RATE=30 ARMS="tickle@head tickle@block fastdds cyclonedds" \
    SUMMARY="$HERE/largemsg_rig_summary.py" OUT="$OUT" "$HERE/rmw_keepall_rig.sh"
rig_rc=$?
echo "rmw_keepall_rig.sh rc=$rig_rc"

# The reading rules above that the summary does not apply.
runs=$OUT.runs
verdict=PASS
field() { # <log> <key>: the last value of key= in the log, empty when it never printed
    grep -o "$2=[0-9]*" "$1" 2>/dev/null | tail -1 | cut -d= -f2
}
declare -A seen=([tickle@head]=0 [tickle@block]=0)
for log in "$runs"/tickle@head_*_pub.log "$runs"/tickle@block_*_pub.log; do
    [ -f "$log" ] || continue
    arm=$(basename "$log"); arm=${arm%%_*}
    seen[$arm]=$((seen[$arm] + 1))
    waits=$(field "$log" large_block_waits)
    abandoned=$(field "$log" tail_abandoned)
    timeouts=$(field "$log" large_block_timeouts)
    echo "$(basename "$log"): large_block_waits=${waits:-none} large_block_timeouts=${timeouts:-none}" \
        "tail_abandoned=${abandoned:-none}"
    case "$arm" in
        tickle@head)
            [ "${waits:-x}" = 0 ] || { echo "  VOID: the control shows the treatment (or printed nothing)"; verdict=VOID; }
            [ -n "$abandoned" ] && [ "$abandoned" -gt 1 ] || { echo "  NOT SATURATED"; verdict=VOID; } ;;
        tickle@block)
            [ -n "$waits" ] && [ "$waits" -gt 0 ] || { echo "  VOID: no publish waited (or printed nothing)"; verdict=VOID; }
            [ -n "$abandoned" ] && [ "$abandoned" -le 1 ] && [ "${timeouts:-x}" = 0 ] ||
                { echo "  FALSIFIED: a wait ran out, or a sample was replaced at the sender, with the option on"
                  verdict=FALSIFIED; } ;;
    esac
done
for arm in tickle@head tickle@block; do # no log is "could not look", never a pass
    [ "${seen[$arm]}" -ge 1 ] || { echo "  VOID: no $arm publisher log in $runs"; verdict=VOID; }
done
# PREDICTED's medians, from the summary's own table (its "metric" header names the arms in column order).
python3 - "$OUT.txt" <<'PY' || verdict=VOID
import re, sys
rows = {}
arms = []
for line in open(sys.argv[1]):
    if line.strip().startswith("metric") and "tickle@block" in line:
        arms = line.split()[1:-1]
    for key in ("latency median ms", "delivered % of offered", "rate Hz"):
        if line.strip().startswith(key):
            rows[key] = [None if v == "-" else float(v) for v in re.findall(r"(-|[0-9.]+) \[[0-9]+\]", line)]
def get(key, arm):
    vals = rows.get(key, [])
    i = arms.index(arm) if arm in arms else -1
    return vals[i] if 0 <= i < len(vals) else None
lat_h, lat_b = get("latency median ms", "tickle@head"), get("latency median ms", "tickle@block")
off_h, off_b = get("delivered % of offered", "tickle@head"), get("delivered % of offered", "tickle@block")
print(f"latency median ms: head {lat_h} block {lat_b} | delivered % of offered: head {off_h} block {off_b}")
for vendor in ("fastdds", "cyclonedds"):
    print(f"  {vendor}: latency median {get('latency median ms', vendor)}, of offered {get('delivered % of offered', vendor)}")
if None in (lat_h, lat_b, off_h, off_b):
    print("PREDICTED: VOID - a median is missing (no clock, or no usable rep)")
    sys.exit(1)
print("PREDICTED latency:", "HOLDS (lower)" if lat_b < lat_h else "FALSIFIED (not lower)")
print("PREDICTED delivered-of-offered:", "within 2 points" if off_b >= off_h - 2.0 else "COST (more than 2 points lower)")
PY
echo "LARGE_BLOCK_PC treatment/saturation: $verdict"
[ "$rig_rc" = 0 ] && rc=0
