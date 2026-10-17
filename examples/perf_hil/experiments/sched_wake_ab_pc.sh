#!/usr/bin/env bash
# sched_wake_ab_pc.sh - the core reliable_latency ping-pong (campaign cell c10's TickLE client and server, p1, -i 0.1)
# on the PC, main against ab/sched-wake-deadline: two private network namespaces over a veth, 192.168.10.0/24 (the
# bench's broadcast). Both arms' clients are the creation-time one (CreationTime.h, on main since 037de4d1), so both
# report crt_* and gap_* (how late each ping left after its due time) from the same code.
#
# Why (2026-10-17): the PC dry run of cell c10 read gap_p50 538-548 us and crt_p50 0.667-0.688 ms against stamp RTT
# p50 0.141-0.151 ms. The ping's 100 ms wait is a relative epoll_pwait2() timeout, whose kernel slack is 0.1% of it at
# nice 0 and 0.5% at nice > 0 - and this PC's session shells run at nice 5, so the dry run's waits carried ~500 us of
# slack where the rig's (launched over ssh, nice 0) carry ~100 us. Each arm therefore runs at both: NICE=5 (the dry
# run) and NICE=0 (the rig's scheduling class).
#
# HOW TO READ IT - written before the first run, and enforced by the summary at the end:
#   PRIMARY  gap_p50_us of the client (send delay past due), per arm and nice, mean over BLOCKS.
#   Expected (the hypothesis): fix - main = about -(0.005 x 100 ms - 50 us) = -450 us at nice 5, and about
#            -(0.001 x 100 ms - 50 us) = -50 us at nice 0; crt_p50 moves by the same amount; stamp rtt_p50 does not
#            move (the wait for a reply ends on the reply in both arms).
#   CONFIRMED at a nice level when |fix - main| on gap_p50 exceeds max(2 x |main - main2|, 20 us) and has the expected
#            sign; REFUTED when the fix's gap_p50 is not below main's by that much.
#   RTT      stamp rtt_p50 fix - main within max(2 x A/A, 10 us), else flagged RTT_MOVED.
#   CPU      client CPU per round trip ((utime+stime)/sent, us): fix - main <= max(2 x A/A, 30 us) - one extra wake-up
#            per round trip at most; more is flagged CPU_RISE (a precise wake must not turn into spinning: a spin to
#            the deadline would cost the last part of each 100 ms wait, ~0.4-0.8 ms per round trip). The server's CPU
#            (from /usr/bin/time) is printed beside it under the same bound.
#   VOID     a run with sent=0, or fewer than 80 measured round trips, or loadavg(1 min) above LOAD_MAX at its start,
#            is marked VOID and left out of the means (the PC is shared with other users' VMs and builds).
# ARMS: A=main, B=fix, A2=main again (the A/A floor). Order rotated per block. Usage: sched_wake_ab_pc.sh [BLOCKS]
#   MAIN=<commit> FIX=<commit> OUT= WORK= (defaults below). Run it detached.
# The last line is always "sched_wake_ab_pc done rc=<n>".
set -u
BLOCKS=${1:-4}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
WORK=${WORK:-/home/semih/tmp_build/sched/ab}
OUT=${OUT:-$WORK/results.txt}
MAIN=${MAIN:-origin/main}
FIX=${FIX:-ab/sched-wake-deadline}
LOAD_MAX=${LOAD_MAX:-4}
VMEM_KB=${VMEM_KB:-2097152}
NS1=sw-cli-$$
NS2=sw-srv-$$
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}
rc=1
cleanup() { # the one EXIT trap
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    echo "sched_wake_ab_pc done rc=$rc $(date -Is)"
    return 0
}
trap cleanup EXIT
mkdir -p "$WORK"

