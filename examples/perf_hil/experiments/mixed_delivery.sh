#!/usr/bin/env bash
# One publisher, two subscribers: one on the publisher's own host (the segment), one on the other Pi (the network).
# The user's question of 2026-10-05: does a stream that goes both ways at once stay correct, and what does each
# subscriber get compared with having the publisher to itself?
#
# Arms, rotated within every rep so drift lands on all three:
#   MIXED        client + local subscriber on 10.1.1.214, remote subscriber on 10.1.1.213
#   LOCAL_ONLY   client + local subscriber only          (the shared-memory control)
#   REMOTE_ONLY  client + remote subscriber only          (the network control)
#
# Reading rules, written before the run and enforced below:
#   WITNESS (MIXED) - the client's tx_shm AND tx_udp are both > 0, the local subscriber's rx_shm share is >= 0.99 and the
#     remote one's rx_udp share >= 0.99. Otherwise the arm did not take the two paths it is named for: VOID.
#   CORRECTNESS - RELIABLE: every subscriber post_match_lost=0 and the client write_fail=0. BEST_EFFORT: each
#     subscriber's loss is reported beside its rate; a BEST_EFFORT loss is a result, not a failure.
#     post_match_lost, not lost: `lost` also counts the samples published before that subscriber was matched
#     (prematch_window), which a VOLATILE subscriber never gets by design. The first run read MIXED's local subscriber
#     as losing 70-75 RELIABLE samples per rep; all of them were prematch (first_seq=76) - the publisher started on the
#     remote subscriber's match and the local one matched a moment later. Reported apart, not counted as loss.
#   PERFORMANCE - each MIXED subscriber's delivered rate against the same subscriber alone (LOCAL_ONLY / REMOTE_ONLY):
#     a difference counts only if the ranges do not overlap.
#   A rep where any expected RESULT line is missing is VOID and said, never read as zero.
#
# FRAMEWORK=fastdds runs the same three arms with FastDDS as shipped (data-sharing) plus its shared-memory and eth0 UDP
# transports (fastdds_shm_and_eth0.xml). It has no tx_shm/tx_udp counters, so the WITNESS rule is TickLE's only; for
# FastDDS the remote subscriber's eth0 packets per received sample (wire_rx_packets / recv) are printed instead, and must
# be > 0. FastDDS's subscriber uses the default reader QoS (KEEP_LAST 1), so on one host a fast writer overwrites what the
# reader has not taken yet: its delivered rate, not the client's send rate, is what this harness reports.
#
# Usage: SHA=<sha> FRAMEWORK=tickle|fastdds SCEN=best_effort_throughput SIZE=p2 CLI_ARGS="" REPS=3 DUR=5 mixed_delivery.sh
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
SHA=${SHA:-$(git -C "$REPO" rev-parse --short HEAD)}
FRAMEWORK=${FRAMEWORK:-tickle}
SCEN=${SCEN:-best_effort_throughput}
SIZE=${SIZE:-p2}
CLI_ARGS=${CLI_ARGS:-}
REPS=${REPS:-3}
DUR=${DUR:-5}
OUT=${OUT:-$HOME/rig_results_safe/mixed_delivery.txt}
K=$HOME/.ssh/tickle_ci_ed25519
PUB_HOST=10.1.1.214
REMOTE_HOST=10.1.1.213
SAVE=/tmp/mixdel_$FRAMEWORK
FDIR=/home/ci/tickle/examples/perf_hil/fastdds
FENV=""
[ "$FRAMEWORK" = fastdds ] && FENV="env BENCH_IFACE=eth0 LD_LIBRARY_PATH=/opt/ros/jazzy/lib FASTRTPS_DEFAULT_PROFILES_FILE=$FDIR/fastdds_shm_and_eth0.xml"
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "${@:2}"; }
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }

stop_servers() { # by the executable under $SAVE, never by pattern
    for h in "$PUB_HOST" "$REMOTE_HOST"; do
        sh_ "$h" "for p in /proc/[0-9]*; do case \"\$(readlink \$p/exe 2>/dev/null)\" in $SAVE/server) kill -INT \${p#/proc/};; esac; done" \
            </dev/null >/dev/null 2>&1
    done
}
trap stop_servers EXIT

