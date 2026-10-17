#!/usr/bin/env bash
# sched_wake_tput_pc.sh - what the deadline timerfd for EVERY wait for a scheduler entry (TT_RX_TIMER_LONG_NS 0,
# ab/sched-wake-timerfd-all) costs the campaign's throughput cells, on the PC: two private network namespaces over a
# veth, the client's egress shaped to 1 Gbit/s by tbf (the rig's link), 192.168.10.0/24 (the bench's broadcast).
#
# Why (2026-10-18): below 10 ms a wait for an entry used to take the relative epoll timeout, which needs no system call
# of its own. With the threshold at 0 such a wait arms the timerfd whenever its deadline differs from the one armed:
# one timerfd_settime() each time. A ping client pays ~1 more a round trip (sched_wake_ab_pc.sh measures that); a
# throughput client whose waits end for ACKNACKs before a retry or give-up entry could pay one a wait, many a second.
# This counts it where the rig's gate v2 would see it.
#
# HOW TO READ IT - written before the first run:
#   TREATMENT  timerfd_settime() per sample (strace -f -c, one pass per arm and cell): main's against each arm's.
#   COST       client and server instructions (user, kernel) and task-clock per sample (perf stat), mean over BLOCKS,
#              arm - main in %, beside the A/A % (main run twice, A2). An arm whose client or server task-clock per
#              sample rises by more than max(2 x |A/A|, 3%) in a cell is flagged CPU_RISE: the rig would likely read that
#              cell's CPU row WORSE. Instructions are listed, not judged (as on the rig).
#   VOID       a run with no RESULT line, sent=0, a missing counter, or loadavg(1 min) above LOAD_MAX at its start.
# NOT a rig figure: x86, veth; never tabled as one.
# Usage: ARMS="label=commit ..." (the first is main; A2 is added) [BLOCKS=3] [CELLS="name|scenario|args" ...] OUT= WORK=
# Run it detached. The last line is always "sched_wake_tput_pc done rc=<n>".
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
WORK=${WORK:-/home/semih/tmp_build/sched/tput}
OUT=${OUT:-$WORK/results.txt}
BLOCKS=${BLOCKS:-3}
LOAD_MAX=${LOAD_MAX:-4}
DUR=${DUR:-5}
ARMS=${ARMS:?ARMS=\"label=commit ...\"}
CELLS=${CELLS:-"rel_q0|reliable_throughput|-Q_-N_2048_-B_100 rel_q2|reliable_throughput|-K_64_-B_100 be|best_effort_throughput|"}
NS1=swt-cli-$$
NS2=swt-srv-$$
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}
rc=1
cleanup() { # the one EXIT trap
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    echo "sched_wake_tput_pc done rc=$rc $(date -Is)"
    return 0
}
trap cleanup EXIT
mkdir -p "$WORK"

# Cells: "name|scenario|args" with the args' spaces written as _ (so the list splits on spaces).
cells=()
for c in $CELLS; do cells+=("$c"); done

declare -A arm_src arm_sha
labels=()
for spec in $ARMS; do
    label=${spec%%=*}
    sha=$(git -C "$TREE" rev-parse --short=8 "${spec#*=}") || exit 1
    d=$WORK/src_$sha
    for c in "${cells[@]}"; do
        scen=$(cut -d'|' -f2 <<<"$c")
        if [ ! -x "$d/examples/perf_hil/tickle/$scen/client" ]; then
            [ -d "$d/src" ] || { mkdir -p "$d/home" && git -C "$TREE" archive "$sha" | tar -x -C "$d"; } || exit 1
            (cd "$d/examples/perf_hil/tickle" && HOME=$d/home MAKEFLAGS=-j4 ./build.sh "$scen") >"$WORK/build_${sha}_$scen.log" 2>&1 ||
                { tail -5 "$WORK/build_${sha}_$scen.log"; exit 1; }
        fi
    done
    arm_src[$label]=$d
    arm_sha[$label]=$sha
    labels+=("$label")
