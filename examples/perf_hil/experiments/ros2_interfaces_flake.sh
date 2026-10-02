#!/usr/bin/env bash
# ros2_interfaces_flake.sh - is the shared-memory module the trigger for CI's intermittent
# "std_msgs did not round-trip through rmw_tickle"?
#
# WHY. `Check all` failed on f8caed35 (2026-10-01) and on 5d53b149 (2026-10-02) with the same step and
# the same message, 14 consecutive successes between them. Both logs carry `Node ... presumed dead
# (silent for 3500 ms)` and a subscriber that got 5 of 100. Only the later one also carries `Segment
# ring of context 123 is full`.
#
# THE SUSPICION IS OF OUR OWN CHANGE. fc34c8d1 made a same-host pair attach after ~1 send instead of
# ~257. check_ros2_interfaces publishes 100 messages and exits - exactly the shape that never reached
# the old countdown - so before fc34c8d1 it ran entirely over the kernel and now it runs over the
# segment. If the segment loses messages under that shape, fc34c8d1 raised the failure rate of a test
# that was already occasionally red, and that is ours to own.
#
# DEV'S PREDICTION, recorded before the run because it is falsifiable and it is the useful half: the
# control will ALSO fail, at a similar rate. `delivered=5` beside `presumed dead` says the subscriber
# was not running; with the module on a stalled reader lets the ring fill and drop-on-full discards,
# with it off a stalled reader lets the socket buffer overflow and the kernel discards. Same cause,
# different queue.
#
# HOW TO READ IT, written before the run:
#   both arms fail at similar rates     -> Dev is right. The segment is not the trigger, fc34c8d1 did
#                                          not raise the rate, and the cause is a process starved past
#                                          the liveliness limit - which is where to look next.
#   module-on fails materially more     -> the segment IS the trigger and fc34c8d1 raised the rate.
#                                          The lever is then back-pressure (make the writer wait),
#                                          which is the user's open decision 2 and not ours to take.
#                                          Rerouting a full ring to UDP is NOT available: it was tried
#                                          and reverted, because a UDP datagram overtakes the records
#                                          already in the ring and the reader discards everything
#                                          older behind it - 97.6% of CI's same-host traffic.
#   neither arm fails at all            -> VOID, not a pass. This machine is quieter than the runner
#                                          and the failure is load-dependent; re-run with LOAD=1,
#                                          which is what the second phase below is for.
#   the two builds are byte-identical   -> VOID. The -D never reached the compiler and both arms are
#                                          the control, which would read as "no difference" for the
#                                          wrong reason.
#
# WHAT HAPPENED WHEN IT RAN (2026-10-02), kept because the design failed before the arms did.
#
# 0 of 10 failed on both arms, quiet, and 0 of 10 again with 16 spinners. Pre-registered as VOID, which
# is what stopped it being reported as "the segment is fine". Two things it taught:
#
#   The run was UNDERPOWERED and the arithmetic says so without any result. At CI's observed 10% rate,
#   P(0 failures in 10) = 0.9^10 ~ 35%, so "0 of 10" is what a 10% rate produces a third of the time.
#   ~30 reps per arm just to expect one failure, far more to compare two rates, and this is a 16-core box
#   standing in for a 2-core runner. Work the sample size out before running a rate comparison.
#
#   And the HYPOTHESIS WAS DEAD BEFORE ANY ARM RAN, for free. It rested on fc34c8d1 having routed this
#   test onto the segment, and the two failing runs' own logs say tx_shm=42/tx_udp=429 before it and
#   tx_shm=40/tx_udp=427 after - the path never changed, and the test is ~9% segment traffic either way.
#   Check a hypothesis's precondition before measuring its consequence; this one cost two 25-minute runs
#   for something one command answered.
#
# The remaining cause is the one both failures share: `presumed dead` beside `delivered=5` is a
# subscriber that was not running, i.e. a process starved past the liveliness limit on a loaded runner.
#
# Usage: REPS=20 LOAD=0 ros2_interfaces_flake.sh      Output: $OUT
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
WS=${WS:-$HOME/rmw_accept_ws}
REPS=${REPS:-20}
LOAD=${LOAD:-0}
DISTRO=${ROS_DISTRO_DIR:-/opt/ros/lyrical}
OUT=${OUT:-$HOME/rig_results_safe/ros2_interfaces_flake.txt}
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
say "=== ros2 interfaces flake $(date -Is) reps=$REPS load=$LOAD ws=$WS ==="

