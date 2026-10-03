#!/usr/bin/env bash
# s6_witness_check.sh - does the loopback packet counter actually witness which transport carried the samples?
#
# WHY THIS RUNS BEFORE S6 IS BUILT. RMW_GAPS_PLAN's S6 requires every arm to prove which transport it used, vendors
# included, and the design settled on the loopback interface's own packet counter rather than each framework's
# introspection - because introspection reports what a framework was CONFIGURED with, and this project has now shipped
# four checks that read a configuration and believed it. But a witness nobody has watched discriminate is exactly the
# same defect one level up. So this validates the instrument on the one framework whose transport we can force, before
# any vendor arm is built on it.
#
# THE ARMS. Both roles on one Pi, so a segment is possible, and BENCH_IFACE=lo so the counters read the loopback
# interface rather than eth0. The arms differ only by -Dtt_SEGMENT_ENABLED=0.
#
# HOW TO READ IT, written before the run:
#   ON  wire_packets_per_sample <= 0.05 AND tx_shm > 0     -> the segment carried it and the witness saw that
#   OFF wire_packets_per_sample >= 0.80                    -> the kernel carried it and the witness saw that
#   Both together                                          -> WITNESS VALID: it discriminates, and S6 may use it
#
#   ON's witness >= 0.80 while tx_shm > 0    -> the two instruments disagree. One is wrong and S6 cannot use either
#                                               until it is known which. This is the outcome that would matter most.
#   OFF's witness <= 0.05                    -> the counter cannot see loopback traffic at all, so it is not a witness
#                                               on this interface. The design needs a different one.
#   anything between 0.05 and 0.80 on either arm -> the arm did not use one transport; report it, conclude nothing.
#   fewer than 3 usable reps on an arm       -> VOID, said explicitly.
#
# The identity check is inherited from the other harnesses and is not optional: if the two builds come out
# byte-identical the flag never arrived, and an arm that ran the same binary twice would "show no difference" for the
# wrong reason.
# Usage: s6_witness_check.sh [SHA] [REPS]     Output: $OUT (default /tmp/s6_witness.txt)
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
SHA=${1:-$(git -C "$REPO" rev-parse --short HEAD)}
REPS=${2:-3}
DUR=${DUR:-5}
SCEN=${SCEN:-best_effort_throughput}
SIZE=${SIZE:-p1}
OFF_FLAG=${OFF_FLAG:--Dtt_SEGMENT_ENABLED=0}
CLI_ARGS=${CLI_ARGS:-}   # forwarded from s6_transport_cells.sh so all three frameworks get the same client arguments
# Build flags both arms carry, for measuring a cell at a geometry other than the compiled default - SHM_PLAN
# 6e(a) does nothing at the default slot of one datagram, because a sample too large for a slot is exactly
# what it refuses to send whole. Added to BOTH arms so the ON/OFF difference stays the segment itself.
BUILD_FLAGS=${BUILD_FLAGS:-}
OUT=${OUT:-/tmp/s6_witness.txt}
HOST=10.1.1.214          # both roles on the client Pi
K=$HOME/.ssh/tickle_ci_ed25519
SAVE=/tmp/s6wit
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "${@:2}"; }
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
note() { echo "$*" >>"$OUT"; echo "$*" >&2; }
say "  BUILD_FLAGS='$BUILD_FLAGS'"
say "=== S6 witness check $(date -Is) sha=$SHA scen=$SCEN size=$SIZE dur=$DUR reps=$REPS iface=lo host=$HOST ==="

srv_pid=""
kill_server() {
    [ -z "${srv_pid:-}" ] && return 0
    sh_ "$HOST" "case \"\$(readlink /proc/$srv_pid/exe 2>/dev/null)\" in $SAVE/*/server*) kill -TERM $srv_pid;; esac
for i in 1 2 3 4 5; do [ -d /proc/$srv_pid ] || break; sleep 1; done; true" </dev/null >/dev/null
    srv_pid=""
}
trap kill_server EXIT