done
arm_src[A2]=${arm_src[${labels[0]}]}
arm_sha[A2]=${arm_sha[${labels[0]}]}
labels+=(A2)

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add swa netns "$NS1" type veth peer name swb netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev swa
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev swb
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set swa up
sudo -n ip -n "$NS2" link set swb up
sudo -n ip netns exec "$NS1" tc qdisc add dev swa root tbf rate 1gbit burst 64kb latency 10ms || exit 1
line="=== sched_wake_tput_pc $(date -Is)"
for l in "${labels[@]}"; do line+=" $l=${arm_sha[$l]}"; done
echo "$line blocks=$BLOCKS dur=$DUR load=$(cut -d' ' -f1-3 /proc/loadavg) ===" | tee -a "$OUT"

field() { grep -oE " $1=[-0-9.]+" <<<"$2" | head -1 | cut -d= -f2; }
counter() { grep -m1 ",$1," "$2" 2>/dev/null | cut -d, -f1; }

run() { # <arm> <cell> <block> [strace]
    local arm=$1 cell=$2 block=$3 mode=${4:-perf} name scen args dir load waited=0
    name=$(cut -d'|' -f1 <<<"$cell"); scen=$(cut -d'|' -f2 <<<"$cell"); args=$(cut -d'|' -f3 <<<"$cell" | tr _ ' ')
    dir=${arm_src[$arm]}/examples/perf_hil/tickle/$scen
    while [ "$waited" -lt "${QUIET_WAIT:-0}" ] && awk -v l="$(cut -d' ' -f1 /proc/loadavg)" -v m="$LOAD_MAX" \
        'BEGIN { exit !(l > m) }'; do
        sleep 5
        waited=$((waited + 5))
    done
    load=$(cut -d' ' -f1 /proc/loadavg)
    local spf=$WORK/srv.txt cpf=$WORK/cli.txt slog=$WORK/srv.log clog=$WORK/cli.log swrap cwrap
    rm -f "$spf" "$cpf"
    if [ "$mode" = strace ]; then
        swrap="strace -f -c -o $spf"; cwrap="strace -f -c -o $cpf"
    else
        swrap="perf stat -x, -e task-clock,instructions:u,instructions:k -o $spf"
        cwrap="perf stat -x, -e task-clock,instructions:u,instructions:k -o $cpf"
    fi
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS2" bash -c "renice -n 0 -p \$\$ >/dev/null; ulimit -v 2097152; cd $dir && \
        exec $swrap timeout -s INT 40 ./server $args -d $DUR" >"$slog" 2>&1 &
    local spid=$!
    sleep 1
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NS1" bash -c "renice -n 0 -p \$\$ >/dev/null; ulimit -v 2097152; cd $dir && \
        exec $cwrap timeout -s INT 40 ./client $args -d $DUR" >"$clog" 2>&1
    wait "$spid"
    local cres sres sent recv
    cres=$(grep -m1 '^RESULT' "$clog"); sres=$(grep -m1 '^RESULT' "$slog")
    sent=$(field sent "$cres"); recv=$(field recv "$sres")
    if [ "$mode" = strace ]; then
        local who f
        for who in cli srv; do
            f=$WORK/$who.txt
            echo "SYSCALLS arm=$arm sha=${arm_sha[$arm]} cell=$name role=$who sent=$sent per_sample: $(awk -v n="${sent:-1}" '
                $NF != "total" && $NF ~ /^[a-z_0-9]+$/ && NF >= 5 { if ($4 + 0 > 0) printf "%s=%.4f ", $NF, $4 / n }' "$f")" |
                tee -a "$OUT"
        done
        return
    fi
    local verdict=ok ctc ciu cik stc siu sik
    ctc=$(counter task-clock "$cpf"); ciu=$(counter instructions:u "$cpf"); cik=$(counter instructions:k "$cpf")
    stc=$(counter task-clock "$spf"); siu=$(counter instructions:u "$spf"); sik=$(counter instructions:k "$spf")
    if [ -z "$sent" ] || [ "$sent" = 0 ]; then verdict=VOID_RUN; fi
    if [ -z "$ctc" ] || [ -z "$stc" ] || [ -z "$ciu" ] || [ -z "$siu" ]; then verdict=VOID_CPU; fi
    if awk -v l="$load" -v m="$LOAD_MAX" 'BEGIN { exit !(l > m) }'; then verdict=VOID_LOAD; fi
    per() { awk -v c="${1:-0}" -v k="$2" -v n="${sent:-1}" 'BEGIN { printf "%.3f", c * k / (n > 0 ? n : 1) }'; }
    echo "RUN arm=$arm sha=${arm_sha[$arm]} cell=$name block=$block verdict=$verdict load=$load sent=$sent recv=$recv" \
        "cli_task_us=$(per "$ctc" 1e3) cli_insn_u=$(per "$ciu" 1) cli_insn_k=$(per "$cik" 1)" \
        "srv_task_us=$(per "$stc" 1e3) srv_insn_u=$(per "$siu" 1) srv_insn_k=$(per "$sik" 1)" | tee -a "$OUT"
    [ -n "$cres" ] || echo "  client tail: $(tail -2 "$clog" | tr '\n' ' ')" | tee -a "$OUT"
}