build_arm() { # build_arm <name> [extra-cmake-args...]
    local name=$1
    shift
    rm -rf "$WS/rmw_$name"
    # shellcheck disable=SC1091
    { set +u; source "$DISTRO/setup.bash"; set -u; }
    (cd "$REPO" && colcon build --packages-select rmw_tickle rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp \
        --build-base "$WS/build_$name" --install-base "$WS/rmw_$name" \
        --cmake-args -DBUILD_SHARED_LIBS=ON "$@") >>"$OUT.build_$name" 2>&1 || return 1
    local lib="$WS/rmw_$name/rmw_tickle/lib/librmw_tickle.so"
    [ -f "$lib" ] || return 1
    say "  arm $name: $(sha256sum "$lib" | cut -c1-16)"
}

build_arm on || { say "VOID: the module-on build failed"; exit 1; }
build_arm off "-DCMAKE_C_FLAGS=-Dtt_SEGMENT_ENABLED=0" || { say "VOID: the module-off build failed"; exit 1; }
on_sha=$(sha256sum "$WS/rmw_on/rmw_tickle/lib/librmw_tickle.so" | cut -c1-16)
off_sha=$(sha256sum "$WS/rmw_off/rmw_tickle/lib/librmw_tickle.so" | cut -c1-16)
if [ "$on_sha" = "$off_sha" ]; then
    say "VOID: the two builds are byte-identical - the -D never reached the compiler, so both arms are"
    say "      the control and 'no difference' would be the run agreeing with itself."
    exit 1
fi

loadgen=()
if [ "$LOAD" = "1" ]; then
    # The failure is load-dependent on the runner and this machine is quieter. One spinner per CPU, so the
    # two test processes have to compete for a core the way they do there. PIDs are captured from the launch
    # with $! and kept in an array - never found afterwards by pattern, which is how a real session was once
    # SIGKILLed here.
    for _ in $(seq 1 "$(nproc)"); do (while :; do :; done) & loadgen+=("$!"); done
    say "  load: $(nproc) spinners (pids ${loadgen[*]})"
fi
cleanup() { [ ${#loadgen[@]} -gt 0 ] && kill "${loadgen[@]}" 2>/dev/null; true; }
trap cleanup EXIT

run_arm() { # run_arm <name> -> prints "<fails> <reps>"
    local name=$1 fails=0
    for i in $(seq 1 "$REPS"); do
        if ! timeout 120 bash -c "set +u; source '$DISTRO/setup.bash'; source '$WS/rmw_$name/setup.bash'; \
             source '$WS/ifaces/install/local_setup.bash'; set -u; \
             '$REPO/rmw_tickle/scripts/check_ros2_interfaces.sh' -w '$WS/ifaces'" \
             >>"$OUT.run_$name" 2>&1; then
            fails=$((fails + 1))
            say "    $name rep $i: FAILED"
        fi
    done
    echo "$fails"
}

say "--- module ON, $REPS reps ---"
on_fails=$(run_arm on)
say "--- module OFF (control), $REPS reps ---"
off_fails=$(run_arm off)
say "=== done $(date -Is) ==="
say ""
say "  module ON  : $on_fails of $REPS failed"
say "  module OFF : $off_fails of $REPS failed  (control)"
say ""
if [ "$on_fails" -eq 0 ] && [ "$off_fails" -eq 0 ]; then
    say "VOID: neither arm failed, so the flake did not reproduce here and nothing was discriminated."
    say "      This machine is quieter than the runner. Re-run with LOAD=1 before concluding anything."
elif [ "$on_fails" -gt $((off_fails * 2 + 2)) ]; then
    say "THE SEGMENT IS THE TRIGGER: $on_fails against the control's $off_fails. fc34c8d1 raised the rate"
    say "  of a test that was already occasionally red, by routing a 100-message exchange onto the segment"
    say "  where it used to take the kernel. The lever is back-pressure, which is the user's decision 2 -"
    say "  rerouting a full ring to UDP is not available, having been tried and reverted at a cost of 97.6%."
else
    say "NOT THE SEGMENT: $on_fails against the control's $off_fails, which is Dev's prediction. A stalled"
    say "  subscriber loses the same traffic whichever queue overflows - the ring with the module on, the"
    say "  socket buffer with it off. fc34c8d1 did not raise the rate, and the cause is the older one both"
    say "  CI failures share: a process starved past the liveliness limit."
fi
