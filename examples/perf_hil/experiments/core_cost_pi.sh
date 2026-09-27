#!/usr/bin/env bash
# core_cost_pi.sh - core_cost_ab.sh's A/B, run on a rig Raspberry Pi (WIRE_PLAN.md 8, 2026-09-27): TickLE core's
# own CPU per sample, send and receive, built natively on the Pi at each given commit and run alternately on one
# pinned CPU. The PC bench did not reproduce the Pi clients' +40-50 ns of user time per RELIABLE sample under v10;
# this measures the core alone where the campaign saw it.
#
# Each ref's src/ and include/ go over as `git archive`; the bench and the p1 codec are this checkout's, so every
# arm runs the same harness. A failed build stops the run: a stale binary must never stand in for a ref
# (core_cost_ab.sh learned that the hard way).
#
# The Pi is the rig's, so this runs under the rig lock, hil scope - the campaign uses both Pis, and a bench
# pinned to one of their CPUs mid-campaign would measure the campaign and perturb it.
#
# Usage: core_cost_pi.sh [-r ROUNDS] [-n SAMPLES] [-c CPU] REF...   (BENCH_ARGS as core_cost_ab.sh, e.g. "-R")
#   PI (default the client Pi, 10.1.1.214), OUT (default /tmp/core_cost_pi.txt). PI=local runs every step on this
#   machine instead, through bash rather than ssh and under the box lock - to test the runner off the rig.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PH="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
SCOPE=hil
[ "${PI:-}" = local ] && SCOPE=box
held="RIG_LOCK_HELD_$(echo "$SCOPE" | tr '[:lower:]' '[:upper:]')"
if [ "${!held:-0}" != "1" ]; then
    RIG_LOCK_SCOPE=$SCOPE exec "$PH/rig_lock.sh" "${BASH_SOURCE[0]}" "$@"
fi

ROUNDS=15
SAMPLES=200000
CPU=3
while getopts "r:n:c:" opt; do
    case "$opt" in
    r) ROUNDS="$OPTARG" ;;
    n) SAMPLES="$OPTARG" ;;
    c) CPU="$OPTARG" ;;
    *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ "$#" -ge 1 ] || {
    echo "usage: $0 [-r ROUNDS] [-n SAMPLES] [-c CPU] REF..." >&2
    exit 2
}
PI="${PI:-10.1.1.214}"
SSH_KEY="${SSH_KEY:-$HOME/.ssh/tickle_ci_ed25519}"
OUT="${OUT:-/tmp/core_cost_pi.txt}"
REMOTE=/tmp/core_cost_pi
SSH=(ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=10 "ci@$PI")
[ "$PI" = local ] && SSH=(bash -c)
LOCAL="$(mktemp -d /tmp/core_cost_pi.XXXXXX)"
trap 'rm -rf "$LOCAL"' EXIT

"${SSH[@]}" "rm -rf $REMOTE && mkdir -p $REMOTE/harness"
# BENCH=socket: experiments/core_cost_socket.c instead - the same scheduler-driven RELIABLE loop through the real HAL
# (hal_linux.c, real UDP sockets on the Pi's own interface, WIRE_PLAN.md 8.8). BENCH_BROADCAST is that interface's
# broadcast address (default the rig link's); BENCH_ARGS is the round size (core_cost_socket.c: 256 on the Pi, whose
# socket buffer drops a 512 round).
BENCH="${BENCH:-core}"
BENCH_BROADCAST="${BENCH_BROADCAST:-192.168.10.255}"
tar -C "$REPO/examples/perf_hil" -cf - experiments/core_cost_bench.c experiments/core_cost_socket.c tickle/common/p1 |
    "${SSH[@]}" "tar -C $REMOTE/harness -xf -"

for ref in "$@"; do
    sha="$(git -C "$REPO" rev-parse --short=8 "$ref")"
    git -C "$REPO" archive "$sha" src include | "${SSH[@]}" "mkdir -p $REMOTE/$sha && tar -C $REMOTE/$sha -xf -"
    # Built there, the old binary removed first, and a failure is fatal here.
    if [ "$BENCH" = socket ]; then
        build="gcc -O2 -DNDEBUG -I$sha/include -Iharness/tickle/common/p1 -o bench_$sha \
            harness/experiments/core_cost_socket.c harness/tickle/common/p1/Bench.c $sha/src/tickle.c \
            $sha/src/hal_linux.c $sha/src/encoding.c $sha/src/log.c -lm -lpthread"
    else
        build="gcc -O2 -DNDEBUG -I$sha/include -Iharness/tickle/common/p1 \
            -DTICKLE_C='\"$REMOTE/$sha/src/tickle.c\"' -o bench_$sha harness/experiments/core_cost_bench.c \
            harness/tickle/common/p1/Bench.c $sha/src/encoding.c $sha/src/log.c -lm -lpthread"
    fi
    if ! "${SSH[@]}" "cd $REMOTE && rm -f bench_$sha && $build && test -x bench_$sha" </dev/null; then
        echo "build of $ref ($sha) on $PI failed" >&2
        exit 1
    fi
    echo "$ref $sha" >>"$LOCAL/refs"
