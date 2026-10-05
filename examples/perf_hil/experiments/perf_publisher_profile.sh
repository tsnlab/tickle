#!/usr/bin/env bash
# Where does a same-host publisher's user time go? (ROADMAP "Now" 3: ~0.65 us of user time per sample at p3, the
# largest unexplained cost.) perf record of the best_effort_throughput client (the publisher) on one rig Pi, both
# roles on that Pi so the segment carries the data, as in RESULTS S1-S3. perf became usable on 2026-10-05
# (kernel.perf_event_paranoid=1 on both Pis, linux-tools 6.8.12).
#
# Two arms per rep, same binaries, alternated:
#   plain - the client alone: the rate and the user time the profile is converted against;
#   perf  - the client under `perf record -e cycles:u -F $FREQ`: the profile.
#
# HOW TO READ IT, written before running and enforced below:
#   VOID rep: either arm delivered nothing, the segment did not carry the data (tx_shm share < 0.95), or the perf arm's
#     send rate is not within 10% of the plain arm's (perf would then be profiling a different program).
#   VOID overall: fewer than 2 valid reps, or the profile holds fewer than 10,000 samples in total.
#   Per symbol: its share of the client's user-mode cycles (perf report --no-children, symbol level), converted to
#     us per sample as share x plain utime_s / plain sent, median over valid reps. A symbol whose share differs by more
#     than 3 percentage points between reps is marked UNSTABLE rather than averaged into a claim.
#   The breakdown is published only as "what the user time is spent in"; it decides nothing by itself. A follow-up
#     change is chosen from it and measured on its own, against its own control.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
K=$HOME/.ssh/tickle_ci_ed25519
HOST=${HOST:-10.1.1.214}
SHA=${SHA:?set SHA to a pushed commit}
SCEN=${SCEN:-best_effort_throughput}; SIZE=${SIZE:-p3}; DUR=${DUR:-10}; REPS=${REPS:-3}; FREQ=${FREQ:-2999}
# CALLGRAPH=1: DWARF call graphs, and a caller report for tt_get_ns (who reads the clock, and how often). DWARF unwinding
# costs the profiled process more, so a CALLGRAPH run's rate check (10%) decides whether its attribution is usable.
CALLGRAPH=${CALLGRAPH:-0}
CG=""
[ "$CALLGRAPH" = 1 ] && CG="--call-graph dwarf,8192"
OUT=${OUT:-$HOME/rig_results_safe/perf_publisher_${SCEN}_${SIZE}_${SHA}_$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT.d"
SUM="$OUT.txt"
SAVE=/home/ci/perf_pub
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$HOST" "$@"; }
say() { echo "$*" | tee -a "$SUM"; }
srv_pid=""
stop_server() {
    [ -z "$srv_pid" ] && return 0
    sh_ "case \"\$(readlink /proc/$srv_pid/exe 2>/dev/null)\" in $SAVE/server) kill -INT $srv_pid;; esac" </dev/null >/dev/null 2>&1
    srv_pid=""
}
trap stop_server EXIT

say "=== publisher perf profile, $(date -Is), sha $SHA, $SCEN $SIZE, ${DUR}s x $REPS reps, perf -F $FREQ, host $HOST ==="
sh_ "perf --version; cat /proc/sys/kernel/perf_event_paranoid" </dev/null | sed 's/^/  /' | tee -a "$SUM"
out=$(sh_ "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx -e install -e build -e log
cd examples/perf_hil/tickle && ./build.sh $SCEN $SIZE > /tmp/perfpub_build.log 2>&1 || { echo BUILD_FAILED; tail -5 /tmp/perfpub_build.log; exit 0; }
rm -rf $SAVE && mkdir -p $SAVE && cp ${SCEN}_${SIZE}/client ${SCEN}_${SIZE}/server $SAVE/ && sha256sum $SAVE/client | cut -c1-16" </dev/null 2>&1)
case "$out" in *BUILD_FAILED*) say "BUILD FAILED: $out"; exit 1 ;; esac
say "  client sha256 $out"

run_arm() { # $1 arm (plain|perf), $2 rep
    local arm=$1 rep=$2 tag="$1_r$2"
    srv_pid=$(sh_ "cd $SAVE && rm -f /tmp/perfpub.pid
(setsid sh -c 'echo \$\$ > /tmp/perfpub.pid; exec env BENCH_IFACE=lo taskset -c 1 ./server -Q -d $((DUR + 30))' > /tmp/perfpub_server.log 2>&1 < /dev/null &); sleep 2; cat /tmp/perfpub.pid" </dev/null)
    local pre=""
    [ "$arm" = perf ] && pre="perf record -q -e cycles:u -F $FREQ $CG -o /tmp/perfpub.data --"
    sh_ "cd $SAVE && env BENCH_IFACE=lo taskset -c 2 $pre ./client -Q -d $DUR > /tmp/perfpub_client.log 2>&1; cat /tmp/perfpub_client.log" \
        </dev/null >"$OUT.d/${tag}_client.log" 2>&1
    stop_server
    sleep 1
    sh_ "cat /tmp/perfpub_server.log" </dev/null >"$OUT.d/${tag}_server.log" 2>&1
    if [ "$arm" = perf ]; then
        sh_ "perf report -q -i /tmp/perfpub.data --no-children --sort symbol --stdio 2>/dev/null | head -60; perf report -i /tmp/perfpub.data --stdio 2>/dev/null | grep -m1 '^# Samples'
if [ $CALLGRAPH = 1 ]; then echo CALLERS_OF_tt_get_ns; perf report -q -i /tmp/perfpub.data --children --sort symbol --stdio -G -S tt_get_ns --percent-limit 0.5 2>/dev/null | head -60; fi" \
            </dev/null >"$OUT.d/${tag}_report.txt" 2>&1
    fi
    say "  $(date +%T) $tag done"
}

for rep in $(seq 1 "$REPS"); do
    if [ $((rep % 2)) = 1 ]; then run_arm plain "$rep"; run_arm perf "$rep"; else run_arm perf "$rep"; run_arm plain "$rep"; fi
done

python3 - "$OUT.d" "$REPS" <<'PYEOF' | tee -a "$SUM"
import re, statistics as st, sys
d, reps = sys.argv[1], int(sys.argv[2])
def result(path):
    for l in open(path, errors='replace'):
        if l.startswith('RESULT:') and 'role=server' not in l:
            return dict(kv.split('=', 1) for kv in l.split() if '=' in kv)
    return None
valid, shares, total_samples = [], {}, 0
for r in range(1, reps + 1):
    p, q = result(f'{d}/plain_r{r}_client.log'), result(f'{d}/perf_r{r}_client.log')
    if p is None or q is None:
        print(f'VOID rep {r}: a client RESULT is missing'); continue
    sent_p, sent_q = int(p.get('sent', 0)), int(q.get('sent', 0))
    el_p, el_q = float(p.get('elapsed_s', 1)), float(q.get('elapsed_s', 1))
    shm = int(p.get('tx_shm', 0)) / max(int(p.get('tx_shm', 0)) + int(p.get('tx_udp', 0)), 1)
    rate_p, rate_q = sent_p / el_p, sent_q / el_q
    if sent_p == 0 or sent_q == 0:
        print(f'VOID rep {r}: nothing sent'); continue
    if shm < 0.95:
        print(f'VOID rep {r}: tx_shm share {shm:.3f} - the segment did not carry it'); continue
    if abs(rate_q / rate_p - 1) > 0.10:
        print(f'VOID rep {r}: perf arm rate {rate_q:.0f}/s vs plain {rate_p:.0f}/s - perf changed the program'); continue
    us_user = float(p['utime_s']) / sent_p * 1e6
    rep_shares = {}
    cnt = 0
    for l in open(f'{d}/perf_r{r}_report.txt', errors='replace'):
        m = re.match(r'\s*([\d.]+)%\s+\[\.\]\s+(\S+)', l)
        if m:
            rep_shares[m.group(2)] = float(m.group(1))
        m = re.match(r'# Samples:\s*([\d.]+)([KM]?)', l)
        if m:
            cnt = int(float(m.group(1)) * {'': 1, 'K': 1000, 'M': 1000000}[m.group(2)])
    total_samples += cnt
    valid.append(r)
    for k, v in rep_shares.items():
        shares.setdefault(k, {})[r] = (v, v / 100 * us_user)
    print(f'rep {r}: plain {rate_p/1e3:.1f} k/s, perf {rate_q/1e3:.1f} k/s, user {us_user:.3f} us/sample, '
          f'{len(rep_shares)} symbols, {cnt} samples')
if len(valid) < 2 or total_samples < 10000:
    print(f'VOID OVERALL: {len(valid)} valid reps, {total_samples} samples'); sys.exit(0)
print(f'\nuser time per sample by symbol (median of {len(valid)} reps; share of user cycles):')
rows = []
for k, per in shares.items():
    if len(per) < 2:
        continue
    pct = [v[0] for v in per.values()]
    us = [v[1] for v in per.values()]
    rows.append((st.median(us), st.median(pct), max(pct) - min(pct), k))
for us, pct, spread, k in sorted(rows, reverse=True)[:25]:
    print(f'  {us:7.3f} us  {pct:5.1f}%  {"UNSTABLE " if spread > 3 else ""}{k}')
PYEOF
say "=== done $(date -Is) ==="
