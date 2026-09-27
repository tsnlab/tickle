#!/usr/bin/env bash
# copy_cost_pi.sh - rmw_tickle/RMW_PERF_PLAN.md 11 M-b and M-c on the rig's client Pi (2026-09-27): copy_cost.c built
# natively there and run pinned, under the rig lock (hil scope, taken inside), 20 rounds. Per round, every M-b field
# size (the two arms' order alternating by round) and the two M-c copy sizes. A summary, paired by round, at the end.
#
# Usage: copy_cost_pi.sh    OUT (default /tmp/copy_cost_pi.txt), "COPY_COST_DONE" at the end. ~10 min.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PH="$(cd "$HERE/.." && pwd)"
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then
    RIG_LOCK_SCOPE=hil exec "$PH/rig_lock.sh" "${BASH_SOURCE[0]}" "$@"
fi
PI="${PI:-10.1.1.214}"
CPU="${CPU:-3}"
ROUNDS="${ROUNDS:-20}"
OPS="${OPS:-100000}"
COPIES="${COPIES:-1000000}"
OUT="${OUT:-/tmp/copy_cost_pi.txt}"
SSH=(ssh -i "${SSH_KEY:-$HOME/.ssh/tickle_ci_ed25519}" -o BatchMode=yes -o ConnectTimeout=10 "ci@$PI")
"${SSH[@]}" "mkdir -p /tmp/copy_cost && cat > /tmp/copy_cost/copy_cost.c" <"$HERE/copy_cost.c"
"${SSH[@]}" "cd /tmp/copy_cost && rm -f copy_cost && gcc -O2 -o copy_cost copy_cost.c && test -x copy_cost" </dev/null
: >"$OUT"
echo "copy_cost_pi $(date -Is) pi=$PI cpu=$CPU rounds=$ROUNDS ops=$OPS copies=$COPIES" >>"$OUT"
for round in $(seq "$ROUNDS"); do
    order=""
    [ $((round % 2)) = 0 ] && order=iovec-first
    for size in 64 256 1024 4096 16384 61440; do
        line=$("${SSH[@]}" "taskset -c $CPU /tmp/copy_cost/copy_cost iovec $size $OPS $order" </dev/null | grep '^RESULT' || echo "RESULT: failed size=$size")
        echo "round=$round $line" >>"$OUT"
    done
    for size in 96 1472; do
        line=$("${SSH[@]}" "taskset -c $CPU /tmp/copy_cost/copy_cost copy $size $COPIES" </dev/null | grep '^RESULT' || echo "RESULT: failed size=$size")
        echo "round=$round $line" >>"$OUT"
    done
done
summary=$(python3 - "$OUT" <<'PY'

import re, statistics, sys
iov, wall, cp = {}, {}, {}
for line in open(sys.argv[1]):
    kv = dict(re.findall(r"(\w+)=([\w.-]+)", line))
    if kv.get("mode") == "iovec":
        iov.setdefault(int(kv["size"]), []).append(float(kv["ns_per_op_copy"]) - float(kv["ns_per_op_iovec"]))
        wall.setdefault(int(kv["size"]), []).append(float(kv["wall_ns_per_op_copy"]) - float(kv["wall_ns_per_op_iovec"]))
    elif kv.get("mode") == "copy":
        cp.setdefault(int(kv["size"]), []).append(float(kv["ns_per_copy"]))
for size, d in sorted(iov.items()):
    se = statistics.stdev(d) / len(d) ** 0.5 if len(d) > 1 else 0
    w = wall[size]
    wse = statistics.stdev(w) / len(w) ** 0.5 if len(w) > 1 else 0
    print(f"SUMMARY iovec size={size} copy_minus_iovec_cpu_ns={statistics.mean(d):+.1f} se={se:.1f} "
          f"wall_ns={statistics.mean(w):+.1f} se={wse:.1f} n={len(d)}")
for size, v in sorted(cp.items()):
    print(f"SUMMARY copy size={size} ns_per_copy_median={statistics.median(v):.2f} n={len(v)}")
PY
)
echo "$summary" >>"$OUT"
echo "COPY_COST_DONE" >>"$OUT"
