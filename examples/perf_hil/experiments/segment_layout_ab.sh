#!/usr/bin/env bash
# Does padding the segment header's shared indices to their own cache lines (segment version 5) make a same-host
# publisher faster? perf on the rig put 8.3% of a p3 publisher's user cycles in its load of reader_waiting and 4% in
# the write_index CAS - three fields on one cache line, written by different cores (perf_publisher_profile.sh).
#
# A = the build before the change, B = the build with it. Both roles on one rig Pi (BENCH_IFACE=lo), best_effort
# throughput at max rate, alternated A B B A per round so drift lands on both.
#   seg arm - the default build: the segment carries the data. The arm the change is for.
#   udp arm - the same two commits built with -Dtt_SEGMENT_ENABLED=0: no segment at all, so the change cannot touch it.
#             Its A/B delta is the layout floor of this comparison (code moved, nothing else).
#
# HOW TO READ IT, written before running and enforced below:
#   VOID run: sent == 0; a seg run whose tx_shm share is < 0.95; a udp run with any tx_shm.
#   VOID overall: fewer than 3 valid runs of any (arm, build).
#   For each arm: rate B/A and publisher user us/sample B/A, means over valid runs, with 2 x SE of the difference.
#   BETTER  if seg's rate gain exceeds 2 x SE AND exceeds udp's own |B/A - 1| by more than 1 point (the layout floor);
#   WORSE   symmetric;  otherwise HELD. The user-time verdict is computed the same way and reported beside it.
#   Falsification: a HELD or WORSE seg rate means the false sharing was not what limited the rate, whatever perf said.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
K=$HOME/.ssh/tickle_ci_ed25519
HOST=${HOST:-10.1.1.214}
SHA_A=${SHA_A:?before}; SHA_B=${SHA_B:?after}
SCEN=${SCEN:-best_effort_throughput}; SIZE=${SIZE:-p3}; DUR=${DUR:-8}; ROUNDS=${ROUNDS:-4}
OUT=${OUT:-$HOME/rig_results_safe/segment_layout_ab_${SIZE}_${SHA_A}_${SHA_B}_$(date +%Y%m%d-%H%M%S).txt}
SAVE=/home/ci/seglayout
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$HOST" "$@"; }
say() { echo "$*" | tee -a "$OUT"; }
srv_pid=""
stop_server() {
    [ -z "$srv_pid" ] && return 0
    sh_ "case \"\$(readlink /proc/$srv_pid/exe 2>/dev/null)\" in $SAVE/*/server) kill -INT $srv_pid;; esac" </dev/null >/dev/null 2>&1
    srv_pid=""
}
trap stop_server EXIT

say "=== segment layout A/B, $(date -Is), A $SHA_A, B $SHA_B, $SCEN $SIZE, ${DUR}s, $ROUNDS rounds of A B B A, host $HOST ==="
sh_ "rm -rf $SAVE && mkdir -p $SAVE" </dev/null
for b in A B; do
    sha=$SHA_A; [ "$b" = B ] && sha=$SHA_B
    for arm in seg udp; do
        flags=""; [ "$arm" = udp ] && flags="-Dtt_SEGMENT_ENABLED=0"
        out=$(sh_ "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $sha && git clean -fdqx -e install -e build -e log
cd examples/perf_hil/tickle && TICKLE_EXTRA_CFLAGS='$flags' ./build.sh $SCEN $SIZE > /tmp/seglay_build.log 2>&1 || { echo BUILD_FAILED; tail -5 /tmp/seglay_build.log; exit 0; }
mkdir -p $SAVE/${b}_$arm && cp ${SCEN}_${SIZE}/client ${SCEN}_${SIZE}/server $SAVE/${b}_$arm/ && sha256sum $SAVE/${b}_$arm/client | cut -c1-16" </dev/null 2>&1)
        case "$out" in *BUILD_FAILED*) say "BUILD FAILED ${b}_$arm: $out"; exit 1 ;; esac
        say "  built ${b}_$arm at $sha (client $out)"
    done
