#!/usr/bin/env bash
# sched_wake_sweep_pc.sh - sched_wake_check --sweep (platform/linux/sched_wake_check.c) against several builds of the
# HAL, in a private network namespace at nice 0: how late a scheduler entry runs after its deadline, per interval, with
# the raw waits (clock_nanosleep, epoll_pwait2/ppoll with a relative timeout, an absolute timerfd) beside it.
#
# Why (2026-10-18): 596e9c9c gave waits longer than 10 ms for an entry the deadline timerfd; a 5 ms ping still left
# ~60 us late. The question is whether a lower threshold (0, 1 ms) brings the short intervals to the timerfd's ~10 us.
#
# HOW TO READ IT - written before the first run:
#   Per arm and interval, the median over ROUNDS of the tickle arm's late_p50_us; the raw arms come from the same runs
#   and are the same code in every build (their spread is the A/A floor of the host).
#   Expected: with threshold T, intervals above T read like timerfd_abs (~10-15 us) and those at or below T like
#   epoll_pwait2_rel (~55-60 us: the 50 us timer slack). CONFIRMED for an interval when the arm's tickle median is
#   within 20 us of the same run's timerfd_abs median while main's is more than 30 us above it; printed per interval.
#   cpu_per_wait_us is printed beside it; a rise above main's by more than 20 us at an interval is flagged CPU_RISE.
#   VOID: a round started above LOAD_MAX (loadavg 1 min) is left out of the medians; an arm with no valid round is VOID.
# Usage: ARMS="label=commit ..." [ROUNDS=3] [CHECK_SRC=<commit with the sweep's sched_wake_check.c>] OUT= WORK=
# Run it detached. The last line is always "sched_wake_sweep_pc done rc=<n>".
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
WORK=${WORK:-/home/semih/tmp_build/sched/sweep}
OUT=${OUT:-$WORK/results.txt}
ROUNDS=${ROUNDS:-3}
LOAD_MAX=${LOAD_MAX:-4}
CHECK_SRC=${CHECK_SRC:-HEAD}
ARMS=${ARMS:?ARMS=\"label=commit ...\"}
NS=swsweep-$$
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}
rc=1
cleanup() { # the one EXIT trap
    sudo -n ip netns del "$NS" 2>/dev/null
    echo "sched_wake_sweep_pc done rc=$rc $(date -Is)"
    return 0
}
trap cleanup EXIT
mkdir -p "$WORK"

declare -A bin
labels=()
for spec in $ARMS; do
    label=${spec%%=*}
    sha=$(git -C "$TREE" rev-parse --short=8 "${spec#*=}") || exit 1
    d=$WORK/src_$sha
    if [ ! -x "$d/platform/linux/sched_wake_check" ]; then
        rm -rf "$d" && mkdir -p "$d"
        git -C "$TREE" archive "$sha" | tar -x -C "$d" || exit 1
        # Every arm runs the same check program; only the library under it differs.
        git -C "$TREE" show "$CHECK_SRC:platform/linux/sched_wake_check.c" >"$d/platform/linux/sched_wake_check.c" || exit 1
        make -C "$d/platform/linux" -j4 sched_wake_check >"$WORK/build_$sha.log" 2>&1 || { tail -5 "$WORK/build_$sha.log"; exit 1; }
    fi
    bin[$label]=$d/platform/linux/sched_wake_check
    labels+=("$label")
    echo "ARM $label=$sha threshold: $(grep -m1 -E '^#define TT_RX_TIMER_LONG_NS' "$d/src/hal_linux.c")" | tee -a "$OUT"
done

sudo -n ip netns add "$NS" || exit 1
sudo -n ip -n "$NS" link set lo up || exit 1
sudo -n ip -n "$NS" route add default dev lo || exit 1
echo "=== sched_wake_sweep_pc $(date -Is) rounds=$ROUNDS load_max=$LOAD_MAX load=$(cut -d' ' -f1-3 /proc/loadavg) ===" |
    tee -a "$OUT"

