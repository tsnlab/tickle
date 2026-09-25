#!/usr/bin/env bash
# What does a default rclcpp::Node cost in memory under rmw_tickle, against rmw_fastrtps_cpp and
# rmw_cyclonedds_cpp?
#
# The question (the user's, 2026-09-25): TickLE core never allocates - every buffer is caller-owned
# storage, sized for the worst case at creation. rmw_tickle, the caller, allocates once at creation
# and sizes each buffer from the type's capacity rather than from what a sample actually uses. At
# N = 65507 that reservation is large by construction. Does it make a node cost materially more
# than a DDS node, which allocates as samples arrive?
#
# Why this is a fair dev-box measurement, unlike a timing one (PLAN.md's standing rule): peak RSS
# of a fixed workload is near-deterministic and does not depend on what else the machine is doing.
# Repetitions are here to show that, not to average noise away.
#
# The same node binary (rmw_tickle_interfaces_check's default_node, four publishers: String,
# Header, UInt8MultiArray, JointState) runs under each rmw; only RMW_IMPLEMENTATION differs. Peak
# RSS is read from /proc/PID/status VmHWM, sampled while it runs and once more at the end, so a
# peak between samples is not missed (VmHWM is the kernel's own high-water mark, not a sample).
#
# HOW TO READ IT, written before running:
#   - VOID for a run whose node exited non-zero, or whose maps do not show the rmw it was asked
#     for: a node that silently fell back to another middleware compares nothing.
#   - The figure is VmHWM. RSS at exit understates a peak that has since been freed.
#   - There is no no-publisher baseline mode in this node, so the figure compared is the whole
#     node's peak, and rclcpp itself is a large part of it in every arm. That is why the number
#     read is the DIFFERENCE between arms: the binary, the rclcpp version and the workload are
#     identical, so what differs is the middleware.
#   - Reading rule: if rmw_tickle's peak is within a few MB of both DDS, static sizing is not the
#     memory problem it is suspected to be, whatever else argues for dynamic allocation. If it is
#     tens of MB above, it is. The per-entity reservation is computed separately from the code; this
#     says whether that reservation is actually paid.
set -euo pipefail
REPO=/home/semih/tickle
WS="${WS:-/tmp/p2_all_ws}"
REPS="${REPS:-3}"
OUT="${OUT:-/tmp/tickle_rmw_memory_$(date +%Y%m%d-%H%M%S).txt}"
if [ "${RIG_LOCK_HELD_BOX:-0}" != "1" ]; then
    OUT="$OUT" RIG_LOCK_SCOPE=box exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi
: > "$OUT"
say() { echo "$*" | tee -a "$OUT"; }

set +u
# shellcheck disable=SC1091
. /opt/ros/lyrical/setup.bash
# shellcheck disable=SC1091
. "$REPO/install/setup.bash"
# shellcheck disable=SC1091
. "$WS/install/setup.bash"
set -u
export TICKLE_BROADCAST_ADDR=127.255.255.255
NODE="$WS/install/rmw_tickle_interfaces_check/lib/rmw_tickle_interfaces_check/default_node"
[ -x "$NODE" ] || { say "no default_node at $NODE"; exit 1; }

say "=== default rclcpp::Node memory, $(date -Is), repo $(git -C "$REPO" rev-parse --short HEAD) ==="
say "node: $NODE"
say "tt_MAX_BUFFER_LENGTH in rmw_tickle: $(grep -o 'tt_MAX_BUFFER_LENGTH=[0-9]*' "$REPO/build/rmw_tickle/CMakeFiles/rmw_tickle.dir/flags.make" 2>/dev/null | head -1)"

# One run: $1 rmw, $2 seconds. Prints "VmHWM_kB VmRSS_kB verdict".
run_one() {
    local rmw=$1 secs=$2 hwm=0 rss=0 mapped=no
    RMW_IMPLEMENTATION="$rmw" TICKLE_NODE_ID=$((110 + RANDOM % 20)) \
        "$NODE" pub "$secs" > "$OUT.node" 2>&1 &
    local pid=$!
    for _ in $(seq 1 60); do
        kill -0 "$pid" 2>/dev/null || break
        if [ -r "/proc/$pid/status" ]; then
            local h; h=$(awk '/VmHWM/{print $2}' "/proc/$pid/status" 2>/dev/null || true)
            local r; r=$(awk '/VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null || true)
            [ -n "${h:-}" ] && [ "$h" -gt "$hwm" ] && hwm=$h
            [ -n "${r:-}" ] && [ "$r" -gt "$rss" ] && rss=$r
            # Identity: the process must have mapped the middleware it was asked for.
            grep -q "lib${rmw}\.so" "/proc/$pid/maps" 2>/dev/null && mapped=yes
        fi
        sleep 0.2
    done
    wait "$pid" 2>/dev/null; local rc=$?
    local verdict=OK
    [ "$rc" = 0 ] || verdict="VOID(exit $rc)"
    [ "$mapped" = yes ] || verdict="VOID(did not map lib$rmw.so)"
    echo "$hwm $rss $verdict"
}

for rep in $(seq 1 "$REPS"); do
    for rmw in rmw_tickle rmw_fastrtps_cpp rmw_cyclonedds_cpp; do
        read -r hwm rss verdict <<<"$(run_one "$rmw" 3)"
        say "$(printf '%-20s rep%s peak_rss=%s kB (%.1f MB) rss_at_end=%s kB %s' \
            "$rmw" "$rep" "$hwm" "$(echo "$hwm/1024" | bc -l)" "$rss" "$verdict")"
    done
done
rm -f "$OUT.node"
say ""
say "=== done $(date -Is) ==="
