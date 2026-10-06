#!/usr/bin/env bash
# A/B of the in-order fragment fast path (branch ab/frag-fastpath) against main, 2026-10-06.
#
# The change: a RELIABLE subscriber assembles an in-order DATA_FRAG sample in the node's frag_scratch instead of
# storing every fragment in its reorder ring first; the ring stays for anything out of order. It targets the
# same-host p4 latency gap (first-touch faults on the ring, ROADMAP Now 1) and must not cost the loss path.
#
# Part 1, same host, TickLE only, s6_transport_cells.sh (its own witness and identity rules), A B B A blocks of
# REPS reps each, p3 and p4 reliable_latency at 5 ms spacing, in two windows: first lap (-W 0 -C 0, what the change
# targets) and warmed (the bench's default window).
# Part 2, cross host, campaign_ab_chain.sh, cells 6 (P4 RELIABLE 5% loss), 15 (P4 BEST_EFFORT, untouched control)
# and 17 (P4 latency).
#
# HOW TO READ IT, written before running (pre-registered):
#   Part 1, per window: gap = median rtt_p50 of p4 - median rtt_p50 of p3, per arm. p3's binary is byte-identical in
#     both arms, so p3 is the control: if p3's median moves by more than 2 x its SE between arms, the window is VOID.
#     PASS if B's p4 p50 is not above A's by more than 2 x SE in either window. IMPROVED if, in the first-lap window,
#     B's gap is at least 10 us below A's. WORSE if B's p4 p50 is above A's by more than 2 x SE in either window.
#   Part 2: ab_compare.py with PRIMARY="c17:client.rtt_avg_ms,c6:client.send_mbps,c6:client.wire_bytes_per_sample,
#     c15:client.send_mbps"; its own Bonferroni rule decides PASS/WORSE.
#   The change lands on main only if both parts PASS.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
A=${A:?A=<main sha>}; B=${B:?B=<ab/frag-fastpath sha>}; REPS=${REPS:-5}
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then
    # Preflight both commits on this PC before the lock (rig_preflight.sh): same-host p3/p4 latency in both windows,
    # and the cross-host cells. A broken build or harness never takes the rig.
    for sha in "$A" "$B"; do
        SHA=$sha PREFLIGHT_TOPO=samens FWS=tickle "$REPO/examples/perf_hil/experiments/rig_preflight.sh" \
            "reliable_latency:p3:N0:-i 0.005 -W 0 -C 0" "reliable_latency:p4:N0:-i 0.005 -W 0 -C 0" \
            "reliable_latency:p4:N0:-i 0.005" || { echo "REFUSING TO TAKE THE RIG: preflight failed at $sha" >&2; exit 1; }
        mapfile -t pf_specs < <(PREFLIGHT_CELLS_ONLY=1 FWS=tickle CELLS="6 15 17" \
            "$REPO/examples/perf_hil/experiments/campaign_sweep.sh")
        SHA=$sha FWS=tickle "$REPO/examples/perf_hil/experiments/rig_preflight.sh" "${pf_specs[@]}" ||
            { echo "REFUSING TO TAKE THE RIG: cross-host preflight failed at $sha" >&2; exit 1; }
    done
    exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi
OUTB=${OUTB:-$HOME/rig_results_safe/ab_frag_fastpath_${A:0:8}_${B:0:8}_$(date +%Y%m%d-%H%M%S)}
say() { echo "$*" | tee -a "$OUTB.driver.log"; }
say "=== ab_frag_fastpath $(date -Is) A=$A B=$B REPS=$REPS ==="
X=$REPO/examples/perf_hil/experiments
n=0
for sha in $A $B $B $A; do
    n=$((n + 1))
    for win in first warm; do
        args="-i 0.005"; [ $win = first ] && args="-i 0.005 -W 0 -C 0"
        for size in p3 p4; do
            out="$OUTB.b${n}_${sha:0:8}_${win}_${size}.txt"
            say "--- block $n sha ${sha:0:8} window $win $size ($(date +%T))"
            FRAMEWORKS=tickle SCEN=reliable_latency SIZE=$size DUR=10 REPS=$REPS SHA=$sha CLI_ARGS="$args" OUT="$out" \
                PREFLIGHT=0 "$X/s6_transport_cells.sh" >"$out.log" 2>&1
            say "    exit $?"
        done
    done
