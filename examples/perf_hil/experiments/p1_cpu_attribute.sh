#!/usr/bin/env bash
# p1_cpu_attribute.sh - attributes (or dismisses) the p1 RELIABLE client's CPU rise that WIRE_PLAN.md section 10 left
# open: +0.5-0.7% per sample between 6910d840 and main, confirmed twice, cause unknown. The syscall counts came back
# identical (section 10.1), core's own send path is flat, and g8/g9/g6 are not compiled into a native build at all.
#
# What is new here is the instrument, not the comparison. getrusage's utime/stime are tick-accounted, so ~2 s of CPU
# over a 20 s run is quantised at the tick and an 0.5% difference sits inside it: the old figure could show the effect
# but never locate it. Since 7698f669 every RESULT line also carries sched_cpu_s (the sum of /proc/<tid>/schedstat's
# nanosecond sum_exec_runtime over the process's threads) and sched_by_thread=, so the question becomes which thread.
#
# HOW TO READ IT, written before the run:
#   - the rise appears in sched_cpu_s_per_Msample too, and sched_by_thread= names one thread -> that thread's code is
#     the next target, and the open item becomes a bounded search instead of a mystery.
#   - the rise appears in cpu_s_per_Msample but NOT in sched_cpu_s_per_Msample (2xSE, both directions) -> what was
#     measured twice was tick accounting, not work. The open item closes as an instrument artefact, and WIRE_PLAN 10
#     says so.
#   - neither figure separates the arms -> the regression does not reproduce at this build pair. Record it as
#     unreproducible with this run's n, and do not carry it as a live regression.
#   - sched_unattributed_s above a tenth of cpu_s (instrument=...schedgap) in any run -> the breakdown is incomplete
#     for that run and it is VOID for the per-thread question, though its totals still count.
#   - any run with sent < 100000, or a mismatched identity check, is VOID.
#
# Both arms carry THIS checkout's BenchStats.h, since 6910d840 predates the instrument. That is the same instrumentation
# code in both arms, outside the per-sample loop, and it is the only file taken from the working tree - stated here
# because an A/B whose arms differ in more than the thing under test is not an A/B.
#
# ABBA per block, so drift lands on both arms. Each arm's binaries are built ONCE and saved aside; the loop then runs
# saved binaries and never builds, because a build inside an A/B loop is how the underlay gets rewritten mid-run.
# Usage: p1_cpu_attribute.sh [SHA_A] [SHA_B] [BLOCKS]   Output: $OUT (default /tmp/p1_cpu_attribute.txt)
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
A=${1:-6910d840}
B=${2:-$(git -C "$REPO" rev-parse --short HEAD)}
BLOCKS=${3:-4}
OUT=${OUT:-/tmp/p1_cpu_attribute.txt}
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
SCEN=reliable_throughput; SIZE=p1; DUR=20
SAVE=/tmp/p1cpu
srv_pid=""
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "${@:2}"; }
: >"$OUT"; say() { echo "$*" | tee -a "$OUT"; }
say "=== p1 cpu attribution $(date -Is) A=$A B=$B blocks=$BLOCKS dur=${DUR}s ==="

# --- build each arm once, on both Pis, and save the binaries aside -------------------------------
for arm in A B; do
    sha=$A; [ "$arm" = B ] && sha=$B
    for h in "$CLIENT" "$SERVER"; do
        sh_ "$h" "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $sha && git clean -fdqx -e install -e build -e log" </dev/null \
            || { say "FATAL checkout $sha failed on $h"; exit 1; }
        # The instrument, from this checkout, into both arms.
        sh_ "$h" "cat > ~/tickle/examples/perf_hil/tickle/common/BenchStats.h" <"$REPO/examples/perf_hil/tickle/common/BenchStats.h" \
            || { say "FATAL BenchStats.h copy failed on $h"; exit 1; }
        sh_ "$h" "set -e; cd ~/tickle/examples/perf_hil/tickle && ./build.sh $SCEN $SIZE > /tmp/p1cpu_build.log 2>&1 || { echo BUILD_FAILED; tail -5 /tmp/p1cpu_build.log; exit 1; }
mkdir -p $SAVE/$arm && cp ${SCEN}_${SIZE}/client ${SCEN}_${SIZE}/server $SAVE/$arm/ && sha256sum $SAVE/$arm/client | cut -c1-16" </dev/null \
            | tail -1 | while read -r s; do say "  built arm $arm ($sha) on $h client sha256=$s"; done \
            || { say "FATAL build failed for $sha on $h"; exit 1; }
    done
done

kill_server() {
    [ -z "${srv_pid:-}" ] && return 0
    # By the PID its own launch wrote, checked through /proc/PID/exe - never by a pattern (CLAUDE.md rule 2).
    sh_ "$SERVER" "case \"\$(readlink /proc/$srv_pid/exe 2>/dev/null)\" in $SAVE/*/server) kill -TERM $srv_pid;; esac
for i in 1 2 3 4 5; do [ -d /proc/$srv_pid ] || break; sleep 1; done; true" </dev/null >/dev/null
    srv_pid=""
}

