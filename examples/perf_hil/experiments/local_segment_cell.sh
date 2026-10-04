#!/usr/bin/env bash
# The same-host segment cell on THIS PC, inside a private network namespace - the fast loop for changes to the
# segment or the poll loop, before the rig. Not a published figure: the PC is not the rig and its numbers are not
# comparable to COMPARISON's. What it is for is behaviour: does the ring still deliver, does it drop, which way did
# the per-sample CPU move against a baseline built from the same tree.
#
# Why it exists (2026-10-04): a reader-spin arm passed all 46 unit-test binaries and then delivered 1.2k samples/s
# out of 1.5M sent on the rig - the mock HAL has no segment, so nothing local had ever driven a real ring at rate.
#
# Usage: ARMS="name:flags name:flags" local_segment_cell.sh      (flags comma-separated, like segment_arms.sh)
# Reading rule, enforced: an arm whose delivered/sent < 0.5 is reported BROKEN regardless of its rates, because a
# reader that does not keep up at all is a defect, not a performance result.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
ARMS=${ARMS:-base:}
SCEN=${SCEN:-best_effort_throughput}
SIZE=${SIZE:-p3}
DUR=${DUR:-3}
REPS=${REPS:-3}
NS=tickle_lseg_$$
OUT=${OUT:-/tmp/local_segment_cell_$$.txt}
: >"$OUT"
cleanup() { sudo -n ip netns del "$NS" 2>/dev/null; }
trap cleanup EXIT
sudo -n ip netns add "$NS" || { echo "FATAL cannot create netns" | tee -a "$OUT"; exit 1; }
sudo -n ip netns exec "$NS" ip link set lo up
sudo -n ip netns exec "$NS" ip route add default dev lo
# The bench's compiled-in link is the rig's 192.168.10.255 and a node refuses to start without an interface carrying
# it. A dummy link gives it one; Linux loops a local broadcast back to local sockets, so discovery still works and
# nothing leaves the namespace.
sudo -n ip netns exec "$NS" ip link add bench0 type dummy
sudo -n ip netns exec "$NS" ip addr add 192.168.10.1/24 broadcast 192.168.10.255 dev bench0
sudo -n ip netns exec "$NS" ip link set bench0 up

names=()
for spec in $ARMS; do
    name=${spec%%:*}; flags=${spec#*:}; flags=${flags//,/ }
    names+=("$name")
    dir=/tmp/lseg_$$/$name
    mkdir -p "$dir"
    if ! (cd "$REPO/examples/perf_hil/tickle" && TICKLE_EXTRA_CFLAGS="$flags" ./build.sh "$SCEN" "$SIZE" >"$dir/build.log" 2>&1); then
        echo "FATAL build $name"; tail -5 "$dir/build.log"; exit 1
    fi
    cp "$REPO/examples/perf_hil/tickle/${SCEN}_${SIZE}/client" "$REPO/examples/perf_hil/tickle/${SCEN}_${SIZE}/server" "$dir/"
    echo "arm $name flags='$flags' client $(sha256sum "$dir/client" | cut -c1-16)" | tee -a "$OUT"
done

for rep in $(seq 1 "$REPS"); do
    for name in "${names[@]}"; do
        dir=/tmp/lseg_$$/$name
        # SC2024: the redirects are meant to stay ours - the logs belong to this user, not to root.
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS" env BENCH_IFACE=lo taskset -c 2 "$dir/server" -Q -d $((DUR + 4)) >"$dir/srv.log" 2>&1 &
        srv=$!
        sleep 1
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS" env BENCH_IFACE=lo taskset -c 4 "$dir/client" -Q -d "$DUR" >"$dir/cli.log" 2>&1
        wait "$srv"
        { grep -h '^RESULT' "$dir/cli.log" "$dir/srv.log" || echo "NORESULT"; } | sed "s/^/arm=$name rep=$rep /" >>"$OUT"
    done
done

python3 - "$OUT" "${names[@]}" <<'PYEOF'
import re, sys, statistics as st
path, names = sys.argv[1], sys.argv[2:]
cli, srv = {}, {}
for l in open(path):
    m = re.match(r'arm=(\S+) rep=(\d+) RESULT: (.*)', l)
    if not m:
        if 'NORESULT' in l: print('NO RESULT:', l.strip())
        continue
    f = dict(kv.split('=', 1) for kv in m.group(3).split() if '=' in kv)
    (cli if f.get('role') == 'client' else srv)[(m.group(1), m.group(2))] = f
for n in names:
    rows = []
    for (a, r), c in cli.items():
        if a != n or (a, r) not in srv: continue
        s = srv[(a, r)]; el = float(c['elapsed_s']); sent = int(c['sent']); recv = int(s['recv'])
        rows.append((recv / el / 1e3, recv / max(sent, 1), float(c['sched_cpu_s']) / sent * 1e6,
                     float(s['sched_cpu_s']) / max(recv, 1) * 1e6, int(c.get('tx_shm', 0)) / max(sent, 1)))
    if not rows:
        print(f'{n}: no complete reps'); continue
    d = [x[0] for x in rows]; ratio = min(x[1] for x in rows)
    tag = 'BROKEN' if ratio < 0.5 else 'ok'
    print(f'{n:8} delivered {st.mean(d):8.1f} k/s ({min(d):.1f}..{max(d):.1f})  worst recv/sent {ratio:.3f}'
          f'  client {st.mean(x[2] for x in rows):.3f} us/sent  server {st.mean(x[3] for x in rows):.3f} us/recv'
          f'  shm share {min(x[4] for x in rows):.3f}  [{tag}]')
PYEOF
echo "raw: $OUT"
