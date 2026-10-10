#!/usr/bin/env bash
# rx_drain_pc.sh - where I4s's subscriber-side losses can come from, and whether io_uring keeps a busy receiver's
# socket drained (rx_drain_probe.c). Two private network namespaces on this PC, a veth pair, `tbf rate 1gbit` on the
# sender's egress so the link is saturated as the rig's 1 GbE is in cell I4s, both sockets granted 425,984 bytes as on
# the rig's Pis. The receiver spins SPIN_US in user space after every 2,889 datagrams (one 4 MB sample).
#
# NOT a rig figure. It answers two mechanism questions, for the design in DESIGN.md section 8 ("Saturation").
#
# HOW TO READ IT, written before the first run (2026-10-11) and enforced by the summary at the end:
#   R0 CONTROL  mmsg, spin 0: lost = 0 in every rep. Otherwise the setup loses on its own and the run is VOID.
#   R1 WITNESS  every rep: lost (the receiver's seq gaps) = socket_drops (the kernel's count for that socket), within
#               1%. A gap the socket did not count was lost elsewhere (veth, tbf) and the socket hypothesis is
#               FALSIFIED on this setup.
#   R2 REPRO    mmsg loses datagrams at some spin <= 5 ms. If it never does, a busy receiver alone does not reproduce
#               I4s here (NOT REPRODUCED), and R3 has nothing to say.
#   R3 FIX      uring with 4,096 buffers loses 0 in every rep of every spin at which mmsg lost: FEASIBLE. Any loss:
#               FALSIFIED - the kernel does not drain the socket while the thread is busy in user space.
#   R4 MUST-FAIL uring with 128 buffers at 5 ms loses: the buffers are what holds the datagrams. If it does not, the
#               spin is not happening where R3 assumes and R3's PASS says nothing (VOID).
#   R5 MECHANISM coop (IORING_SETUP_COOP_TASKRUN, completions only on kernel entry) at 5 ms: expected to lose as mmsg
#               does. Informational: it says the drain is the task work forced by the wake-up, not the ring itself.
#
# Usage: rx_drain_pc.sh [REPS] [SECONDS]     results appended to $OUT (default ~/largemsg_pc/rx_drain.txt)
# Needs passwordless `sudo ip` and `tc`.
set -u
REPS=${1:-3}
SECONDS_RUN=${2:-10}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$HOME/largemsg_pc}
OUT=${OUT:-$WORK/rx_drain.txt}
NS1=rxd-pub-$$
NS2=rxd-sub-$$
BIN=$WORK/rx_drain_probe
PORT=7471

cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT

mkdir -p "$WORK"
cc -O2 -Wall -Wextra -o "$BIN" "$HERE/rx_drain_probe.c" || exit 1
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add rxd1 netns "$NS1" type veth peer name rxd2 netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.77.1/24 dev rxd1
sudo -n ip -n "$NS2" addr add 192.168.77.2/24 dev rxd2
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set rxd1 up
sudo -n ip -n "$NS2" link set rxd2 up
sudo -n ip netns exec "$NS1" tc qdisc add dev rxd1 root tbf rate 1gbit burst 256kb latency 100ms || exit 1

echo "=== rx_drain_pc $(date -Is) $(uname -r) $(grep -m1 "^CONFIG_HZ=" "/boot/config-$(uname -r)" 2>/dev/null) reps=$REPS seconds=$SECONDS_RUN tree=${TREE_SHA:-$(git -C "$HERE" rev-parse --short HEAD)} ===" | tee -a "$OUT"

