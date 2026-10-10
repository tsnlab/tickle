#!/usr/bin/env bash
# rx_gro_probe_rig.sh - does UDP GRO (receive) or UDP GSO (send) cut the CPU per MB of a large sample's 1472-byte
# datagrams on the RIG, cross-host Pi5 to Pi5 over eth0? Decides largemsg_cpu_profile.sh's candidates 1 and 2 before
# any product code is written. rx_gro_probe.c at both ends, no TickLE; on veth (rx_gro_probe.sh) GRO could not be
# seen at all - veth polls one packet at a time - so only a real NIC can answer candidate 1.
#
# Sender 10.1.1.214, receiver 10.1.1.213 (management addresses, for ssh); the data goes to the receiver's eth0 address,
# the rig's data link. 1 MB of 1472-byte UDP datagrams 30 times a second. ON THE WIRE every arm sends separate
# 1472-byte UDP datagrams - GSO is split into UDP datagrams by the kernel or NIC, never IP fragments, and the check below
# proves it per run.
#
# ARMS, interleaved, the order rotated each rep:
#   A    sender sendmmsg 64, receiver recvfrom              - today's rmw build (tt_RX_BATCH 1)
#   A2   the same as A                                      - CONTROL: the noise floor of A's own figures
#   M    sender sendmmsg, receiver recvmmsg 32 x 1472       - syscall batching only
#   G    sender sendmmsg, receiver UDP_GRO socket           - candidate 1 alone: does eth0's GRO merge plain trains?
#   S    sender UDP_SEGMENT (GSO, <= 44 a send), recvfrom   - candidate 2 alone
#   SG   sender GSO, receiver UDP_GRO                       - both
#
# PER RUN, recorded: the probe's RESULT lines (per_recv = datagrams per receive call that returned data; max_len and
# oversize = the largest datagram or GRO segment the receiver took and how many exceeded 1472 B; getrusage CPU ms/MB
# per side) and, around each run, deltas read from each Pi: eth0 rx/tx packets and bytes, /proc/net/snmp Ip ReasmReqds
# (IP reassemblies) on the receiver, and the host's /proc/stat CPU (user+system+irq+softirq, and softirq alone) - the
# host figure catches receive softirq work that getrusage does not charge to the process.
#
# HOW TO READ IT (pre-registered 2026-10-10, before any rig run; implemented in the summary below, not only here):
#   VOID run: receiver datagrams < 99% of sent; oversize > 0 or max_len > 1472; ReasmReqds rose (an IP fragment
#     arrived); or the receiver's eth0 mean frame (rx_bytes / rx_packets delta) > 1514 B (1472 + UDP 8 + IP 20 + Ethernet
#     14). A void run is listed and left out. An arm with fewer than 3 usable reps of 5 has no verdict.
#   NOISE FLOOR, per figure: max(|median(A) - median(A2)|, 5% of median(A)).
#   Candidate 1, "GRO helps on the rig": G's median per_recv >= 2 AND G's receiver host CPU ms/MB below A's by more than
#     the noise floor. per_recv < 2 FALSIFIES it for plain sendmmsg trains (eth0's GRO does not merge them); SG against S
#     by the same rule then says whether it helps once the sender sends GSO trains.
#   Candidate 2, "GSO helps on the rig": S's sender host CPU ms/MB below A's by more than the noise floor.
#   M is printed and judged by the same rule as G (expected: no better than A - on veth it saved nothing).
#   The process (getrusage) figures are printed beside the host ones; a verdict reads the host figure.
#   S and SG need tx checksum offload on the sender's eth0 (UDP_SEGMENT returns EIO without it): then their runs void
#     with "send: Input/output error" in the tx log, the read-only preflight's tx-checksumming line says why, and
#     candidate 2 is undecided, not refuted.
#
# Changes NO setting on either Pi (no ethtool -K, no tc, no sysctl): the ethtool/sysctl lines are read-only. The EXIT
# trap stops any probe still running on a Pi (by the PID it wrote at launch, checked against /proc/PID/exe) and
# removes the remote work directory.
#
# Usage: rx_gro_probe_rig.sh [REPS] [SECONDS]    OUT=<prefix> (default ~/rig_results_safe/gro_probe_<date>)
# Duration: about 6 arms x REPS x (SECONDS + 7 s) + 1 min build: 5 reps x 20 s = ~15 min.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then
    exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi
