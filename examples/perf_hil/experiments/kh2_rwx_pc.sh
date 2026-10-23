#!/usr/bin/env bash
# kh2_rwx_pc.sh - recv_wake_xhost.c's receive styles on the PC, two private netns over veth, with the receiver's user
# and kernel instructions per datagram (perf stat on its own process; perf_event_paranoid 1). Diagnostic for RESULTS
# KH2 (2026-10-24), not a verdict: on the rig the same binary put a blocking recvmsg at 8.43 us and the wait ring's
# style (uring_dt, prod_dt) at 9.30 us from the kernel's receive stamp (recv_wake_xhost_061f4f0b); this asks what the
# ring's path costs in instructions, and what production's RECVMSG with the address (prod_dt_msg) adds to RECV.
# Instruction counts do not depend on the PC's load the way its latencies do; the latencies are printed, and are read
# only when the load average was below 4 (printed per run).
# Usage: kh2_rwx_pc.sh <rwx binary> <out dir>   env: ARMS REPS (3) ROUNDS (5000) GAP_US (200)
# ~10 s a run; the default 6 arms x 3 reps ~3 min. The last line is always "KH2_RWX_END rc=<n>".
set -u
BIN=${1:?rwx binary}
OUT=${2:?out dir}
ARMS=${ARMS:-"recvmsg recvfrom recvfrom_aa uring_dt prod_dt prod_dt_msg"}
REPS=${REPS:-3}
ROUNDS=${ROUNDS:-5000}
GAP_US=${GAP_US:-200}
WARMUP=500
NS1=kr1-$$
NS2=kr2-$$
rc=1
cleanup() { # the one EXIT trap
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    echo "KH2_RWX_END rc=$rc $(date -Is)"
}
trap cleanup EXIT
mkdir -p "$OUT" || exit 1
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add kr1 netns "$NS1" type veth peer name kr2 netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.11.1/24 dev kr1 && sudo -n ip -n "$NS2" addr add 192.168.11.2/24 dev kr2 || exit 1
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set kr1 up && sudo -n ip -n "$NS2" link set kr2 up || exit 1
asme() { sudo -n ip netns exec "$1" setpriv --reuid="$(id -u)" --regid="$(id -g)" --init-groups bash -c "$2"; }
read -r -a arms <<<"$ARMS"
for rep in $(seq 1 "$REPS"); do
    for i in "${!arms[@]}"; do
        a=${arms[$(((i + rep) % ${#arms[@]}))]}
        t=$OUT/${a}_r$rep
        load=$(cut -d' ' -f1 /proc/loadavg)
        asme "$NS2" "ulimit -v 2097152; exec perf stat -x, -e instructions:u,instructions:k -o $t.perf -- $BIN rx $a 47500 $ROUNDS $WARMUP 2 192.168.11.1 47501 $t.samples" >"$t.rx" 2>&1 &
        rxp=$!
        sleep 0.5
        asme "$NS1" "ulimit -v 2097152; exec timeout 120 $BIN tx 192.168.11.2 47500 $ROUNDS $WARMUP 4 $GAP_US 47501" >"$t.tx" 2>&1
        wait "$rxp"
        r=$(grep -h '^RXRESULT' "$t.rx")
        n=$(grep -o ' n=[0-9]*' <<<"$r" | head -1 | cut -d= -f2)
        iu=$(grep 'instructions:u' "$t.perf" | cut -d, -f1)
        ik=$(grep 'instructions:k' "$t.perf" | cut -d, -f1)
        per=$(awk -v u="${iu:-0}" -v k="${ik:-0}" -v n="${n:-0}" -v w="$WARMUP" \
            'BEGIN { d = n + w; if (d > 0) printf "insn_u_per=%.0f insn_k_per=%.0f", u / d, k / d; else print "insn n/a" }')
        echo "RWX $a rep$rep load $load $per | $(grep -o 'n=[0-9]* \|missing=[0-9]*\|p10_ns=[0-9]*\|p50_ns=[0-9]*\|p99_ns=[0-9]*\|cpu_ns_per=[0-9]*' <<<"$r" | tr '\n' ' ')"
    done
done
rc=0
