#!/usr/bin/env bash
# hybrid_mode_netns.sh - which mode a RELIABLE AUTO writer settles in at 1% loss, counted per rep (2026-10-24).
#
# The rig's Array1m 1% cell (rmw_keepall_rig.sh, hmr_1m_20261024-192010: 12 reps x 4 arms) sent IP-fragment runs of
# n = 2 in 23 of 48 reps and MTU units in the rest, on every build; within a mode CPU and p99 were the same across
# builds, between the modes they were not. Each rep's n_hist showed one switch from n = 1 to n = 2 at an announce
# tick and none back. This harness counts the same thing off the rig: two private namespaces joined by one veth pair,
# the writer's egress "netem limit 100000 delay 100us rate 1gbit loss L%" (the rig: loss on the publisher's egress),
# the reader's egress the same without loss, large_sample_bench 1 MiB RELIABLE KEEP_LAST 10 at 30 Hz for DUR seconds.
# Several trees, built here, run rep by rep in alternation (never at once: the PC is shared).
#
#   TREES="P=/path/to/parent F=/path/to/change"  (default: P=this tree)
#   REPS=16 DUR=30 LOSS=1 OFFLOAD=1 (0: TT_UDP_OFFLOAD=0 on both nodes - no GSO, no GRO) OUT=dir BIN_<label>=prebuilt
#
# PRE-REGISTERED (written before the first run; the python at the end applies it - it, not this comment, decides):
#   VOID (a rep says nothing): no RESULT line from either node; the PC's 1-minute load above nproc/2 at the rep's start;
#     a reader that delivered fewer samples than the writer published less 2 (RELIABLE KEEP_LAST 10 at 1% delivers all).
#   A rep is IP-RUN when its writer sent more than half of its first-transmission units in runs
#     (run_units > mtu_units), else MTU - the rig's split (gso_sends 0-10,000 against ~39,580).
#   CONTROL the change cannot touch: LOSS=0 - no event, "none seen", n_max on every tree; any tree with an MTU rep
#     there is VOID for this reading (the instrument counts something else).
#   The design's answer at the measured p: the reader's final p (hybrid_rx_loss events / frames, the posterior mean) and
#     n*(p) from the objective (DESIGN.md section 8, Cost(n), u 1454 B, h 26 B; n = 1 above p = 0.886%). A tree whose
#     IP-RUN reps end with the writer's p (hybrid_peer_loss_p) on the other side of 0.886% from the reader's measured p
#     is LATCHED there: the writer chose from a p the reader no longer measures.
#   For a change F against parent P (the announce-on-decision fix, DESIGN.md section 8 "Announcing p"):
#     SHOWN when P has at least 3 IP-RUN reps of its usable ones, F has none, and no F rep is LATCHED.
#     FALSIFIED when F has an IP-RUN rep at a measured p above 0.886% (n* = 1) - the latch is not the only cause - or
#     any LATCHED rep.
#     UNDECIDED when P has fewer than 3 IP-RUN reps (the parent did not show the split: nothing to remove).
#   With P's IP-RUN rate r ~ 0.45 (the rig), 16 usable reps of F show none by chance only if F did not change it with
#   (1 - 0.45)^16 = 7e-5; at r = 0.2, 3e-2.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
REPS=${REPS:-16}
DUR=${DUR:-30}
LOSS=${LOSS:-1}
OFFLOAD=${OFFLOAD:-1}
TREES=${TREES:-"P=$REPO"}
OUT=${OUT:-$HOME/tmp_build/hyb_mode/run_$(date +%Y%m%d_%H%M%S)}
export TMPDIR=${TMPDIR:-$HOME/tmp_build}
mkdir -p "$OUT"
NS_W=hmw$$
NS_R=hmr$$
RESULTS=$OUT/results.txt
say() { echo "$*" | tee -a "$RESULTS"; }

cleanup() {
    sudo -n ip netns del "$NS_W" 2>/dev/null
    sudo -n ip netns del "$NS_R" 2>/dev/null
    echo "HYBRID_MODE_DONE $(date -Is)" >>"$RESULTS"
}
trap cleanup EXIT