REPS=${1:-5}
SECS=${2:-20}
OUT=${OUT:-$HOME/rig_results_safe/gro_probe_$(date +%Y%m%d-%H%M%S)}
ARMS=${ARMS:-"A A2 M G S SG"}
K=$HOME/.ssh/tickle_ci_ed25519
TX=10.1.1.214
RX=10.1.1.213
PORT=7411
RD=/tmp/gro_probe_$$
SRC=$REPO/examples/perf_hil/experiments/rx_gro_probe.c
SUM=$OUT.txt
RUNS=$OUT.runs
mkdir -p "$RUNS"

sh_() {
    local h=$1
    shift
    ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$h" "$@"
}
say() { echo "$*" | tee -a "$SUM"; }

cleanup() {
    for h in $TX $RX; do
        # Each probe wrote its PID at launch; stop it only if that PID is still this probe binary.
        sh_ "$h" "for f in $RD/*.pid; do [ -f \"\$f\" ] || continue; p=\$(cat \"\$f\");
            [ \"\$(readlink /proc/\$p/exe 2>/dev/null)\" = $RD/rx_gro_probe ] && kill \$p; done; rm -rf $RD" \
            </dev/null >/dev/null 2>&1
    done
    return 0
}
trap cleanup EXIT

say "=== rx_gro_probe_rig $(date -Is) repo=$(git -C "$REPO" rev-parse --short HEAD) src_sha256=$(sha256sum "$SRC" | cut -c1-16) reps=$REPS secs=$SECS arms=[$ARMS] sender=$TX receiver=$RX ==="
for h in $TX $RX; do
    say "--- $h (read-only state) ---"
    sh_ "$h" "uname -r; /usr/sbin/ethtool -i eth0 | grep -E '^(driver|version|firmware)'; \
        /usr/sbin/ethtool -k eth0 | grep -E '^(generic-receive-offload|rx-udp-gro-forwarding|rx-gro-list|rx-gro-hw|tx-udp-segmentation|generic-segmentation-offload|rx-checksumming|tx-checksumming|scatter-gather):'; \
        /usr/sbin/ethtool -c eth0 2>&1 | grep -E '^(rx-usecs|rx-frames|tx-usecs|adaptive-rx):'; \
        echo gro_flush_timeout=\$(cat /sys/class/net/eth0/gro_flush_timeout) napi_defer_hard_irqs=\$(cat /sys/class/net/eth0/napi_defer_hard_irqs 2>/dev/null); \
        sysctl -n net.core.rmem_max net.core.wmem_max | paste -sd' ' | sed 's/^/rmem_max wmem_max: /'; \
        mkdir -p $RD" </dev/null 2>&1 | sed 's/^/  /' | tee -a "$SUM"
    scp -q -i "$K" -o BatchMode=yes "$SRC" "ci@$h:$RD/rx_gro_probe.c" || { say "REFUSED: cannot copy the probe to $h"; exit 1; }
    sh_ "$h" "cc -O2 -Wall -o $RD/rx_gro_probe $RD/rx_gro_probe.c" </dev/null || { say "REFUSED: build failed on $h"; exit 1; }
done
DST=$(sh_ "$RX" "ip -4 -o addr show dev eth0" </dev/null | awk '{print $4}' | cut -d/ -f1 | head -1)
[ -n "$DST" ] || { say "REFUSED: no IPv4 address on the receiver's eth0"; exit 1; }
say "data path: $TX eth0 -> $DST:$PORT ($RX eth0)"

# One line of counters from a Pi: eth0 rx_packets rx_bytes tx_packets tx_bytes, Ip ReasmReqds, cpu busy and softirq jiffies.
# shellcheck disable=SC2016 # expanded on the Pi, not here
SNAP='s=/sys/class/net/eth0/statistics; echo $(cat $s/rx_packets) $(cat $s/rx_bytes) $(cat $s/tx_packets) $(cat $s/tx_bytes) \
 $(awk '"'"'/^Ip:/{n++; if(n==1){for(i=1;i<=NF;i++) if($i=="ReasmReqds") c=i} else print $c}'"'"' /proc/net/snmp) \
 $(awk '"'"'/^cpu /{print $2+$3+$4+$7+$8, $8}'"'"' /proc/stat)'

run_arm() { # NAME REP
    local name=$1 rep=$2 mode smode
    case $name in
    A | A2) mode=recvfrom smode="" ;;
    M) mode=mmsg smode="" ;;
    G) mode=gro smode="" ;;
    S) mode=recvfrom smode=gso ;;
    SG) mode=gro smode=gso ;;
    *) say "unknown arm $name"; return 1 ;;
    esac
    local base=$RUNS/${name}_r$rep
    local rx0 tx0 rx1 tx1
    rx0=$(sh_ "$RX" "$SNAP" </dev/null)
    tx0=$(sh_ "$TX" "$SNAP" </dev/null)
    sh_ "$RX" "cd $RD && echo \$\$ > rx.pid && exec ./rx_gro_probe recv $PORT $((SECS + 3)) $mode > rx.log 2>&1" </dev/null &
    local rx_ssh=$!
    sleep 1
    sh_ "$TX" "cd $RD && echo \$\$ > tx.pid && exec ./rx_gro_probe send $DST $PORT $SECS $smode > tx.log 2>&1" </dev/null
    wait "$rx_ssh"
    rx1=$(sh_ "$RX" "$SNAP" </dev/null)
    tx1=$(sh_ "$TX" "$SNAP" </dev/null)
    sh_ "$RX" "cat $RD/rx.log" </dev/null > "$base.rx.log"
    sh_ "$TX" "cat $RD/tx.log" </dev/null > "$base.tx.log"
    echo "$name rep$rep | $(grep -m1 RESULT "$base.tx.log") | $(grep -m1 RESULT "$base.rx.log") | rxsnap $rx0 -> $rx1 | txsnap $tx0 -> $tx1" |
        tee -a "$SUM.raw"
}