declare -A arm_dir arm_sha
prepare() { # <label> <commit>
    local label=$1 sha d
    sha=$(git -C "$TREE" rev-parse --short=8 "$2") || exit 1
    d=$WORK/src_$sha
    if [ ! -x "$d/examples/perf_hil/tickle/reliable_latency/client" ]; then
        rm -rf "$d" && mkdir -p "$d/home"
        git -C "$TREE" archive "$sha" | tar -x -C "$d" || exit 1
        grep -q 'BenchCreation_add' "$d/examples/perf_hil/tickle/reliable_latency/client.c" ||
            { echo "$sha has no creation-time client"; exit 1; }
        (cd "$d/examples/perf_hil/tickle" && HOME=$d/home ./build.sh reliable_latency) >"$WORK/build_$sha.log" 2>&1 ||
            { tail -5 "$WORK/build_$sha.log"; exit 1; }
    fi
    arm_dir[$label]=$d/examples/perf_hil/tickle/reliable_latency
    arm_sha[$label]=$sha
}
prepare A "$MAIN"
prepare B "$FIX"
arm_dir[A2]=${arm_dir[A]}
arm_sha[A2]=${arm_sha[A]}
[ "${arm_sha[A]}" != "${arm_sha[B]}" ] || { echo "MAIN and FIX are the same commit"; exit 1; }
# The fix arm must carry the fix and the main arm must not: read the source each was built from.
grep -q 'rx_wait_long' "${arm_dir[B]}/../../../../src/hal_linux.c" || { echo "fix arm has no rx_wait_long"; exit 1; }
! grep -q 'rx_wait_long' "${arm_dir[A]}/../../../../src/hal_linux.c" || { echo "main arm has rx_wait_long"; exit 1; }

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add swa netns "$NS1" type veth peer name swb netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev swa
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev swb
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set swa up
sudo -n ip -n "$NS2" link set swb up
echo "=== sched_wake_ab_pc $(date -Is) A=${arm_sha[A]} B=${arm_sha[B]} blocks=$BLOCKS load_max=$LOAD_MAX" \
    "load=$(cut -d' ' -f1-3 /proc/loadavg) ===" | tee -a "$OUT"

field() { grep -oE " $1=[-0-9.]+" <<<"$2" | head -1 | cut -d= -f2; }

run() { # <arm> <nice> <block>
    local arm=$1 nice=$2 block=$3 dir=${arm_dir[$1]} slog=$WORK/srv.log clog=$WORK/cli.log load
    load=$(cut -d' ' -f1 /proc/loadavg)
    # Root inside the namespace, so renice can lower the inherited nice 5 to 0; both processes at the arm's nice.
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS2" bash -c "renice -n $nice -p \$\$ >/dev/null; ulimit -v $VMEM_KB; cd $dir && \
        exec /usr/bin/time -f 'SRVTIME u=%U s=%S' timeout -s INT 17 ./server -d 14 -W 100 -C 100 -I 0.001" \
        >"$slog" 2>&1 &
    local spid=$!
    sleep 1
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" bash -c "renice -n $nice -p \$\$ >/dev/null; echo NICE=\$(ps -o ni= -p \$\$); \
        ulimit -v $VMEM_KB; cd $dir && exec timeout -s INT 40 ./client -i 0.1 -d 10 -W 100 -C 100 -I 0.001 -T 3" \
        >"$clog" 2>&1
    wait "$spid"
    local res sent measured realnice srv
    res=$(grep -m1 '^RESULT' "$clog")
    realnice=$(grep -m1 '^NICE=' "$clog" | tr -d ' ' | cut -d= -f2)
    srv=$(grep -m1 '^SRVTIME' "$slog")
    sent=$(field sent "$res")
    measured=$(field measured "$res")
    local verdict=ok
    if [ -z "$sent" ] || [ "$sent" = 0 ] || [ "${measured:-0}" -lt 80 ]; then verdict=VOID_RUN; fi
    if awk -v l="$load" -v m="$LOAD_MAX" 'BEGIN { exit !(l > m) }'; then verdict=VOID_LOAD; fi
    if [ "$realnice" != "$nice" ]; then verdict=VOID_NICE; fi
    local u s cpu_rtt su ss srv_cpu_rtt
    u=$(field utime_s "$res"); s=$(field stime_s "$res")
    su=$(grep -oE 'u=[0-9.]+' <<<"$srv" | cut -d= -f2); ss=$(grep -oE 's=[0-9.]+' <<<"$srv" | cut -d= -f2)
    cpu_rtt=$(awk -v u="${u:-0}" -v s="${s:-0}" -v n="${sent:-1}" 'BEGIN { printf "%.1f", (u + s) * 1e6 / n }')
    srv_cpu_rtt=$(awk -v u="${su:-0}" -v s="${ss:-0}" -v n="${sent:-1}" 'BEGIN { printf "%.1f", (u + s) * 1e6 / n }')
    echo "RUN arm=$arm sha=${arm_sha[$arm]} nice=$nice block=$block verdict=$verdict load=$load sent=$sent" \
        "measured=$measured gap_p50_us=$(field gap_p50_us "$res") gap_p99_us=$(field gap_p99_us "$res")" \
        "crt_p50_ms=$(field crt_p50_ms "$res") crt_p99_ms=$(field crt_p99_ms "$res") rtt_p50_ms=$(field rtt_p50_ms "$res")" \
        "rtt_p99_ms=$(field rtt_p99_ms "$res") cli_cpu_us_per_rtt=$cpu_rtt srv_cpu_us_per_rtt=$srv_cpu_rtt" | tee -a "$OUT"
    [ -n "$res" ] || { echo "  client tail: $(tail -2 "$clog" | tr '\n' ' ')" | tee -a "$OUT"; }
}

