#!/usr/bin/env bash
# Does the latency drift scale with the idle gap between pings? Of the explanations on the table,
# only DVFS predicts that - so the spacing contrast tests the mechanism rather than its symptoms.
#
# Background: my earlier poll A/B accidentally ran the latency client at its DEFAULT spacing
# (interval_s=1.0, so -d 5 gives 5 pings a second apart) while the campaign's latency cells pass
# "-i 0.1 -d 10" (100 pings, 100 ms apart). One second of idle is a far harsher DVFS test than
# 100 ms, and the drift appeared when the spacing changed, not when the poll did. Both readings can
# be right for different idle intervals.
#
# HOW TO READ IT, all written before running:
#   DOSE: if the mean shift and the >1.0 ms excursions appear at 1 s spacing and not at 0.1 s, that
#     is DVFS almost on its own - nothing else on the table scales with idle time.
#   OLD BUILD CONTROL: the old poll wakes ~10,000 times a second between pings, so its core never
#     gets idle time to down-clock. It must therefore show NO spacing effect at either spacing. If
#     it does, part of the effect is not from the poll change at all.
#   CLOCK: in the new build, cpu_mhz_mean and cpu_mhz_at_rtt_max must sit lower at 1 s than at
#     0.1 s, and lower than the old build at the same spacing. An effect without a lower clock
#     reading rules DVFS out whatever the RTT does, and points at the new wait path.
#   RECOVERIES: an excursion with retransmitted > 0 is a loss event, not a latency property. Counted
#     and reported separately, in whichever arm it lands.
#   WHAT WOULD CONVICT THE CODE: the effect is as strong at 0.1 s as at 1 s AND the clock readings
#     do not separate.
#   THRESHOLD: 1.0 ms, fixed here before any data - 3x the worst value either build produced across
#     twenty earlier runs, and well below the 3.029 ms event.
#   SENT GATE (TickLE Dev): every latency line must satisfy sent == round(duration/interval). A run
#     that does not is VOID with a reason, rather than a number someone has to notice. This is the
#     check that would have caught the mistake above the first time instead of the tenth.
#   IDENTITY: one straced run per (build, spacing). The builds differ by three orders of magnitude
#     in ppoll count, so a count in the wrong decade means the wrong tree was built.
set -uo pipefail
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
PIN="taskset -c 1-3"; RUNS=${RUNS:-20}
OUT=${OUT:-/tmp/rtt_dose.txt}
HARNESS=4baa3b76      # instruments live here
OLDCORE=9a230a1b      # core files from before the poll change
NEWSHA=$(git -C "$(dirname "${BASH_SOURCE[0]}")/../../.." rev-parse --short origin/main)
CORE_FILES="src/tickle.c src/hal_linux.c include/tickle/config.h include/tickle/tickle.h"
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=8 "ci@$1" "${@:2}"; }
server_pids() {
    # shellcheck disable=SC2016  # $p must expand on the rig
    sh_ "$SERVER" 'for p in $(ls /proc | grep -E "^[0-9]+$"); do
        e=$(readlink /proc/$p/exe 2>/dev/null)
        case "$e" in */perf_hil/tickle/*/server) echo "$p";; esac; done'
}
kill_servers() { local p; p=$(server_pids); [ -z "$p" ] && return 0
    # shellcheck disable=SC2086
    sh_ "$SERVER" "kill -INT $p" >/dev/null 2>&1 || true; sleep 2; }
trap kill_servers EXIT
: > "$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== RTT spacing dose-response, $(date -Is) ==="
say "harness=$HARNESS  oldcore=$OLDCORE  new=$NEWSHA  runs=$RUNS per combination"

build() {   # $1 = old | new
    local which="$1" pids=() bad=0 recipe
    if [ "$which" = old ]; then
        recipe="git checkout -q $HARNESS && git checkout -q $OLDCORE -- $CORE_FILES"
    else
        recipe="git checkout -q $NEWSHA"
    fi
    say ""; say "--- building $which ---"
    for h in "$CLIENT" "$SERVER"; do
        sh_ "$h" "set -e
cd ~/tickle && git fetch -q origin && git reset -q --hard origin/main && git clean -fdq && $recipe
cd examples/perf_hil/tickle && ./build.sh reliable_latency p1 > /tmp/dose_build.log 2>&1 \
  || { echo \"BUILD FAILED on \$(hostname)\"; tail -8 /tmp/dose_build.log; exit 1; }" &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p" || bad=1; done
    [ "$bad" = 0 ] || { say "BUILD FAILED - stopping"; exit 1; }
    # does the core carry the poll change?
    local has; has=$(sh_ "$CLIENT" "grep -c until_next_event ~/tickle/src/tickle.c 2>/dev/null || echo 0")
    say "  core has until_next_event: $has  (expect 0 for old, >0 for new)"
}
one_run() {   # $1 args  $2 strace(0|1)
    local dir="tickle/examples/perf_hil/tickle/reliable_latency_p1"
    kill_servers; [ -z "$(server_pids)" ] || { echo "ABORT-server"; return; }
    sh_ "$SERVER" "cd ~/$dir; nohup $PIN ./server $1 > /tmp/dose_srv.log 2>&1 < /dev/null &"
    sleep 3
    [ "$(server_pids | wc -l)" = 1 ] || { echo "ABORT-nserver"; return; }
    local cmd="cd ~/$dir && $PIN ./client $1"
    [ "$2" = 1 ] && cmd="cd ~/$dir && strace -c -f -o /tmp/dose_st.txt $PIN ./client $1"
    local o; o=$(sh_ "$CLIENT" "$cmd" 2>&1 | grep -m1 '^RESULT:')
    local pp=""; [ "$2" = 1 ] && pp=$(sh_ "$CLIENT" "awk '/ ppoll\$/{print \$4}' /tmp/dose_st.txt")
    kill_servers
    echo "$o|ppoll=${pp:-na}"
}
combo() {   # $1 tag  $2 args  $3 expected_sent
    local tag="$1" args="$2" want="$3" r line got
    say ""
    line=$(one_run "$args" 1)
    say "$tag IDENT | ppoll=${line##*|ppoll=} $(grep -oE '(sent|cpu_mhz_mean)=[0-9.-]+' <<<"$line" | tr '\n' ' ')"
    for r in $(seq 1 "$RUNS"); do
        line=$(one_run "$args" 0)
        got=$(grep -oE 'sent=[0-9]+' <<<"$line" | head -1 | cut -d= -f2)
        if [ "${got:-x}" != "$want" ]; then
            say "$tag run$r VOID(sent=${got:-none}, wanted $want)"
            continue
        fi
        say "$tag run$r | $(grep -oE '(sent|recv|rtt_avg_ms|rtt_max_ms|cpu_mhz_mean|cpu_mhz_min|cpu_mhz_at_rtt_max|retransmitted|gap_abandoned)=[0-9.-]+' <<<"$line" | tr '\n' ' ')"
    done
}
build old
combo "OLD s=0.1" "-i 0.1 -d 10" 100
combo "OLD s=1.0" "-i 1.0 -d 5"  5
build new
combo "NEW s=0.1" "-i 0.1 -d 10" 100
combo "NEW s=1.0" "-i 1.0 -d 5"  5
say ""; say "=== done ==="
