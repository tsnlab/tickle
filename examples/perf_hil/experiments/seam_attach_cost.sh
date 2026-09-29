#!/usr/bin/env bash
# seam_attach_cost.sh - is TickLE's halved cross-host throughput the transport seam retrying a failed attach?
#
# THE OBSERVATION. Cell 1 (reliable_throughput p1, -d 5, 3 reps), same rig, same harness, same day:
#     9dbffd40   114.62 Mbps   5.311 cpu_s_per_Msample     (before the shared-memory work)
#     d4413383    52.88 Mbps  11.104 cpu_s_per_Msample     (after it)
#     cyclonedds  92.84 -> 93.62,  6.870 -> 6.860          <- control, flat on both metrics
# CycloneDDS being unchanged on the same runs rules out the rig, the load and the harness. Only TickLE moved, ~2x on
# both metrics, which is two orders above WIRE_PLAN 10.4's ~1% throughput floor.
#
# THE HYPOTHESIS, which is a READ of the code and is exactly what this script exists to doubt. peer_segment()
# (src/tickle.c:629) caches an entry only on a SUCCESSFUL attach; when tt_segment_attach() returns NULL nothing is
# written to the entry, so the next send recomputes the name and calls it again. On a two-host link no segment can ever
# exist, so every datagram pays an open() that fails ENOENT - about 87,000 failed syscalls/s at the observed rate. The
# RESULT lines agree that every datagram takes that path: tx_udp_unattached=436722 of tx_udp=436728. The function's own
# comment says a peer "costs one attempt rather than a subscription to its lifecycle", which is the intended behaviour
# and is not what the code does.
#
# TWO ARMS, ONE COMMIT. Both arms are HEAD; they differ only by -Dtt_SEGMENT_ENABLED=0, which compiles the segment
# attempt out and leaves the counting #else in place. Same commit both sides means today's other 608 lines of src/ are
# held constant, so this isolates the seam rather than the day.
#
# HOW TO READ IT, written before the run, by SIGN and not by magnitude:
#   - ON does NOT reproduce ~53 Mbps            -> VOID. The whole run says nothing either way; something other than
#                                                  the thing under test changed, and a quiet rig must not read as a
#                                                  closed question. Checked FIRST, before any comparison.
#   - OFF > ON beyond 2xSE and beyond ~1%       -> FINDING: the per-datagram retry is the cost. Consequences, stated
#                                                  now so they are not negotiated after seeing the number: stage 1's
#                                                  p1-p4 figures are not measurable until it is fixed, and
#                                                  COMPARISON.md 2.7's cell-8 TickLE row (measured on 54c27bbc, after
#                                                  the seam landed) is an artefact - including its "a cell we lose to
#                                                  a DDS implementation" claim.
#   - OFF within 2xSE of ON                     -> NOT THE SEAM. The halving is elsewhere in 9dbffd40..d4413383 and
#                                                  the next step is a bisect over those commits, not a fix here.
#   - OFF < ON beyond 2xSE                      -> UNEXPECTED. Report the numbers and draw no conclusion; a segment
#                                                  path that is faster switched off than compiled out is a third bug.
#   - any run with sent < 100000, or an arm with fewer than 3 usable runs -> that arm is VOID and said so explicitly.
#
# Both scenarios are run because cell 8 is the one already published: cell 1 is the observation above, cell 8 is
# COMPARISON.md 2.7's table.
#
# Discipline carried over from p1_layout_check.sh, each of which is there because it went wrong once: arms are built
# ONCE and saved aside (a build inside an A/B loop rewrites the underlay mid-run); servers are killed by the PID their
# own launch wrote and verified through /proc/PID/exe, never by a pattern; a forced arm whose binary is identical to
# its twin means the flag never reached the compiler and is FATAL rather than "the flag changed nothing".
# Usage: seam_attach_cost.sh [SHA] [BLOCKS]    Output: $OUT (default /tmp/seam_attach_cost.txt)
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
SHA=${1:-$(git -C "$REPO" rev-parse --short HEAD)}
BLOCKS=${2:-3}
OFF_FLAG=${OFF_FLAG:--Dtt_SEGMENT_ENABLED=0}
DUR=${DUR:-5}
SIZE=${SIZE:-p1}
SCENS=${SCENS:-"reliable_throughput best_effort_throughput"}
OUT=${OUT:-/tmp/seam_attach_cost.txt}
# What the ON arm is expected to reproduce. Before the fix this guarded against measuring a situation that had
# changed under us: ON had to come back near the 52.88/53.97 that prompted the run, or the comparison meant nothing.
# After the fix the question is the opposite one - whether ON has caught up with OFF - so the reference moves with
# it, and is passed in rather than edited, so the script keeps working for both questions.
REF_RELIABLE=${REF_RELIABLE:-52.88}
REF_BEST_EFFORT=${REF_BEST_EFFORT:-53.97}
K=$HOME/.ssh/tickle_ci_ed25519; CLIENT=10.1.1.214; SERVER=10.1.1.213
SAVE=/tmp/seamcost
srv_pid=""
LAST_SENT=""
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "${@:2}"; }
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
# Said to the file and stderr, never stdout: build_arm's stdout is captured into a variable.
note() { echo "$*" >>"$OUT"; echo "$*" >&2; }
say "  reference: ON expected near reliable=$REF_RELIABLE best_effort=$REF_BEST_EFFORT"
say "=== seam attach cost $(date -Is) sha=$SHA off='$OFF_FLAG' dur=$DUR blocks=$BLOCKS scens='$SCENS' ==="

