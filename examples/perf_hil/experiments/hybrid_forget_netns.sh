#!/usr/bin/env bash
# hybrid_forget_netns.sh - why a RELIABLE AUTO writer at 1% loss sometimes sends runs of 45 for a few samples and then
# judges its reader's path as dropping IP fragments (2026-10-25).
#
# Seen in 12 of 29+8 GSO-on PC reps (hybrid_mode_netns.sh, ~/tmp_build/hyb_mode/{ab_gso,mech_gso,fix_trace}) and in
# none of 44 GSO-off ones: in every such rep the reader's stale rule forgot 6-8 events (hybrid_stale_forgets=1), the
# writer heard "none seen" (0xff00) before sample 3, sent 3-9 samples at n = 45, and logged "every run to node 2 lost
# whole while its MTU units arrived". The reader's socket dropped nothing (socket_drops 0, receiver_drops 0); its drain
# overran in 6 of those 7 reps and in 16 of the 25 others. This harness asks where the runs went.
#
# The same two namespaces as hybrid_mode_netns.sh (veth, writer egress "netem limit 100000 delay 100us rate 1gbit
# loss L%", large_sample_bench 1 MiB RELIABLE KEEP_LAST 10 at 30 Hz), plus, in the reader's namespace, a sampler of
# its IP reassembly: /proc/net/sockstat "FRAG: inuse N memory B" and /proc/net/snmp Ip ReasmReqds/OKs/Fails, every
# 50 ms for the first 6 s of the writer, then every 500 ms. The namespace's net.ipv4.ipfrag_high_thresh (4 MiB by
# default) and ipfrag_time (30 s) are printed with each arm. Arms run one after another (never at once: the PC is
# shared); each is MODE:LOSS:REPS, MODE auto (the default writer) or ip (-F ip on the writer, -C on the reader: runs of
# n_max = 45 from the first sample, whatever p - the deterministic arm).
#
#   ARMS="auto:1:16 ip:1:4 ip:0:3" DUR=10 OUT=dir BIN=prebuilt bench (default: build this tree)
#
# PRE-REGISTERED (written before the first run; the python at the end applies it - it, not this comment, decides):
#   VOID (a rep says nothing): no RESULT line from either node; the PC's 1-minute load above nproc/2 at its start; no
#     sampler output (the reading below needs it).
#   A rep is a CHAIN rep when its writer logged "every run to node N lost whole" (the writer's rule (e)).
#   Per rep: FAILS = the reader's IP ReasmFails over the rep; PEAK = the largest FRAG memory sampled; FULL when PEAK >=
#     0.9 x ipfrag_high_thresh (the kernel refuses every new fragment above high_thresh; 0.9 for the sampler's spacing).
#     DROPS = the reader's hybrid_receiver_drops (socket drops + drain truncations).
#   H1 (step 1, the estimate is forgotten): every auto CHAIN rep has hybrid_stale_forgets >= 1 with hybrid_stale_events
#     <= 8, and its writer heard 0xff00 ("none seen") at some PEER_P line.
#   H2 (steps 2-3, where the runs went): every CHAIN rep is FULL with FAILS > 0 and DROPS = 0 - the runs died in the
#     reader host's IP reassembly, which the lost runs' incomplete queues filled, not in its socket or its drain.
#   Deterministic arm: every usable ip:1 rep is FULL with FAILS > 0 (runs of 45 at 1% fill reassembly whatever p says).
#   CONTROL the loss cannot reach: ip:0 (runs of 45, no loss: no incomplete queue) - PEAK below 0.5 x high_thresh and
#     FAILS = 0 in every usable rep, else the reading is VOID (the sampler or the kernel counts something else).
#   SHOWN when the control holds, auto has >= 2 usable CHAIN reps, H1 and H2 hold for every one of them, and the
#     deterministic arm holds in every usable rep (>= 2 of them).
#   FALSIFIED (the reassembly explanation) when any CHAIN rep has FAILS = 0, or is not FULL, or has DROPS > 0 - the runs
#     went elsewhere (the socket, the drain, the path) - or a usable ip:1 rep is not FULL.
#   H1 FALSIFIED when an auto CHAIN rep has no stale forget (the 45s came from something other than "none seen").
#   UNDECIDED when auto has fewer than 2 usable CHAIN reps. With the archive's rate r ~ 0.31 (12 of 37 GSO-on reps),
#     16 reps give fewer than 2 with probability 0.69^16 + 16 x 0.31 x 0.69^15 = 2%.
#   Also printed, not part of the verdict: non-CHAIN auto reps that went FULL (the chain without the writer's warning).
#
# AMENDED 2026-10-25 03:15, AFTER THE FIRST RUN (mech1), A READING DEFECT ONLY: "none seen" was looked for in the
#   writer's stdout (pub.log), but the bench prints its PEER_P lines on stderr (pub.err), so every rep read
#   none_seen=0 and mech1 printed "H1 FALSIFIED" from an instrument that could not see the line. The rule is unchanged;
#   none_seen is now read from pub.err. REREAD=<an earlier OUT> applies the reading to that run's files again, runs
#   nothing, and appends to its reread.txt.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
ARMS=${ARMS:-"auto:1:16 ip:1:4 ip:0:3"}
DUR=${DUR:-10}
OUT=${OUT:-$HOME/tmp_build/hyb_forget/run_$(date +%Y%m%d_%H%M%S)}
REREAD=${REREAD:-}
export TMPDIR=${TMPDIR:-$HOME/tmp_build}
if [ -n "$REREAD" ]; then # the reading again, on an earlier run's files; nothing runs
    RESULTS=$REREAD/results.txt
    READING=$REREAD/reread.txt
    THRESH=$(grep -o 'ipfrag_high_thresh=[0-9]*' "$RESULTS" | head -1 | cut -d= -f2)
