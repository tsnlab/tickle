#!/usr/bin/env bash
# bench_sched_check.sh - checks BenchStats.h's per-thread CPU instrument (sched_cpu_s,
# sched_by_thread=, sched_unattributed_s) against two cases it must tell apart. Added 2026-09-28 for
# WIRE_PLAN section 10's open p1 client CPU question: getrusage's utime/stime are tick-accounted, and
# a 0.5-0.7% difference is inside the tick, so the campaign needed an instrument that resolves it and
# says which thread the time went to.
#
# HOW TO READ IT, written before the run:
#   - ARM=live: the worker thread is alive at bench_stats_end. PASS needs sched_threads=2, both
#     threads in sched_by_thread with about the CPU each was asked to burn (0.6 s each), and
#     instrument= WITHOUT schedgap. A fail here means the breakdown cannot attribute a live thread,
#     which is the whole purpose.
#   - ARM=exited: the worker is joined before bench_stats_end, so its CPU belongs to no thread either
#     snapshot can name. PASS needs sched_threads=1, sched_unattributed_s of about 0.6 s, and
#     instrument= WITH schedgap. A clean breakdown here would be the instrument hiding a whole
#     thread - the failure this arm exists to catch, and the reason the field is printed rather than
#     assumed to be zero.
#   - Either arm reporting instrument=...,sched (rather than schedgap) means /proc/self/task was
#     unreadable: the run says nothing about the instrument's accuracy.
#   - net is expected to fail here: this program sends nothing, and the arms are about CPU only.
# Usage: bench_sched_check.sh            both arms on this machine
#        HOST=10.1.1.214 bench_sched_check.sh   both arms on that Pi, under the rig lock
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
HOST=${HOST:-}
if [ -n "$HOST" ]; then
    export RIG_LOCK_SCOPE=hil
    if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
fi
CC_FLAGS="-O2 -Wall -Wextra -I$REPO/examples/perf_hil/tickle/common"
run_local() {
    local bin
    bin=$(mktemp -d)/bench_sched_check
    # shellcheck disable=SC2086 # CC_FLAGS is a deliberate word list
    gcc $CC_FLAGS -o "$bin" "$HERE/bench_sched_check.c" -lpthread || return 1
    for arm in live exited; do ARM=$arm BENCH_IFACE=lo "$bin"; done
}
run_remote() {
    local key=$HOME/.ssh/tickle_ci_ed25519
    local ssh=(ssh -i "$key" -o BatchMode=yes -o ConnectTimeout=10 "ci@$HOST")
    "${ssh[@]}" "mkdir -p /tmp/bench_sched_check && cat > /tmp/bench_sched_check/bench_sched_check.c" \
        <"$HERE/bench_sched_check.c" || return 1
    "${ssh[@]}" "cat > /tmp/bench_sched_check/BenchStats.h" <"$REPO/examples/perf_hil/tickle/common/BenchStats.h" || return 1
    "${ssh[@]}" "cd /tmp/bench_sched_check && gcc -O2 -Wall -Wextra -I. -o bench_sched_check bench_sched_check.c -lpthread" \
        </dev/null || return 1
    for arm in live exited; do
        # taskset: one core, so the two threads' own burn times are not confused by them running at once
        "${ssh[@]}" "cd /tmp/bench_sched_check && ARM=$arm BENCH_IFACE=eth0 taskset -c 1-3 ./bench_sched_check" </dev/null
    done
}
echo "=== bench_sched_check $(date -Is) on ${HOST:-$(hostname)} ==="
if [ -n "$HOST" ]; then run_remote; else run_local; fi
