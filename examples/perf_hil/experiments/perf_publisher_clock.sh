#!/usr/bin/env bash
# How many times does a same-host publisher read the clock per sample, and from where? perf_publisher_profile.sh found
# 24.8% of the p3 publisher's user cycles in the vDSO clock and could not attribute them (unwinding stops at the vDSO).
# This counts every clock_gettime() call through an LD_PRELOAD shim (clock_count_shim.c), by its caller and the caller
# above, in a frame-pointer build of the same bench, on one rig Pi, both roles there as in RESULTS S1-S3.
#
# HOW TO READ IT, written before running: counts are structural, so one rep suffices and the shim's own cost does not
# matter. The run is VOID if the client sent nothing or the segment did not carry the data (tx_shm share < 0.95), or if
# the per-site counts do not add up to at least 99% of all counted calls (the shim's table overflowed). Otherwise the
# result is calls per sent sample, by function; any function above 0.05 calls per sample is a candidate to remove.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
K=$HOME/.ssh/tickle_ci_ed25519
HOST=${HOST:-10.1.1.214}
SHA=${SHA:?set SHA to a pushed commit}
SCEN=${SCEN:-best_effort_throughput}; SIZE=${SIZE:-p3}; DUR=${DUR:-5}
OUT=${OUT:-$HOME/rig_results_safe/perf_publisher_clock_${SCEN}_${SIZE}_${SHA}_$(date +%Y%m%d-%H%M%S).txt}
SAVE=/home/ci/perf_clock
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$HOST" "$@"; }
say() { echo "$*" | tee -a "$OUT"; }
srv_pid=""
stop_server() {
    [ -z "$srv_pid" ] && return 0
    sh_ "case \"\$(readlink /proc/$srv_pid/exe 2>/dev/null)\" in $SAVE/server) kill -INT $srv_pid;; esac" </dev/null >/dev/null 2>&1
    srv_pid=""
}
trap stop_server EXIT

say "=== publisher clock count, $(date -Is), sha $SHA, $SCEN $SIZE, ${DUR}s, host $HOST ==="
sh_ "rm -rf $SAVE && mkdir -p $SAVE && cat > $SAVE/clock_count_shim.c" <"$REPO/examples/perf_hil/experiments/clock_count_shim.c"
out=$(sh_ "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx -e install -e build -e log
cd examples/perf_hil/tickle && TICKLE_EXTRA_CFLAGS='-fno-omit-frame-pointer' ./build.sh $SCEN $SIZE > /tmp/perfclk_build.log 2>&1 || { echo BUILD_FAILED; tail -5 /tmp/perfclk_build.log; exit 0; }
cp ${SCEN}_${SIZE}/client ${SCEN}_${SIZE}/server $SAVE/
cc -O2 -fno-omit-frame-pointer -shared -fPIC -o $SAVE/clock_count_shim.so $SAVE/clock_count_shim.c -ldl && echo built" </dev/null 2>&1)
case "$out" in *built*) ;; *) say "BUILD FAILED: $out"; exit 1 ;; esac
srv_pid=$(sh_ "cd $SAVE && rm -f /tmp/perfclk.pid
(setsid sh -c 'echo \$\$ > /tmp/perfclk.pid; exec env BENCH_IFACE=lo taskset -c 1 ./server -Q -d $((DUR + 30))' > /tmp/perfclk_server.log 2>&1 < /dev/null &); sleep 2; cat /tmp/perfclk.pid" </dev/null)
sh_ "cd $SAVE && rm -f /tmp/perfclk.counts && env BENCH_IFACE=lo CLOCK_COUNT_OUT=/tmp/perfclk.counts LD_PRELOAD=$SAVE/clock_count_shim.so taskset -c 2 ./client -Q -d $DUR 2>&1 | grep '^RESULT'" </dev/null >"$OUT.result" 2>&1
stop_server
sh_ "cat /tmp/perfclk.counts; echo NM; nm -C $SAVE/client | grep -i ' [tTwW] '" </dev/null >"$OUT.raw" 2>&1

python3 - "$OUT.result" "$OUT.raw" <<'PYEOF' | tee -a "$OUT"
import bisect, re, sys
res = open(sys.argv[1]).read()
f = dict(kv.split('=', 1) for kv in res.split() if '=' in kv)
sent = int(f.get('sent', 0))
shm = int(f.get('tx_shm', 0)) / max(int(f.get('tx_shm', 0)) + int(f.get('tx_udp', 0)), 1)
if sent == 0 or shm < 0.95:
    print(f'VOID: sent={sent} tx_shm share={shm:.3f}'); sys.exit(0)
counts, syms, in_nm = [], [], False
for l in open(sys.argv[2], errors='replace'):
    if l.startswith('NM'):
        in_nm = True; continue
    p = l.split()
    if in_nm and len(p) >= 3:
        syms.append((int(p[0], 16), p[2]))
    elif not in_nm and len(p) == 5:
        counts.append((p[0], p[1], p[2], p[3], int(p[4])))
syms.sort(); addrs = [a for a, _ in syms]
def name(module, off):
    if 'client' not in module:
        return module.split('/')[-1] + '+' + off
    i = bisect.bisect_right(addrs, int(off, 16)) - 1
    return syms[i][1] if i >= 0 else off
total = sum(c[4] for c in counts)
other = sum(c[4] for c in counts if c[0] == 'OTHER')
if total == 0 or (total - other) / total < 0.99:
    print(f'VOID: {other} of {total} calls past the shim table'); sys.exit(0)
by = {}
for m1, o1, m2, o2, n in counts:
    if m1 == 'OTHER':
        continue
    key = f'{name(m2, o2)} -> {name(m1, o1)}'
    by[key] = by.get(key, 0) + n
print(f'sent {sent} samples, {total} clock_gettime calls = {total / sent:.3f} per sample')
for k, n in sorted(by.items(), key=lambda kv: -kv[1]):
    print(f'  {n / sent:8.4f} per sample  {n:10d}  {k}')
PYEOF
say "=== done $(date -Is) ==="
