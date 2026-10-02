#!/usr/bin/env bash
# segment_attach_hint.sh - does letting discovery clear the negative attach cache actually attach sooner?
#
# WHAT THIS IS FOR. COMPARISON 2.2c measured that in a same-host ping/pong every repetition read
# shm_attach_absent=1, tx_udp_unattached=257, shm_attach_ok=1: the first attach lost a race with the peer
# building its own segment, and tt_SEGMENT_ATTACH_RETRY_SENDS (256) then held that "no" for 256 more sends.
# note_same_host_peer() now clears the cached miss, because an announce from our own address is exactly the
# peer that is about to have a segment. This asks whether that changed anything.
#
# WHY TWO RATES, which is the point of the design. The countdown is in SENDS and the announce is in TIME
# (tt_CONTEXT_UPDATE_INTERVAL, 1 s), so which one expires first depends entirely on the send rate:
#
#   200/s (-i 0.005)   256 sends = 1.28 s   announce = 1 s    the hint saves ~56 sends: a small gain
#    20/s (-i 0.05)    256 sends = 12.8 s   announce = 1 s    the hint saves ~236: the cell that matters
#
# A 10 s run at 20/s sends 200 samples, FEWER than the countdown, so the unfixed build can never attach at
# all in it. Measuring only at 200/s would have shown a ~20% gain and read as "modest", which is the wrong
# conclusion about a fix whose whole point is the low-rate case - a request/reply service doing a handful of
# calls a second is the shape that never reached 256 sends.
#
# HOW TO READ IT, written before the run:
#   LOW rate:  before tx_shm == 0 (never attached) and after tx_shm > 0          -> the hint works, and this
#              is the result. Report the attach point (tx_udp_unattached) for both.
#   HIGH rate: after tx_udp_unattached < before, by roughly the 56 sends the arithmetic predicts -> consistent.
#              A small gain here is the EXPECTED outcome, not a disappointment.
#   Both rates unchanged  -> the hint never fired. The change is inert and must not be shipped as a fix;
#              find out whether the announce is reached at all before touching the countdown again.
#   after WORSE than before at either rate -> report it first; the hint cost something nobody predicted.
#
#   RTT is NOT the metric here and a worse ON-arm RTT is not a regression from this change. The ON arm's
#   mixture becomes purer as the segment attaches sooner, and 2.2c already measured our segment path as the
#   slower one at p2 (+8.7% against our own kernel arm). More segment means more of a cost already recorded.
#
# The identity check is not optional: two SHAs must produce two different binaries, or an arm ran the other's
# build and "no difference" would mean nothing.
# Usage: BEFORE=<sha> AFTER=<sha> segment_attach_hint.sh     Output: $OUT
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
BEFORE=${BEFORE:?set BEFORE=<sha before the fix>}
AFTER=${AFTER:?set AFTER=<sha with the fix>}
REPS=${REPS:-3}
DUR=${DUR:-10}
SCEN=${SCEN:-reliable_latency}
SIZE=${SIZE:-p2}
RATES=${RATES:-"0.005 0.05"}
OUT=${OUT:-$HOME/rig_results_safe/segment_attach_hint.txt}
HOST=10.1.1.214
K=$HOME/.ssh/tickle_ci_ed25519
SAVE=/tmp/attachhint
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "${@:2}"; }
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }
say "=== segment attach hint $(date -Is) before=$BEFORE after=$AFTER scen=$SCEN size=$SIZE dur=$DUR reps=$REPS rates='$RATES' host=$HOST ==="

srv_pid=""
kill_server() {
    [ -z "${srv_pid:-}" ] && return 0
    sh_ "$HOST" "case \"\$(readlink /proc/$srv_pid/exe 2>/dev/null)\" in $SAVE/*/server*) kill -TERM $srv_pid;; esac
for i in 1 2 3 4 5; do [ -d /proc/$srv_pid ] || break; sleep 1; done; true" </dev/null >/dev/null
    srv_pid=""
}
trap kill_server EXIT

build_arm() { # build_arm <name> <sha>
    local name=$1 sha=$2 out
    out=$(sh_ "$HOST" "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $sha && git clean -fdqx -e install -e build -e log
cd examples/perf_hil/tickle && ./build.sh $SCEN $SIZE > /tmp/attachhint_build.log 2>&1 || { echo BUILD_FAILED; tail -5 /tmp/attachhint_build.log; exit 0; }
mkdir -p $SAVE/$name && cp ${SCEN}_${SIZE}/client ${SCEN}_${SIZE}/server $SAVE/$name/ && sha256sum $SAVE/$name/client | cut -c1-16" </dev/null 2>&1)
    case "$out" in *BUILD_FAILED*|*error:*|*"No such file"*) say "FATAL build failed for $name:"; say "$out"; return 1;; esac
    out=$(printf '%s' "$out" | tail -1)
    case "$out" in [0-9a-f][0-9a-f]*) ;; *) say "FATAL no sha256 for $name: $out"; return 1;; esac
    say "  arm $name ($sha) built, client sha256=$out"
    echo "$out"
}

