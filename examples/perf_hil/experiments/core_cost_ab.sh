#!/usr/bin/env bash
# core_cost_ab.sh - core_cost_bench.c built at each given commit (or "WORKTREE" for this checkout's working
# tree) and run alternately on one pinned CPU: A B C .. A B C .. for ROUNDS rounds (WIRE_PLAN.md 8,
# 2026-09-27). No kernel and no network: TickLE core's own CPU per sample, send and receive, and how many
# clock reads each makes. PC-only, so it needs no rig lock.
#
# Usage: core_cost_ab.sh [-r ROUNDS] [-n SAMPLES] [-c CPU] REF... ; results to OUT (default
# /tmp/core_cost_ab.txt), one RESULT line per run prefixed with the ref and round. BENCH_ARGS passes the
# bench's own flags to every run: -c (the clients' scheduler-driven send), -w N writers, -D, -e N, and -p
# (a publishing thread on the draining node, whose call latency is summarised too - give -c two CPUs,
# e.g. -c 14,15).
#
# Reading it: per ref, the median of send_ns_per_sample and recv_ns_per_sample over the rounds, and the
# clock reads per sample (exact, not timed). A difference is real when it exceeds the rounds' spread
# (max - min) of both refs; the clock-read counts say how much of it is clock_gettime().
set -euo pipefail

ROUNDS=15
SAMPLES=200000
CPU=15
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

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
WORK="${WORK:-/tmp/core_cost_ab}"
OUT="${OUT:-/tmp/core_cost_ab.txt}"
SHAPE="$REPO/examples/perf_hil/tickle/common/p1"
mkdir -p "$WORK"

build() { # ref -> binary path
    local ref="$1" tree bin
    if [ "$ref" = WORKTREE ]; then
        tree="$REPO"
        bin="$WORK/bench_WORKTREE"
    else
        tree="$WORK/tree_$ref"
        bin="$WORK/bench_$ref"
        if [ ! -d "$tree" ]; then
            git -C "$REPO" worktree add --detach "$tree" "$ref" >/dev/null 2>&1
        fi
    fi
    # The bench and the p1 codec come from this checkout, so every arm runs the same harness; only
    # tickle.c and its headers differ.
    # BENCH_FROM_REF=1 takes each arm's bench from its own tree instead, for a pair across an API rename that the
    # one bench cannot compile against both sides of (CONTEXT_NODE_PLAN.md stage 1, 2026-09-27). It is the same
    # harness only if the two bench sources differ by the rename alone - check that before reading the result.
    local bench="$HERE/core_cost_bench.c"
    [ "${BENCH_FROM_REF:-0}" = 1 ] && bench="$tree/examples/perf_hil/experiments/core_cost_bench.c"
    # The old binary goes first: a failed build must not leave the last good one to be measured in its
    # place. That happened on 2026-09-27 - build() runs in a command substitution, where set -e does not
    # reach, and a compile error left D3's binary standing in for D2's.
    rm -f "$bin"
    # BENCH_CFLAGS: compile-time settings applied to every arm alike, e.g. WIRE_PLAN.md 9.1's 32-writer builds.
    # shellcheck disable=SC2086 # a list of flags
    if ! gcc -O2 -DNDEBUG ${BENCH_CFLAGS:-} -I"$tree/include" -I"$SHAPE" -DTICKLE_C="\"$tree/src/tickle.c\"" \
        -o "$bin" "$bench" "$SHAPE/Bench.c" "$tree/src/encoding.c" "$tree/src/log.c" -lm -lpthread >&2; then
        echo "build of $ref failed" >&2
        return 1
    fi
    echo "$bin"
}

declare -A BIN
for ref in "$@"; do
    BIN[$ref]="$(build "$ref")" || exit 1
    [ -x "${BIN[$ref]}" ] || exit 1
done

: >"$OUT"
echo "core_cost_ab $(date -Is) rounds=$ROUNDS samples=$SAMPLES cpu=$CPU bench_args=${BENCH_ARGS:-} bench_cflags=${BENCH_CFLAGS:-} bench_from_ref=${BENCH_FROM_REF:-0} refs=$*" >>"$OUT"
for round in $(seq "$ROUNDS"); do
    for ref in "$@"; do
        # shellcheck disable=SC2086 # BENCH_ARGS is a list of the bench's own flags
        line="$(taskset -c "$CPU" "${BIN[$ref]}" "$SAMPLES" ${BENCH_ARGS:-} 2>/dev/null |
            grep -E '^(RESULT|PUBLISH|W1):' | tr '\n' ' ' || echo "RESULT: failed")"
        [ -n "$line" ] || line="RESULT: failed"
        echo "ref=$ref round=$round $line" >>"$OUT"
    done
done

python3 - "$OUT" "$@" <<'EOF'
import re, statistics, sys
path, refs = sys.argv[1], sys.argv[2:]
rows = {}
for line in open(path):
    m = re.match(r"ref=(\S+) round=\d+ RESULT: (.*)", line)
    if not m or "failed" in m.group(2):
        continue
    kv = dict(p.split("=", 1) for p in m.group(2).split() if "=" in p)
    rows.setdefault(m.group(1), []).append(kv)
for ref in refs:
    r = rows.get(ref, [])
    if not r:
        print(f"{ref}: no rows"); continue
    s = [float(x["send_ns_per_sample"]) for x in r]
    v = [float(x["recv_ns_per_sample"]) for x in r]
    print(f"{ref}: n={len(r)} send median {statistics.median(s):.1f} ns (spread {max(s)-min(s):.1f}) "
          f"recv median {statistics.median(v):.1f} ns (spread {max(v)-min(v):.1f}) "
          f"clock/sample send {r[0]['send_clock_per_sample']} recv {r[0]['recv_clock_per_sample']} "
          f"datagrams {r[0]['datagrams']} v{r[0]['tt_version']}")
    if "p99_ns" in r[0]:
        p50 = [float(x["p50_ns"]) for x in r]
        p99 = [float(x["p99_ns"]) for x in r]
        print(f"{ref}: publish p50 median {statistics.median(p50):.0f} ns, p99 median {statistics.median(p99):.0f} ns "
              f"(max of p99 {max(p99):.0f})")
EOF
echo "AB_DONE"
