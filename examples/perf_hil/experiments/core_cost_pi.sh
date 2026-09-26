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
tar -C "$REPO/examples/perf_hil" -cf - experiments/core_cost_bench.c tickle/common/p1 |
    "${SSH[@]}" "tar -C $REMOTE/harness -xf -"

for ref in "$@"; do
    sha="$(git -C "$REPO" rev-parse --short=8 "$ref")"
    git -C "$REPO" archive "$sha" src include | "${SSH[@]}" "mkdir -p $REMOTE/$sha && tar -C $REMOTE/$sha -xf -"
    # Built there, the old binary removed first, and a failure is fatal here.
    if ! "${SSH[@]}" "cd $REMOTE && rm -f bench_$sha && gcc -O2 -DNDEBUG -I$sha/include -Iharness/tickle/common/p1 \
            -DTICKLE_C='\"$REMOTE/$sha/src/tickle.c\"' -o bench_$sha harness/experiments/core_cost_bench.c \
            harness/tickle/common/p1/Bench.c $sha/src/encoding.c $sha/src/log.c -lm -lpthread && test -x bench_$sha"; then
        echo "build of $ref ($sha) on $PI failed" >&2
        exit 1
    fi
    echo "$ref $sha" >>"$LOCAL/refs"
done

: >"$OUT"
echo "core_cost_pi $(date -Is) pi=$PI rounds=$ROUNDS samples=$SAMPLES cpu=$CPU bench_args=${BENCH_ARGS:-} refs=$*" >>"$OUT"
for round in $(seq "$ROUNDS"); do
    while read -r ref sha; do
        # </dev/null: ssh would otherwise read the rest of the refs file this loop is reading, and every round
        # would run the first ref only (2026-09-27, the first Pi run; PI=local cannot show it, bash -c reads nothing).
        line="$("${SSH[@]}" "taskset -c $CPU $REMOTE/bench_$sha $SAMPLES ${BENCH_ARGS:-} 2>/dev/null" </dev/null |
            grep -E '^(RESULT|PUBLISH):' | tr '\n' ' ' || true)"
        [ -n "$line" ] || line="RESULT: failed"
        echo "ref=$ref round=$round $line" >>"$OUT"
    done <"$LOCAL/refs"
done

# Paired against the first ref, round by round.
python3 - "$OUT" "$@" <<'EOF'
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
        for key, name in (("send_ns_per_sample", "send"), ("recv_ns_per_sample", "recv")):
            d = [float(r[ref][key]) - float(r[base][key]) for r in rows.values() if ref in r and base in r]
            if len(d) > 1:
                line += f"; {name} vs {base} {statistics.mean(d):+.2f} +- {statistics.stdev(d) / len(d) ** 0.5:.2f}"
    print(line)
EOF
echo "PI_DONE"
