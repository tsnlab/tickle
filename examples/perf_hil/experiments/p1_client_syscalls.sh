#!/usr/bin/env bash
# Why the p1 RELIABLE client's CPU per sample rose 0.5-0.7% between 6910d840 and 268dd20e
# (WIRE_PLAN.md section 10, confirmed twice on the rig). Core's own per-sample send path is flat, g8/g9/g6 are not
# compiled into a native build at all, g10 costs 0.3 ns, and tt_Context's hot fields sit at the same offsets. What is
# left is the real send path: the number of system calls, or their cost.
#
# This counts the client's system calls per sample with `strace -c` at both commits, one short run each. strace makes
# the timing meaningless and that is fine: the counts are exact, and counts are the question.
#
# HOW TO READ IT, written before the run:
#   - sendmmsg (or sendto/sendmsg) calls per sample differ by more than 1% -> the rise is the call COUNT, and the next
#     step is which change added calls.
#   - the counts agree within 1% -> it is not the call count. The rise is then the cost per call or code placement in
#     the client binary, and the remaining lever is a targeted A/B with per-thread schedstat, not this bench.
#   - any run whose sample count is below 100000, or which reports loss, is VOID.
# Usage: p1_client_syscalls.sh [SHA_A] [SHA_B]   Output: $OUT (default /tmp/p1_client_syscalls.txt)
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
A=${1:-6910d840}; B=${2:-268dd20e}
srv_pid=""
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
OUT=${OUT:-/tmp/p1_client_syscalls.txt}
SCEN=reliable_throughput; SIZE=p1
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "${@:2}"; }
: >"$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== p1 client syscalls per sample, $(date -Is), A $A, B $B ==="
for sha in "$A" "$B"; do
    for h in "$CLIENT" "$SERVER"; do
        sh_ "$h" "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $sha && git clean -fdqx -e install -e build -e log
cd examples/perf_hil/tickle && ./build.sh $SCEN $SIZE > /tmp/p1sc_build.log 2>&1 || { echo BUILD_FAILED; tail -5 /tmp/p1sc_build.log; exit 1; }
test -x ~/tickle/examples/perf_hil/tickle/${SCEN}_${SIZE}/client" || { say "BUILD FAILED for $sha on $h"; exit 1; }
    done
    say "--- built $sha on both rpis ---"
    d=/home/ci/tickle/examples/perf_hil/tickle/${SCEN}_${SIZE} # the Pi's own path: ~ would expand here, not there
    # setsid, not bare nohup: without its own session the server dies when this ssh closes, and the client then
    # times out waiting for a match (seen 2026-09-28). -Q on both sides, as the campaign's own matrix passes for this
    # cell - without it the QoS does not match and nothing is ever delivered.
    # The previous arm's server is killed by the PID its own launch printed, checked through /proc/PID/exe - never by
    # a pattern (CLAUDE.md rule 2). The first version of this script used `pkill -f ${SCEN}_${SIZE}/server`, which
    # matches nothing: the real command line is "./server -Q -d 20" after a cd. Arm A's v10 server therefore survived
    # into arm B and ended arm B's v11 server through the wire-version defect (RMW_GAPS_PLAN g11), which is what the
    # VOID guard caught.
    if [ -n "${srv_pid:-}" ]; then
        sh_ "$SERVER" "[ \"\$(readlink /proc/$srv_pid/exe 2>/dev/null)\" = $d/server ] && kill -TERM $srv_pid; for i in 1 2 3 4 5; do [ -d /proc/$srv_pid ] || break; sleep 1; done; true" >/dev/null
    fi
    # -d 60, not 20: the gap between starting the server and starting the client is ssh-dependent, and a server that
    # has already reached its own deadline leaves the client timing out with nothing to match (seen 2026-09-28: the
    # server opened at 14:53:56 and exited at 14:54:31, the client ran 14:54:34). The client's own -d bounds the
    # measurement; the server is stopped by PID below. The PID is the server's own, written by the shell that execs it
    # - `echo $!` after `setsid nohup ... &` gives setsid's pid, which exits at once.
    # The launch must sit in its own SUBSHELL: `setsid ... &` on its own keeps this ssh open until the server exits,
    # so the command substitution blocks for the server's whole life and the client then runs against a dead server
    # (2026-09-28: three runs voided this way - the server opened at 14:53:56 and exited at 14:54:31, the client ran
    # 14:54:34). `( ... & )` lets ssh return at once, which is why the same thing worked by hand.
    srv_pid=$(sh_ "$SERVER" "cd $d && rm -f /tmp/p1sc_server.log /tmp/p1sc_server.pid && (setsid sh -c 'echo \$\$ > /tmp/p1sc_server.pid; exec taskset -c 1-3 ./server -Q -d 60' > /tmp/p1sc_server.log 2>&1 < /dev/null &); sleep 2; cat /tmp/p1sc_server.pid")
    srv=$(sh_ "$SERVER" "grep -c 'Node open' /tmp/p1sc_server.log; grep -c 'Illegal version' /tmp/p1sc_server.log")
    say "  server pid=$srv_pid opened/version_errors: $(echo "$srv" | tr '\n' '/')"
    sh_ "$CLIENT" "cd $d && rm -f /tmp/p1sc_strace.txt /tmp/p1sc_client.log && strace -c -f -o /tmp/p1sc_strace.txt taskset -c 1-3 ./client -Q -d 8 > /tmp/p1sc_client.log 2>&1; true" >/dev/null
    scp -q -i "$K" -o BatchMode=yes "ci@$CLIENT:/tmp/p1sc_strace.txt" "$OUT.strace.$sha" 2>/dev/null || say "  (no strace file)"
    res=$(sh_ "$CLIENT" "grep -E '^RESULT|rror|No such|denied|timed out' /tmp/p1sc_client.log | head -3")
    sent=$(echo "$res" | grep -oE 'sent=[0-9]+' | head -1 | cut -d= -f2)
    say "sha=$sha sent=${sent:-0}"
    if [ "${sent:-0}" -lt 100000 ]; then
        say "VOID(sent=${sent:-0} below 100000)"
        say "  client said: $(echo "$res" | head -2 | tr '\n' ' ' | cut -c1-160)"
        continue
    fi
    # Calls per sample, from strace's own summary table: the last field is the call name, the one before errors/calls.
    # strace's summary columns are always: %time seconds usecs/call calls [errors] syscall - so calls is $4, whether
    # or not the errors column carries a number. Indexing from NF got this wrong and printed nothing.
    awk -v s="${sent:-1}" '$1 + 0 == $1 && $NF ~ /^[a-z_0-9]+$/ && $4 + 0 > 0 {
        printf "  %-14s calls=%-9d per_sample=%.5f\n", $NF, $4, $4 / s }' "$OUT.strace.$sha" | sort -t= -k3 -rn | head -8 | tee -a "$OUT"
done
say "=== done $(date -Is) ==="
