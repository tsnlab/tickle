#!/usr/bin/env bash
# core_cost_ab.sh - core_cost_bench.c built at each given commit (or "WORKTREE" for this checkout's working
# tree) and run alternately on one pinned CPU: A B C .. A B C .. for ROUNDS rounds (WIRE_PLAN.md 8,
# 2026-09-27). No kernel and no network: TickLE core's own CPU per sample, send and receive, and how many
# clock reads each makes. PC-only, so it needs no rig lock.
#
# Usage: core_cost_ab.sh [-r ROUNDS] [-n SAMPLES] [-c CPU] REF... ; results to OUT (default
# /tmp/core_cost_ab.txt), one RESULT line per run prefixed with the ref and round.
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
    gcc -O2 -DNDEBUG -I"$tree/include" -I"$SHAPE" -DTICKLE_C="\"$tree/src/tickle.c\"" \
        -o "$bin" "$HERE/core_cost_bench.c" "$SHAPE/Bench.c" "$tree/src/encoding.c" "$tree/src/log.c" -lm
    echo "$bin"
}

declare -A BIN
for ref in "$@"; do
    BIN[$ref]="$(build "$ref")"
done

: >"$OUT"
echo "core_cost_ab $(date -Is) rounds=$ROUNDS samples=$SAMPLES cpu=$CPU refs=$*" >>"$OUT"
for round in $(seq "$ROUNDS"); do
    for ref in "$@"; do
        line="$(taskset -c "$CPU" "${BIN[$ref]}" "$SAMPLES" 2>/dev/null | grep '^RESULT' || echo "RESULT: failed")"
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
    kv = dict(p.split("=") for p in m.group(2).split())
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
EOF
echo "AB_DONE"
