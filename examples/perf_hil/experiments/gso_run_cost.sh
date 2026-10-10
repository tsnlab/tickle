#!/usr/bin/env bash
# gso_run_cost.sh - the sender's instructions per datagram for a run of n same-size datagrams sent as one UDP_SEGMENT
# message against n messages of one sendmmsg() (gso_run_cost.c), n swept, on this PC, in two private namespaces joined
# by a veth whose sending end cannot cut a run (tx-udp-segmentation off: the kernel segments in software, as on the
# Pi 5, whose macb has it "off [fixed]"). The receiving end drops everything at tc ingress, so no socket or process
# there adds to the count, and what is left of the receive path is the same per datagram in both modes.
#
# WHAT IT DECIDES (hal_linux.c TT_GSO_MIN_SEGMENTS, written before the run): per rep, straight lines through
# insns_per_run against n for each mode, n >= 2: plain = p x n + c, gso = s x n + F. Break-even n* = (F - c) / (p - s).
# The rule takes the smallest integer n with gso below plain by more than the A/A spread (rep-to-rep range of plain),
# and checks it against the fit. A run below it goes as plain messages. FALSIFIED (no rule from here) if p <= s, or if
# the gso line does not cross the plain one inside 2..64.
#
# Instructions, not time, and never a published figure: it says where the fixed cost of a run is paid back, which
# depends on the code path (the same kernel functions run on the Pi's aarch64), not on clock or cache.
# Usage: gso_run_cost.sh [REPS=5] [RUNS=20000] [SIZE=1468]   output: $OUT (default ~/rig_results_safe/gso_run_cost_<stamp>.txt)
set -u
REPS=${REPS:-5}
RUNS=${RUNS:-20000}
SIZE=${SIZE:-1468}
NS_LIST="1 2 3 4 5 6 8 12 16 24 32 44"
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HOME/rig_results_safe/gso_run_cost_$(date +%Y%m%d-%H%M%S).txt}
WORK=$(mktemp -d /tmp/gso_run_cost.XXXXXX) || exit 1
NS1=gsorc-tx-$$
NS2=gsorc-rx-$$
cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    rm -rf "$WORK"
    return 0
}
trap cleanup EXIT
mkdir -p "$(dirname "$OUT")"
cc -O2 -Wall -Wextra -o "$WORK/gso_run_cost" "$HERE/gso_run_cost.c" || exit 1
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add gsorc1 netns "$NS1" type veth peer name gsorc2 netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.77.1/24 dev gsorc1
sudo -n ip -n "$NS2" addr add 192.168.77.2/24 dev gsorc2
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set gsorc1 up
sudo -n ip -n "$NS2" link set gsorc2 up
sudo -n ip netns exec "$NS1" ethtool -K gsorc1 tx-udp-segmentation off >/dev/null || exit 1
seg=$(sudo -n ip netns exec "$NS1" ethtool -k gsorc1 | sed -n 's/^tx-udp-segmentation: \([a-z]*\).*/\1/p')
sudo -n ip netns exec "$NS2" tc qdisc add dev gsorc2 ingress || exit 1
sudo -n ip netns exec "$NS2" tc filter add dev gsorc2 ingress matchall action drop || exit 1
# The neighbour entry, so no run waits on ARP inside a count.
sudo -n ip -n "$NS1" neigh replace 192.168.77.2 lladdr "$(sudo -n ip -n "$NS2" -o link show gsorc2 |
    sed -n 's/.*link\/ether \([0-9a-f:]*\).*/\1/p')" dev gsorc1 nud permanent || exit 1

echo "=== gso_run_cost $(date -Is) kernel=$(uname -r) veth_tx_udp_segmentation=$seg size=$SIZE runs=$RUNS reps=$REPS ===" |
    tee -a "$OUT"
for rep in $(seq 1 "$REPS"); do
    # shellcheck disable=SC2086 # NS_LIST is a word list on purpose
    sudo -n ip netns exec "$NS1" "$WORK/gso_run_cost" 192.168.77.2 7411 "$SIZE" "$RUNS" $NS_LIST |
        sed "s/^/rep=$rep /" | tee -a "$OUT"
done

python3 - "$OUT" <<'PYEOF' | tee -a "$OUT"
import re, sys, statistics as st
rows = {}
for line in open(sys.argv[1]):
    m = re.match(r"rep=(\d+) n=(\d+) mode=(\S+) runs=\d+ insns_per_datagram=(\S+) insns_per_run=(\S+)", line)
    if m:
        rows.setdefault((int(m.group(2)), m.group(3)), []).append((int(m.group(1)), float(m.group(5))))
ns = sorted({n for n, _ in rows})
print("n    plain/dgram   gso/dgram   gso-plain/run   A/A range plain/run")
rule = None
for n in ns:
    pl = [v for _, v in rows.get((n, "plain"), [])]
    gs = [v for _, v in rows.get((n, "gso"), [])]
    if not pl or not gs:
        continue
    d = st.median(gs) - st.median(pl)
    aa = max(pl) - min(pl)
    print(f"{n:<4} {st.median(pl)/n:11.0f} {st.median(gs)/n:11.0f} {d:15.0f} {aa:12.0f}")
    if n >= 2 and rule is None and -d > aa:
        rule = n
def fit(mode):
    xs = [n for n in ns if n >= 2 and (n, mode) in rows]
    ys = [st.median([v for _, v in rows[(n, mode)]]) for n in xs]
    mx, my = st.mean(xs), st.mean(ys)
    slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
    return slope, my - slope * mx
p, c = fit("plain")
s, F = fit("gso")
print(f"fit plain: {p:.0f} insns/datagram + {c:.0f} per call; gso: {s:.0f} insns/datagram + {F:.0f} per run")
if p <= s:
    print("FALSIFIED: a GSO datagram costs no less than a plain one - no break-even")
else:
    nstar = (F - c) / (p - s)
    print(f"break-even n* = {nstar:.2f}; first n measured cheaper beyond the A/A range: {rule}")
PYEOF
echo "=== gso_run_cost done $(date -Is) out=$OUT ===" | tee -a "$OUT"
