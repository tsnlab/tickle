#!/usr/bin/env bash
# Which system calls does a same-host segment sample pay for? Counted per sample, on the rig, both roles.
#
# Why (COMPARISON 2026-10-04): the p3 BEST_EFFORT segment arm spends 1.507 us per sample and the ring itself
# (copies + header layout) is ~7% of that. Its own RESULT line says where to look next: the publisher is CPU-bound
# (sched_cpu_s 4.90 of a 5.0 s run) and HALF of that is system time (utime 2.40, stime 2.51). A shared-memory
# publish that needs the kernel for half its cost is paying for something other than shared memory. This counts
# what it pays for.
#
# Method: the same build as s6_witness_check.sh (both roles on one Pi, BENCH_IFACE=lo, server on core 1, client on
# core 2), each process run under `strace -c -f`. strace inflates TIME, so only COUNTS are read from it; the
# unperturbed time split is the RESULT line of a run without strace, taken in the same invocation.
#
# Reading rules, written before the run and enforced by the analysis at the bottom:
#   CONTROL - the OFF arm (segment compiled out) must show the client sending 0.9..1.1 datagrams per sample by
#     sendto/sendmsg/sendmmsg combined (p3 is one datagram). That is the one count this hypothesis cannot touch and
#     whose right answer is known; if strace does not reproduce it, it is not counting what we think, and the run is
#     VOID.
#   VOID too if the ON arm's tx_shm/sent < 0.99 (the segment was not what carried the samples), or if either arm's
#     strace table is missing (a table that could not be read is "could not look", never "no syscalls").
#   VERDICT on the ON client (the publisher, whose rate is the cell's figure):
#     total syscalls per sample >= 0.20 -> syscalls are a material term; the top entry is named and is the next
#                                          thing to remove or batch.
#     total syscalls per sample <= 0.05 -> the stime is not from syscalls the process makes (look at faults,
#                                          preemption, wake-ups charged to it).
#     between                           -> report the counts, conclude nothing.
#   The server side is reported with the same table but carries no verdict: its rate is not the cell's figure.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
SHA=${SHA:-$(cd "$REPO" && git rev-parse --short HEAD)}
DUR=${DUR:-5}
SCEN=${SCEN:-best_effort_throughput}
SIZE=${SIZE:-p3}
HOST=${HOST:-10.1.1.214}
K=$HOME/.ssh/tickle_ci_ed25519
SAVE=/tmp/sccensus
OUT=${OUT:-$HOME/rig_results_safe/syscall_census.txt}
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$HOST" "$@"; }
: >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }

cleanup() {
    # Only processes whose executable is one of ours under $SAVE, never by pattern.
    sh_ "for p in \$(ls /proc | grep -E '^[0-9]+\$'); do case \"\$(readlink /proc/\$p/exe 2>/dev/null)\" in
        $SAVE/*/server|$SAVE/*/client) kill -INT \$p;; esac; done" </dev/null >/dev/null 2>&1
}
trap cleanup EXIT

say "=== syscall census $(date -Is) sha=$SHA scen=$SCEN size=$SIZE dur=${DUR}s host=$HOST ==="

build_arm() { # build_arm <arm> <extra-cflags>
    local name=$1 extra=$2 out
    out=$(sh_ "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx -e install -e build -e log
cd examples/perf_hil/tickle && TICKLE_EXTRA_CFLAGS='$extra' ./build.sh $SCEN $SIZE >/tmp/sccensus_build.log 2>&1 || { echo BUILD_FAILED; tail -5 /tmp/sccensus_build.log; exit 0; }
mkdir -p $SAVE/$name && cp ${SCEN}_${SIZE}/client ${SCEN}_${SIZE}/server $SAVE/$name/ && sha256sum $SAVE/$name/client | cut -c1-16" </dev/null 2>&1)
    case "$out" in *BUILD_FAILED*|*error:*) say "FATAL build $name: $out"; return 1;; esac
    say "  arm $name built (extra='$extra') client sha256=$(printf '%s' "$out" | tail -1)"
}

run_arm() { # run_arm <arm> <traced 0|1>
    local name=$1 traced=$2 wrap_s="" wrap_c=""
    if [ "$traced" = 1 ]; then
        wrap_s="strace -c -f -o /tmp/sccensus_srv_$name.txt"
        wrap_c="strace -c -f -o /tmp/sccensus_cli_$name.txt"
    fi
    sh_ "rm -f /tmp/sccensus_srv_$name.txt /tmp/sccensus_cli_$name.txt
cd $SAVE/$name && (setsid sh -c 'exec env BENCH_IFACE=lo taskset -c 1 $wrap_s ./server -Q -d $((DUR + 6))' >/tmp/sccensus_srv.log 2>&1 </dev/null &)
sleep 2
env BENCH_IFACE=lo taskset -c 2 $wrap_c ./client -Q -d $DUR >/tmp/sccensus_cli.log 2>&1
# the server ends on its own -d; wait for its RESULT rather than killing it, so its strace table is written
for i in \$(seq 1 30); do grep -q '^RESULT' /tmp/sccensus_srv.log && break; sleep 1; done
sleep 1
grep '^RESULT' /tmp/sccensus_cli.log | sed 's/^/arm=$name traced=$traced /'
grep '^RESULT' /tmp/sccensus_srv.log | sed 's/^/arm=$name traced=$traced /'
for side in cli srv; do f=/tmp/sccensus_\${side}_$name.txt
  [ $traced = 1 ] || continue
  if [ -s \$f ]; then sed \"s/^/STRACE arm=$name side=\$side /\" \$f; else echo \"STRACE arm=$name side=\$side MISSING\"; fi
done" </dev/null | tee -a "$OUT"
}

