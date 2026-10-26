#!/usr/bin/env bash
# hybrid_stale_forget_ab.sh - PC A/B of "after a stale forget, announce no estimate" (the user's options (a) + (e),
# 2026-10-26; DESIGN.md section 8, "Train losses and the stale rule").
#
# Arms: M = the parent (M_REF, default origin/main), A = M's own bench binary again (A/A: the run-to-run noise), F = the
# fix (F_REF, default HEAD). Each tree is taken with `git archive` (committed state only) and built with
# hybrid_forget_netns.sh's own command. Each round runs every arm once, in an order that rotates (MAF, AFM, FMA), and
# each arm-round is one hybrid_forget_netns.sh invocation, BIN=<the arm's bench> ARMS="auto:0:1 auto:1:1" GAP=0: a 0%
# rep, then a 1% rep (writer-egress netem, GSO on, 1 MiB RELIABLE KEEP_LAST 10 at 30 Hz, 10 s), in fresh namespaces, so
# the 1% rep's incomplete reassembly queues go with them and no wait is needed between reps.
#
#   OUT=dir ROUNDS=45 START_AT=<epoch> GO_FILE=<path> DEADLINE=<epoch> ./hybrid_stale_forget_ab.sh
#   REREAD=<an earlier OUT> applies the reading below to that run's files again; nothing runs.
#
# START_AT / GO_FILE: the driver waits until the epoch START_AT or until GO_FILE exists (the PC must be quiet: another
# A/B may be running), then runs. DEADLINE: no round starts after it. The rig is not used; TickLE runs only in the
# private namespaces the harness makes.
#
# PRE-REGISTERED (written and committed before the first run; the python at the end applies it - it, not this comment,
# decides):
#   A rep is usable when both nodes printed a RESULT line and the PC's 1-minute load at its start was <= nproc / 2.
#   CHAIN rep: the writer logged "lost whole while its MTU units arrived" (its rule (e)), as in hybrid_forget_netns.sh.
#   Per arm and cell (0%, 1%): CHAIN count over usable reps; the median over usable reps of latency p99 and p50 (sub),
#   delivered samples (sub), and CPU ms per MB (sub and pub).
#   CHAIN rule (the treatment): F has fewer CHAIN reps than M in the 1% cell. The one-sided Fisher p of F against M is
#   printed with it, not part of the rule.
#   Cost rule, per metric and cell: WORSE when F's median moves the bad way (p99, p50, CPU up; delivered down) from M's by
#   more than max(|A - M|, floor), relative to M; floors p99 5%, p50 3%, delivered 0.5%, CPU 3%.
#   UNDECIDED when M and A together have fewer than 2 CHAIN reps in the 1% cell (the treatment did not occur: nothing to
#   remove), or any arm has fewer than 10 usable reps in a cell.
#   PASS when the CHAIN rule holds and no row is WORSE. FAIL when the CHAIN rule fails (F >= M), or any row is WORSE -
#   the WORSE rows are named; the expected cost of (a) is pub CPU (one sample one MTU unit a send call per forget) and,
#   at 0% with early events, n_max one sample later (no event comes on this 0% link, so no change is expected there).
#   Also printed, not part of the verdict: per arm the reps with a stale forget, "none seen" heard by the writer
#   (code=0xff00 on pub.err), and the writer's n histogram.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
OUT=${OUT:-/home/semih/hsf_results/ab_$(date +%Y%m%d_%H%M%S)}
REREAD=${REREAD:-}
if [ -n "$REREAD" ]; then
    OUT=$REREAD
fi
RESULTS=$OUT/results.txt

if [ -z "$REREAD" ]; then
M_REF=${M_REF:-origin/main}
F_REF=${F_REF:-HEAD}
ROUNDS=${ROUNDS:-45}
DUR=${DUR:-10}
START_AT=${START_AT:-0}
GO_FILE=${GO_FILE:-$OUT/GO}
DEADLINE=${DEADLINE:-0}
STATUS=${STATUS:-}
export TMPDIR=${TMPDIR:-$HOME/tmp_build}
mkdir -p "$OUT"
say() {
    echo "$(date +%H:%M:%S) $*" | tee -a "$RESULTS"
    [ -n "$STATUS" ] && echo "$(date +%H:%M) hsf ab: $*" >>"$STATUS"
    return 0
}
finish() { echo "HSF_AB_DONE $(date -Is)" >>"$RESULTS"; }
trap finish EXIT