done

run_one() { # build arm round
    local dir="$SAVE/$1_$2"
    srv_pid=$(sh_ "cd $dir && rm -f /tmp/seglay.pid
(setsid sh -c 'echo \$\$ > /tmp/seglay.pid; exec env BENCH_IFACE=lo taskset -c 1 ./server -Q -d $((DUR + 30))' > /tmp/seglay_server.log 2>&1 < /dev/null &); sleep 2; cat /tmp/seglay.pid" </dev/null)
    local res
    res=$(sh_ "cd $dir && env BENCH_IFACE=lo taskset -c 2 ./client -Q -d $DUR 2>&1 | grep '^RESULT'" </dev/null)
    stop_server
    sleep 1
    echo "RUN build=$1 arm=$2 round=$3 $res" >>"$OUT"
}
for r in $(seq 1 "$ROUNDS"); do
    for b in A B B A; do
        run_one "$b" seg "$r"
        run_one "$b" udp "$r"
    done
    say "  $(date +%T) round $r done"
done

python3 - "$OUT" <<'PYEOF' | tee -a "$OUT.verdict"
import math, re, statistics as st, sys
runs = {}
void = []
for l in open(sys.argv[1]):
    m = re.match(r'RUN build=(\w) arm=(\w+) round=(\d+) (.*)', l)
    if not m:
        continue
    f = dict(kv.split('=', 1) for kv in m.group(4).split() if '=' in kv)
    b, arm = m.group(1), m.group(2)
    sent = int(f.get('sent', 0))
    shm = int(f.get('tx_shm', 0))
    tot = shm + int(f.get('tx_udp', 0))
    if sent == 0 or (arm == 'seg' and shm / max(tot, 1) < 0.95) or (arm == 'udp' and shm > 0):
        void.append(f'{b} {arm} round {m.group(3)}: sent={sent} tx_shm={shm}/{tot}')
        continue
    rate = sent / float(f['elapsed_s'])
    us = float(f['utime_s']) / sent * 1e6
    runs.setdefault((arm, b), []).append((rate, us))
for v in void:
    print('VOID run', v)
if any(len(runs.get((a, b), [])) < 3 for a in ('seg', 'udp') for b in ('A', 'B')):
    print('VOID OVERALL: fewer than 3 valid runs in some (arm, build)'); sys.exit(0)
def delta(arm, idx):
    a = [x[idx] for x in runs[(arm, 'A')]]
    b = [x[idx] for x in runs[(arm, 'B')]]
    se = math.sqrt(st.variance(a) / len(a) + st.variance(b) / len(b))
    return st.mean(a), st.mean(b), st.mean(b) - st.mean(a), se
res = {}
for arm in ('seg', 'udp'):
    for idx, name in ((0, 'rate/s'), (1, 'user us/sample')):
        ma, mb, d, se = delta(arm, idx)
        res[(arm, idx)] = (ma, mb, d, se)
        print(f'{arm} {name}: A {ma:.4g}  B {mb:.4g}  B/A {mb / ma:.4f}  diff {d:+.4g}  2SE {2 * se:.4g}')
for idx, name, better_up in ((0, 'rate', True), (1, 'user time', False)):
    ma, mb, d, se = res[('seg', idx)]
    ua, ub, _, _ = res[('udp', idx)]
    floor = abs(ub / ua - 1) + 0.01
    gain = (mb / ma - 1) if better_up else (1 - mb / ma)
    if abs(d) > 2 * se and gain > floor:
        v = 'BETTER'
    elif abs(d) > 2 * se and -gain > floor:
        v = 'WORSE'
    else:
        v = 'HELD'
    print(f'VERDICT {name}: {v} (seg {gain * 100:+.2f}% against a floor of {floor * 100:.2f}%)')
PYEOF
say "=== done $(date -Is) ==="