else
mkdir -p "$OUT"
NS_W=hfw$$
NS_R=hfr$$
RESULTS=$OUT/results.txt
READING=$RESULTS
say() { echo "$*" | tee -a "$RESULTS"; }

cleanup() {
    sudo -n ip netns del "$NS_W" 2>/dev/null
    sudo -n ip netns del "$NS_R" 2>/dev/null
    echo "HYBRID_FORGET_DONE $(date -Is)" >>"$RESULTS"
}
trap cleanup EXIT

if [ -n "${BIN:-}" ]; then
    cp "$BIN" "$OUT/bench"
    say "bench: prebuilt $BIN"
elif cc -O2 -DNDEBUG -Wall -Wextra -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
    -I"$REPO/include" -I"$REPO/src" -o "$OUT/bench" "$HERE/large_sample_bench.c" "$REPO/src/tickle.c" \
    "$REPO/src/encoding.c" "$REPO/src/log.c" "$REPO/src/hal_linux.c" -lpthread -lm >"$OUT/build.log" 2>&1; then
    say "bench: $REPO sha=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null) dirty_core_paths=$(git -C "$REPO" \
        status --porcelain -- src include 2>/dev/null | wc -l)"
else
    say "VOID  bench did not build ($OUT/build.log)"
    exit 1
fi
say "=== hybrid_forget_netns $(date -Is) arms=$ARMS dur=${DUR}s nproc=$(nproc) ==="

sudo -n ip netns add "$NS_W" || exit 1
sudo -n ip netns add "$NS_R" || exit 1
sudo -n ip -n "$NS_W" link set lo up
sudo -n ip -n "$NS_R" link set lo up
sudo -n ip link add hfw0 netns "$NS_W" type veth peer name hfr0 netns "$NS_R" || exit 1
sudo -n ip -n "$NS_W" addr add 10.78.0.1/24 broadcast 10.78.0.255 dev hfw0
sudo -n ip -n "$NS_R" addr add 10.78.0.2/24 broadcast 10.78.0.255 dev hfr0
sudo -n ip -n "$NS_W" link set hfw0 up
sudo -n ip -n "$NS_R" link set hfr0 up
sudo -n ip -n "$NS_W" route add default dev hfw0
sudo -n ip -n "$NS_R" route add default dev hfr0
sudo -n ip netns exec "$NS_W" tc qdisc add dev hfw0 root netem limit 100000 delay 100us rate 1gbit || exit 1
sudo -n ip netns exec "$NS_R" tc qdisc add dev hfr0 root netem limit 100000 delay 100us rate 1gbit || exit 1
THRESH=$(sudo -n ip netns exec "$NS_R" cat /proc/sys/net/ipv4/ipfrag_high_thresh)
FRAGTIME=$(sudo -n ip netns exec "$NS_R" cat /proc/sys/net/ipv4/ipfrag_time)
say "reader netns: ipfrag_high_thresh=$THRESH ipfrag_time=$FRAGTIME"

