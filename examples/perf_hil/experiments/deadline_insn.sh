#!/usr/bin/env bash
# QoS plan D4 hot-path check on this PC: instructions per sample of an rmw pub/sub (deadline_insn.c), with and without
# a DEADLINE, for two rmw_tickle builds - A (the base) and B (the change). Counts are perf stat instructions:u over the
# whole process (every thread), differenced between N1 and N2 samples so setup and teardown cancel. Interleaved
# A0 A1 B0 B1 per round, in one private netns.
#
# Reading rules, enforced below, decided before the run (2026-10-18):
#   VOID  if any run delivered fewer samples than it published, a run loaded a librmw_tickle.so that is not its arm's,
#         a count is missing, or the control is too noisy to decide: A0's rounds (the base without a deadline, which
#         the change cannot touch) spread by more than 5% of their median.
#   PASS  if, against the bound max(1% of A0's median, A0's spread), the change costs nothing without a deadline
#         (|B0 - A0| within the bound) and the deadline costs B nothing per sample (|B1 - B0| within the bound).
#   FAIL  otherwise.
#
# Usage: deadline_insn.sh <A build dir> <A source root> <B build dir> <B source root> [ROUNDS] [N1] [N2]
#   (a build dir is colcon's build/rmw_tickle, holding librmw_tickle.so). ROUNDS 5, N1 2000, N2 12000.
# Output: per-run lines, a median table, and a final "VERDICT=<PASS|FAIL|VOID>".
set -uo pipefail

