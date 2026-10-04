#!/usr/bin/env bash
# Same-host segment cell, several compile-time arms, interleaved reps. A generic A/B/C driver for the work that
# syscall_census.sh pointed at (2026-10-04): the p3 BEST_EFFORT publisher spends half its CPU in the kernel, on
# 0.240 empty recvmmsg and 0.124 doorbell sendto per sample.
#
# ARMS="name:flags name:flags ..." - every arm is the segment-ON build plus its flags. The FIRST arm is the
# baseline. Reps run round-robin (A B C A B C ...) so slow drift on the rig lands on every arm alike, and the
# baseline's own rep-to-rep spread is the noise floor a difference has to clear.
#
# Reading rules, enforced below:
#   - A rep with shm_full_dropped != 0 is excluded from rates: dropping is cheaper than delivering and reads faster
#     (+0.925 correlation, 2026-10-03/04). It is still listed, and its client CPU per sample is reported apart.
#   - An arm with fewer than 3 drop-free reps gets no rate verdict ("could not look", not "no change").
#   - A rep with tx_shm/sent < 0.99 that did not drop is VOID: the segment was not the path.
#   - An arm's rate differs from baseline only if the gap between means exceeds BOTH arms' full ranges' half-sum
#     (i.e. the ranges do not overlap). Anything else is "no separable difference".
#   - Client CPU per sample (sched_cpu_s / sent) is reported for every rep, dropping or not, because it is what the
#     publisher spends - the CPU goal in COMPARISON.md - and it moves less with drops than the rate does.
#   - DELIVERED rate (server recv / elapsed) over ALL reps, dropping or not, is reported and compared too, with the
#     same non-overlap rule (added 2026-10-04 after 5 of 7 baseline reps dropped and left the send-rate verdict
#     VOID). What a subscriber received is a figure a drop cannot inflate.
#   - TREATMENT CHECK: an arm built with tt_HAL_RX_HINT=1 (read) must report rx_hint=read on every client RESULT line, and
#     every other arm rx_hint=uring (the default since 2026-10-04; rx_hint=refused means the rig refused io_uring).
#     Likewise an arm built with tt_SEGMENT_BELL_FIFO=0 must report bells_rung=0, and every other arm that rang a
#     doorbell must have rung it through the FIFO (bells_rung > 0).; otherwise the arm is VOID - an arm
#     that silently ran without its treatment looks exactly like a null result.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
SHA=${SHA:-$(cd "$REPO" && git rev-parse --short HEAD)}
DUR=${DUR:-5}
REPS=${REPS:-5}
SCEN=${SCEN:-best_effort_throughput}
SIZE=${SIZE:-p3}
ARMS=${ARMS:?ARMS=\"name:flags ...\" required, first is the baseline}
CLI_ARGS=${CLI_ARGS:-}
HOST=${HOST:-10.1.1.214}
K=$HOME/.ssh/tickle_ci_ed25519
SAVE=/tmp/segarms
OUT=${OUT:-$HOME/rig_results_safe/segment_arms.txt}
# Optional: a local patch applied on the rig after checkout, for arms that need code not yet on origin.
PATCH=${PATCH:-}
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$HOST" "$@"; }
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }

cleanup() {
    sh_ "for p in \$(ls /proc | grep -E '^[0-9]+\$'); do case \"\$(readlink /proc/\$p/exe 2>/dev/null)\" in
        $SAVE/*/server|$SAVE/*/client) kill -INT \$p;; esac; done" </dev/null >/dev/null 2>&1
}
trap cleanup EXIT

say "=== segment arms $(date -Is) sha=$SHA scen=$SCEN size=$SIZE dur=${DUR}s reps=$REPS host=$HOST cli_args='$CLI_ARGS' patch='${PATCH:+$(basename "$PATCH") $(sha256sum "$PATCH" | cut -c1-12)}' ==="
say "    arms: $ARMS"

