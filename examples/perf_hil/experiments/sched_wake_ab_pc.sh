#!/usr/bin/env bash
# sched_wake_ab_pc.sh - the core reliable_latency ping-pong (campaign cell c10's TickLE client and server, p1, -i 0.1)
# on the PC, main against a fix for the scheduler's late wake: two private network namespaces over a veth,
# 192.168.10.0/24 (the bench's broadcast). Every arm's client is the creation-time one (CreationTime.h, on main since
# 037de4d1), so each reports crt_* and gap_* (how late each ping left after its due time) from the same code.
#
# Why (2026-10-17): the PC dry run of cell c10 read gap_p50 538-548 us and crt_p50 0.667-0.688 ms against stamp RTT
# p50 0.141-0.151 ms. The ping's 100 ms wait is a relative epoll_pwait2() timeout, whose kernel slack is 0.1% of it at
# nice 0 and 0.5% at nice > 0 - and this PC's session shells run at nice 5, so the dry run's waits carried ~500 us of
# slack where the rig's (launched over ssh, nice 0) carry ~100 us. Each arm therefore runs at both: NICE=5 (the dry
# run) and NICE=0 (the rig's scheduling class).
#
# Second design (ab/sched-wake-timerfd, 2026-10-17 afternoon): the first fix split each long wait in two, and the rig
# A/B read the second wake-up as client insn_u +3.4-3.7% a round trip and c17 client CPU +1.07% (blocking). This one
# waits once, on the wait set's absolute-deadline timerfd (edge-triggered, no read on a fire). Its predicted cost is
# system calls, not wake-ups: ~2 timerfd_settime() a client round trip (the reply's 500 ms give-up deadline after a
# fire, then the next ping's earlier one), 0 read(), and the same epoll_pwait2() count as main. SPLIT=<commit> adds the
# first design as arm S, printed against main and not judged.
#
# HOW TO READ IT - written before the first run, and enforced by the summary at the end:
#   PRIMARY  gap_p50_us of the client (send delay past due), per arm and nice, mean over BLOCKS.
#   Expected (the hypothesis): fix - main = about -(0.005 x 100 ms - 50 us) = -450 us at nice 5, and about
#            -(0.001 x 100 ms - 50 us) = -50 us at nice 0; crt_p50 moves by the same amount; stamp rtt_p50 does not
#            move (the wait for a reply ends on the reply in both arms).
#   CONFIRMED at a nice level when |fix - main| on gap_p50 exceeds max(2 x |main - main2|, 20 us) and has the expected
#            sign; REFUTED when the fix's gap_p50 is not below main's by that much.
#   RTT      stamp rtt_p50 fix - main within max(2 x A/A, 10 us), else flagged RTT_MOVED.
#   CPU      client CPU per round trip (sched_cpu_s/sent, us): fix - main <= max(2 x A/A, 30 us); more is flagged
#            CPU_RISE (a precise wake must not turn into spinning: a spin to the deadline would cost ~0.4-0.8 ms a
#            round trip). The server's CPU (perf stat task-clock / sent) is printed beside it under the same bound.
#            The rig's landing rule is stricter (no CPU or latency cell WORSE than main), so the % is printed too.
#   INSN     client and server instructions:u and :k per round trip (perf stat), fix - main in %, beside the A/A %.
#            Listed, not blocking, as on the rig. Prediction for this design: client insn_u within +1%.
#   SYSCALLS one strace -f -c pass per arm at nice 0, after the blocks: calls per round trip by name, client and
#            server. Prediction above; a fix arm with more epoll_pwait2() a round trip than main is a second wake-up.
#   VOID     a run with sent=0, or fewer than 80 measured round trips, or loadavg(1 min) above LOAD_MAX at its start,
#            or a missing CPU figure, is marked VOID and left out of the means (the PC is shared with other users' VMs
#            and builds).
# ARMS: A=main, B=fix, A2=main again (the A/A floor), S=SPLIT if set. Order rotated per block. Usage:
#   sched_wake_ab_pc.sh [BLOCKS]   MAIN= FIX= FIX_MARK=<string only the fix's hal_linux.c has> SPLIT= SPLIT_MARK= OUT= WORK=.
# INTERVAL=<s> (default 0.1, cell c10's): the client's -i. Added 2026-10-18 for the short-wait question (a 5 ms ping
# still left ~60 us late on 596e9c9c): the expectations above are for 0.1 s; at another interval the PRIMARY reading is
# the same rule with that interval's own expected delta, and the header and every RUN row say which interval it was.
# Run it detached. The last line is always "sched_wake_ab_pc done rc=<n>".
set -u
BLOCKS=${1:-4}
INTERVAL=${INTERVAL:-0.1}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
WORK=${WORK:-/home/semih/tmp_build/sched/ab}
OUT=${OUT:-$WORK/results.txt}
MAIN=${MAIN:-origin/main}
FIX=${FIX:-ab/sched-wake-timerfd}
FIX_MARK=${FIX_MARK:-TT_RX_TIMER_LONG_NS}
SPLIT=${SPLIT:-}
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
arms=(A B A2)
if [ -n "$SPLIT" ]; then
    prepare S "$SPLIT"
    # SPLIT_MARK: what the extra arm's hal_linux.c must contain (the split design's rx_wait_long by default; set it to
    # another design's marker to run that one as arm S).
    grep -q "${SPLIT_MARK:-rx_wait_long}" "${arm_dir[S]}/../../../../src/hal_linux.c" ||
        { echo "arm S has no ${SPLIT_MARK:-rx_wait_long}"; exit 1; }
    arms+=(S)