build_arm() { # build_arm <arm> <extra-cflags>
    local name=$1 extra=$2 out
    sh_ "$HOST" "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx -e install -e build -e log
cat > examples/perf_hil/tickle/common/BenchStats.h" <"$REPO/examples/perf_hil/tickle/common/BenchStats.h" >/dev/null 2>&1 || { note "FATAL checkout/instrument copy failed"; return 1; }
    # TICKLE_DATAGRAM_BYTES crosses the ssh explicitly. Only TICKLE_EXTRA_CFLAGS used to, so setting the
    # datagram size the way build.sh documents had no effect on the rig at all - the flag stayed in the
    # local shell and every arm compiled identically (2026-10-03). Passing it as -Dtt_MAX_BUFFER_LENGTH
    # through BUILD_FLAGS instead is refused by build.sh on purpose, because then the RESULT line's
    # datagram_bytes= label would report 1472 for a build that is not: "datagram_bytes= would misreport
    # this build". That guard is right, so the fix is to forward the variable it wants.
    out=$(sh_ "$HOST" "set -e; cd ~/tickle/examples/perf_hil/tickle && TICKLE_EXTRA_CFLAGS='$extra' TICKLE_DATAGRAM_BYTES='${TICKLE_DATAGRAM_BYTES:-}' TICKLE_RELIABLE_STATS='${TICKLE_RELIABLE_STATS:-}' TICKLE_FRAG_SLOTS='${TICKLE_FRAG_SLOTS:-}' ./build.sh $SCEN $SIZE > /tmp/s6wit_build.log 2>&1 || { echo BUILD_FAILED; tail -5 /tmp/s6wit_build.log; exit 0; }
mkdir -p $SAVE/$name && cp ${SCEN}_${SIZE}/client ${SCEN}_${SIZE}/server $SAVE/$name/ && sha256sum $SAVE/$name/client | cut -c1-16" </dev/null 2>&1)
    case "$out" in *BUILD_FAILED*|*error:*|*"No such file"*) note "FATAL build failed for $name:"; note "$out"; return 1;; esac
    out=$(printf '%s' "$out" | tail -1)
    case "$out" in [0-9a-f][0-9a-f]*) ;; *) note "FATAL no sha256 for $name: $out"; return 1;; esac
    say "  arm $name (extra='$extra') built, client sha256=$out"
    echo "$out"
}