say "=== mixed delivery $(date -Is) sha=$SHA framework=$FRAMEWORK scen=$SCEN size=$SIZE cli_args='$CLI_ARGS' reps=$REPS dur=${DUR}s ==="
for h in "$PUB_HOST" "$REMOTE_HOST"; do
    out=$(sh_ "$h" "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx -e install -e build -e log
cd examples/perf_hil/$FRAMEWORK && ./build.sh $SCEN $SIZE > /tmp/mixdel_build.log 2>&1 || { echo BUILD_FAILED; tail -3 /tmp/mixdel_build.log; exit 0; }
mkdir -p $SAVE && cp ${SCEN}_${SIZE}/client ${SCEN}_${SIZE}/server $SAVE/ && sha256sum $SAVE/client | cut -c1-16" </dev/null 2>&1 | tail -1)
    case "$out" in *BUILD_FAILED*) say "FATAL build on $h: $out"; exit 1;; esac
    say "  built on $h: client sha256=$out"
done

start_server() { # start_server <host> <cpus> <tag>
    sh_ "$1" "cd $SAVE && rm -f /tmp/mixdel_$3.log && (setsid sh -c 'exec $FENV taskset -c $2 ./server $CLI_ARGS -d $((DUR + 6))' > /tmp/mixdel_$3.log 2>&1 < /dev/null &)" </dev/null
}
read_server() { # read_server <host> <tag>
    sh_ "$1" "for i in \$(seq 1 25); do grep -q '^RESULT' /tmp/mixdel_$2.log && break; sleep 1; done; grep '^RESULT' /tmp/mixdel_$2.log" </dev/null
}

run_arm() { # run_arm <arm> <rep>
    local arm=$1 rep=$2 c l r
    stop_servers
    case "$arm" in MIXED|LOCAL_ONLY) start_server "$PUB_HOST" 1 local;; esac
    case "$arm" in MIXED|REMOTE_ONLY) start_server "$REMOTE_HOST" 1-3 remote;; esac
    sleep 2
    c=$(sh_ "$PUB_HOST" "cd $SAVE && $FENV taskset -c 2 ./client $CLI_ARGS -d $DUR 2>&1 | grep '^RESULT'" </dev/null)
    say "arm=$arm rep=$rep who=client ${c:-NORESULT}"
    case "$arm" in MIXED|LOCAL_ONLY) l=$(read_server "$PUB_HOST" local); say "arm=$arm rep=$rep who=local ${l:-NORESULT}";; esac
    case "$arm" in MIXED|REMOTE_ONLY) r=$(read_server "$REMOTE_HOST" remote); say "arm=$arm rep=$rep who=remote ${r:-NORESULT}";; esac
    stop_servers
}

for rep in $(seq 1 "$REPS"); do
    case $((rep % 3)) in
    1) order="MIXED LOCAL_ONLY REMOTE_ONLY" ;;
    2) order="LOCAL_ONLY REMOTE_ONLY MIXED" ;;
    *) order="REMOTE_ONLY MIXED LOCAL_ONLY" ;;
    esac
    for arm in $order; do run_arm "$arm" "$rep"; done
done
say "=== done $(date -Is) ==="

python3 - "$OUT" "$SCEN" "$FRAMEWORK" <<'PYEOF' | tee -a "$OUT"
import re, sys, statistics as st
path, scen = sys.argv[1], sys.argv[2]
framework = sys.argv[3] if len(sys.argv) > 3 else "tickle"
reliable = "reliable" in scen
rows, void = {}, []
for line in open(path):
    m = re.match(r"arm=(\S+) rep=(\d+) who=(\S+) (.*)", line.rstrip())
    if not m:
        continue
    arm, rep, who, rest = m.groups()
    if rest.startswith("NORESULT"):
        void.append(f"{arm} rep {rep}: no RESULT from {who}")
        continue
    f = dict(kv.split("=", 1) for kv in rest.split() if "=" in kv)
    rows.setdefault((arm, rep), {})[who] = f

