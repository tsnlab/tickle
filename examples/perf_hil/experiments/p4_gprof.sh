#!/usr/bin/env bash
# Where does a p4 sample's extra CPU go on one host? p4_wake_count.sh found p4 and p3 ring and sleep alike, yet the
# p4 client spends about 9 us more CPU per round trip (31.8 -> 40.9 us on this PC). This profiles reliable_latency's
# server and client with gprof (-pg) at p3 and p4, pinging every INTERVAL so the processes stay busy, and prints each
# function's self time per round trip side by side. Behaviour, on THIS PC in a private netns; not a published figure.
#
# HOW TO READ IT, written before running:
#   The functions whose self time per round trip grows by more than 1 us from p3 to p4 are where the extra work is.
#   A rise spread thinly over many functions (none above 1 us) means there is no single place, and the work is
#   proportional to the bytes - then encode-into-slot or whole-record sends (ROADMAP item 6) are the remedy.
#   gprof samples at 100 Hz: a function under ~0.3 us per round trip is noise at this sample count, and the total
#   per round trip must land within 30% of the bench's own sched_cpu_s per round trip, or the profile is VOID.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SIZES=${SIZES:-"p3 p4"}; DUR=${DUR:-20}; INTERVAL=${INTERVAL:-0.0001}
NS=tickle_gprof_$$
OUT=${OUT:-$HOME/rig_results_safe/p4_gprof_$(git -C "$REPO" rev-parse --short HEAD)_$(date +%Y%m%d-%H%M%S).txt}
WORK=$(mktemp -d /tmp/p4gprof.XXXXXX)
: >"$OUT"
cleanup() {
    sudo -n ip netns del "$NS" 2>/dev/null
}
trap cleanup EXIT
sudo -n ip netns add "$NS" || { echo "FATAL cannot create netns" | tee -a "$OUT"; exit 1; }
sudo -n ip netns exec "$NS" ip link set lo up
sudo -n ip netns exec "$NS" ip route add default dev lo
sudo -n ip netns exec "$NS" ip link add bench0 type dummy
sudo -n ip netns exec "$NS" ip addr add 192.168.10.1/24 broadcast 192.168.10.255 dev bench0
sudo -n ip netns exec "$NS" ip link set bench0 up

echo "=== p4 gprof, $(git -C "$REPO" rev-parse --short HEAD), $(date -Is), ${DUR}s at -i $INTERVAL, work $WORK ===" | tee -a "$OUT"
for sz in $SIZES; do
    (cd "$REPO/examples/perf_hil/tickle" && TICKLE_EXTRA_CFLAGS="-pg" ./build.sh reliable_latency "$sz" >"$WORK/build_$sz.log" 2>&1) \
        || { echo "FATAL build $sz" | tee -a "$OUT"; tail -5 "$WORK/build_$sz.log"; exit 1; }
    for role in server client; do
        mkdir -p "$WORK/$sz/$role"
        cp "$REPO/examples/perf_hil/tickle/reliable_latency_$sz/$role" "$WORK/$sz/$role/"
    done
    # gmon.out lands in each process's working directory, so each runs in its own.
    # shellcheck disable=SC2024 # the logs stay ours
    (cd "$WORK/$sz/server" && sudo -n ip netns exec "$NS" env BENCH_IFACE=lo taskset -c 2 ./server -d $((DUR + 6)) >srv.log 2>&1) &
    srv=$!
    sleep 1
    # shellcheck disable=SC2024
    (cd "$WORK/$sz/client" && sudo -n ip netns exec "$NS" env BENCH_IFACE=lo taskset -c 4 ./client -i "$INTERVAL" -d "$DUR" >cli.log 2>&1)
    wait "$srv"
    for role in server client; do
        d="$WORK/$sz/$role"
        grep -h '^RESULT' "$d/"*.log | sed "s/^/size=$sz role=$role /" >>"$OUT"
        gprof -b -p "$d/$role" "$d/gmon.out" >"$d/flat.txt" 2>&1 || echo "gprof failed for $sz $role" | tee -a "$OUT"
    done
done

# shellcheck disable=SC2086 # SIZES is a list, one argument per size
python3 - "$WORK" "$OUT" $SIZES <<'PYEOF' | tee -a "$OUT"
import re, sys
work, out, sizes = sys.argv[1], sys.argv[2], sys.argv[3:]
rts, cpu = {}, {}
for l in open(out):
    m = re.match(r'size=(\S+) role=(\S+) RESULT: (.*)', l)
    if m:
        f = dict(kv.split('=', 1) for kv in m.group(3).split() if '=' in kv)
        if m.group(2) == 'client':
            rts[m.group(1)] = int(f['recv'])
            cpu[(m.group(1), 'client')] = float(f['sched_cpu_s'])
for role in ('client', 'server'):
    prof = {}
    for sz in sizes:
        fn = {}
        for l in open(f'{work}/{sz}/{role}/flat.txt'):
            m = re.match(r'\s*[\d.]+\s+[\d.]+\s+([\d.]+)\s+.*?\s(\S+)\s*$', l)
            if m:
                fn[m.group(2)] = float(m.group(1)) / rts[sz] * 1e6  # us per round trip
        prof[sz] = fn
        total = sum(fn.values())
        check = cpu.get((sz, role))
        note = f'  bench sched_cpu {check / rts[sz] * 1e6:.2f} us/rt' if check else ''
        print(f'{role} {sz}: gprof total {total:.2f} us/rt over {rts[sz]} round trips{note}')
        if check and not (0.7 <= total / (check / rts[sz] * 1e6) <= 1.3):
            print(f'  VOID for {role} {sz}: gprof total is not within 30% of the bench\'s own CPU (kernel time is '
                  f'outside gprof; read the rise, not the total)')
    a, b = prof[sizes[0]], prof[sizes[-1]]
    rows = sorted(set(a) | set(b), key=lambda k: b.get(k, 0) - a.get(k, 0), reverse=True)
    print(f'{role}: function, {sizes[0]} -> {sizes[-1]} self us per round trip (largest rise first)')
    for k in rows[:12]:
        print(f'  {k:40} {a.get(k, 0):7.2f} -> {b.get(k, 0):7.2f}  ({b.get(k, 0) - a.get(k, 0):+.2f})')
PYEOF
echo "raw: $OUT  profiles: $WORK"