build_arm() { # build_arm <scen> <arm-name> <extra-cflags>
    local scen=$1 name=$2 extra=$3 h out
    for h in "$CLIENT" "$SERVER"; do
        sh_ "$h" "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx -e install -e build -e log
cat > examples/perf_hil/tickle/common/BenchStats.h" <"$REPO/examples/perf_hil/tickle/common/BenchStats.h" >/dev/null \
            || { note "FATAL checkout/instrument copy failed on $h"; return 1; }
        out=$(sh_ "$h" "set -e; cd ~/tickle/examples/perf_hil/tickle && TICKLE_EXTRA_CFLAGS='$extra' ./build.sh $scen $SIZE > /tmp/seam_build.log 2>&1 || { echo BUILD_FAILED; tail -5 /tmp/seam_build.log; exit 0; }
mkdir -p $SAVE/$scen/$name && cp ${scen}_${SIZE}/client ${scen}_${SIZE}/server $SAVE/$scen/$name/ && sha256sum $SAVE/$scen/$name/client | cut -c1-16" </dev/null 2>&1 | tail -1)
        case "$out" in *BUILD_FAILED*) note "FATAL build failed for $scen arm $name on $h: $out"; return 1;; esac
    done
    note "  $scen arm $name (extra='$extra') built, client sha256=$out"
    echo "$out"
}

kill_server() {
    [ -z "${srv_pid:-}" ] && return 0
    sh_ "$SERVER" "case \"\$(readlink /proc/$srv_pid/exe 2>/dev/null)\" in $SAVE/*/*/server|\"$SAVE\"/*/*/\"server (deleted)\") kill -TERM $srv_pid;; esac
for i in 1 2 3 4 5; do [ -d /proc/$srv_pid ] || break; sleep 1; done; true" </dev/null >/dev/null
    srv_pid=""
}

assert_one_server() {
    # shellcheck disable=SC2016 # single-quoted on purpose: $p and $e expand on the Pi, not here
    sh_ "$SERVER" 'n=0; for p in /proc/[0-9]*; do e=$(readlink $p/exe 2>/dev/null) || continue; case "$e" in /tmp/seamcost/*/*/server*) n=$((n+1));; esac; done; echo $n' </dev/null
}

run_one() { # run_one <scen> <arm-name>; sets LAST_SENT
    local scen=$1 name=$2 line alive
    LAST_SENT=""
    kill_server
    srv_pid=$(sh_ "$SERVER" "cd $SAVE/$scen/$name && rm -f /tmp/seam_server.pid && (setsid sh -c 'echo \$\$ > /tmp/seam_server.pid; exec taskset -c 1-3 ./server -Q -d $((DUR + 40))' > /tmp/seam_server.log 2>&1 < /dev/null &); sleep 2; cat /tmp/seam_server.pid" </dev/null)
    sh_ "$SERVER" "grep -q 'Node open' /tmp/seam_server.log" </dev/null || { say "    no server opened for $scen arm=$name"; return 0; }
    alive=$(assert_one_server)
    [ "$alive" = 1 ] || { say "    ABORT: $alive of our servers alive, expected 1"; return 0; }
    line=$(sh_ "$CLIENT" "cd $SAVE/$scen/$name && taskset -c 1-3 ./client -Q -d $DUR 2>&1 | grep '^RESULT'" </dev/null)
    LAST_SENT=$(echo "$line" | grep -oE '(^| )sent=[0-9]+' | head -1 | cut -d= -f2)
    [ -n "$LAST_SENT" ] && say "scen=$scen arm=$name $line"
}