done
python3 - "$OUTB" "$A" "$B" <<'PYEOF' | tee -a "$OUTB.driver.log"
import glob, re, statistics as st, sys
outb, a, b = sys.argv[1], sys.argv[2][:8], sys.argv[3][:8]
vals = {}
for path in glob.glob(outb + '.b*_*.txt'):
    m = re.search(r'\.b\d_([0-9a-f]{8})_(first|warm)_(p\d)\.txt$', path)
    if not m:
        continue
    sha, win, size = m.groups()
    for line in open(path, errors='replace'):
        r = re.match(r'arm=ON RESULT: framework=tickle .*?rtt_p50_ms=([\d.]+)', line)
        if r and 'role=server' not in line:
            vals.setdefault((sha, win, size), []).append(float(r.group(1)) * 1000.0)
def ms(k):
    v = vals.get(k, [])
    if len(v) < 2:
        return None
    return st.median(v), st.stdev(v) / len(v) ** 0.5, len(v)
verdict = []
for win in ('first', 'warm'):
    c = {(s, z): ms((s, win, z)) for s in (a, b) for z in ('p3', 'p4')}
    if any(x is None for x in c.values()):
        print(f'{win}: NO VERDICT - fewer than 2 reps in an arm ({ {k: (v[2] if v else 0) for k, v in c.items()} })')
        verdict.append('NO VERDICT'); continue
    (p3a, se3a, _), (p3b, se3b, _) = c[(a, 'p3')], c[(b, 'p3')]
    (p4a, se4a, na), (p4b, se4b, nb) = c[(a, 'p4')], c[(b, 'p4')]
    ctrl = abs(p3b - p3a) > 2 * (se3a ** 2 + se3b ** 2) ** 0.5
    worse = p4b - p4a > 2 * (se4a ** 2 + se4b ** 2) ** 0.5
    gap_a, gap_b = p4a - p3a, p4b - p3b
    print(f'{win}: p3 A {p3a:.1f} B {p3b:.1f} us  p4 A {p4a:.1f} B {p4b:.1f} us (n {na}/{nb})  gap A {gap_a:+.1f} B {gap_b:+.1f} us')
    if ctrl:
        print(f'{win}: VOID - the p3 control moved beyond 2 x SE'); verdict.append('VOID'); continue
    if worse:
        print(f'{win}: WORSE - B p4 p50 above A beyond 2 x SE'); verdict.append('WORSE'); continue
    tag = 'IMPROVED' if win == 'first' and gap_a - gap_b >= 10.0 else 'PASS'
    print(f'{win}: {tag}'); verdict.append(tag)
print('PART 1 VERDICT:', 'WORSE' if 'WORSE' in verdict else ('VOID/NO VERDICT' if any(v in ('VOID', 'NO VERDICT') for v in verdict) else 'PASS'), verdict)
PYEOF
say "--- part 2, cross host ($(date +%T))"
RIG_LOCK_HELD_HIL=1 PREFLIGHT=0 CELLS="6 15 17" REPS=$REPS \
    PRIMARY="c17:client.rtt_avg_ms,c6:client.send_mbps,c6:client.wire_bytes_per_sample,c15:client.send_mbps" \
    A=$A B=$B TAG=abf_cross "$X/campaign_ab_chain.sh" >"$OUTB.cross.log" 2>&1
say "    exit $?"
cp /tmp/abf_cross_* "$(dirname "$OUTB")/" 2>/dev/null
grep -E 'VERDICT|primary' /tmp/abf_cross_compare.txt 2>/dev/null | tee -a "$OUTB.driver.log"
say "=== ALL_DONE $(date -Is) ==="