run_one() { # run_one <arm>
    local name=$1 line
    kill_server
    srv_pid=$(sh_ "$HOST" "cd $SAVE/$name && rm -f /tmp/s6wit.pid
(setsid sh -c 'echo \$\$ > /tmp/s6wit.pid; exec env BENCH_IFACE=lo taskset -c 1 ./server -Q -d $((DUR + 40))' > /tmp/s6wit_server.log 2>&1 < /dev/null &); sleep 2; cat /tmp/s6wit.pid" </dev/null)
    sh_ "$HOST" "grep -q 'Node open' /tmp/s6wit_server.log" </dev/null || { say "    no server opened for arm=$name"; return 0; }
    # Kept in a file rather than piped straight into grep. Everything the client said that was not a RESULT
    # line used to go in the pipe's bin, so a diagnostic the publisher prints - and the publisher is the only
    # side that can refuse a whole-record send - was unreadable: on 2026-10-03 its absence from the SERVER log
    # was nearly read as "it did not refuse", when the truth was that nothing had ever captured it.
    line=$(sh_ "$HOST" "cd $SAVE/$name && env BENCH_IFACE=lo taskset -c 2 ./client -Q -d $DUR $CLI_ARGS >/tmp/s6wit_client.log 2>&1; grep '^RESULT' /tmp/s6wit_client.log" </dev/null)
    [ -n "$line" ] && say "arm=$name $line"
    kill_server
}

sha_on=$(build_arm ON "$BUILD_FLAGS") || exit 1
sha_off=$(build_arm OFF "$BUILD_FLAGS $OFF_FLAG") || exit 1
say "  built: ON=$sha_on OFF=$sha_off"
if [ "$sha_on" = "$sha_off" ]; then
    say "FATAL the OFF arm's binary is identical to ON - '$OFF_FLAG' never reached the compiler."
    say "  Refusing to report: an arm that ran the same binary twice shows no difference for the wrong reason."
    exit 1
fi

for r in $(seq 1 "$REPS"); do
    say "--- rep $r/$REPS $(date -Is) ---"
    for arm in ON OFF; do run_one "$arm"; done
done
say "=== done $(date -Is) ==="

python3 - "$OUT" <<'PYEOF' | tee -a "$OUT"
import re, sys, collections, statistics as st
rows = collections.defaultdict(list)
for line in open(sys.argv[1]):
    m = re.match(r"arm=(\w+) (RESULT:.*)", line.strip())
    if not m:
        continue
    f = dict(kv.split("=", 1) for kv in m.group(2).split() if "=" in kv)
    if f.get("role") != "client":
        continue
    try:
        if int(f.get("sent", "0")) < 100000:
            continue
    except ValueError:
        continue
    rows[m.group(1)].append(f)

def num(f, k, d=0.0):
    try:
        return float(str(f.get(k, d)).replace(",", ""))
    except ValueError:
        return d

verdicts = []
for arm in ("ON", "OFF"):
    rs = rows.get(arm, [])
    print("\n=== arm %s, %d usable reps ===" % (arm, len(rs)))
    if len(rs) < 3:
        print("  VOID: fewer than 3 usable reps"); verdicts.append((arm, None)); continue
    w = [num(f, "wire_packets_per_sample") for f in rs]
    shm = [num(f, "tx_shm") for f in rs]
    udp = [num(f, "tx_udp") for f in rs]
    print("  wire_packets_per_sample  %.4f  (reps: %s)" % (st.mean(w), ", ".join("%.4f" % x for x in w)))
    print("  iface=%s  tx_shm mean %.0f   tx_udp mean %.0f" % (rs[0].get("iface", "?"), st.mean(shm), st.mean(udp)))
    # send_mbps is the PUBLISHER's rate, and since 2026-09-29 a full ring DROPS the datagram rather than
    # rerouting it (BenchStats.h), so a rep with shm_full_dropped > 0 counts samples that were never
    # delivered. On 2026-10-03 this printed one mean over three p4 BEST_EFFORT reps of which two had
    # dropped ~16% of their datagrams - and those two read HIGHER (12,987 and 12,679 Mbps) than the clean
    # one (11,310), because dropping is cheaper than delivering. A throughput figure that rises as
    # delivery fails is not a throughput figure, and it was about to be published as one.
    #
    # So the drop-free reps are the measurement and the rest are reported, not averaged in and not
    # silently removed - the same rule the harnesses beside this one follow for a thin arm.
    clean = [f for f in rs if num(f, "shm_full_dropped") == 0]
    dirty = [f for f in rs if num(f, "shm_full_dropped") > 0]
    if dirty:
        print("  %d/%d reps DROPPED at the ring (shm_full_dropped=%s) - those are not throughput" %
              (len(dirty), len(rs), ", ".join("%.0f" % num(f, "shm_full_dropped") for f in dirty)))
    if clean:
        mbps = [num(f, "send_mbps") for f in clean]
        print("  send_mbps %.2f  (n=%d drop-free reps: %s)" %
              (st.mean(mbps), len(clean), ", ".join("%.0f" % x for x in mbps)))
        if len(clean) < 3:
            print("    NOTE: fewer than 3 drop-free reps, so this figure has no spread worth quoting.")
    else:
        print("  send_mbps VOID: every rep dropped at the ring. The ring is undersized for this cell, and")
        print("    THAT is this cell's result - not a rate taken while %d%% of the datagrams were discarded." %
              round(100.0 * st.mean([num(f, "shm_full_dropped") for f in rs]) /
                    max(1.0, st.mean([num(f, "tx_shm") for f in rs]))))
    verdicts.append((arm, (st.mean(w), st.mean(shm), st.mean(udp))))

d = dict(verdicts)
print()
if d.get("ON") is None or d.get("OFF") is None:
    print("VERDICT=VOID - an arm had too few usable reps, so the witness was not exercised both ways.")
    sys.exit(0)
won, son, _ = d["ON"]
woff, _, _ = d["OFF"]
if woff <= 0.05:
    print("VERDICT=NOT_A_WITNESS - the OFF arm put %.4f packets per sample on lo, so the counter cannot see" % woff)
    print("  loopback traffic at all on this interface. S6 needs a different witness; do not build on this one.")
elif won >= 0.80 and son > 0:
    print("VERDICT=INSTRUMENTS_DISAGREE - the ON arm reports tx_shm=%.0f while the witness saw %.4f packets per" % (son, won))
    print("  sample on lo. One of the two is wrong and S6 cannot use either until it is known which.")
elif won <= 0.05 and woff >= 0.80:
    print("VERDICT=WITNESS_VALID - it discriminates: %.4f packets per sample with the segment, %.4f without," % (won, woff))
    print("  and our own tx_shm agrees. S6 may use the loopback counter as its transport witness for every framework.")
else:
    print("VERDICT=INCONCLUSIVE - ON %.4f, OFF %.4f. At least one arm is in the middle band, meaning it did not use" % (won, woff))
    print("  one transport. Report the numbers and conclude nothing about the witness.")
PYEOF