n=${#labels[@]}
for block in $(seq 1 "$BLOCKS"); do
    for c in "${cells[@]}"; do
        for i in $(seq 0 $((n - 1))); do run "${labels[$(((i + block - 1) % n))]}" "$c" "$block"; done
    done
done
for c in "${cells[@]}"; do
    for l in "${labels[@]}"; do [ "$l" = A2 ] || run "$l" "$c" s strace; done
done

start=$(grep -n "^=== sched_wake_tput_pc " "$OUT" | tail -1 | cut -d: -f1)
tail -n +"$start" "$OUT" | awk -v main="${labels[0]}" -v labels="${labels[*]}" '
function val(k,   i, kv) { for (i = 1; i <= NF; i++) { split($i, kv, "="); if (kv[1] == k) return kv[2] } return "" }
function m(k) { return n[k] ? s[k] / n[k] : -1 }
function pct(x, a) { return a > 0 ? 100 * (x - a) / a : 0 }
function abs(x) { return x < 0 ? -x : x }
/^RUN / && val("verdict") == "ok" {
    c = val("cell"); cell[c] = 1
    split("cli_task_us cli_insn_u cli_insn_k srv_task_us srv_insn_u srv_insn_k", M, " ")
    for (j in M) { k = val("arm") SUBSEP c SUBSEP M[j]; s[k] += val(M[j]); n[k]++ }
}
/^SYSCALLS / { k = val("arm") SUBSEP val("cell") SUBSEP val("role"); set[k] = val("timerfd_settime") + 0; ep[k] = val("epoll_pwait2") + 0 }
END {
    nl = split(labels, L, " ")
    for (c in cell) {
        printf "SUMMARY cell=%s | settime/sample cli %s=%.4f", c, main, set[main SUBSEP c SUBSEP "cli"]
        for (a = 2; a <= nl; a++) if (L[a] != "A2") printf " %s=%.4f", L[a], set[L[a] SUBSEP c SUBSEP "cli"]
        printf " srv %s=%.4f", main, set[main SUBSEP c SUBSEP "srv"]
        for (a = 2; a <= nl; a++) if (L[a] != "A2") printf " %s=%.4f", L[a], set[L[a] SUBSEP c SUBSEP "srv"]
        split("cli_task_us srv_task_us cli_insn_u cli_insn_k srv_insn_u srv_insn_k", M, " ")
        for (j = 1; j <= 6; j++) {
            base = m(main SUBSEP c SUBSEP M[j]); aa = pct(m("A2" SUBSEP c SUBSEP M[j]), base)
            printf " | %s main=%.2f A/A=%+.1f%%", M[j], base, aa
            for (a = 2; a <= nl; a++) {
                if (L[a] == "A2") continue
                d = pct(m(L[a] SUBSEP c SUBSEP M[j]), base)
                printf " %s=%+.1f%%", L[a], d
                if (j <= 2 && d > (2 * abs(aa) > 3 ? 2 * abs(aa) : 3)) printf "(CPU_RISE)"
            }
        }
        printf "\n"
    }
}' | tee -a "$OUT"
rc=0