names=()
for spec in $ARMS; do
    name=${spec%%:*}; flags=${spec#*:}; flags=${flags//,/ }
    names+=("$name")
    out=$(sh_ "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx -e install -e build -e log
cat > /tmp/segarms.patch
if [ -s /tmp/segarms.patch ]; then git apply /tmp/segarms.patch; fi
cd examples/perf_hil/tickle && TICKLE_EXTRA_CFLAGS='$flags' ./build.sh $SCEN $SIZE >/tmp/segarms_build.log 2>&1 || { echo BUILD_FAILED; tail -8 /tmp/segarms_build.log; exit 0; }
mkdir -p $SAVE/$name && cp ${SCEN}_${SIZE}/client ${SCEN}_${SIZE}/server $SAVE/$name/ && sha256sum $SAVE/$name/client | cut -c1-16" \
        <"${PATCH:-/dev/null}" 2>&1)
    case "$out" in *BUILD_FAILED*|*error*) say "FATAL build $name: $out"; exit 1;; esac
    say "  arm $name flags='$flags' client sha256=$(printf '%s' "$out" | tail -1)"
done

for rep in $(seq 1 "$REPS"); do
    for name in "${names[@]}"; do
        sh_ "cd $SAVE/$name && (setsid sh -c 'exec env BENCH_IFACE=lo taskset -c 1 ./server -Q -d $((DUR + 6))' >/tmp/segarms_srv.log 2>&1 </dev/null &)
sleep 2
env BENCH_IFACE=lo taskset -c 2 ./client -Q -d $DUR $CLI_ARGS >/tmp/segarms_cli.log 2>&1
for i in \$(seq 1 30); do grep -q '^RESULT' /tmp/segarms_srv.log && break; sleep 1; done
grep -q '^RESULT' /tmp/segarms_cli.log || { echo 'NORESULT client'; tail -3 /tmp/segarms_cli.log; }
grep -q '^RESULT' /tmp/segarms_srv.log || { echo 'NORESULT server'; tail -3 /tmp/segarms_srv.log; }
grep -h '^RESULT' /tmp/segarms_cli.log /tmp/segarms_srv.log" </dev/null | sed "s/^/arm=$name rep=$rep /" | tee -a "$OUT"
    done
done
say "=== done $(date -Is) ==="

python3 - "$OUT" "$ARMS" <<'PYEOF' | tee -a "$OUT"
import re, sys, statistics as st
path = sys.argv[1]
specs = [a.split(':', 1) for a in sys.argv[2].split()]
names = [n for n, _ in specs]
flags = dict(specs)
cli, srv, noresult = {}, {}, []
for line in open(path):
    m = re.match(r'arm=(\S+) rep=(\d+) (RESULT: .*|NORESULT.*)', line)
    if not m:
        continue
    if m.group(3).startswith('NORESULT'):
        noresult.append(f'{m.group(1)} rep {m.group(2)} {m.group(3)}'); continue
    f = dict(kv.split('=', 1) for kv in m.group(3).split() if '=' in kv)
    (cli if f.get('role') == 'client' else srv)[(m.group(1), m.group(2))] = f
if noresult:
    print('NO RESULT (could not look): ' + '; '.join(noresult))
summary = {}
for name in names:
    reps = sorted({r for (a, r) in cli if a == name}, key=int)
    clean, dropping = [], []
    print(f'=== {name} ===')
    for r in reps:
        c = cli[(name, r)]; s = srv.get((name, r), {})
        sent = int(c['sent']); el = float(c['elapsed_s'])
        drop = int(c.get('shm_full_dropped', 0)); txs = int(c.get('tx_shm', 0))
        rate = sent / el / 1e3
        ccpu = float(c['sched_cpu_s']) / sent * 1e6
        cu = float(c['utime_s']) / sent * 1e6; cs = float(c['stime_s']) / sent * 1e6
        recv = int(s.get('recv', 0)) if s else 0
        scpu = float(s['sched_cpu_s']) / recv * 1e6 if s and recv else float('nan')
        tag = 'DROP' if drop else ('VOID' if txs / sent < 0.99 else 'ok')
        print(f'  rep {r}: {rate:8.1f} ksamples/s  client {ccpu:.3f} us/sample (user {cu:.3f} sys {cs:.3f})'
              f'  server {scpu:.3f} us/recv  dropped={drop} [{tag}]')
        if tag == 'ok':
            clean.append((rate, ccpu, scpu))
        elif tag == 'DROP':
            dropping.append((rate, ccpu, scpu))
    if len(clean) >= 3:
        rates = [x[0] for x in clean]
        summary[name] = (st.mean(rates), min(rates), max(rates),
                         st.mean(x[1] for x in clean), st.mean(x[2] for x in clean), len(clean))
        print(f'  drop-free n={len(clean)}: {summary[name][0]:.1f} ksamples/s ({min(rates):.1f}..{max(rates):.1f})'
              f'  client {summary[name][3]:.3f} us/sample  server {summary[name][4]:.3f} us/recv')
    else:
        print(f'  drop-free n={len(clean)} < 3 - no rate verdict for this arm')
    if dropping:
        print(f'  dropping reps n={len(dropping)}: client {st.mean(x[1] for x in dropping):.3f} us/sample (rates not read)')
print()
print('=== delivered (server recv / elapsed), all reps ===')
dsum, void_arm = {}, set()
for name in names:
    reps = [r for (a, r) in cli if a == name]
    want = 'read' if 'tt_HAL_RX_HINT=1' in flags[name] else 'uring'
    hints = {cli[(name, r)].get('rx_hint', 'read') for r in reps}
    if hints != {want}:
        void_arm.add(name)
        print(f'  {name}: VOID - treatment check wanted rx_hint={want}, got {sorted(hints)}')
        continue
    # The doorbell's treatment (2026-10-04): an arm built with tt_SEGMENT_BELL_FIFO=0 must ring nothing through the
    # FIFO, and every other arm that rang at all must have rung through it.
    fifo_off = 'tt_SEGMENT_BELL_FIFO=0' in flags[name]
    bad = [r for r in reps if 'bells_rung' in cli[(name, r)] and
           ((int(cli[(name, r)]['bells_rung']) != 0) if fifo_off else
            (int(cli[(name, r)].get('doorbells_sent', 0)) > 0 and int(cli[(name, r)]['bells_rung']) == 0))]
    if bad:
        void_arm.add(name)
        print(f'  {name}: VOID - doorbell treatment check failed in reps {bad} (fifo_off={fifo_off})')
        continue
    d = [int(srv[(name, r)]['recv']) / float(cli[(name, r)]['elapsed_s']) / 1e3 for r in reps if (name, r) in srv]
    if len(d) < 3:
        print(f'  {name}: {len(d)} complete reps < 3 - no verdict'); continue
    dsum[name] = (st.mean(d), min(d), max(d))
    print(f'  {name:8} {st.mean(d):8.1f} k/s ({min(d):.1f}..{max(d):.1f}) n={len(d)}')
if names[0] in dsum:
    b = dsum[names[0]]
    for name in names[1:]:
        if name in dsum:
            a = dsum[name]; sep = a[1] > b[2] or a[2] < b[1]
            print(f'  {name} vs {names[0]}: delivered {a[0]/b[0]:.3f}x ({"SEPARABLE" if sep else "ranges overlap"})')
print()
base = names[0]
if base not in summary:
    print(f'VOID: baseline {base} has < 3 drop-free reps; no arm is compared.')
    sys.exit(0)
b = summary[base]
for name in names[1:]:
    if name not in summary:
        print(f'{name}: no verdict (< 3 drop-free reps)'); continue
    a = summary[name]
    sep = a[1] > b[2] or a[2] < b[1]
    print(f'{name} vs {base}: rate {a[0]/b[0]:.3f}x ({"SEPARABLE" if sep else "ranges overlap - no separable difference"}),'
          f' client CPU/sample {a[3]-b[3]:+.3f} us, server CPU/recv {a[4]-b[4]:+.3f} us')
PYEOF
