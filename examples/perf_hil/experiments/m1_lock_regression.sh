#!/usr/bin/env bash
# M1: what do core's new locks cost? Pre-registered before db0f8be3 existed.
#   baseline = 97e1a278 (core byte-identical to e670caa5, harness identical too)
#   locked   = db0f8be3
#
# A-B-A, baseline -> locked -> baseline, because for a change whose expected answer is "almost
# nothing" a drift in the session manufactures a finding rather than hiding one. If BASE2 differs
# from BASE1 outside its spread the session drifted and the comparison is void, not negative.
#
# HOW TO READ IT, as pre-registered:
#   PASS  every metric's ranges overlap between baseline and locked.
#   FAIL  any metric outside the spread.
#   Read in ABSOLUTE terms as well as percent: at 5.76 us per sample, 1% is 58 ns, and one
#   uncontended pthread_mutex pair measured 12.0 ns on this hardware. So 20-60 ns of added per-sample
#   CPU is "locks cost what locks cost"; 500 ns is something held across work. TickLE Dev counts about
#   4 pairs on the max-rate publisher path (run_due_entry's state+sched, publish's recursive state,
#   the self-reschedule's sched), so ~50 ns is the predicted figure.
#   THE DECISIVE METRIC IS LATENCY CPU, NOT THROUGHPUT. reliable_latency's cpu_s_per_Msample reads 157
#   against CycloneDDS's 230 - a win that did not exist this morning. If locking pushes it past 230,
#   thread safety was bought with the newest result, and that is the headline rather than a footnote.
set -uo pipefail
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
PIN="taskset -c 1-3"; REPS=${REPS:-3}
BASE=97e1a278; LOCKED=db0f8be3
CYCLONE_LATENCY_CPU=230
OUT=${OUT:-/tmp/m1_locks.txt}
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$1" "${@:2}"; }
srv_pids() {
    # shellcheck disable=SC2016
    sh_ "$SERVER" 'for p in $(ls /proc | grep -E "^[0-9]+$"); do
        e=$(readlink /proc/$p/exe 2>/dev/null)
        case "$e" in */perf_hil/tickle/*/server) echo "$p";; esac; done'
}
kill_srv() { local p; p=$(srv_pids); [ -z "$p" ] && return 0
    # shellcheck disable=SC2086
    sh_ "$SERVER" "kill -INT $p" >/dev/null 2>&1 || true; sleep 2; }
trap kill_srv EXIT
: > "$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== M1 lock regression, $(date -Is) ==="
say "baseline=$BASE  locked=$LOCKED  (CycloneDDS latency CPU reference: $CYCLONE_LATENCY_CPU)"
build() {
    local sha="$1" pids=() bad=0
    say ""; say "--- building $sha ---"
    for h in "$CLIENT" "$SERVER"; do
        sh_ "$h" "set -u
cd ~/tickle && git fetch -q origin && git reset -q --hard $sha && git clean -fdqx
cd examples/perf_hil/tickle
for v in 'reliable_throughput p1' 'reliable_throughput p2' 'reliable_latency p1'; do
  # shellcheck disable=SC2086
  ./build.sh \$v > /tmp/m1b.log 2>&1 || { echo \"BUILD FAILED \$v on \$(hostname)\"; tail -5 /tmp/m1b.log; exit 1; }
done
for d in reliable_throughput_p1 reliable_throughput_p2 reliable_latency_p1; do
  { [ -x \$d/client ] && [ -x \$d/server ]; } || { echo \"MISSING \$d on \$(hostname)\"; exit 1; }
done
echo \"\$(hostname) at \$(git rev-parse --short HEAD)\"" &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p" || bad=1; done
    [ "$bad" = 0 ] || { say "BUILD FAILED - stopping"; exit 1; }
    say "  locks in core: $(sh_ "$CLIENT" "grep -c 'tt_lock_t\|state_lock' ~/tickle/src/tickle.c 2>/dev/null || echo 0" | head -1)"
}
run1() {   # $1 dir  $2 args
    kill_srv; [ -z "$(srv_pids)" ] || { echo "ABORT-server"; return; }
    sh_ "$SERVER" "cd ~/tickle/examples/perf_hil/tickle/$1; nohup $PIN ./server $2 > /tmp/m1_srv.log 2>&1 < /dev/null &"
    sleep 3
    [ "$(srv_pids | wc -l)" = 1 ] || { echo "ABORT-nserver"; return; }
    local cl; cl=$(sh_ "$CLIENT" "cd ~/tickle/examples/perf_hil/tickle/$1 && $PIN ./client $2" | grep -m1 '^RESULT:')
    kill_srv
    echo "${cl:-NO-RESULT}"
}
arm() {
    local tag="$1" r
    for spec in "reliable_throughput_p1|-Q -d 5" "reliable_throughput_p2|-Q -d 5" "reliable_latency_p1|-i 0.1 -d 10"; do
        d=${spec%%|*}; a=${spec##*|}
        for r in $(seq 1 "$REPS"); do
            say "$tag $d rep$r | $(run1 "$d" "$a" | grep -oE '(sent|recv|send_mbps|cpu_s_per_Msample|peak_rss_kb|rtt_avg_ms|rtt_max_ms|write_fail)=[0-9.]+' | tr '\n' ' ')"
        done
    done
}
build "$BASE";   arm "BASE1"
build "$LOCKED"; arm "LOCK "
build "$BASE";   arm "BASE2"
say ""; say "=== done ==="
