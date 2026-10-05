#!/usr/bin/env bash
# Runs rmw_qos_support_probe.py on a rig Pi under each rmw, so COMPARISON rows 42-47 can say whether rmw_zenoh_cpp's
# empty cells are "not measured" or "cannot be expressed". The rig Pis carry ROS 2 jazzy with rmw_zenoh_cpp and
# rmw_zenohd (checked 2026-10-05); this PC's distro has no rmw_zenoh, which is why it runs there.
#
# Reading rule, written before the run and enforced below:
#   CONTROL - rmw_fastrtps_cpp and rmw_cyclonedds_cpp implement all five policies. Each must report create=ok for all
#     six cases and event=ok for the three that have one; if either control refuses anything, the probe is measuring
#     itself, not the rmw, and the run is VOID.
#   Otherwise rmw_zenoh_cpp's (and rmw_tickle's) rows are reported per case as they came back: create refused, event
#     refused, or both ok.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
HOST=${HOST:-10.1.1.213}
K=$HOME/.ssh/tickle_ci_ed25519
OUT=${OUT:-$HOME/rig_results_safe/rmw_qos_support_probe.txt}
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$HOST" "$@"; }
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
router_pid=""
stop_router() {
    [ -z "$router_pid" ] && return 0
    sh_ "case \"\$(readlink /proc/$router_pid/exe 2>/dev/null)\" in */rmw_zenohd) kill -INT $router_pid;; esac" \
        </dev/null >/dev/null 2>&1
    router_pid=""
}
trap stop_router EXIT

say "=== rmw QoS support probe $(date -Is) host=$HOST ==="
sh_ "cat > /tmp/rmw_qos_support_probe.py" <"$REPO/examples/perf_hil/experiments/rmw_qos_support_probe.py" \
    || { say "FATAL copy failed"; exit 1; }
for rmw in rmw_fastrtps_cpp rmw_cyclonedds_cpp rmw_tickle rmw_zenoh_cpp; do
    setup="source /opt/ros/jazzy/setup.bash"
    # rmw_perf_ws is the rig's workspace with tickle typesupport for the interface packages; ~/tickle/install alone has
    # rmw_tickle and none, so every node creation there failed before QoS was asked about (first run, 2026-10-05).
    [ "$rmw" = rmw_tickle ] && setup="$setup && source ~/rmw_perf_ws/install/setup.bash"
    if [ "$rmw" = rmw_zenoh_cpp ]; then
        router_pid=$(sh_ "bash -c '$setup && (setsid sh -c \"echo \\\$\\\$ > /tmp/qos_zenohd.pid; exec ros2 run rmw_zenoh_cpp rmw_zenohd\" > /tmp/qos_zenohd.log 2>&1 < /dev/null &) ; sleep 3; cat /tmp/qos_zenohd.pid'" </dev/null)
        say "  rmw_zenohd started (pid $router_pid)"
    fi
    sh_ "bash -c '$setup && RMW_IMPLEMENTATION=$rmw timeout 60 python3 /tmp/rmw_qos_support_probe.py 2>&1'" </dev/null \
        | sed "s/^/$rmw | /" | tee -a "$OUT" >/dev/null
    stop_router
done
say "=== done $(date -Is) ==="

python3 - "$OUT" <<'PYEOF' | tee -a "$OUT"
import re, sys
rows = {}
for line in open(sys.argv[1]):
    m = re.match(r"(\S+) \|\s+(\S+)\s+create=(.*?)\s+event=(.*)$", line.rstrip())
    if m:
        rows.setdefault(m.group(1), []).append((m.group(2), m.group(3).strip(), m.group(4).strip()))
void = []
for control in ("rmw_fastrtps_cpp", "rmw_cyclonedds_cpp"):
    cases = rows.get(control, [])
    if len(cases) != 6:
        void.append(f"{control}: {len(cases)} cases reported, expected 6")
    for policy, create, event in cases:
        if create != "ok" or event not in ("ok", "-"):
            void.append(f"{control} {policy}: create={create} event={event}")
print()
if void:
    print("VOID - a control refused or did not report, so the probe is measuring itself:")
    for v in void:
        print("  " + v)
    sys.exit(0)
print("CONTROLS HELD: both DDS rmws accepted all six cases and every event.")
for rmw in ("rmw_tickle", "rmw_zenoh_cpp"):
    print(f"{rmw}:")
    for policy, create, event in rows.get(rmw, []):
        print(f"  {policy:<28} create={'ok' if create == 'ok' else 'REFUSED'} event={event if event in ('ok', '-') else 'REFUSED'}")
PYEOF