# cell MODE SPIN_US BUFS
cell() {
    local mode=$1 spin=$2 bufs=$3 rep
    for rep in $(seq 1 "$REPS"); do
        local sublog=$WORK/rxd_sub.log publog=$WORK/rxd_pub.log
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS2" "$BIN" recv "$PORT" "$SECONDS_RUN" "$mode" "$spin" 2889 "$bufs" >"$sublog" 2>&1 &
        local sub_pid=$!
        sleep 0.5
        # shellcheck disable=SC2024
        sudo -n ip netns exec "$NS1" "$BIN" send 192.168.77.2 "$PORT" "$((SECONDS_RUN + 1))" >"$publog" 2>&1
        wait "$sub_pid"
        local s p
        s=$(grep -m1 '^RESULT:' "$sublog" | sed 's/^RESULT: //')
        p=$(grep -m1 '^RESULT:' "$publog" | sed 's/^RESULT: //')
        if [ -z "$s" ]; then
            echo "CELL mode=$mode spin_us=$spin bufs=$bufs rep=$rep VOID: no RESULT ($(tail -1 "$sublog"))" | tee -a "$OUT"
            continue
        fi
        echo "CELL rep=$rep $s | $p" | tee -a "$OUT"
    done
}

start_line=$(wc -l <"$OUT")
cell mmsg 0 0
for spin in 1000 2000 3000 5000 10000; do
    cell mmsg "$spin" 0
    cell uring "$spin" 4096
done
cell uring 5000 128
cell coop 3000 4096
cell coop 5000 4096
echo "=== runs done $(date -Is); tc $(sudo -n ip netns exec "$NS1" tc -s qdisc show dev rxd1 | tr '\n' ' ') ===" | tee -a "$OUT"

# The reading, from this run's CELL lines only.
tail -n +"$((start_line + 1))" "$OUT" | grep '^CELL rep=' | python3 -c '
import re, sys
rows = []
for line in sys.stdin:
    kv = dict(re.findall(r"(\w+)=(\S+)", line.split("|")[0]))
    rows.append({k: (v if k == "mode" else int(v)) for k, v in kv.items()})
def cells(mode, spin=None, bufs=None):
    return [r for r in rows if r["mode"] == mode and (spin is None or r["spin_us"] == spin) and (bufs is None or r["bufs"] == bufs)]
verdict = {}
ctl = cells("mmsg", 0)
verdict["R0"] = "PASS" if ctl and all(r["lost"] == 0 for r in ctl) else "VOID"
bad = [r for r in rows if r["received"] == 0 or abs(r["lost"] - r["socket_drops"]) > max(1, r["lost"] // 100)]
verdict["R1"] = "PASS" if rows and not bad else "FALSIFIED (%d reps where lost != socket_drops)" % len(bad)
lossy = sorted({r["spin_us"] for r in cells("mmsg") if r["lost"] > 0})
verdict["R2"] = "REPRODUCED at spin %s us" % lossy if lossy else "NOT REPRODUCED"
if lossy:
    u = [r for s in lossy for r in cells("uring", s, 4096)]
    verdict["R3"] = "FEASIBLE" if u and all(r["lost"] == 0 for r in u) else "FALSIFIED (uring lost %s)" % [r["lost"] for r in u]
else:
    verdict["R3"] = "NO READING"
m = cells("uring", 5000, 128)
verdict["R4"] = "PASS" if m and all(r["lost"] > 0 for r in m) else "VOID (128 buffers did not lose)"
c = cells("coop", 5000, 4096)
verdict["R5"] = "loses as mmsg" if c and all(r["lost"] > 0 for r in c) else "does not lose: %s" % [r["lost"] for r in c]
for mode, spin, bufs in sorted({(r["mode"], r["spin_us"], r["bufs"]) for r in rows}):
    rs = cells(mode, spin, bufs)
    print("SUMMARY %-5s spin %5d us bufs %4d: received %s lost %s socket_drops %s cpu_user+sys ms %s" % (mode, spin, bufs,
          [r["received"] for r in rs], [r["lost"] for r in rs], [r["socket_drops"] for r in rs],
          [r["cpu_user_ms"] + r["cpu_sys_ms"] for r in rs]))
for k in sorted(verdict):
    print("VERDICT %s %s" % (k, verdict[k]))
if verdict["R0"] != "PASS" or verdict["R4"] != "PASS":
    print("VERDICT run VOID: the control or the must-fail arm did not hold")
' | tee -a "$OUT"
echo "=== done $(date -Is) ===" | tee -a "$OUT"