declare -A BUILD=([A]=$1 [B]=$3) SRC=([A]=$2 [B]=$4)
ROUNDS=${5:-5} N1=${6:-2000} N2=${7:-12000}
DEADLINE_MS=10000 # far longer than a run: the check never fires, so only per-sample work is measured
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/deadline_insn.XXXXXX")
NS="dlinsn$$"
cleanup() {
    sudo -n ip netns del "$NS" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT # the only EXIT trap here

ROS=/opt/ros/lyrical
TS=$(dirname "$(dirname "$(find /home/semih/rmw_loans_ws/rmw/install/rosidl_typesupport_tickle_c/lib \
    -name 'librosidl_typesupport_tickle_c.so' -print -quit)")")
for arm in A B; do
    build=${BUILD[$arm]} src=${SRC[$arm]}
    # DT_RPATH (not RUNPATH) so LD_LIBRARY_PATH cannot put another rmw_tickle ahead of this arm's.
    cc -O2 -Wl,--disable-new-dtags -o "$WORK/$arm.bin" "$HERE/deadline_insn.c" \
        -I"$src/include" -I"$src/rmw_tickle/rmw_tickle/include" -I"$TS/include" \
        -I"$ROS/include/rcutils" -I"$ROS/include/rmw" -I"$ROS/include/rosidl_runtime_c" \
        -I"$ROS/include/rosidl_typesupport_interface" -I"$ROS/include/rosidl_dynamic_typesupport" \
        -I"$ROS/include/rosidl_buffer" \
        -Wl,-rpath,"$build:$ROS/lib:$TS/lib" -L"$build" -L"$ROS/lib" -L"$TS/lib" \
        -lrmw_tickle -lrmw -lrcutils -lrosidl_runtime_c -lrosidl_typesupport_tickle_c -ldl \
        || { echo "deadline_insn: build of arm $arm failed"; echo "VERDICT=VOID"; exit 1; }
done

if ! sudo -n ip netns add "$NS" || ! sudo -n ip netns exec "$NS" ip link set lo up \
    || ! sudo -n ip netns exec "$NS" ip route add default dev lo; then
    echo "deadline_insn: no private netns"
    echo "VERDICT=VOID"
    exit 1
fi
in_ns() { sudo -n ip netns exec "$NS" sudo -n -u "$USER" "$@"; }
# shellcheck disable=SC2016  # expanded by the inner shell, inside the netns
in_ns bash -c '[ "$(ip -o addr show | grep -c 10\\.1\\.1\\.)" = 0 ]' \
    || { echo "deadline_insn: management LAN visible in the netns"; echo "VERDICT=VOID"; exit 1; }

# One run: prints "<arm> <deadline> <n> <instructions>", or VOID with the reason.
run() {
    local arm=$1 deadline=$2 n=$3
    local out
    out=$(in_ns nice -n 19 perf stat -x, -e instructions:u -o "$WORK/stat" "$WORK/$arm.bin" "$n" "$deadline" 2>&1)
    local insn
    insn=$(sed -n 's/^\([0-9][0-9]*\),.*instructions.*/\1/p' "$WORK/stat")
    if ! grep -qF "librmw_tickle=${BUILD[$arm]}/librmw_tickle.so" <<<"$out"; then
        echo "VOID $arm $deadline $n wrong library: $(grep -o 'librmw_tickle=[^ ]*' <<<"$out")"
    elif ! grep -qE "samples=$n delivered=$n " <<<"$out"; then
        echo "VOID $arm $deadline $n not every sample delivered: $(grep -o 'samples=.*' <<<"$out")"
    elif [ -z "$insn" ]; then
        echo "VOID $arm $deadline $n no instruction count"
    else
        echo "$arm $deadline $n $insn"
    fi
}

uptime
: >"$WORK/all"
for round in $(seq 1 "$ROUNDS"); do
    for cell in "A 0" "A $DEADLINE_MS" "B 0" "B $DEADLINE_MS"; do
        read -r arm deadline <<<"$cell"
        for n in "$N1" "$N2"; do
            run "$arm" "$deadline" "$n" | sed "s/^/round $round /" | tee -a "$WORK/all"
        done
    done
done
uptime

python3 -I - "$WORK/all" "$N1" "$N2" "$DEADLINE_MS" <<'EOF'
import statistics, sys
path, n1, n2, dl = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
rows = [l.split() for l in open(path)]
if any(r[2] == "VOID" for r in rows):
    print("deadline_insn: a run was VOID (above)"); print("VERDICT=VOID"); sys.exit()
count = {}
for r in rows:
    count[(r[1], r[2], r[3], r[4])] = int(r[5])
per = {}
for (rnd, arm, d, n), v in count.items():
    if n != str(n1):
        continue
    hi = count.get((rnd, arm, d, str(n2)))
    if hi is None:
        print("deadline_insn: missing count"); print("VERDICT=VOID"); sys.exit()
    per.setdefault((arm, "0" if d == "0" else "D"), []).append((hi - v) / (n2 - n1))
cells = [("A", "0"), ("A", "D"), ("B", "0"), ("B", "D")]
med = {c: statistics.median(per[c]) for c in cells}
spread = {c: max(per[c]) - min(per[c]) for c in cells}
print(f"{'arm':4} {'deadline':>9} {'median insn/sample':>19} {'spread':>8} {'rounds':>7}")
for c in cells:
    print(f"{c[0]:4} {('none' if c[1] == '0' else dl + ' ms'):>9} {med[c]:19.1f} {spread[c]:8.1f} {len(per[c]):7d}")
a0 = med[("A", "0")]
if spread[("A", "0")] > 0.05 * a0:
    print("deadline_insn: control A0 spread above 5% of its median"); print("VERDICT=VOID"); sys.exit()
bound = max(0.01 * a0, spread[("A", "0")])
d_change = med[("B", "0")] - a0
d_deadline = med[("B", "D")] - med[("B", "0")]
print(f"bound {bound:.1f}  B0-A0 {d_change:+.1f}  B1-B0 {d_deadline:+.1f}  (A1-A0 {med[('A', 'D')] - a0:+.1f})")
print("VERDICT=" + ("PASS" if abs(d_change) <= bound and abs(d_deadline) <= bound else "FAIL"))
EOF