arms=(A B A2)
for block in $(seq 1 "$BLOCKS"); do
    for nice in 5 0; do
        for i in 0 1 2; do
            run "${arms[$(((i + block - 1) % 3))]}" "$nice" "$block"
        done
    done
done

# The reading rules above, applied to this run's rows only (after the header this run wrote).
start=$(grep -n "^=== sched_wake_ab_pc " "$OUT" | tail -1 | cut -d: -f1)
tail -n +"$start" "$OUT" | awk '
function val(k,   i, kv) { for (i = 1; i <= NF; i++) { split($i, kv, "="); if (kv[1] == k) return kv[2] } return "" }
/^RUN / && val("verdict") == "ok" {
    key = val("arm") SUBSEP val("nice"); n[key]++
    gap[key] += val("gap_p50_us"); rtt[key] += val("rtt_p50_ms") * 1000
    crt[key] += val("crt_p50_ms") * 1000; cpu[key] += val("cli_cpu_us_per_rtt"); srv[key] += val("srv_cpu_us_per_rtt")
}
function m(a, k) { return n[k] ? a[k] / n[k] : -1 }
function abs(x) { return x < 0 ? -x : x }
END {
    for (ni = 0; ni <= 5; ni += 5) {
        a = "A" SUBSEP ni; b = "B" SUBSEP ni; a2 = "A2" SUBSEP ni
        if (!n[a] || !n[b] || !n[a2]) { printf "SUMMARY nice=%d VOID - runs ok: A=%d B=%d A2=%d\n", ni, n[a], n[b], n[a2]; continue }
        dg = m(gap, b) - m(gap, a); fg = abs(m(gap, a2) - m(gap, a)); bg = 2 * fg > 20 ? 2 * fg : 20
        dr = m(rtt, b) - m(rtt, a); fr = abs(m(rtt, a2) - m(rtt, a)); br = 2 * fr > 10 ? 2 * fr : 10
        dc = m(cpu, b) - m(cpu, a); fc = abs(m(cpu, a2) - m(cpu, a)); bc = 2 * fc > 30 ? 2 * fc : 30
        ds = m(srv, b) - m(srv, a); fs = abs(m(srv, a2) - m(srv, a)); bs = 2 * fs > 30 ? 2 * fs : 30
        printf "SUMMARY nice=%d runs A=%d B=%d A2=%d | gap_p50_us A=%.1f B=%.1f A2=%.1f d=%+.1f bound=%.1f -> %s", ni, n[a], n[b], n[a2], m(gap, a), m(gap, b), m(gap, a2), dg, bg, (dg < -bg ? "CONFIRMED" : "REFUTED")
        printf " | crt_p50_us A=%.1f B=%.1f d=%+.1f", m(crt, a), m(crt, b), m(crt, b) - m(crt, a)
        printf " | rtt_p50_us A=%.1f B=%.1f d=%+.1f bound=%.1f -> %s", m(rtt, a), m(rtt, b), dr, br, (abs(dr) <= br ? "RTT_SAME" : "RTT_MOVED")
        printf " | cli_cpu_us/rtt A=%.1f B=%.1f d=%+.1f bound=%.1f -> %s", m(cpu, a), m(cpu, b), dc, bc, (dc <= bc ? "CPU_OK" : "CPU_RISE")
        printf " | srv_cpu_us/rtt A=%.1f B=%.1f d=%+.1f bound=%.1f -> %s\n", m(srv, a), m(srv, b), ds, bs, (ds <= bs ? "CPU_OK" : "CPU_RISE")
    }
}' | tee -a "$OUT"
echo "=== done $(date -Is) ===" | tee -a "$OUT"
rc=0