run_one() { # run_one <arm>
    local arm=$1 line
    kill_server
    # ( setsid ... & ) in a subshell: a bare `setsid ... &` keeps this ssh open until the server exits, which had the
    # client running against a dead server three times on 2026-09-28. -d is generous: the client's own -d bounds the
    # measurement, and a server already past its deadline leaves the client with nothing to match.
    srv_pid=$(sh_ "$SERVER" "cd $SAVE/$arm && rm -f /tmp/p1cpu_server.log /tmp/p1cpu_server.pid && (setsid sh -c 'echo \$\$ > /tmp/p1cpu_server.pid; exec taskset -c 1-3 ./server -Q -d $((DUR + 40))' > /tmp/p1cpu_server.log 2>&1 < /dev/null &); sleep 2; cat /tmp/p1cpu_server.pid" </dev/null)
    if ! sh_ "$SERVER" "grep -q 'Node open' /tmp/p1cpu_server.log" </dev/null; then
        say "arm=$arm VOID(server did not open) pid=$srv_pid"
        return
    fi
    line=$(sh_ "$CLIENT" "cd $SAVE/$arm && taskset -c 1-3 ./client -Q -d $DUR 2>&1 | grep '^RESULT'" </dev/null)
    if [ -z "$line" ]; then
        say "arm=$arm VOID(no RESULT line)"
        return
    fi
    say "arm=$arm $line"
}

# --- ABBA blocks ---------------------------------------------------------------------------------
for b in $(seq 1 "$BLOCKS"); do
    say "--- block $b/$BLOCKS $(date -Is) ---"
    for arm in A B B A; do run_one "$arm"; done
done
kill_server
say "=== done $(date -Is) ==="

# --- the comparison, at the two figures the reading above distinguishes --------------------------
# Done here rather than through ab_compare.py: that tool reads campaign_sweep.sh and rmw_crosshost_rtt.sh files, and
# these are raw client RESULT lines tagged with their arm. Same arithmetic - mean, SE, t = (B-A)/sqrt(SE_A^2+SE_B^2),
# |t| > 2 to call it - and the same rule that a held figure is a result and not an absence of one.
python3 - "$OUT" <<'PYEOF' | tee -a "$OUT"
import math, re, sys

rows = {"A": [], "B": []}
void = {"A": 0, "B": 0}
for line in open(sys.argv[1]):
    m = re.match(r"arm=([AB]) (.*)", line.strip())
    if not m:
        continue
    arm, rest = m.group(1), m.group(2)
    if not rest.startswith("RESULT"):
        void[arm] += 1  # a VOID( ... ) line
        continue
    fields = dict(kv.split("=", 1) for kv in rest.split() if "=" in kv)
    try:
        sent = int(fields.get("sent", 0))
    except ValueError:
        sent = 0
    if sent < 100000:
        void[arm] += 1
        continue
    rows[arm].append(fields)

def stat(values):
    n = len(values)
    mean = sum(values) / n
    se = 0.0 if n < 2 else math.sqrt(sum((v - mean) ** 2 for v in values) / (n - 1) / n)
    return n, mean, se

print("n: A=%d B=%d  void: A=%d B=%d" % (len(rows["A"]), len(rows["B"]), void["A"], void["B"]))
if not rows["A"] or not rows["B"]:
    print("VERDICT: no comparison - one arm has no usable run")
    sys.exit(0)

gap = [f for arm in "AB" for f in rows[arm] if "schedgap" in f.get("instrument", "")]
if gap:
    print("NOTE: %d run(s) reported schedgap - their per-thread breakdown is incomplete; totals still count" % len(gap))

for field in ("cpu_s_per_Msample", "sched_cpu_s_per_Msample"):
    try:
        va = [float(f[field]) for f in rows["A"]]
        vb = [float(f[field]) for f in rows["B"]]
    except KeyError:
        print("%s: absent from at least one arm's lines - cannot compare" % field)
        continue
    na, ma, sa = stat(va)
    nb, mb, sb = stat(vb)
    denom = math.sqrt(sa * sa + sb * sb)
    if denom == 0:
        t = 0.0 if ma == mb else float("inf")
    else:
        t = (mb - ma) / denom
    pct = 100.0 * (mb - ma) / ma if ma else 0.0
    if abs(t) <= 2:
        verdict = "held"
    elif mb > ma:
        verdict = "WORSE (B costs more)"
    else:
        verdict = "better (B costs less)"
    print("%s: A %.4f+-%.4f (n=%d)  B %.4f+-%.4f (n=%d)  diff %+.4f (%+.2f%%)  t=%.2f  %s"
          % (field, ma, sa, na, mb, sb, nb, mb - ma, pct, t, verdict))

print("per-thread, mean seconds over each arm's runs:")
for arm in "AB":
    totals = {}
    for f in rows[arm]:
        for part in f.get("sched_by_thread", "").split(","):
            if ":" in part:
                name, secs = part.rsplit(":", 1)
                try:
                    totals[name] = totals.get(name, 0.0) + float(secs)
                except ValueError:
                    pass
    n = len(rows[arm]) or 1
    parts = ", ".join("%s=%.4f" % (k, v / n) for k, v in sorted(totals.items(), key=lambda kv: -kv[1]))
    print("  arm %s: %s" % (arm, parts))
PYEOF
