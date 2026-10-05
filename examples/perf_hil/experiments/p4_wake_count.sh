#!/usr/bin/env bash
# Why is a p4 round trip on one host 21 us slower than p3's (RESULTS S9 0.030 ms, S10 0.051 ms)? Counts, per round
# trip, what each side did over the segment: datagrams, doorbells rung, sleeps. Runs reliable_latency at p3 and p4
# on THIS PC in a private network namespace. Counts are accounting, not a performance figure, so the PC is fine; no
# RTT from here is published.
#
# H1: p4 pays for a second wake per direction. Its sample crosses as two datagrams; the writer rings the reader's
#   sleep after the first, and if the reader drains that one and sleeps again before the second lands, the second
#   rings again. A wake on the rig Pi is several us, two per direction is about the 21 us.
#
# HOW TO READ IT, written before running and enforced below:
#   TREATMENT CHECK (an arm must report its treatment): p3's client tx_shm per round trip must be 1.0 +- 0.05 and
#     p4's 2.0 +- 0.1, and both sides' rx_shm must match the other side's tx_shm within 1%. Otherwise the arm did
#     not run the shape it names, or the segment did not carry it, and the run is VOID.
#   CONTROL: p3's doorbells per round trip, per side, must be about 1 (0.8..1.2): one sample each way, one ring. If
#     p3 is not, the count does not mean what H1 needs it to mean and the run is VOID.
#   H1 SUPPORTED if p4's doorbells per round trip exceed p3's by 0.5 or more on either side (a second wake in at
#     least half of the round trips; a reader is only woken by a doorbell). H1 REFUTED if they are within 0.1 on
#     both sides: the reader takes both fragments in one wake, and the 21 us is in the work, not the wakes. Between
#     the two: PARTIAL, with the numbers. Sleeps are printed beside them, not judged.
#   (Revised after the first run, 2026-10-05 11:19: the control was "sleeps per round trip about 1", and the client
#     sleeps twice per round trip - once for the reply, once for its own ping timer - in p3 and p4 alike, so that
#     run read VOID. Doorbells are what a second wake needs, and are what H1 is about.)
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SIZES=${SIZES:-"p3 p4"}; REPS=${REPS:-3}; DUR=${DUR:-10}; INTERVAL=${INTERVAL:-0.005}
NS=tickle_p4w_$$
OUT=${OUT:-$HOME/rig_results_safe/p4_wake_count_$(git -C "$REPO" rev-parse --short HEAD)_$(date +%Y%m%d-%H%M%S).txt}
WORK=$(mktemp -d /tmp/p4w.XXXXXX)
: >"$OUT"
cleanup() {
    sudo -n ip netns del "$NS" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT
sudo -n ip netns add "$NS" || { echo "FATAL cannot create netns" | tee -a "$OUT"; exit 1; }
sudo -n ip netns exec "$NS" ip link set lo up
sudo -n ip netns exec "$NS" ip route add default dev lo
sudo -n ip netns exec "$NS" ip link add bench0 type dummy
sudo -n ip netns exec "$NS" ip addr add 192.168.10.1/24 broadcast 192.168.10.255 dev bench0
sudo -n ip netns exec "$NS" ip link set bench0 up

echo "=== p4 wake count, $(git -C "$REPO" rev-parse --short HEAD), $(date -Is), sizes $SIZES, $REPS reps, ${DUR}s at -i $INTERVAL ===" | tee -a "$OUT"
for sz in $SIZES; do
    (cd "$REPO/examples/perf_hil/tickle" && ./build.sh reliable_latency "$sz" >"$WORK/build_$sz.log" 2>&1) \
        || { echo "FATAL build $sz" | tee -a "$OUT"; tail -5 "$WORK/build_$sz.log"; exit 1; }
    mkdir -p "$WORK/$sz"
    cp "$REPO/examples/perf_hil/tickle/reliable_latency_$sz/client" "$REPO/examples/perf_hil/tickle/reliable_latency_$sz/server" "$WORK/$sz/"
done
for rep in $(seq 1 "$REPS"); do
    for sz in $SIZES; do
        # SC2024: the redirects are meant to stay ours - the logs belong to this user, not to root.
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS" env BENCH_IFACE=lo taskset -c 2 "$WORK/$sz/server" -d $((DUR + 6)) >"$WORK/srv.log" 2>&1 &
        srv=$!
        sleep 1
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS" env BENCH_IFACE=lo taskset -c 4 "$WORK/$sz/client" -i "$INTERVAL" -d "$DUR" >"$WORK/cli.log" 2>&1
        wait "$srv"
        { grep -h '^RESULT' "$WORK/cli.log" "$WORK/srv.log" || echo "NORESULT"; } | sed "s/^/size=$sz rep=$rep /" >>"$OUT"
    done
done

python3 - "$OUT" <<'PYEOF' | tee -a "$OUT"
import re, sys, statistics as st
cli, srv = {}, {}
for l in open(sys.argv[1]):
    m = re.match(r'size=(\S+) rep=(\d+) RESULT: (.*)', l)
    if not m:
        continue
    f = dict(kv.split('=', 1) for kv in m.group(3).split() if '=' in kv)
    (srv if f.get('role') == 'server' else cli)[(m.group(1), m.group(2))] = f
per, void = {}, []
for key, c in sorted(cli.items()):
    s = srv.get(key)
    if s is None:
        void.append(f'{key}: no server RESULT'); continue
    rt = int(c['recv'])
    if rt == 0:
        void.append(f'{key}: no round trips'); continue
    row = dict(tx=int(c['tx_shm']) / rt, ctx=int(c['tx_shm']), crx=int(c['rx_shm']), stx=int(s['tx_shm']),
               srx=int(s['rx_shm']), csleep=int(c['sleeps']) / rt, ssleep=int(s['sleeps']) / rt,
               cbell=int(c['doorbells_sent']) / rt, sbell=int(s['doorbells_sent']) / rt, rtt=float(c['rtt_avg_ms']))
    want = {'p3': 1.0, 'p4': 2.0}.get(key[0])
    if want is None or abs(row['tx'] - want) > 0.05 * want:
        void.append(f'{key}: tx_shm per round trip {row["tx"]:.3f}, the shape would give {want}'); continue
    if abs(row['srx'] - row['ctx']) > 0.01 * row['ctx'] or abs(row['crx'] - row['stx']) > 0.01 * max(row['stx'], 1):
        void.append(f'{key}: rx/tx do not match across sides ({row})'); continue
    per.setdefault(key[0], []).append(row)
for v in void:
    print('VOID', v)
for sz, rows in sorted(per.items()):
    print(f'{sz}: n={len(rows)}  shm datagrams/rt {st.mean(r["tx"] for r in rows):.3f}  sleeps/rt client '
          f'{st.mean(r["csleep"] for r in rows):.3f} server {st.mean(r["ssleep"] for r in rows):.3f}  doorbells/rt '
          f'client {st.mean(r["cbell"] for r in rows):.3f} server {st.mean(r["sbell"] for r in rows):.3f}  '
          f'(rtt {st.mean(r["rtt"] for r in rows):.3f} ms on this PC, not a figure)')
if 'p3' not in per or 'p4' not in per or len(per['p3']) < 2 or len(per['p4']) < 2:
    print('NO VERDICT: fewer than 2 usable reps of p3 or p4'); sys.exit(0)
p3c, p3s = st.mean(r['cbell'] for r in per['p3']), st.mean(r['sbell'] for r in per['p3'])
if not (0.8 <= p3c <= 1.2 and 0.8 <= p3s <= 1.2):
    print(f'VOID: control failed - p3 doorbells per round trip {p3c:.3f} / {p3s:.3f}, not about 1'); sys.exit(0)
dc = st.mean(r['cbell'] for r in per['p4']) - p3c
ds = st.mean(r['sbell'] for r in per['p4']) - p3s
if dc >= 0.5 or ds >= 0.5:
    print(f'H1 SUPPORTED: p4 rings more doorbells per round trip than p3 by {dc:+.3f} (client) {ds:+.3f} (server)')
elif abs(dc) <= 0.1 and abs(ds) <= 0.1:
    print(f'H1 REFUTED: p4 and p3 ring alike ({dc:+.3f} client, {ds:+.3f} server); the extra time is in the work')
else:
    print(f'H1 PARTIAL: p4 - p3 doorbells per round trip {dc:+.3f} (client) {ds:+.3f} (server)')
PYEOF
echo "raw: $OUT"
