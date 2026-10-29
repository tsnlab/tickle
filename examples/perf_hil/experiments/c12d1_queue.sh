#!/usr/bin/env bash
# C12-D1: does a send from the socket the receiver then waits on make the next wake and return slower - for the
# DEFER_TASKRUN ring (prod_dt, prod_dt_msg) and for a blocking recvmsg? (2026-10-29)
#
# Why: in the rig's KH2B pong records (libc arm) TickLE's tap -> ring return p50 is 15.0-16.0 us when the receiving
# thread sent within 1 ms before waiting (it sends from data_sock, the socket the ring waits on) and 12.5-14.0 when its
# last send was >= 20 ms earlier; the vendors are at 12.5-14.0. recv_wake_xhost.c echoed from a second socket, so it
# never had that condition; its `same` echo mode adds it. A rise here says the send itself costs the wake; none says
# the pong split's difference has another cause (and the production wake is traced instead).
#
# What runs: recv_wake_xhost_rig.sh (its header has the rig, the guards and the per-run VOID rules) with
#   ARMS = recvfrom recvfrom_aa prod_dt@same prod_dt@second recvmsg@same recvmsg@second prod_dt_msg@same
#          prod_dt_msg@second
# every rep running every arm once, the order rotated by one per rep (interleaved); REPS 5 (the reader needs >= 3 valid
# reps an arm); READER = c12d1_read.py, whose rules (--prereg) were written before any run and are code: per mechanism
# delta = P50(same) - P50(second); VOID on short counts or an A/A (recvfrom vs recvfrom_aa) above 0.3 us;
# RING-SPECIFIC, ANY-READER, FALSIFIED or UNDECIDED.
#
# Guards, here:
#   - the rig lock is taken INSIDE this detached process (rig_lock.sh around the rig script, RIG_LOCK_WAIT 36000 s,
#     scope hil); the rig script runs under it and does not take it again;
#   - one EXIT trap, which prints "C12D1 QUEUE DONE rc=<n>" on every exit path (to stdout and to OUT.txt);
#   - SHA must be set (the rig script checks it is on origin/main and builds that blob on both Pis).
#
# USAGE (detached, from a durable checkout - /home/semih/tickle - whose working tree has this script, at a pushed SHA):
#   TS=$(date +%Y%m%d-%H%M%S); OUT=$HOME/rig_results_safe/c12d1_$TS
#   setsid nohup env SHA=<sha on origin/main> OUT=$OUT examples/perf_hil/experiments/c12d1_queue.sh \
#     > $OUT.launch.log 2>&1 < /dev/null &
#   Wait on "C12D1 QUEUE DONE" in $OUT.launch.log (printed on every exit). Verdict: $OUT.verdict.txt, or
#   python3 examples/perf_hil/experiments/c12d1_read.py $OUT
# DURATION (rig): build + probe ~1 min; 8 arms x 5 reps = 40 runs x ~11 s (the 061f4f0b run: 10 arms a rep in 110 s)
#   = ~7.5 min; total ~9 min, analyse an overrun at 14 min. Plus the wait for the lock.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
X=$REPO/examples/perf_hil/experiments
OUT=${OUT:-$HOME/rig_results_safe/c12d1_$(date +%Y%m%d-%H%M%S)}
rc=1
# shellcheck disable=SC2329 # called by the EXIT trap
finish() { # the one EXIT trap
    local line
    line="C12D1 QUEUE DONE rc=$rc $(date -Is) out=$OUT"
    echo "$line"
    echo "$line" >>"$OUT.txt" 2>/dev/null
}
trap finish EXIT
trap 'rc=143; exit 143' TERM INT HUP # a signal: the EXIT trap still prints the marker, with this rc

case "${SHA:-}" in "" | *[!0-9a-f]*) echo "REFUSED: set SHA to a commit id on origin/main"; rc=64; exit "$rc" ;; esac
mkdir -p "$(dirname "$OUT")" || { rc=1; exit "$rc"; }
export SHA OUT
export REPS=${REPS:-5}
ARMS="recvfrom recvfrom_aa prod_dt@same prod_dt@second recvmsg@same recvmsg@second"
export ARMS="$ARMS prod_dt_msg@same prod_dt_msg@second"
export READER=c12d1_read.py
echo "C12D1 QUEUE START $(date -Is) sha=$SHA reps=$REPS out=$OUT"
python3 "$X/c12d1_read.py" --selftest >/dev/null 2>&1 || { echo "REFUSED: c12d1_read.py --selftest fails"; exit "$rc"; }

if [ "${PC_PREFLIGHT:-0}" = 1 ]; then # the harness on two PC namespaces, no rig, no lock
    "$X/recv_wake_xhost_rig.sh"
    rc=$?
else
    RIG_LOCK_WAIT=36000 RIG_LOCK_SCOPE=hil "$REPO/examples/perf_hil/rig_lock.sh" "$X/recv_wake_xhost_rig.sh"
    rc=$?
fi
# The rig script exits 0 also when its reader failed: the run counts as done only with its RWX_DONE and a VERDICT.
if [ "$rc" = 0 ]; then
    grep -q '^RWX_DONE ' "$OUT.txt" 2>/dev/null || rc=2
    verdict=$(grep -m1 '^VERDICT: ' "$OUT.verdict.txt" 2>/dev/null)
    echo "${verdict:-no VERDICT line in $OUT.verdict.txt}"
    [ -n "$verdict" ] || rc=3
fi
exit "$rc"