run_one() { # run_one <arm> <rate>
    local name=$1 rate=$2 line
    kill_server
    srv_pid=$(sh_ "$HOST" "cd $SAVE/$name && rm -f /tmp/attachhint.pid
(setsid sh -c 'echo \$\$ > /tmp/attachhint.pid; exec env BENCH_IFACE=lo taskset -c 1 ./server -Q -d $((DUR + 40))' > /tmp/attachhint_server.log 2>&1 < /dev/null &); sleep 2; cat /tmp/attachhint.pid" </dev/null)
    sh_ "$HOST" "grep -q 'Node open' /tmp/attachhint_server.log" </dev/null || { say "    no server opened for arm=$name rate=$rate"; return 0; }
    line=$(sh_ "$HOST" "cd $SAVE/$name && env BENCH_IFACE=lo taskset -c 2 ./client -Q -d $DUR -i $rate 2>&1 | grep '^RESULT'" </dev/null)
    [ -n "$line" ] && say "arm=$name rate=$rate $line"
    kill_server
}

sha_before=$(build_arm before "$BEFORE") || exit 1
sha_after=$(build_arm after "$AFTER") || exit 1
if [ "$sha_before" = "$sha_after" ]; then
    say "FATAL the two arms' binaries are identical - the fix is not in $AFTER, or both checkouts resolved the same tree."
    say "  Refusing to report: an arm that ran the other's build shows no difference for the wrong reason."
    exit 1
fi

for rate in $RATES; do
    for r in $(seq 1 "$REPS"); do
        say "--- rate $rate rep $r/$REPS $(date -Is) ---"
        for arm in before after; do run_one "$arm" "$rate"; done
    done
done
say "=== done $(date -Is) ==="

python3 - "$OUT" <<'PYEOF' | tee -a "$OUT"
import re, sys, collections, statistics as st
rows = collections.defaultdict(list)
for line in open(sys.argv[1]):
    m = re.match(r"arm=(\w+) rate=([0-9.]+) (RESULT:.*)", line.strip())
    if not m:
        continue
    f = dict(kv.split("=", 1) for kv in m.group(3).split() if "=" in kv)
    if "tx_shm" not in f:
        continue
    rows[(m.group(2), m.group(1))].append(f)

def med(rs, k):
    v = []
    for r in rs:
        try:
            v.append(float(str(r.get(k, "")).replace(",", "")))
        except ValueError:
            pass
    return st.median(v) if v else float("nan")

print()
print("=== attach point, by send rate ===")
print("%-8s %-7s %3s %10s %10s %10s %12s %10s" % ("rate", "arm", "n", "sent", "tx_shm", "tx_udp", "unattached", "attempts"))
rates = sorted({k[0] for k in rows}, key=float)
for rate in rates:
    for arm in ("before", "after"):
        rs = rows.get((rate, arm), [])
        if not rs:
            print("%-8s %-7s   0   (no usable reps)" % (rate, arm)); continue
        print("%-8s %-7s %3d %10.0f %10.0f %10.0f %12.0f %10.0f"
              % (rate, arm, len(rs), med(rs, "sent"), med(rs, "tx_shm"), med(rs, "tx_udp"),
                 med(rs, "tx_udp_unattached"), med(rs, "shm_attach_attempts")))
print()
for rate in rates:
    b, a = rows.get((rate, "before"), []), rows.get((rate, "after"), [])
    if len(b) < 2 or len(a) < 2:
        print("  rate %s: VOID, fewer than 2 usable reps on an arm" % rate); continue
    bs, as_ = med(b, "tx_shm"), med(a, "tx_shm")
    bu, au = med(b, "tx_udp_unattached"), med(a, "tx_udp_unattached")
    if bs == 0 and as_ > 0:
        print("  rate %s: the unfixed build NEVER attached (tx_shm 0); with the hint it attached after %.0f sends."
              % (rate, au))
    elif au < bu:
        print("  rate %s: attached %.0f sends earlier (%.0f -> %.0f unattached)." % (rate, bu - au, bu, au))
    elif au > bu:
        print("  rate %s: WORSE - attached %.0f sends LATER (%.0f -> %.0f). Report this first."
              % (rate, au - bu, bu, au))
    else:
        print("  rate %s: unchanged (%.0f unattached both arms)." % (rate, bu))
print()
print("  If every rate is unchanged the hint never fired: the change is inert and is not a fix.")
print("  A worse ON-arm RTT is not a regression here - a purer segment mixture costs what 2.2c already measured.")
PYEOF
