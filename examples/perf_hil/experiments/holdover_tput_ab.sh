#!/usr/bin/env bash
# holdover_tput_ab.sh - PC A/B of rmw_tickle same-host tput cells (holdover_tput_pc.sh), arms interleaved rep by rep,
# then one line per run of what the subscriber did. Written 2026-10-20 for the hold-over's Array4k BEST_EFFORT
# regression on the rig (ab/rmw-wake-holdover); it reads, it does not judge.
#
# Usage: holdover_tput_ab.sh OUT_DIR MSG QOS REPS ARM...      ARM = name:pc_preflight_build_dir[:ENV=VALUE]
#        holdover_tput_ab.sh --read OUT_DIR                   the per-run lines again
# PERF_REC=stat adds perf stat counts of the subscriber's getsockopt() and timerfd_settime() over 3 s of the run.
# Per run: rate (the median of the subscriber's per-second received, first and last dropped; n rows), delivered (core
# into rmw), waits (rmw_wait() polls), holdovers, skipped (superseded in the ring), missed (hybrid missed_samples), the
# publisher's sent/s median, and the two system call counts. The last line is always "HOLDOVER_TPUT_AB done".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

field() { grep -m1 -oE "$1=[0-9]+" "$2" 2>/dev/null | cut -d= -f2; }
median_col() { # file column: median of the positive per-second values, the first and last row dropped
    grep -E '^[0-9]' "$1" | awk -F',' -v c="$2" '{gsub(/[\t ]/, "", $c); if ($c > 0) print $c}' | sed '1d;$d' |
        sort -n | awk '{a[NR] = $1} END {if (NR) print a[int((NR + 1) / 2)] " n=" NR; else print "none n=0"}'
}
read_runs() {
    local d
    for d in "$1"/*/; do
        d=${d%/}
        [ -f "$d/sub.log" ] || continue
        echo "$(basename "$d") rate=$(median_col "$d/sub.log" 3) pub=$(median_col "$d/pub.log" 4 | cut -d' ' -f1)" \
            "delivered=$(grep -oE 'delivery: delivered=[1-9][0-9]*' "$d/sub.log" | head -1 | cut -d= -f2)" \
            "waits=$(field executor_poll_waits "$d/sub.log") holdovers=$(field ' holdovers' "$d/sub.log")" \
            "skipped=$(field rx_shm_skipped_superseded "$d/sub.log") missed=$(field missed_samples "$d/sub.log")" \
            "getsockopt=$(awk '/sys_enter_getsockopt/ {print $1}' "$d/perf_stat.log" 2>/dev/null)" \
            "timerfd_settime=$(awk '/sys_enter_timerfd_settime/ {print $1}' "$d/perf_stat.log" 2>/dev/null)"
    done
}

if [ "${1:-}" = --read ]; then
    read_runs "$2"
    echo "HOLDOVER_TPUT_AB done"
    exit 0
fi
OUT=$1 MSG=$2 QOS=$3 REPS=$4
shift 4
mkdir -p "$OUT"
for r in $(seq 1 "$REPS"); do
    for arm in "$@"; do
        IFS=: read -r name dir envv <<<"$arm"
        if [ -n "$envv" ]; then
            "$HERE/holdover_tput_pc.sh" "$OUT/${name}_r$r" "$dir/install" "$dir/perf/install" "$MSG" "$QOS" 6 "$envv"
        else
            "$HERE/holdover_tput_pc.sh" "$OUT/${name}_r$r" "$dir/install" "$dir/perf/install" "$MSG" "$QOS" 6
        fi
    done
done
read_runs "$OUT"
echo "HOLDOVER_TPUT_AB done"