done

: >"$OUT"
echo "core_cost_pi $(date -Is) pi=$PI bench=$BENCH rounds=$ROUNDS samples=$SAMPLES cpu=$CPU bench_args=${BENCH_ARGS:-} alternate=${ALTERNATE:-0} refs=$*" >>"$OUT"
# ALTERNATE=1: every second round runs the refs in reverse order, so a drift along a round (the Pi warming, a long
# ref list) lands on every arm alike instead of growing along the list (WIRE_PLAN.md 8.8 follow-up's bisect).
for round in $(seq "$ROUNDS"); do
    order="$LOCAL/refs"
    if [ "${ALTERNATE:-0}" = 1 ] && [ $((round % 2)) = 0 ]; then
        tac "$LOCAL/refs" >"$LOCAL/refs.reversed"
        order="$LOCAL/refs.reversed"
    fi
    while read -r ref sha; do
        # </dev/null: ssh would otherwise read the rest of the refs file this loop is reading, and every round
        # would run the first ref only (2026-09-27, the first Pi run; PI=local cannot show it, bash -c reads nothing).
        line="$("${SSH[@]}" "BENCH_BROADCAST=$BENCH_BROADCAST taskset -c $CPU $REMOTE/bench_$sha $SAMPLES ${BENCH_ARGS:-} 2>/dev/null" </dev/null |
            grep -E '^(RESULT|PUBLISH):' | tr '\n' ' ' || true)"
        [ -n "$line" ] || line="RESULT: failed"
        # A stalled run lost datagrams it never got back and measured only its first round: not a sample.
        case "$line" in *stalled=1*) line="RESULT: failed stalled ${line#RESULT: }" ;; esac
        echo "ref=$ref round=$round $line" >>"$OUT"
    done <"$order"
done

# Paired against the first ref, round by round.
STEPS="${STEPS:-0}" python3 - "$OUT" "$@" <<'EOF'
import re, statistics, sys
path, refs = sys.argv[1], sys.argv[2:]
rows = {}
for line in open(path):
    m = re.match(r"ref=(\S+) round=(\d+) RESULT: (.*)", line)
    if m and "failed" not in m.group(3):
        kv = dict(p.split("=", 1) for p in m.group(3).split() if "=" in p)
        rows.setdefault(int(m.group(2)), {})[m.group(1)] = kv
base = refs[0]
for ref in refs:
    runs = [r[ref] for r in rows.values() if ref in r]
    if not runs:
        print(f"{ref}: no rows")
        continue
    s = statistics.median(float(x["send_ns_per_sample"]) for x in runs)
    v = statistics.median(float(x["recv_ns_per_sample"]) for x in runs)
    line = f"{ref}: n={len(runs)} send median {s:.1f} ns, recv median {v:.1f} ns"
    if ref != base:
        keys = [("send_ns_per_sample", "send"), ("recv_ns_per_sample", "recv")]
        if "send_utime_ns_per_sample" in runs[0]:
            keys += [("send_utime_ns_per_sample", "send utime"), ("send_stime_ns_per_sample", "send stime"),
                     ("recv_utime_ns_per_sample", "recv utime")]
        for key, name in keys:
            d = [float(r[ref][key]) - float(r[base][key]) for r in rows.values() if ref in r and base in r]
            if len(d) > 1:
                line += f"; {name} vs {base} {statistics.mean(d):+.2f} +- {statistics.stdev(d) / len(d) ** 0.5:.2f}"
        if "send_utime_ns_per_sample" in runs[0]:
            # The user/system split moves with the tick that happens to land (seen on the PC: a ref against itself
            # +-140 ns at 2 rounds); their sum is the steadier figure.
            cpu = lambda x: float(x["send_utime_ns_per_sample"]) + float(x["send_stime_ns_per_sample"])
            d = [cpu(r[ref]) - cpu(r[base]) for r in rows.values() if ref in r and base in r]
            if len(d) > 1:
                line += f"; send user+sys vs {base} {statistics.mean(d):+.2f} +- {statistics.stdev(d) / len(d) ** 0.5:.2f}"
    print(line)
# STEPS=1: each ref against the one listed before it, paired by round - where along a commit list a cost appears.
import os
if os.environ.get("STEPS") == "1":
    for prev, ref in zip(refs, refs[1:]):
        for key, name in [("send_ns_per_sample", "send wall"), ("recv_ns_per_sample", "recv wall")]:
            d = [float(r[ref][key]) - float(r[prev][key]) for r in rows.values() if ref in r and prev in r]
            if len(d) > 1:
                print(f"step {prev} -> {ref}: {name} {statistics.mean(d):+.2f} +- {statistics.stdev(d) / len(d) ** 0.5:.2f}")
EOF
echo "PI_DONE"