fi
[ "${arm_sha[A]}" != "${arm_sha[B]}" ] || { echo "MAIN and FIX are the same commit"; exit 1; }
# The fix arm must carry the fix and the main arm must not: read the source each was built from.
grep -q "$FIX_MARK" "${arm_dir[B]}/../../../../src/hal_linux.c" || { echo "fix arm has no $FIX_MARK"; exit 1; }
! grep -q "$FIX_MARK" "${arm_dir[A]}/../../../../src/hal_linux.c" || { echo "main arm has $FIX_MARK"; exit 1; }

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add swa netns "$NS1" type veth peer name swb netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev swa
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev swb
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set swa up
sudo -n ip -n "$NS2" link set swb up
line="=== sched_wake_ab_pc $(date -Is)"
for l in "${arms[@]}"; do line+=" $l=${arm_sha[$l]}"; done
echo "$line interval_s=$INTERVAL blocks=$BLOCKS load_max=$LOAD_MAX load=$(cut -d' ' -f1-3 /proc/loadavg) ===" | tee -a "$OUT"

field() { grep -oE " $1=[-0-9.]+" <<<"$2" | head -1 | cut -d= -f2; }
counter() { grep -m1 ",$1," "$2" 2>/dev/null | cut -d, -f1; } # counter <event> <perf stat -x, file>