def share(f, a, b):
    x, y = int(f.get(a, 0)), int(f.get(b, 0))
    return x / (x + y) if x + y else 0.0

stats = {}
for (arm, rep), w in sorted(rows.items()):
    c = w.get("client")
    if c is None:
        continue
    el = float(c.get("elapsed_s", "0") or 0) or 1.0
    note = []
    if arm == "MIXED" and framework != "tickle":
        # FastDDS's RESULT carries wire_rx_packets (the subscriber's own eth0 count), not a per-sample field - the
        # first run read a field that does not exist, got 0, and voided every MIXED rep for a reason that was ours.
        rf = w.get("remote", {})
        rp = int(rf.get("wire_rx_packets", "0") or 0) / max(int(rf.get("recv", "0") or 0), 1)
        print(f"  witness ({framework}) MIXED rep {rep}: remote eth0 packets per sample {rp:.3f}")
        if rp <= 0:
            void.append(f"MIXED rep {rep}: the remote subscriber saw no eth0 packets")
            continue
    if arm == "MIXED" and framework == "tickle":
        if not (int(c.get("tx_shm", 0)) > 0 and int(c.get("tx_udp", 0)) > 0):
            void.append(f"MIXED rep {rep}: tx_shm={c.get('tx_shm')} tx_udp={c.get('tx_udp')} - not both paths")
            continue
        if "local" in w and share(w["local"], "rx_shm", "rx_udp") < 0.99:
            void.append(f"MIXED rep {rep}: local rx_shm share {share(w['local'], 'rx_shm', 'rx_udp'):.3f}")
            continue
        if "remote" in w and share(w["remote"], "rx_udp", "rx_shm") < 0.99:
            void.append(f"MIXED rep {rep}: remote rx_udp share {share(w['remote'], 'rx_udp', 'rx_shm'):.3f}")
            continue
    for who in ("local", "remote"):
        if who not in w:
            continue
        s = w[who]
        recv = int(s.get("recv", 0)); lost = int(s.get("post_match_lost", s.get("lost", 0)))
        pre = int(s.get("prematch_window", 0))
        rate = recv / el / 1e3
        stats.setdefault((arm, who), []).append((rate, lost, float(s.get("loss_pct", 0)), int(c.get("write_fail", 0)), pre))
print()
if void:
    print("VOID reps:")
    for v in void:
        print("  " + v)
for (arm, who), v in sorted(stats.items()):
    r = [x[0] for x in v]
    print(f"{arm:12} {who:6} n={len(v)} delivered {st.mean(r):8.1f} k/s ({min(r):.1f}..{max(r):.1f})"
          f"  post-match lost {[x[1] for x in v]}  prematch {[x[4] for x in v]}  loss% {[x[2] for x in v]}"
          f"  client write_fail {[x[3] for x in v]}")
print()
bad = [k for k, v in stats.items() if reliable and any(x[1] or x[3] for x in v)]
if reliable:
    print("CORRECTNESS: " + ("PASS - every RELIABLE subscriber lost 0 and the client gave up nothing" if not bad
                             else "FAIL - " + ", ".join(f"{a}/{w}" for a, w in bad)))
for who, alone in (("local", "LOCAL_ONLY"), ("remote", "REMOTE_ONLY")):
    a, b = stats.get(("MIXED", who)), stats.get((alone, who))
    if not a or not b or len(a) < 3 or len(b) < 3:
        print(f"{who}: fewer than 3 usable reps in MIXED or {alone} - no performance verdict")
        continue
    ra, rb = [x[0] for x in a], [x[0] for x in b]
    sep = min(ra) > max(rb) or max(ra) < min(rb)
    print(f"{who}: MIXED {st.mean(ra):.1f} k/s vs {alone} {st.mean(rb):.1f} k/s = {st.mean(ra)/st.mean(rb):.3f}x"
          f" ({'SEPARABLE' if sep else 'ranges overlap'})")
PYEOF