for arm in M F; do
    ref=$M_REF
    [ "$arm" = F ] && ref=$F_REF
    sha=$(git -C "$REPO" rev-parse --short "$ref") || exit 1
    mkdir -p "$OUT/src_$arm"
    git -C "$REPO" archive "$sha" src include | tar -x -C "$OUT/src_$arm" || exit 1
    if ! cc -O2 -DNDEBUG -Wall -Wextra -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
        -I"$OUT/src_$arm/include" -I"$OUT/src_$arm/src" -o "$OUT/bench_$arm" "$HERE/large_sample_bench.c" \
        "$OUT/src_$arm/src/tickle.c" "$OUT/src_$arm/src/encoding.c" "$OUT/src_$arm/src/log.c" \
        "$OUT/src_$arm/src/hal_linux.c" -lpthread -lm >"$OUT/build_$arm.log" 2>&1; then
        say "VOID  arm $arm ($sha) did not build ($OUT/build_$arm.log)"
        exit 1
    fi
    say "arm $arm: $ref = $sha, bench $OUT/bench_$arm"
done
cp "$OUT/bench_M" "$OUT/bench_A"
say "arm A: bench_M again (A/A); bench source $(git -C "$REPO" rev-parse --short HEAD) large_sample_bench.c"

while [ ! -e "$GO_FILE" ] && [ "$(date +%s)" -lt "$START_AT" ]; do
    sleep 20
done
say "start: rounds=$ROUNDS dur=${DUR}s deadline=$(date -d "@$DEADLINE" +%H:%M 2>/dev/null) load=$(cut -d' ' -f1 /proc/loadavg)"
orders=("M A F" "A F M" "F M A")
for ((r = 1; r <= ROUNDS; r++)); do
    if [ "$DEADLINE" -gt 0 ] && [ "$(date +%s)" -ge "$DEADLINE" ]; then
        say "deadline reached before round $r"
        break
    fi
    for arm in ${orders[$(((r - 1) % 3))]}; do
        dir=$OUT/r${r}_$arm
        bin=$OUT/bench_$arm
        OUT=$dir BIN=$bin GAP=0 DUR=$DUR ARMS="auto:0:1 auto:1:1" \
            bash "$HERE/hybrid_forget_netns.sh" >"$dir.log" 2>&1
        echo "ROUND $r $arm dir=$dir" >>"$RESULTS"
    done
    [ $((r % 5)) -eq 0 ] && say "round $r done (load $(cut -d' ' -f1 /proc/loadavg))"
done
say "ROUNDS_END"
fi # REREAD

# --- the reading (applies the PRE-REGISTERED rules above) -----------------------------------------------------------
python3 -I - "$RESULTS" "$(nproc)" <<'PY' | tee -a "$OUT/reading.txt"
import re, statistics, sys
from math import comb
path, nproc = sys.argv[1], int(sys.argv[2])
def f(text, key):
    m = re.search(r'(?:^|\s)' + re.escape(key) + r'=(\S+)', text)
    return m.group(1) if m else None
def read(p):
    try:
        return open(p, errors='replace').read()
    except OSError:
        return ''
data = {}  # (arm, cell) -> list of rep dicts
voids = {}
for line in open(path):
    if not line.startswith('ROUND '):
        continue
    _, r, arm, d = line.split()
    d = d.split('=', 1)[1]
    for rl in read(d + '/results.txt').splitlines():
        if not rl.startswith('REP '):
            continue
        _, cell, rep, load, rd = rl.split()
        load, rd = float(load.split('=')[1]), rd.split('=', 1)[1]
        pub, sub, perr = read(rd + '/pub.log'), read(rd + '/sub.log'), read(rd + '/pub.err')
        key = (arm, cell)
        if 'RESULT: role=pub' not in pub or 'RESULT: role=sub' not in sub or load > nproc / 2:
            voids.setdefault(key, []).append('r%s(load=%.1f)' % (r, load))
            continue
        data.setdefault(key, []).append(dict(
            round=int(r), chain='lost whole while its MTU units arrived' in perr,
            p99=float(f(sub, 'latency_p99_us')), p50=float(f(sub, 'latency_median_us')),
            delivered=float(f(sub, 'delivered')), sub_cpu=float(f(sub, 'cpu_ms_per_mb')),
            pub_cpu=float(f(pub, 'cpu_ms_per_mb')), forgets=int(f(sub, 'hybrid_stale_forgets') or 0),
            none_seen='code=0xff00' in perr, n_hist=f(pub, 'hybrid_n_hist')))
