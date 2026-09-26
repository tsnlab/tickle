#!/usr/bin/env bash
# Is the new poll's +3.27% RTT entirely the lower CPU clock, or is part of it per-core idle cost?
#
# cpufreq/policy0 related_cpus = "0 1 2 3" - one policy for the whole package (TickLE Dev) - so a
# spinner pinned to a core the client does NOT use raises the clock without keeping the client's own
# core awake. That separates the two terms, which no arm so far has done. C-states are already ruled
# out as an additive term: both Pi 5s report cpuidle current_driver=none, so idle is plain WFI.
#
# Spacing is fixed at 0.1 s throughout - the tightest standard errors (+/-0.0005) and where the build
# effect is largest (+3.27%). The spacing question is settled and is not being re-asked.
#
# HOW TO READ IT, written before running:
#   A OLD no spinner    the reference the clock explanation has to reach   (measured 0.2064)
#   B NEW no spinner    the gap to be explained                           (measured 0.2132)
#   C NEW + spinner     clock lifted, client core still idle between pings
#        C ~ A            -> the clock is the whole story
#        C between A,B    -> the remainder is per-core idle: tick restart, cold cache, not frequency
#        C ~ B            -> the clock is NOT the story and the mechanism is elsewhere
#   D OLD + spinner     CONTROL for the spinner itself. eth0's IRQ 108 is entirely on CPU0 (493M
#        interrupts there, zero on the other three), and CPU0 is the only core outside the client's
#        1-3 taskset, so the spinner has to share with the interrupt handler. If D differs from A
#        outside its spread, the spinner is perturbing the measurement and C says NOTHING.
#   VOID, not negative: if arm C's cpu_mhz_mean does not rise off the ~1500 MHz floor, the spinner
#        failed to lift the clock and the arm did not test what it was for.
set -uo pipefail
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
PIN="taskset -c 1-3"; RUNS=${RUNS:-15}; ARGS="-i 0.1 -d 10"
OUT=${OUT:-/tmp/rtt_spin.txt}
HARNESS=4baa3b76; OLDCORE=9a230a1b
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
# The spinner runs on CPU0 at nice 19 on BOTH hosts: the server's echo path idles and down-clocks
# exactly as the client's does, so lifting only one side would leave half the mechanism in place.
spin_start() { for h in "$CLIENT" "$SERVER"; do
        sh_ "$h" "setsid nohup nice -n 19 taskset -c 0 sh -c 'while :; do :; done' >/dev/null 2>&1 < /dev/null & echo started" >/dev/null 2>&1
    done; sleep 2; }
spin_stop() { for h in "$CLIENT" "$SERVER"; do
        # shellcheck disable=SC2016  # expands on the rig
        sh_ "$h" 'for p in $(ls /proc | grep -E "^[0-9]+$"); do
            c=$(tr "\0" " " < /proc/$p/cmdline 2>/dev/null)
            case "$c" in "sh -c while :; do :; done "*) kill "$p" 2>/dev/null;; esac; done' >/dev/null 2>&1
    done; sleep 1; }
trap 'spin_stop; kill_servers' EXIT
: > "$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== spinner arm: clock or per-core idle, $(date -Is) ==="
build() {
    local which="$1" pids=() bad=0 recipe
    [ "$which" = old ] && recipe="git checkout -q $HARNESS && git checkout -q $OLDCORE -- $CORE_FILES" \
                       || recipe="git checkout -q $HARNESS"
    say ""; say "--- building $which ---"
    for h in "$CLIENT" "$SERVER"; do
        sh_ "$h" "set -e
cd ~/tickle && git fetch -q origin && git reset -q --hard origin/main && git clean -fdqx -e install -e build -e log && $recipe
cd examples/perf_hil/tickle && ./build.sh reliable_latency p1 > /tmp/spin_build.log 2>&1 \
  || { echo FAIL; tail -8 /tmp/spin_build.log; exit 1; }" &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p" || bad=1; done
    [ "$bad" = 0 ] || { say "BUILD FAILED"; exit 1; }
    say "  until_next_event in core: $(sh_ "$CLIENT" "grep -c until_next_event ~/tickle/src/tickle.c || true" | head -1)"
}
arm() {
    local tag="$1" spin="$2" r line
    if [ "$spin" = 1 ]; then spin_start; else spin_stop; fi
    say ""
    say "  $tag: client core freq now $(sh_ "$CLIENT" 'cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq') kHz"
    for r in $(seq 1 "$RUNS"); do
        kill_servers; [ -z "$(server_pids)" ] || { say "$tag run$r ABORT-server"; continue; }
        sh_ "$SERVER" "cd ~/tickle/examples/perf_hil/tickle/reliable_latency_p1; nohup $PIN ./server $ARGS > /tmp/spin_srv.log 2>&1 < /dev/null &"
        sleep 3
        [ "$(server_pids | wc -l)" = 1 ] || { say "$tag run$r ABORT-nserver"; continue; }
        line=$(sh_ "$CLIENT" "cd ~/tickle/examples/perf_hil/tickle/reliable_latency_p1 && $PIN ./client $ARGS" | grep -m1 '^RESULT:')
        kill_servers
        local got; got=$(grep -oE 'sent=[0-9]+' <<<"$line" | head -1 | cut -d= -f2)
        [ "${got:-x}" = 100 ] || { say "$tag run$r VOID(sent=${got:-none})"; continue; }
        say "$tag run$r | $(grep -oE '(rtt_avg_ms|rtt_max_ms|cpu_mhz_mean|cpu_mhz_min|retransmitted)=[0-9.-]+' <<<"$line" | tr '\n' ' ')"
    done
}
build old;  arm "A OLD nospin" 0; arm "D OLD spin  " 1
build new;  arm "B NEW nospin" 0; arm "C NEW spin  " 1
spin_stop
say ""; say "=== done ==="