# The sampler: root inside the reader's namespace, bounded by its own count (ends by itself; nothing kills it).
# shellcheck disable=SC2016 # expanded by the sampler's own sh, not here
SAMPLER='fast=$1; slow=$2; i=0
while [ $i -lt $((fast + slow)) ]; do
  echo "T $(date +%s.%N)"; grep FRAG /proc/net/sockstat; grep "^Ip:" /proc/net/snmp | tail -1
  if [ $i -lt $fast ]; then sleep 0.05; else sleep 0.5; fi; i=$((i + 1))
done'

for arm in $ARMS; do
    IFS=: read -r mode loss reps <<<"$arm"
    if [ "$loss" = 0 ]; then
        sudo -n ip netns exec "$NS_W" tc qdisc change dev hfw0 root netem limit 100000 delay 100us rate 1gbit || exit 1
    else
        sudo -n ip netns exec "$NS_W" tc qdisc change dev hfw0 root netem limit 100000 delay 100us rate 1gbit \
            loss "$loss%" || exit 1
    fi
    pub_args=()
    sub_args=()
    [ "$mode" = ip ] && pub_args=(-F ip) && sub_args=(-C)
    for ((rep = 1; rep <= reps; rep++)); do
        load=$(cut -d' ' -f1 /proc/loadavg)
        dir=$OUT/${mode}${loss}_r$rep
        mkdir -p "$dir"
        # shellcheck disable=SC2024 # written by root inside the namespace: intended
        sudo -n ip netns exec "$NS_R" timeout $((DUR + 20)) prlimit --as=2000000000 "$OUT/bench" sub -s 1048576 \
            -d $((DUR + 6)) -R -b 10.78.0.255 ${sub_args[@]+"${sub_args[@]}"} >"$dir/sub.log" 2>"$dir/sub.err" &
        sub_pid=$!
        sleep 1
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS_R" sh -c "$SAMPLER" sampler 120 $((DUR * 2 - 8)) >"$dir/frag.txt" 2>&1 &
        sampler_pid=$!
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS_W" timeout $((DUR + 20)) prlimit --as=2000000000 "$OUT/bench" pub -s 1048576 -r 30 \
            -d "$DUR" -R -b 10.78.0.255 ${pub_args[@]+"${pub_args[@]}"} >"$dir/pub.log" 2>"$dir/pub.err"
        wait "$sub_pid"
        wait "$sampler_pid"
        for f in "$dir"/*.err "$dir/frag.txt"; do
            [ "$(stat -c %s "$f")" -gt 20000000 ] && truncate -s 20000000 "$f"
        done
        echo "REP ${mode}${loss} r$rep load=$load dir=$dir" >>"$RESULTS"
        echo "  $(date +%T) ${mode}${loss} r$rep done (load $load)"
        sleep 31 # the reader namespace's incomplete queues expire (ipfrag_time 30 s) before the next rep
    done
done

fi # REREAD

# --- the reading (applies the PRE-REGISTERED rules above) -----------------------------------------------------------
python3 -I - "$RESULTS" "$(nproc)" "$THRESH" <<'PY' | tee -a "$READING"
import re, sys
path, nproc, thresh = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
def f(text, key):
    m = re.search(r'(?:^|\s)' + re.escape(key) + r'=(\S+)', text)
    return m.group(1) if m else None
def read(p):
    try:
        return open(p, errors='replace').read()
    except OSError:
        return ''
def frag(p):
    peak, fails, head, first, last = 0, None, None, None, None
    for line in read(p).splitlines():
        if line.startswith('FRAG:'):
            peak = max(peak, int(line.split()[4]))
        elif line.startswith('Ip:'):
            vals = line.split()[1:]
            if head is None:
                hdr = [l for l in open('/proc/net/snmp') if l.startswith('Ip:')][0].split()[1:]
                head = hdr.index('ReasmFails')
            v = int(vals[head])
            first = v if first is None else first
            last = v
    return peak, (None if first is None else last - first + 0), first
arms, rows = {}, []
for line in open(path):
    if not line.startswith('REP '):
        continue
    _, arm, rep, load, d = line.split()
    load, d = float(load.split('=')[1]), d.split('=', 1)[1]
    a = arms.setdefault(arm, dict(usable=0, void=[], chain=[], full=0, rows=[]))
    pub, sub, perr = read(d + '/pub.log'), read(d + '/sub.log'), read(d + '/pub.err')
    peak, fails, first = frag(d + '/frag.txt')
    why = []
    if 'RESULT: role=pub' not in pub or 'RESULT: role=sub' not in sub: why.append('no_result')
    if load > nproc / 2: why.append('load=%.1f' % load)
    if fails is None: why.append('no_sampler')
    if why:
        a['void'].append('%s(%s)' % (rep, ','.join(why)))
        continue
    a['usable'] += 1
    chain = 'lost whole while its MTU units arrived' in perr
    full = peak >= 0.9 * thresh
    drops = int(f(sub, 'hybrid_receiver_drops') or 0)
    forgets, fevents = int(f(sub, 'hybrid_stale_forgets') or 0), int(f(sub, 'hybrid_stale_events') or 0)
    none_seen = 'code=0xff00' in perr # the PEER_P lines are on stderr (AMENDED 2026-10-25)
    r = dict(rep=rep, chain=chain, full=full, fails=fails, drops=drops, forgets=forgets, fevents=fevents,
             none_seen=none_seen, peak=peak)
    a['full'] += full
    if chain:
        a['chain'].append(r)
    a['rows'].append('%s %s %-6s FULL=%d peak=%d fails=%d drops=%d forgets=%d/%d none_seen=%d n_hist=%s overruns=%s '
                     'p99=%s' % (arm, rep, 'CHAIN' if chain else '-', full, peak, fails, drops, forgets, fevents,
                                 none_seen, f(pub, 'hybrid_n_hist'), f(sub, 'drain_overruns'), f(sub, 'latency_p99_us')))
    a.setdefault('recs', []).append(r)
print('=== reading (ipfrag_high_thresh %d) ===' % thresh)
for arm, a in arms.items():
    for row in a['rows']: print('  ' + row)
    print('%s: usable %d, CHAIN %d, FULL %d, VOID %s' % (arm, a['usable'], len(a['chain']), a['full'],
                                                        ' '.join(a['void']) or '-'))
auto, det, ctl = arms.get('auto1'), arms.get('ip1'), arms.get('ip0')
verdict = None
if ctl is None or ctl['usable'] == 0:
    verdict = 'VOID: no usable control rep (ip:0)'
elif any(r['peak'] >= 0.5 * thresh or r['fails'] != 0 for r in ctl['recs']):
    verdict = 'VOID: the control (runs of 45, no loss) filled reassembly or counted ReasmFails'
if verdict is None and auto is not None:
    bad = [r['rep'] for r in auto['chain'] if r['fails'] == 0 or not r['full'] or r['drops'] > 0]
    detbad = [r['rep'] for r in (det or {}).get('recs', []) if not (r['full'] and r['fails'] > 0)]
    if bad or detbad:
        verdict = 'FALSIFIED (H2): CHAIN reps not FULL / no FAILS / with DROPS: %s; ip:1 reps not FULL: %s' % (
            bad or '-', detbad or '-')
    elif len(auto['chain']) < 2:
        verdict = 'UNDECIDED: auto has %d usable CHAIN reps (< 2)' % len(auto['chain'])
    elif any(r['forgets'] < 1 or r['fevents'] > 8 or not r['none_seen'] for r in auto['chain']):
        verdict = 'H1 FALSIFIED: a CHAIN rep without a stale forget of <= 8 events or without "none seen"'
    elif det is None or det['usable'] < 2:
        verdict = 'UNDECIDED: the deterministic arm has %d usable reps (< 2)' % (0 if det is None else det['usable'])
    else:
        verdict = 'SHOWN: %d CHAIN reps of %d, each a stale forget then a full reassembly; ip:1 FULL %d/%d' % (
            len(auto['chain']), auto['usable'], det['full'], det['usable'])
    nc = [r['rep'] for r in auto['recs'] if r['full'] and not r['chain']] if auto.get('recs') else []
    print('auto reps FULL without the writer\'s warning: %s' % (nc or '-'))
print('VERDICT ' + (verdict or 'UNDECIDED: no auto arm'))
PY