read -r -a arm_list <<<"$ARMS"
n=${#arm_list[@]}
for rep in $(seq 1 "$REPS"); do
    for i in $(seq 0 $((n - 1))); do
        run_arm "${arm_list[$(((i + rep - 1) % n))]}" "$rep"
    done
done

python3 - "$SUM.raw" <<'EOF' | tee -a "$SUM"
import re, statistics, sys
raw = sys.argv[1]
HZ = 100.0  # USER_HZ on the Pis' arm64 kernels; /proc/stat counts in it
runs = {}
for line in open(raw):
    m = re.match(r'(\S+) rep(\d+) \| (.*?) \| (.*?) \| rxsnap (.*?) -> (.*?) \| txsnap (.*?) -> (.*)$', line.strip())
    if not m:
        continue
    arm = m.group(1)
    tx = dict(re.findall(r'(\w+)=([\d.]+)', m.group(3)))
    rx = dict(re.findall(r'(\w+)=([\d.]+)', m.group(4)))
    r0, r1, t0, t1 = ([float(x) for x in m.group(k).split()] for k in (5, 6, 7, 8))
    if 'datagrams' not in tx or 'datagrams' not in rx or len(r0) != 7 or len(r1) != 7 or len(t0) != 7 or len(t1) != 7:
        print(f'{arm} rep{m.group(2)} VOID: missing RESULT or counters')
        continue
    sent, got = float(tx['datagrams']), float(rx['datagrams'])
    mb = float(rx['bytes']) / 1e6
    d = lambda a, b, i: b[i] - a[i]
    frame = d(r0, r1, 1) / d(r0, r1, 0) if d(r0, r1, 0) > 0 else 0
    reasm = d(r0, r1, 4)
    why = []
    if got < 0.99 * sent: why.append(f'delivered {got:.0f}/{sent:.0f}')
    if float(rx['oversize']) > 0 or float(rx['max_len']) > 1472: why.append(f"max_len {rx['max_len']} oversize {rx['oversize']}")
    if reasm > 0: why.append(f'ReasmReqds +{reasm:.0f}')
    if frame > 1514: why.append(f'mean rx frame {frame:.0f} B')
    rec = dict(per_recv=float(rx['per_recv']), rx_proc=float(rx['cpu_ms_per_mb']), tx_proc=float(tx['cpu_ms_per_mb']),
               rx_host=d(r0, r1, 5) * 1000 / HZ / mb, rx_sirq=d(r0, r1, 6) * 1000 / HZ / mb,
               tx_host=d(t0, t1, 5) * 1000 / HZ / mb, tx_sirq=d(t0, t1, 6) * 1000 / HZ / mb, frame=frame,
               tx_pkts_per_dgram=d(t0, t1, 2) / sent if sent else 0)
    print(f"{arm:3} rep{m.group(2)} {'VOID: ' + '; '.join(why) if why else 'ok  '} per_recv {rec['per_recv']:.2f} "
          f"frame {frame:.0f} B  rx host {rec['rx_host']:.2f} (sirq {rec['rx_sirq']:.2f}) proc {rec['rx_proc']:.2f}  "
          f"tx host {rec['tx_host']:.2f} (sirq {rec['tx_sirq']:.2f}) proc {rec['tx_proc']:.2f} ms/MB")
    if not why:
        runs.setdefault(arm, []).append(rec)
med = {a: {k: statistics.median(r[k] for r in v) for k in v[0]} for a, v in runs.items() if len(v) >= 3}
print('\nmedians over usable reps (CPU in ms per MB delivered):')
print(f"{'arm':4} {'n':>2} {'per_recv':>8} {'rx_host':>8} {'rx_sirq':>8} {'rx_proc':>8} {'tx_host':>8} {'tx_sirq':>8} {'tx_proc':>8} {'frame':>6}")
for a, v in runs.items():
    if a in med:
        x = med[a]
        print(f"{a:4} {len(v):>2} {x['per_recv']:8.2f} {x['rx_host']:8.2f} {x['rx_sirq']:8.2f} {x['rx_proc']:8.2f} "
              f"{x['tx_host']:8.2f} {x['tx_sirq']:8.2f} {x['tx_proc']:8.2f} {x['frame']:6.0f}")
    else:
        print(f'{a:4} {len(v):>2} fewer than 3 usable reps: no verdict')
if 'A' not in med or 'A2' not in med:
    print('NO VERDICTS: A or the A2 control has fewer than 3 usable reps')
    sys.exit(0)
floor = {k: max(abs(med['A'][k] - med['A2'][k]), 0.05 * med['A'][k]) for k in ('rx_host', 'tx_host')}
print(f"noise floor: rx_host {floor['rx_host']:.2f}  tx_host {floor['tx_host']:.2f} ms/MB (|A - A2|, at least 5% of A)")
def recv_verdict(arm, ref):
    if arm not in med or ref not in med:
        return f'{arm}: no verdict ({arm} or {ref} lacks 3 usable reps)'
    x, r = med[arm], med[ref]
    gain = r['rx_host'] - x['rx_host']
    if x['per_recv'] < 2:
        return f"{arm} vs {ref}: FALSIFIED - per_recv {x['per_recv']:.2f} < 2 (receiver host {gain:+.2f} ms/MB saved)"
    if gain > floor['rx_host']:
        return f"{arm} vs {ref}: HELPS - per_recv {x['per_recv']:.2f}, receiver host -{gain:.2f} ms/MB (floor {floor['rx_host']:.2f})"
    return f"{arm} vs {ref}: NO GAIN - per_recv {x['per_recv']:.2f} but receiver host {gain:+.2f} ms/MB within floor {floor['rx_host']:.2f}"
print('candidate 1 (GRO), plain trains: ' + recv_verdict('G', 'A'))
print('candidate 1 (GRO), GSO trains:   ' + recv_verdict('SG', 'S'))
print('recvmmsg, same rule:             ' + recv_verdict('M', 'A'))
if 'S' in med:
    gain = med['A']['tx_host'] - med['S']['tx_host']
    v = 'HELPS' if gain > floor['tx_host'] else 'NO GAIN'
    print(f"candidate 2 (GSO): S vs A: {v} - sender host {gain:+.2f} ms/MB saved (floor {floor['tx_host']:.2f})")
EOF
say "=== rx_gro_probe_rig done $(date -Is) out=$SUM ==="