for scen in $SCENS; do
    say "### $scen ###"
    sha_on=$(build_arm "$scen" ON "") || exit 1
    sha_off=$(build_arm "$scen" OFF "$OFF_FLAG") || exit 1
    say "  built: ON=$sha_on OFF=$sha_off"
    if [ "$sha_on" = "$sha_off" ]; then
        say "FATAL $scen: the OFF arm's binary is identical to ON - '$OFF_FLAG' never reached the compiler."
        say "  Reporting no verdict for this scenario: it would otherwise read as 'the seam costs nothing'."
        exit 1
    fi
    for b in $(seq 1 "$BLOCKS"); do
        say "--- $scen block $b/$BLOCKS $(date -Is) ---"
        for arm in ON OFF OFF ON; do
            run_one "$scen" "$arm"
            [ -z "$LAST_SENT" ] || [ "$LAST_SENT" -ge 100000 ] || say "    VOID run $scen arm=$arm sent=$LAST_SENT"
        done
    done
    kill_server
done
say "=== done $(date -Is) ==="

REF_RELIABLE="$REF_RELIABLE" REF_BEST_EFFORT="$REF_BEST_EFFORT" python3 - "$OUT" <<'PYEOF' | tee -a "$OUT"
import math, re, sys, collections
rows = collections.defaultdict(lambda: collections.defaultdict(list))
for line in open(sys.argv[1]):
    m = re.match(r"scen=(\S+) arm=(\w+) (RESULT.*)", line.strip())
    if not m:
        continue
    f = dict(kv.split("=", 1) for kv in m.group(3).split() if "=" in kv)
    try:
        sent = int(f.get("sent", 0))
    except ValueError:
        continue
    if sent < 100000:
        continue
    for k in ("send_mbps", "cpu_s_per_Msample", "sched_cpu_s_per_Msample"):
        if k in f:
            try:
                rows[m.group(1)][(m.group(2), k)].append(float(f[k]))
            except ValueError:
                pass

def stat(v):
    n = len(v); mu = sum(v) / n
    return mu, (0.0 if n < 2 else math.sqrt(sum((x - mu) ** 2 for x in v) / (n - 1) / n)), n

# The observation this run has to reproduce before it may compare anything.
import os
OBSERVED_ON_MBPS = {"reliable_throughput": float(os.environ.get("REF_RELIABLE", "52.88")),
                    "best_effort_throughput": float(os.environ.get("REF_BEST_EFFORT", "53.97"))}

for scen, d in rows.items():
    print("\n=== %s ===" % scen)
    on = d.get(("ON", "send_mbps"), [])
    off = d.get(("OFF", "send_mbps"), [])
    if len(on) < 3 or len(off) < 3:
        print("VOID: ON has %d usable runs, OFF has %d; fewer than 3 in an arm" % (len(on), len(off)))
        continue
    mon, son, non = stat(on)
    moff, soff, noff = stat(off)
    # Checked first: the ON arm must land near the figure that prompted the run.
    ref = OBSERVED_ON_MBPS.get(scen)
    if ref is not None and abs(mon - ref) / ref > 0.15:
        print("VERDICT=VOID - ON came back %.2f Mbps against the %.2f that prompted this run (>15%% apart)." % (mon, ref))
        print("  Something other than the thing under test changed; this run says nothing either way.")
        continue
    print("  send_mbps   ON %.2f+-%.2f (n=%d)   OFF %.2f+-%.2f (n=%d)" % (mon, son, non, moff, soff, noff))
    for k in ("cpu_s_per_Msample", "sched_cpu_s_per_Msample"):
        a, b = d.get(("ON", k), []), d.get(("OFF", k), [])
        if len(a) >= 3 and len(b) >= 3:
            ma, sa, _ = stat(a); mb, sb, _ = stat(b)
            print("  %-22s ON %.3f+-%.3f   OFF %.3f+-%.3f   %+.1f%%" % (k, ma, sa, mb, sb, 100 * (mb - ma) / ma))
    den = math.hypot(son, soff)
    t = 0.0 if den == 0 else (moff - mon) / den
    pct = 100 * (moff - mon) / mon
    print("  OFF vs ON: %+.2f%%  t=%.2f" % (pct, t))
    if abs(t) <= 2 or abs(pct) <= 1.0:
        print("VERDICT=NOT_THE_SEAM - compiling the segment out does not recover the throughput (%+.2f%%, t=%.2f)." % (pct, t))
        print("  The halving is elsewhere in 9dbffd40..d4413383; the next step is a bisect over those commits.")
    elif pct > 0:
        print("VERDICT=FINDING - the seam is the cost. Compiling it out recovers %+.2f%% (t=%.2f)." % (pct, t))
        print("  Stage 1's p1-p4 figures are not measurable until the failed attach is remembered, and")
        print("  COMPARISON.md 2.7's cell-8 TickLE row is an artefact of this - including its 'a cell we lose' claim.")
    else:
        print("VERDICT=UNEXPECTED - OFF is SLOWER than ON (%+.2f%%, t=%.2f). No conclusion drawn." % (pct, t))
        print("  A segment path faster compiled in than compiled out is a third thing, and wants its own look.")
PYEOF
