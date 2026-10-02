#!/usr/bin/env bash
# wake_cost.sh - runs wake_cost.c's three arms on one rig Pi and reads them against a threshold set before the run.
#
# THE QUESTION. COMPARISON 2.2c measured our shared-memory path at 0.058 ms against our own kernel path at 0.050 on
# a p2 round trip, pure arm against pure arm, ranges separable. Both vendors GAIN 29-41% from their segments; we
# lose 16%. The hypothesis is the doorbell: a segment-carried sample still sends a zero-length UDP datagram to wake
# a reader asleep on its socket (`segment_doorbells_sent`), where CycloneDDS and Fast DDS signal inside their own
# segments. This measures the three mechanisms directly, with no transport around them.
#
# THE ARITHMETIC, written before the run. A round trip carries two wakes, one each way, so for the doorbell to
# account for the whole 8 us gap an alternative must save about 4 us PER WAKE, i.e. about 8 us on this benchmark's
# own round trip. Hence:
#
#   udp - alternative >= ~8 us on this RTT  -> the doorbell accounts for 2.2c's gap. What remains is choosing
#                                              between unix and futex on structure, not on this number.
#   all three within ~2 us                  -> the doorbell is NOT the cause, and no notification work should
#                                              start. The 16% is in the slot write, the drain loop or cache
#                                              behaviour, and that is where to look next. This outcome saves the
#                                              most time and is the one to report loudest.
#   in between                              -> the doorbell is part of it. Report the fraction it could explain
#                                              and do not call it the cause.
#
# Both roles on the client Pi, taskset to separate cores, as s6_witness_check.sh does - the comparison is between
# mechanisms on one machine, so anything that moves all three equally cancels.
# Usage: wake_cost.sh [ITERS] [REPS]     Output: $OUT
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
ITERS=${1:-20000}
REPS=${2:-3}
OUT=${OUT:-$HOME/rig_results_safe/wake_cost.txt}
HOST=10.1.1.214
K=$HOME/.ssh/tickle_ci_ed25519
SAVE=/tmp/wakecost
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "${@:2}"; }
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
say "=== wake cost $(date -Is) host=$HOST iters=$ITERS reps=$REPS ==="

cleanup() {
    sh_ "$HOST" "pkill -f '$SAVE/wake_cost' 2>/dev/null; rm -f /tmp/wake_cost_*.sock /dev/shm/wake_cost_futex /tmp/wake_cost_fifo_*" </dev/null >/dev/null 2>&1
}
trap cleanup EXIT

say "--- building on $HOST ---"
build=$(sh_ "$HOST" "mkdir -p $SAVE && cat > $SAVE/wake_cost.c && cd $SAVE && gcc -O2 -Wall -Wextra -D_GNU_SOURCE -o wake_cost wake_cost.c 2>&1 && sha256sum wake_cost | cut -c1-16" <"$REPO/examples/perf_hil/experiments/wake_cost.c" 2>&1)
case "$build" in
    [0-9a-f][0-9a-f]*) say "  built, sha256=$build" ;;
    *) say "FATAL build failed:"; say "$build"; exit 1 ;;
esac

for rep in $(seq 1 "$REPS"); do
    say "--- rep $rep/$REPS $(date -Is) ---"
    for mech in udp unix fifo futex; do
        line=$(sh_ "$HOST" "cd $SAVE && rm -f /tmp/wake_cost_*.sock /dev/shm/wake_cost_futex
rm -f /tmp/wake_cost_fifo_s /tmp/wake_cost_fifo_c
(setsid taskset -c 1 ./wake_cost $mech server $ITERS > /tmp/wake_srv.log 2>&1 &); sleep 1
taskset -c 2 ./wake_cost $mech client $ITERS 2>&1
echo '--- server said ---'; cat /tmp/wake_srv.log" </dev/null)
        result=$(printf '%s' "$line" | grep '^RESULT: role=client' | head -1)
        if [ -n "$result" ]; then
            say "rep=$rep $result"
        else
            say "  NO RESULT for $mech - what the two sides said:"
            say "$(printf '%s' "$line" | head -8 | sed 's/^/      /')"
        fi
    done
done
say "=== done $(date -Is) ==="

python3 - "$OUT" <<'PYEOF' | tee -a "$OUT"
import re, sys, collections, statistics as st
rows = collections.defaultdict(list)
for line in open(sys.argv[1]):
    m = re.search(r"RESULT: role=client mechanism=(\w+) .*", line)
    if not m:
        continue
    f = dict(kv.split("=", 1) for kv in m.group(0).split() if "=" in kv)
    try:
        rows[m.group(1)].append((float(f["rtt_mean_us"]), float(f["rtt_p50_us"]), float(f["rtt_min_us"])))
    except (KeyError, ValueError):
        continue

print()
print("=== round-trip cost of one wake mechanism, medians over reps ===")
print("%-8s %3s %12s %12s %12s" % ("mech", "n", "mean us", "p50 us", "min us"))
med = {}
for mech in ("udp", "unix", "fifo", "futex"):
    rs = rows.get(mech, [])
    if not rs:
        print("%-8s   0   (no usable reps)" % mech); continue
    med[mech] = (st.median([r[0] for r in rs]), st.median([r[1] for r in rs]), st.median([r[2] for r in rs]))
    print("%-8s %3d %12.3f %12.3f %12.3f" % (mech, len(rs), med[mech][0], med[mech][1], med[mech][2]))

print()
if "udp" not in med:
    print("VERDICT=VOID - the udp arm produced nothing, so there is no baseline to compare against.")
    sys.exit(0)
base = med["udp"][1]   # p50, which a wake benchmark's tail should not decide
best_name, best = None, None
for mech in ("unix", "fifo", "futex"):
    if mech in med:
        d = base - med[mech][1]
        print("  %-6s saves %.3f us per round trip (%.3f us per wake)" % (mech, d, d / 2.0))
        if best is None or d > best:
            best_name, best = mech, d
if best is None:
    print("VERDICT=VOID - no alternative arm produced a figure.")
elif best >= 8.0:
    print()
    print("VERDICT=DOORBELL_ACCOUNTS_FOR_IT - %s saves %.3f us per round trip, at or above the ~8 us that 2.2c's"
          % (best_name, best))
    print("  gap needs. The notification is where the 16%% is. Choosing between unix and futex is now a structural")
    print("  question (unix keeps the single ppoll wait point; futex does not), not a question about this number.")
elif best <= 2.0:
    print()
    print("VERDICT=NOT_THE_DOORBELL - the best alternative saves only %.3f us per round trip, against the ~8 us" % best)
    print("  2.2c's gap needs. Do not start notification work: the 16%% is in the slot write, the drain loop or")
    print("  cache behaviour, and that is where to look. This is the result that saves the most time.")
else:
    print()
    print("VERDICT=PARTIAL - the best alternative saves %.3f us per round trip against the ~8 us needed, so the" % best)
    print("  doorbell could account for about %.0f%% of 2.2c's gap and not all of it. Report the fraction; do not" % (best / 8.0 * 100))
    print("  call the doorbell the cause.")
PYEOF