n=${#labels[@]}
for round in $(seq 1 "$ROUNDS"); do
    for i in $(seq 0 $((n - 1))); do
        label=${labels[$(((i + round - 1) % n))]}
        # QUIET_WAIT=<s>: wait up to that long for the load to fall to LOAD_MAX first; judged on the load at the start.
        waited=0
        while [ "$waited" -lt "${QUIET_WAIT:-0}" ] && awk -v l="$(cut -d' ' -f1 /proc/loadavg)" -v m="$LOAD_MAX" \
            'BEGIN { exit !(l > m) }'; do
            sleep 5
            waited=$((waited + 5))
        done
        load=$(cut -d' ' -f1 /proc/loadavg)
        verdict=ok
        awk -v l="$load" -v m="$LOAD_MAX" 'BEGIN { exit !(l > m) }' && verdict=VOID_LOAD
        # Root inside the namespace; renice to 0 (the session shells run at nice 5).
        sudo -n ip netns exec "$NS" bash -c "renice -n 0 -p \$\$ >/dev/null; ulimit -v 2097152; exec ${bin[$label]} --sweep" \
            </dev/null >"$WORK/run.log" 2>&1
        code=$?
        [ "$code" -eq 0 ] || verdict=VOID_EXIT$code
        # Whole rows only: the context's log lines (stderr) can land inside a buffered stdout row (2026-10-18: one 500 ms
        # row in 45 read interval_ms=5[2026-..., and a nanosleep median of 0).
        grep -E '^sched_wake_check: arm=[a-z_0-9]+ interval_ms=[0-9]+ n=[0-9]+ late_p50_us=[0-9.]+ late_max_us=[0-9.]+ cpu_per_wait_us=[0-9.]+$' \
            "$WORK/run.log" | sed "s/^/ROW label=$label round=$round verdict=$verdict load=$load /" |
            tee -a "$OUT"
    done
done

start=$(grep -n "^=== sched_wake_sweep_pc " "$OUT" | tail -1 | cut -d: -f1)
tail -n +"$start" "$OUT" | awk -v labels="${labels[*]}" '
function val(k,   i, kv) { for (i = 1; i <= NF; i++) { split($i, kv, "="); if (kv[1] == k) return kv[2] } return "" }
function med(key,   c, i, j, t, a) { c = cnt[key]; if (!c) return -1; for (i = 1; i <= c; i++) a[i] = v[key, i]
    for (i = 1; i <= c; i++) for (j = i + 1; j <= c; j++) if (a[j] < a[i]) { t = a[i]; a[i] = a[j]; a[j] = t }
    return a[int((c + 1) / 2)] }
/^ROW / && val("verdict") == "ok" {
    k = val("label") SUBSEP val("arm") SUBSEP val("interval_ms"); cnt[k]++; v[k, cnt[k]] = val("late_p50_us")
    kc = k SUBSEP "cpu"; cnt[kc]++; v[kc, cnt[kc]] = val("cpu_per_wait_us"); iv[val("interval_ms")] = 1
}
END {
    nl = split(labels, L, " "); ni = 0; for (x in iv) I[++ni] = x + 0
    for (i = 1; i <= ni; i++) for (j = i + 1; j <= ni; j++) if (I[j] < I[i]) { t = I[i]; I[i] = I[j]; I[j] = t }
    for (i = 1; i <= ni; i++) {
        ms = I[i]; base = L[1] SUBSEP "tickle" SUBSEP ms
        printf "SUMMARY interval_ms=%d", ms
        for (a = 1; a <= nl; a++) {
            k = L[a] SUBSEP "tickle" SUBSEP ms; tf = med(L[a] SUBSEP "timerfd_abs" SUBSEP ms)
            if (cnt[k] == 0) { printf " | %s VOID", L[a]; continue }
            printf " | %s tickle=%.1f timerfd=%.1f epoll_rel=%.1f nanosleep=%.1f cpu=%.1f n=%d", L[a], med(k), tf,
                med(L[a] SUBSEP "epoll_pwait2_rel" SUBSEP ms), med(L[a] SUBSEP "clock_nanosleep_abs" SUBSEP ms),
                med(k SUBSEP "cpu"), cnt[k]
            if (a > 1 && cnt[base]) {
                tfb = med(L[1] SUBSEP "timerfd_abs" SUBSEP ms)
                printf " -> %s", (med(k) <= tf + 20 && med(base) > tfb + 30) ? "CONFIRMED" : (med(base) <= tfb + 30 ? "NO_DEFECT_ON_MAIN" : "REFUTED")
                if (med(k SUBSEP "cpu") - med(base SUBSEP "cpu") > 20) printf " CPU_RISE"
            }
        }
        printf "\n"
    }
}' | tee -a "$OUT"
rc=0