def fisher_less(a, n1, b, n2):  # one-sided: P(X <= a) for F's count a of n1, M's b of n2, margins fixed
    k, n = a + b, n1 + n2
    return sum(comb(n1, i) * comb(n2, k - i) for i in range(0, a + 1) if k - i <= n2) / comb(n, k)
METRICS = [('p99', 1, 0.05), ('p50', 1, 0.03), ('delivered', -1, 0.005), ('sub_cpu', 1, 0.03), ('pub_cpu', 1, 0.03)]
print('=== hybrid_stale_forget_ab reading ===')
undecided, worse = [], []
for cell in ('auto0', 'auto1'):
    med = {}
    for arm in ('M', 'A', 'F'):
        reps = data.get((arm, cell), [])
        if len(reps) < 10:
            undecided.append('%s %s has %d usable reps' % (arm, cell, len(reps)))
        med[arm] = {m: (statistics.median([x[m] for x in reps]) if reps else float('nan')) for m, _, _ in METRICS}
        print('%s %s: usable %d, VOID %s, CHAIN %d, stale forgets in %d reps, none seen heard in %d reps' % (
            cell, arm, len(reps), ' '.join(voids.get((arm, cell), [])) or '-', sum(x['chain'] for x in reps),
            sum(x['forgets'] > 0 for x in reps), sum(x['none_seen'] for x in reps)))
        print('   medians: ' + ' '.join('%s=%.3f' % (m, med[arm][m]) for m, _, _ in METRICS))
    for m, sign, floor in METRICS:
        base = med['M'][m]
        if not base == base or base == 0:
            continue
        noise = abs(med['A'][m] - base) / base
        delta = (med['F'][m] - base) / base
        bound = max(noise, floor)
        bad = sign * delta > bound
        print('  %-6s %-9s F-M %+7.2f%%  |A-M| %6.2f%%  bound %5.2f%%  %s' % (
            cell, m, 100 * delta, 100 * noise, 100 * bound, 'WORSE' if bad else 'ok'))
        if bad:
            worse.append('%s %s %+.2f%%' % (cell, m, 100 * delta))
cm = [x['chain'] for x in data.get(('M', 'auto1'), [])]
ca = [x['chain'] for x in data.get(('A', 'auto1'), [])]
cf = [x['chain'] for x in data.get(('F', 'auto1'), [])]
print('CHAIN at 1%%: M %d/%d, A %d/%d, F %d/%d; one-sided Fisher F < M p = %.4f' % (
    sum(cm), len(cm), sum(ca), len(ca), sum(cf), len(cf),
    fisher_less(sum(cf), len(cf), sum(cm), len(cm)) if cm and cf else float('nan')))
for arm in ('M', 'A', 'F'):
    print('  n_hist %s 1%%: %s' % (arm, ' | '.join(str(x['n_hist']) for x in data.get((arm, 'auto1'), [])[:6])))
if sum(cm) + sum(ca) < 2:
    undecided.append('M and A have %d CHAIN reps at 1%% (< 2): the treatment did not occur' % (sum(cm) + sum(ca)))
if undecided:
    verdict = 'UNDECIDED: ' + '; '.join(undecided)
elif sum(cf) >= sum(cm):
    verdict = 'FAIL: CHAIN F %d >= M %d' % (sum(cf), sum(cm)) + ('; WORSE ' + ', '.join(worse) if worse else '')
elif worse:
    verdict = 'FAIL: CHAIN F %d < M %d, but WORSE %s' % (sum(cf), sum(cm), ', '.join(worse))
else:
    verdict = 'PASS: CHAIN F %d < M %d, no row WORSE' % (sum(cf), sum(cm))
print('VERDICT ' + verdict)
PY