run() { # <arm> <nice> <block> [strace]
    local arm=$1 nice=$2 block=$3 mode=${4:-perf} dir=${arm_dir[$1]} slog=$WORK/srv.log clog=$WORK/cli.log
    local spf=$WORK/srv_perf.txt cpf=$WORK/cli_perf.txt sst=$WORK/srv_strace.txt cst=$WORK/cli_strace.txt load swrap cwrap
    load=$(cut -d' ' -f1 /proc/loadavg)
    rm -f "$spf" "$cpf" "$sst" "$cst"
    if [ "$mode" = strace ]; then
        swrap="strace -f -c -o $sst"
        cwrap="strace -f -c -o $cst"
    else
        swrap="perf stat -x, -e task-clock,instructions:u,instructions:k,cycles:u,cycles:k -o $spf"
        cwrap="perf stat -x, -e task-clock,instructions:u,instructions:k,cycles:u,cycles:k -o $cpf"
    fi
    # Root inside the namespace, so renice can lower the inherited nice 5 to 0; both processes at the arm's nice.
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS2" bash -c "renice -n $nice -p \$\$ >/dev/null; ulimit -v $VMEM_KB; cd $dir && \
        exec $swrap timeout -s INT 17 ./server -d 14 -W 100 -C 100 -I 0.001" >"$slog" 2>&1 &
    local spid=$!
    sleep 1
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" bash -c "renice -n $nice -p \$\$ >/dev/null; echo NICE=\$(ps -o ni= -p \$\$); \
        ulimit -v $VMEM_KB; cd $dir && exec $cwrap timeout -s INT 40 ./client -i $INTERVAL -d 10 -W 100 -C 100 -I 0.001 \
        -T 3" >"$clog" 2>&1
    wait "$spid"
    local res sent measured realnice
    res=$(grep -m1 '^RESULT' "$clog")
    realnice=$(grep -m1 '^NICE=' "$clog" | tr -d ' ' | cut -d= -f2)
    sent=$(field sent "$res")
    measured=$(field measured "$res")
    if [ "$mode" = strace ]; then
        local who f
        for who in cli srv; do
            f=$WORK/${who}_strace.txt
            echo "SYSCALLS arm=$arm sha=${arm_sha[$arm]} interval_s=$INTERVAL role=$who sent=$sent per_rtt: $(awk -v n="${sent:-1}" '
                $NF != "total" && $NF ~ /^[a-z_0-9]+$/ && NF >= 5 { calls = $4; if (calls + 0 > 0) printf "%s=%.3f ", $NF, calls / n }' "$f")" |
                tee -a "$OUT"
        done
        return
    fi
    local verdict=ok
    if [ -z "$sent" ] || [ "$sent" = 0 ] || [ "${measured:-0}" -lt 80 ]; then verdict=VOID_RUN; fi
    if awk -v l="$load" -v m="$LOAD_MAX" 'BEGIN { exit !(l > m) }'; then verdict=VOID_LOAD; fi
    if [ "$realnice" != "$nice" ]; then verdict=VOID_NICE; fi
    # CPU from the client's sched_cpu_s (schedstat, a begin-to-end delta in ns) - not utime+stime, whose 10 ms ticks are
    # 33 us a round trip over 300 round trips, coarser than the 30 us bound (the first run, 2026-10-17 10:05, read
    # every server row as exactly 33.3 us). A row with any CPU figure missing is VOID_CPU, not a zero.
    local sc srv ciu cik siu sik
    sc=$(field sched_cpu_s "$res")
    srv=$(counter task-clock "$spf")
    ciu=$(counter instructions:u "$cpf"); cik=$(counter instructions:k "$cpf")
    siu=$(counter instructions:u "$spf"); sik=$(counter instructions:k "$spf")
    if [ -z "$sc" ] || [ -z "$srv" ] || [ -z "$ciu" ] || [ -z "$siu" ]; then verdict=VOID_CPU; fi
    # Diagnostic, printed and not judged (added 2026-10-17 after task-clock read the server +50-120% with flat
    # instructions): cycles, and the client's task-clock beside its sched_cpu_s.
    local ctc ccu cck scu sck
    ctc=$(counter task-clock "$cpf"); ccu=$(counter cycles:u "$cpf"); cck=$(counter cycles:k "$cpf")
    scu=$(counter cycles:u "$spf"); sck=$(counter cycles:k "$spf")
    per() { awk -v c="${1:-0}" -v k="$2" -v n="${sent:-1}" 'BEGIN { printf "%.1f", c * k / n }'; }
    echo "RUN arm=$arm sha=${arm_sha[$arm]} interval_s=$INTERVAL nice=$nice block=$block verdict=$verdict load=$load sent=$sent" \
        "measured=$measured gap_p50_us=$(field gap_p50_us "$res") gap_p99_us=$(field gap_p99_us "$res")" \
        "crt_p50_ms=$(field crt_p50_ms "$res") crt_p99_ms=$(field crt_p99_ms "$res") rtt_p50_ms=$(field rtt_p50_ms "$res")" \
        "rtt_p99_ms=$(field rtt_p99_ms "$res") cli_cpu_us_per_rtt=$(per "$sc" 1e6) srv_cpu_us_per_rtt=$(per "$srv" 1e3)" \
        "cli_insn_u_per_rtt=$(per "$ciu" 1) cli_insn_k_per_rtt=$(per "$cik" 1) srv_insn_u_per_rtt=$(per "$siu" 1)" \
        "srv_insn_k_per_rtt=$(per "$sik" 1) cli_task_us_per_rtt=$(per "$ctc" 1e3) cli_cyc_u_per_rtt=$(per "$ccu" 1)" \
        "cli_cyc_k_per_rtt=$(per "$cck" 1) srv_cyc_u_per_rtt=$(per "$scu" 1) srv_cyc_k_per_rtt=$(per "$sck" 1)" |
        tee -a "$OUT"
    [ -n "$res" ] || { echo "  client tail: $(tail -2 "$clog" | tr '\n' ' ')" | tee -a "$OUT"; }
}