# --- build each tree ------------------------------------------------------------------------------------------------
LABELS=()
for t in $TREES; do
    label=${t%%=*}
    tree=${t#*=}
    LABELS+=("$label")
    pre="BIN_$label"
    if [ -n "${!pre:-}" ]; then
        cp "${!pre}" "$OUT/bench_$label"
        say "tree $label: prebuilt ${!pre}"
        continue
    fi
    if ! cc -O2 -DNDEBUG -Wall -Wextra -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
        -I"$tree/include" -I"$tree/src" -o "$OUT/bench_$label" "$HERE/large_sample_bench.c" "$tree/src/tickle.c" \
        "$tree/src/encoding.c" "$tree/src/log.c" "$tree/src/hal_linux.c" -lpthread -lm >"$OUT/build_$label.log" 2>&1; then
        say "VOID  tree $label did not build ($OUT/build_$label.log)"
        exit 1
    fi
    sha=$(git -C "$tree" rev-parse --short HEAD 2>/dev/null)
    dirty=$(git -C "$tree" status --porcelain -- src include 2>/dev/null | wc -l)
    say "tree $label: $tree sha=$sha dirty_core_paths=$dirty"
done
say "=== hybrid_mode_netns $(date -Is) reps=$REPS dur=${DUR}s loss=$LOSS% offload=$OFFLOAD nproc=$(nproc) ==="

# --- namespaces -----------------------------------------------------------------------------------------------------
sudo -n ip netns add "$NS_W" || exit 1
sudo -n ip netns add "$NS_R" || exit 1
sudo -n ip -n "$NS_W" link set lo up
sudo -n ip -n "$NS_R" link set lo up
sudo -n ip link add hmw0 netns "$NS_W" type veth peer name hmr0 netns "$NS_R" || exit 1
sudo -n ip -n "$NS_W" addr add 10.77.0.1/24 broadcast 10.77.0.255 dev hmw0
sudo -n ip -n "$NS_R" addr add 10.77.0.2/24 broadcast 10.77.0.255 dev hmr0
sudo -n ip -n "$NS_W" link set hmw0 up
sudo -n ip -n "$NS_R" link set hmr0 up
sudo -n ip -n "$NS_W" route add default dev hmw0
sudo -n ip -n "$NS_R" route add default dev hmr0
if [ "$LOSS" = 0 ]; then
    sudo -n ip netns exec "$NS_W" tc qdisc add dev hmw0 root netem limit 100000 delay 100us rate 1gbit || exit 1
else
    sudo -n ip netns exec "$NS_W" tc qdisc add dev hmw0 root netem limit 100000 delay 100us rate 1gbit loss "$LOSS%" ||
        exit 1
fi
sudo -n ip netns exec "$NS_R" tc qdisc add dev hmr0 root netem limit 100000 delay 100us rate 1gbit || exit 1
ENVS=()
[ "$OFFLOAD" = 0 ] && ENVS=(env TT_UDP_OFFLOAD=0)

# --- reps, trees in alternation --------------------------------------------------------------------------------------
for ((rep = 1; rep <= REPS; rep++)); do
    for label in "${LABELS[@]}"; do
        load=$(cut -d' ' -f1 /proc/loadavg)
        dir=$OUT/${label}_r$rep
        mkdir -p "$dir"
        # shellcheck disable=SC2024 # written by root inside the namespace: intended
        sudo -n ip netns exec "$NS_R" timeout $((DUR + 20)) ${ENVS[@]+"${ENVS[@]}"} prlimit --as=2000000000 \
            "$OUT/bench_$label" sub -s 1048576 -d $((DUR + 6)) -R -b 10.77.0.255 >"$dir/sub.log" 2>"$dir/sub.err" &
        sub_pid=$!
        sleep 1
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS_W" timeout $((DUR + 20)) ${ENVS[@]+"${ENVS[@]}"} prlimit --as=2000000000 \
            "$OUT/bench_$label" pub -s 1048576 -r 30 -d "$DUR" -R -b 10.77.0.255 >"$dir/pub.log" 2>"$dir/pub.err"
        wait "$sub_pid"
        for f in "$dir"/*.err; do # cap what a trace build writes
            [ "$(stat -c %s "$f")" -gt 20000000 ] && truncate -s 20000000 "$f"
        done
        publine=$(grep -m1 '^RESULT:' "$dir/pub.log")
        subline=$(grep -m1 '^RESULT:' "$dir/sub.log")
        echo "REP $label r$rep load=$load | pub: ${publine#RESULT: } | sub: ${subline#RESULT: }" >>"$RESULTS"
        echo "  $(date +%T) $label r$rep done (load $load)"
    done
done

# --- the reading (applies the PRE-REGISTERED rules above) -----------------------------------------------------------
python3 -I - "$RESULTS" "$(nproc)" "$LOSS" <<'PY' | tee -a "$RESULTS"
import re, sys
path, nproc, loss = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
U, H = 1454.0, 26.0
def n_star(p, n_max=45):
    if p <= 0: return n_max
    if p >= 1: return 1
    best, bn, clean = 0.0, 1, 1.0
    for n in range(1, n_max + 1):
        clean *= 1 - p
        c = U + H / n + (1 - clean) * (U + H)
        if n == 1 or c < best: best, bn = c, n
    return bn
def decode(code):
    if code == 0: return 1.0
    p = (256 + (code & 0xFF)) / 512.0
    for _ in range(code >> 8): p *= 0.5
    return p
def f(line, key):
    m = re.search(r'(?:^|\s)' + re.escape(key) + r'=(\S+)', line)
    return m.group(1) if m else None
THRESH = 0.00886
arms = {}
for line in open(path):
    if not line.startswith('REP '): continue
    head, pub, sub = line.split(' | ')
    _, label, rep, load = head.split()[:4]
    load = float(load.split('=')[1])
    a = arms.setdefault(label, dict(usable=0, void=[], ip=0, mtu=0, latched=0, ip_above=0, rows=[]))
    why = []
    if 'role=pub' not in pub or 'role=sub' not in sub: why.append('no_result')
    if load > nproc / 2: why.append('load=%.1f' % load)
    published, delivered = f(pub, 'published'), f(sub, 'delivered')
    if not why and int(delivered) < int(published) - 2: why.append('delivered=%s/%s' % (delivered, published))
    if why:
        a['void'].append('%s(%s)' % (rep, ','.join(why))); continue
    a['usable'] += 1
    ru, mu = int(f(pub, 'hybrid_run_units')), int(f(pub, 'hybrid_mtu_units'))
    ev, fr = map(int, f(sub, 'hybrid_rx_loss').split('/'))
    p_meas = (ev + 1) / fr if fr else 1.0
    p_w = decode(int(f(pub, 'hybrid_peer_loss_p') or '0', 16))
    mode = 'IP-RUN' if ru > mu else 'MTU'
    latched = mode == 'IP-RUN' and ((p_w < THRESH) != (p_meas < THRESH))
    a['ip' if mode == 'IP-RUN' else 'mtu'] += 1
    a['latched'] += latched
    a['ip_above'] += (mode == 'IP-RUN' and p_meas >= THRESH)
    a['rows'].append('%s %s %-6s n_hist=%s p_reader=%.4f%% n*=%d p_writer=%.4f%% n*=%d%s lat_p99=%s cpu_sub=%s cpu_pub=%s' % (
        label, rep, mode, f(pub, 'hybrid_n_hist'), 100 * p_meas, n_star(p_meas), 100 * p_w, n_star(p_w),
        ' LATCHED' if latched else '', f(sub, 'latency_p99_us'), f(sub, 'cpu_ms_per_mb'), f(pub, 'cpu_ms_per_mb')))
print('=== reading (loss %g%%) ===' % loss)
for label, a in arms.items():
    for r in a['rows']: print('  ' + r)
    print('%s: usable %d, IP-RUN %d, MTU %d, LATCHED %d, IP-RUN at n*(p)=1 %d, VOID %s' % (
        label, a['usable'], a['ip'], a['mtu'], a['latched'], a['ip_above'], ' '.join(a['void']) or '-'))
if loss == 0:
    for label, a in arms.items():
        print('CONTROL %s: %s' % (label, 'VOID (an MTU rep at 0%% loss)' if a['mtu'] else 'ok (every rep in runs)'))
elif 'P' in arms and 'F' in arms:
    P, F = arms['P'], arms['F']
    if F['ip_above'] or F['latched']:
        print('VERDICT FALSIFIED: F has %d IP-RUN rep(s) at n*(p)=1 and %d LATCHED' % (F['ip_above'], F['latched']))
    elif P['ip'] < 3:
        print('VERDICT UNDECIDED: P has %d IP-RUN reps (< 3)' % P['ip'])
    elif F['ip'] == 0:
        print('VERDICT SHOWN: P IP-RUN %d/%d, F 0/%d' % (P['ip'], P['usable'], F['usable']))
    else:
        print('VERDICT NOT SHOWN: F IP-RUN %d/%d, each at a measured p below 0.886%% (n* >= 2, the design\'s own choice)'
              % (F['ip'], F['usable']))
PY
