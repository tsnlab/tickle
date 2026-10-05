#!/usr/bin/env bash
# Is the p4 same-host latency gap (RESULTS S10 0.051 ms against S9 0.030 at p3) a cost of idling, on the rig?
#
# On this PC (p4_wake_count.sh, 2026-10-05) p4 and p3 rang the same doorbells and slept alike, and the p4 - p3 RTT gap
# was 0 at back-to-back pings, +3 us at 0.5 ms, +19 us at 5 ms: the extra time appears only after the processes idle.
# The rig's S9/S10 ran at 5 ms with the cores at their 1.5 GHz floor (cpu_mhz_mean 1500). This runs reliable_latency
# at p3 and p4, ON (segment) arms, on one rig Pi at three ping intervals, through s6_transport_cells.sh (its witness
# and identity checks unchanged), with CycloneDDS as the control: its p4 is one datagram, so its own p4 - p3 gap is
# what touching twice the bytes costs any framework on this core after idle.
#
# HOW TO READ IT, written before running and enforced below:
#   gap(f, i) = median rtt_avg of f's ON arm at p4 minus at p3, ping interval i; reps that fail the witness are
#   already VOID in s6_transport_cells.sh and absent here. A gap needs 2 reps of each size or it is not computed.
#   IDLE-DEPENDENT (ours) if TickLE's gap at 5 ms exceeds its gap at 0.5 ms by more than 5 us AND by more than
#     CycloneDDS's own rise between the same two intervals: the extra p4 time grows with idle, beyond what the
#     platform does to any framework. The remedy is fewer bytes touched per sample after a wake (whole records,
#     encode into the slot), not fewer wakes.
#   PLATFORM if both rise alike (within 5 us of each other): idling costs every framework for bytes touched; ours is
#     the same effect and the remedy is the same, but it is not a defect of the transport.
#   FLAT if TickLE's gap moves by 5 us or less across intervals: on the Pi the gap is a fixed per-sample cost of the
#     fragment path, and the PC result does not carry over; look at the fragment path's work instead.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SHA=${SHA:?set SHA to a pushed commit}
INTERVALS=${INTERVALS:-"0.0005 0.005 0.02"}
REPS=${REPS:-3}
OUTB=${OUTB:-$HOME/rig_results_safe/p4_interval_rig_${SHA}_$(date +%Y%m%d-%H%M%S)}
SUM="$OUTB.summary.txt"
: >"$SUM"
echo "=== p4 interval sweep on the rig, $SHA, $(date -Is), intervals $INTERVALS, $REPS reps ===" | tee -a "$SUM"
for iv in $INTERVALS; do
    # About 2,000 round trips per rep at every interval, and never under 10 s.
    dur=$(python3 -c "print(max(10, round(2000 * $iv)))")
    for sz in p3 p4; do
        out="$OUTB.i${iv}_${sz}.txt"
        echo "--- interval $iv size $sz dur ${dur}s -> $out ($(date +%T))" | tee -a "$SUM"
        FRAMEWORKS="tickle cyclonedds" SCEN=reliable_latency SIZE=$sz DUR=$dur REPS=$REPS SHA=$SHA \
            CLI_ARGS="-i $iv" OUT="$out" "$REPO/examples/perf_hil/experiments/s6_transport_cells.sh" >"$out.log" 2>&1
        echo "    exit $? ($(date +%T))" | tee -a "$SUM"
    done
done

python3 - "$OUTB" "$INTERVALS" <<'PYEOF' | tee -a "$SUM"
import glob, re, statistics as st, sys
outb, intervals = sys.argv[1], sys.argv[2].split()
rtt = {}  # (framework, interval, size) -> [rtt_avg]
for iv in intervals:
    for sz in ('p3', 'p4'):
        for path in glob.glob(f'{outb}.i{iv}_{sz}.txt*'):
            if path.endswith('.log'):
                continue
            for l in open(path, errors='replace'):
                m = re.match(r'arm=ON RESULT: framework=(\S+) scenario=reliable_latency (?!role=server)(.*)', l)
                if m:
                    f = dict(kv.split('=', 1) for kv in m.group(2).split() if '=' in kv)
                    if int(f.get('recv', 0)) > 0:
                        rtt.setdefault((m.group(1), iv, sz), []).append(float(f['rtt_avg_ms']) * 1000)
gap = {}
for fw in ('tickle', 'cyclonedds'):
    for iv in intervals:
        a, b = rtt.get((fw, iv, 'p3'), []), rtt.get((fw, iv, 'p4'), [])
        if len(a) >= 2 and len(b) >= 2:
            gap[(fw, iv)] = st.median(b) - st.median(a)
            print(f'{fw:10} i={iv:<7} p3 {st.median(a):6.1f} us (n={len(a)})  p4 {st.median(b):6.1f} us (n={len(b)})'
                  f'  gap {gap[(fw, iv)]:+6.1f} us')
        else:
            print(f'{fw:10} i={iv:<7} not computed: p3 n={len(a)}, p4 n={len(b)}')
lo, hi = '0.0005', '0.005'
need = [('tickle', lo), ('tickle', hi), ('cyclonedds', lo), ('cyclonedds', hi)]
if any(k not in gap for k in need):
    print('NO VERDICT: a gap at 0.5 ms or 5 ms is missing for one framework')
    sys.exit(0)
rise_t = gap[('tickle', hi)] - gap[('tickle', lo)]
rise_c = gap[('cyclonedds', hi)] - gap[('cyclonedds', lo)]
if abs(rise_t) <= 5:
    print(f'FLAT: TickLE gap moves {rise_t:+.1f} us from 0.5 to 5 ms - a fixed cost of the fragment path on the Pi')
elif rise_t > 5 and rise_t - rise_c > 5:
    print(f'IDLE-DEPENDENT (ours): TickLE gap rises {rise_t:+.1f} us, CycloneDDS {rise_c:+.1f} us')
elif abs(rise_t - rise_c) <= 5:
    print(f'PLATFORM: both rise alike, TickLE {rise_t:+.1f} us, CycloneDDS {rise_c:+.1f} us')
else:
    print(f'MIXED: TickLE gap rises {rise_t:+.1f} us, CycloneDDS {rise_c:+.1f} us - none of the three readings')
PYEOF