build_arm ON "" || exit 1
build_arm OFF "-Dtt_SEGMENT_ENABLED=0" || exit 1
say "--- untraced (time split) ---"
run_arm ON 0
run_arm OFF 0
say "--- traced (counts only) ---"
run_arm ON 1
run_arm OFF 1
say "=== done $(date -Is) ==="

python3 - "$OUT" <<'PYEOF' | tee -a "$OUT"
import re, sys
res = {}      # (arm, traced, role) -> fields
tab = {}      # (arm, side) -> {syscall: calls}
missing = []
for line in open(sys.argv[1]):
    m = re.match(r'arm=(\w+) traced=(\d) RESULT: (.*)', line)
    if m:
        f = dict(kv.split('=', 1) for kv in m.group(3).split() if '=' in kv)
        res[(m.group(1), m.group(2), f.get('role'))] = f
        continue
    m = re.match(r'STRACE arm=(\w+) side=(\w+) (.*)', line)
    if not m:
        continue
    arm, side, rest = m.groups()
    if rest.strip() == 'MISSING':
        missing.append(f'{arm}/{side}')
        continue
    cols = rest.split()
    # strace -c rows: % time, seconds, usecs/call, calls, [errors], syscall
    if len(cols) >= 5 and re.match(r'^[0-9.]+$', cols[0]) and cols[-1] != 'total':
        try:
            calls = int(cols[3])
        except ValueError:
            continue
        tab.setdefault((arm, side), {})[cols[-1]] = calls

void = []
if missing:
    void.append('strace table missing: ' + ', '.join(missing))
for arm in ('ON', 'OFF'):
    for side in ('cli', 'srv'):
        if (arm, side) not in tab and f'{arm}/{side}' not in missing:
            void.append(f'no strace rows parsed for {arm}/{side}')

def sent(arm, traced):
    f = res.get((arm, traced, 'client'))
    return int(f['sent']) if f and 'sent' in f else 0

print()
print('=== untraced time split (per Msample) ===')
for arm in ('ON', 'OFF'):
    for role in ('client', 'server'):
        f = res.get((arm, '0', role))
        if not f:
            print(f'  {arm} {role}: no RESULT'); void.append(f'{arm} {role} untraced RESULT missing'); continue
        n = int(f.get('sent') or f.get('recv') or 0)
        if n == 0:
            void.append(f'{arm} {role} untraced count 0'); continue
        u = float(f['utime_s']) / n * 1e6; s = float(f['stime_s']) / n * 1e6
        print(f'  {arm} {role:6}: n={n:>9}  user {u:.3f} us  sys {s:.3f} us  per sample'
              f'  tx_shm={f.get("tx_shm")} shm_full_dropped={f.get("shm_full_dropped")}')

f_on = res.get(('ON', '1', 'client'))
if f_on and int(f_on.get('sent', 0)) > 0:
    if int(f_on.get('tx_shm', 0)) / int(f_on['sent']) < 0.99:
        void.append(f'ON tx_shm/sent = {int(f_on["tx_shm"])/int(f_on["sent"]):.3f} < 0.99: the segment did not carry it')
else:
    void.append('ON traced client RESULT missing')

print()
print('=== syscalls per sample (traced runs; counts only) ===')
for arm in ('ON', 'OFF'):
    n = sent(arm, '1')
    for side in ('cli', 'srv'):
        t = tab.get((arm, side), {})
        if not n or not t:
            continue
        tot = sum(t.values())
        top = sorted(t.items(), key=lambda kv: -kv[1])[:8]
        print(f'  {arm} {side}: {tot/n:.4f} syscalls/sample  (n={n})')
        for k, v in top:
            print(f'      {k:<16} {v:>10}  {v/n:.4f}/sample')

n_off = sent('OFF', '1')
t_off = tab.get(('OFF', 'cli'), {})
if n_off and t_off:
    sends = sum(t_off.get(k, 0) for k in ('sendto', 'sendmsg', 'sendmmsg'))
    ratio = sends / n_off
    print(f'\nCONTROL: OFF client send calls per sample = {ratio:.3f} (must be 0.9..1.1)')
    if not 0.9 <= ratio <= 1.1:
        void.append(f'control failed: OFF client sends/sample {ratio:.3f}')
else:
    void.append('control unreadable (OFF client)')

print()
if void:
    print('VOID: ' + '; '.join(void))
    sys.exit(0)
n_on = sent('ON', '1')
t_on = tab[('ON', 'cli')]
per = sum(t_on.values()) / n_on
top = max(t_on.items(), key=lambda kv: kv[1])
if per >= 0.20:
    print(f'VERDICT: ON publisher makes {per:.3f} syscalls/sample - a material term. Top: {top[0]} at {top[1]/n_on:.3f}/sample.')
elif per <= 0.05:
    print(f'VERDICT: ON publisher makes {per:.3f} syscalls/sample - its system time is not from its own syscalls.')
else:
    print(f'VERDICT: {per:.3f} syscalls/sample is between the bands - counts reported, nothing concluded.')
PYEOF