n=${#arms[@]}
for block in $(seq 1 "$BLOCKS"); do
    for nice in ${NICES:-5 0}; do
        for i in $(seq 0 $((n - 1))); do
            run "${arms[$(((i + block - 1) % n))]}" "$nice" "$block"
        done
    done
done
for l in "${arms[@]}"; do [ "$l" = A2 ] || run "$l" 0 s strace; done

# The reading rules above, applied to this run's rows only (after the header this run wrote).
start=$(grep -n "^=== sched_wake_ab_pc " "$OUT" | tail -1 | cut -d: -f1)
tail -n +"$start" "$OUT" | awk '
function val(k,   i, kv) { for (i = 1; i <= NF; i++) { split($i, kv, "="); if (kv[1] == k) return kv[2] } return "" }
/^RUN / && val("verdict") == "ok" {
    key = val("arm") SUBSEP val("nice"); n[key]++
    gap[key] += val("gap_p50_us"); rtt[key] += val("rtt_p50_ms") * 1000
    crt[key] += val("crt_p50_ms") * 1000; cpu[key] += val("cli_cpu_us_per_rtt"); srv[key] += val("srv_cpu_us_per_rtt")
    ciu[key] += val("cli_insn_u_per_rtt"); cik[key] += val("cli_insn_k_per_rtt")
    siu[key] += val("srv_insn_u_per_rtt"); sik[key] += val("srv_insn_k_per_rtt")
    ccy[key] += val("cli_cyc_u_per_rtt") + val("cli_cyc_k_per_rtt"); scy[key] += val("srv_cyc_u_per_rtt") + val("srv_cyc_k_per_rtt")
    ctk[key] += val("cli_task_us_per_rtt")
}
function diag(lbl, x) { printf " | %s cli_cyc/rtt=%.0f srv_cyc/rtt=%.0f cli_task_us/rtt=%.1f srv_task_us/rtt=%.1f", lbl, m(ccy, x), m(scy, x), m(ctk, x), m(srv, x) }
function m(a, k) { return n[k] ? a[k] / n[k] : -1 }
function abs(x) { return x < 0 ? -x : x }
function pct(arr, x, a) { return m(arr, a) > 0 ? 100 * (m(arr, x) - m(arr, a)) / m(arr, a) : 0 }
function insn(label, arr, x, a, a2) { printf " | %s A=%.0f %s=%.0f d=%+.2f%% A/A=%+.2f%%", label, m(arr, a), substr(x, 1, index(x, SUBSEP) - 1), m(arr, x), pct(arr, x, a), pct(arr, a2, a) }
END {
    for (ni = 0; ni <= 5; ni += 5) {
        a = "A" SUBSEP ni; b = "B" SUBSEP ni; a2 = "A2" SUBSEP ni; s = "S" SUBSEP ni
        if (!n[a] || !n[b] || !n[a2]) { printf "SUMMARY nice=%d VOID - runs ok: A=%d B=%d A2=%d\n", ni, n[a], n[b], n[a2]; continue }
        dg = m(gap, b) - m(gap, a); fg = abs(m(gap, a2) - m(gap, a)); bg = 2 * fg > 20 ? 2 * fg : 20
        dr = m(rtt, b) - m(rtt, a); fr = abs(m(rtt, a2) - m(rtt, a)); br = 2 * fr > 10 ? 2 * fr : 10
        dc = m(cpu, b) - m(cpu, a); fc = abs(m(cpu, a2) - m(cpu, a)); bc = 2 * fc > 30 ? 2 * fc : 30
        ds = m(srv, b) - m(srv, a); fs = abs(m(srv, a2) - m(srv, a)); bs = 2 * fs > 30 ? 2 * fs : 30
        printf "SUMMARY nice=%d runs A=%d B=%d A2=%d | gap_p50_us A=%.1f B=%.1f A2=%.1f d=%+.1f bound=%.1f -> %s", ni, n[a], n[b], n[a2], m(gap, a), m(gap, b), m(gap, a2), dg, bg, (dg < -bg ? "CONFIRMED" : "REFUTED")
        printf " | crt_p50_us A=%.1f B=%.1f d=%+.1f", m(crt, a), m(crt, b), m(crt, b) - m(crt, a)
        printf " | rtt_p50_us A=%.1f B=%.1f d=%+.1f bound=%.1f -> %s", m(rtt, a), m(rtt, b), dr, br, (abs(dr) <= br ? "RTT_SAME" : "RTT_MOVED")
        printf " | cli_cpu_us/rtt A=%.1f B=%.1f d=%+.1f (%+.1f%%) bound=%.1f -> %s", m(cpu, a), m(cpu, b), dc, pct(cpu, b, a), bc, (dc <= bc ? "CPU_OK" : "CPU_RISE")
        printf " | srv_cpu_us/rtt A=%.1f B=%.1f d=%+.1f (%+.1f%%) bound=%.1f -> %s", m(srv, a), m(srv, b), ds, pct(srv, b, a), bs, (ds <= bs ? "CPU_OK" : "CPU_RISE")
        insn("cli_insn_u/rtt", ciu, b, a, a2); insn("cli_insn_k/rtt", cik, b, a, a2)
        insn("srv_insn_u/rtt", siu, b, a, a2); insn("srv_insn_k/rtt", sik, b, a, a2)
        printf "\n"
        printf "DIAG nice=%d (not judged)", ni; diag("A", a); diag("B", b); diag("A2", a2); if (n[s]) diag("S", s)
        printf "\n"
        if (n[s]) {
            printf "ARM_S nice=%d runs S=%d (not judged) | gap_p50_us S=%.1f d=%+.1f | cli_cpu_us/rtt S=%.1f d=%+.1f (%+.1f%%) | srv_cpu_us/rtt S=%.1f d=%+.1f", ni, n[s], m(gap, s), m(gap, s) - m(gap, a), m(cpu, s), m(cpu, s) - m(cpu, a), pct(cpu, s, a), m(srv, s), m(srv, s) - m(srv, a)
            insn("cli_insn_u/rtt", ciu, s, a, a2); insn("cli_insn_k/rtt", cik, s, a, a2)
            insn("srv_insn_u/rtt", siu, s, a, a2); insn("srv_insn_k/rtt", sik, s, a, a2)
            printf "\n"
        }
    }
}' | tee -a "$OUT"
echo "=== done $(date -Is) ===" | tee -a "$OUT"
rc=0
