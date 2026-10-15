#!/usr/bin/env bash
# srv_gap_pc.sh - the core campaign cells' SERVER and client, commit by commit, on the PC: executed instructions (user
# and kernel, perf stat over each process's whole life, as gate v2 counts them on the rig) per sample. Two private
# network namespaces over a veth, 192.168.10.0/24 (the bench's broadcast). lat_gap_pc.sh counted the latency client
# only; this counts both roles, in a latency cell and in the throughput cells, so a server-side step can be named.
#
# Why (2026-10-14): the ab/rx-gap3 rig gate v2 (A = 481b9e3e, B = 32d999ca, core p1-p4, A P B B P A) read the SERVER's
# insn_u_per_sample +0.16..+0.74% in every throughput cell (+6..+15 a sample) and every latency cell (+25..+29 a round
# trip), t 7..98, while the client fell (kernel -20%, CPU -13%: the intended fix). NOT a rig figure (x86, veth).
#
# Why, second use (2026-10-14 night): the ab/rx-gap3b rig gate v2 (A = main 0c9f9a3b, B = 723ec1f0) read the CLIENT's
# insn_u_per_sample WORSE in the throughput cells (c1-c3, c8, c13, c14: +0.5..+1.8%; c3's client CPU witness +2.25%,
# which blocks the landing) and client and server insn_u WORSE in every latency cell (+100..+175 a round trip). The
# arms are every production commit from main to 723ec1f0 in order, M = 0c9f9a3b first, plus the placement pair PM, PF;
# T:p3:Q0 (c3, the witness cell) is added to the cells. Reading, pre-registered (srv_gap_pc_read.py --steps over the
# chain, --total M>F, --layout M>F,PM>PF): a commit is NAMED for a cell and role when its step's insn_u MOVED up;
# the named steps must cover the M->F total ("additive"); a delta only the L arms show and PM->PF does not is
# LAYOUT and names nothing. A fix is then judged on the same harness: it must leave F->fix MOVED down in every cell
# whose M->F MOVED up, and fix->M not MOVED up (else it is not enough).
#
# The cells are the rig's, with the rig's arguments (campaign_sweep.sh: cell_args below), the server started 3 s
# before the client and SIGINTed when the client returns (tickle/run_scenario.sh), so a whole-life count covers what
# the rig's does: L = reliable_latency (c10 p1, c17 p4), T = reliable_throughput -Q KEEP_ALL (Q0, c2 p2) and
# best_effort_throughput (Q1, c13 p2). Per sample = the process's own sent= (client) or recv= (throughput server); the
# latency server divides by its client's sent= (campaign_summary.py, INSN_METRICS).
#
# Builds as the gate does: every arm with TICKLE_LAYOUT_CFLAGS (gate_v2_lib.sh A1); an arm "label:commit:P" with
# TICKLE_PLACEMENT_CFLAGS (A3, -falign-functions=128), the placement arm.
#
# HOW TO READ IT - written before the first run, and done by srv_gap_pc_read.py (which implements it):
#   - The first arm is run twice (BASE2, its own blocks in the rotation): |BASE2 - first| per (cell, role, metric) is
#     the A/A floor.
#   - A step (arm vs the arm named before it on the reader's STEPS line) MOVED when |d| > max(2 x A/A floor,
#     3 x SE(d), 1 instruction). The steps between A and B whose server insn_u MOVED up name the cause; their sum must
#     come within 3 SE of B - A (else "NOT ADDITIVE": something between the arms is not a step).
#   - Layout: B - A against PB - PA (both placement builds). The delta "SURVIVES LAYOUT" when both MOVED the same way
#     and differ by less than half of B - A; "LAYOUT" when PB - PA did not move; anything else "MIXED".
#   - VOID: a run without both RESULT lines, without both counts, or with 0 samples; an arm with fewer than 2 valid
#     blocks in a cell is not read there.
# Usage: srv_gap_pc.sh [BLOCKS]   ARMS="label:commit[:FLAVOR[:VAR=v,VAR2=v[:PERT]]] ..." (VAR=v: environment to both
# roles; FLAVOR L (default) the gate's layout flags, P its placement flags, D the plain build, X the flags in FLAGS_X;
# PERT a layout_perturb.sh perturbation of the arm's source copy, e.g. "T1:main:L::T1"),
# CELLS (default "L:p1 T:p2:Q0 T:p3:Q0 T:p2:Q1 L:p4"),
# STRACE=1 (one extra pass per arm and cell under strace -f -c: of the server, or with STRACE_ROLE=client the client),
# PROFILE=1 (one extra pass per arm and cell, both roles under perf record -e instructions:u, the data kept as
# $WORK/prof_<arm>_<cell>_{cli,srv}.data, reported per symbol (instructions) in .report beside them, the run's samples
# in prof_<arm>_<cell>.txt: where the user instructions go),
# WORK= (default ~/largemsg_pc/rx_gap3_server),
# BUILD_CFLAGS= (to every arm's build, e.g. -Dtt_SOCKET_BUFFER_SIZE=212992: the rig Pis' send buffer; its own WORK),
# CLI_EVENTS= (more perf events for the client, e.g. syscalls:sys_enter_poll, counted per run on its RUN line),
# RATE= (e.g. 1gbit: tbf on both veth ends, the rig's line rate and its full send buffer), OUT= (default
# $WORK/results.txt), TREE=. Run it detached. Every bench process runs under ulimit -v and a timeout.
# The last line is always "srv_gap_pc done rc=<n>".
set -u
BLOCKS=${1:-3}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
WORK=${WORK:-$HOME/largemsg_pc/rx_gap3_server}
OUT=${OUT:-$WORK/results.txt}
ARMS=${ARMS:?ARMS=\"label:commit[:P] ...\"}
CELLS=${CELLS:-L:p1 T:p2:Q0 T:p3:Q0 T:p2:Q1 L:p4}
VMEM_KB=${VMEM_KB:-2097152}
PRE_CLIENT_SLEEP=${PRE_CLIENT_SLEEP:-3}
RATE=${RATE:-}
BUILD_JOBS=${BUILD_JOBS:-4}
NS1=sg-cli-$$
NS2=sg-srv-$$
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}
# shellcheck disable=SC2034 # read by gate_v2_lib.sh
GATE_BUILD_ARCH=$(uname -m) # the arms build here, not on a Pi: the gate's flags as composed for this compiler
# shellcheck source=examples/perf_hil/experiments/gate_v2_lib.sh
# shellcheck disable=SC1091
. "$HERE/gate_v2_lib.sh"
rc=1
cleanup() { # the one EXIT trap
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    echo "srv_gap_pc done rc=$rc $(date -Is)" | tee -a "$OUT"
    return 0
}
trap cleanup EXIT
mkdir -p "$WORK" "$TMPDIR"

scenario_of() { # <cell>: the scenario directory build.sh fills
    IFS=: read -r shape payload qos <<<"$1"
    if [ "$shape" = L ]; then echo "reliable_latency_$payload"
    elif [ "$qos" = Q1 ]; then echo "best_effort_throughput_$payload"
    else echo "reliable_throughput_$payload"; fi
}
cell_args() { # <cell>: campaign_sweep.sh's arguments for it (TickLE), to both roles
    IFS=: read -r shape payload qos <<<"$1"
    if [ "$shape" = L ]; then echo "-i 0.1 -d 10 -W 4096 -C 4096 -I 0.001 -T 20"; return; fi
    local keep
    case "$payload" in p1) keep=2048 ;; p2) keep=405 ;; p3) keep=368 ;; *) keep=187 ;; esac
    if [ "$qos" = Q1 ]; then echo "-d 5 --warmup-s 2 --cooldown-s 2"
    else echo "-Q -N $keep -B 100 -d 5 --warmup-s 2 --cooldown-s 2"; fi
}

declare -A arm_dir arm_sha arm_flavor arm_env
labels=()
builds=()
for spec in $ARMS; do
    IFS=: read -r label commit flavor envs pert <<<"$spec"
    sha=$(git -C "$TREE" rev-parse --short=8 "$commit") || exit 1
    flavor=${flavor:-L}
    d=$WORK/src_${sha}_$flavor${pert:+_$pert}
    arm_dir[$label]=$d/examples/perf_hil/tickle
    arm_sha[$label]=$sha
    arm_flavor[$label]=$flavor${pert:+/$pert}
    arm_env[$label]=${envs//,/ }
    labels+=("$label")
    case " ${builds[*]} " in *" $sha:$flavor:$pert "*) ;; *) builds+=("$sha:$flavor:$pert") ;; esac
done

flavor_flags() { # <flavor>: L the gate's layout flags, P its placement flags, D none (the product's build), any other
    # name X the flags in FLAGS_X (set, possibly empty; unset is an error, not the plain build)
    case $1 in
        L) echo "$TICKLE_LAYOUT_CFLAGS" ;;
        P) echo "$TICKLE_PLACEMENT_CFLAGS" ;;
        D) echo "" ;;
        *)
            local v="FLAGS_$1"
            [ -n "${!v+set}" ] || { echo "flavor $1: FLAGS_$1 is not set" >&2; return 1; }
            echo "${!v}"
            ;;
    esac
}

build_one() { # <sha> <flavor> [<pert>]: every cell's scenario, into its own source copy (own HOME: own install
    # prefixes); <pert> is a layout_perturb.sh name applied to the copy: a layout-only change, for layout A/A arms
    local sha=$1 flavor=$2 pert=${3:-} d=$WORK/src_${1}_$2${3:+_$3} cell flags
    flags=$(flavor_flags "$flavor") || return 1
    flags="$flags ${BUILD_CFLAGS:-}" # every arm alike: give the run its own WORK, the builds are kept by commit
    if [ ! -f "$d/.built" ]; then
        rm -rf "$d" && mkdir -p "$d/home"
        git -C "$TREE" archive "$sha" | tar -x -C "$d" || return 1
        if [ -n "$pert" ]; then "$HERE/layout_perturb.sh" "$d" "$pert" || return 1; fi
    fi
    for cell in $CELLS; do
        local scen
        scen=$(scenario_of "$cell")
        [ -x "$d/examples/perf_hil/tickle/$scen/server" ] && continue
        (cd "$d/examples/perf_hil/tickle" && HOME=$d/home TICKLE_EXTRA_CFLAGS="$flags" \
            ./build.sh "${scen%_p?}" "${scen##*_}") >>"$WORK/build_${sha}_$flavor${pert:+_$pert}.log" 2>&1 || return 1
    done
    touch "$d/.built"
}
echo "building ${builds[*]} ($(date -Is))"
pids=()
for b in "${builds[@]}"; do
    while [ "$(jobs -rp | wc -l)" -ge "$BUILD_JOBS" ]; do sleep 2; done
    IFS=: read -r b_sha b_flavor b_pert <<<"$b"
    build_one "$b_sha" "$b_flavor" "$b_pert" &
    pids+=($!)
done
for p in "${pids[@]}"; do wait "$p" || { echo "BUILD FAILED (see $WORK/build_*.log)"; exit 1; }; done
echo "built ($(date -Is))"

arm_dir[BASE2]=${arm_dir[${labels[0]}]}
arm_sha[BASE2]=${arm_sha[${labels[0]}]}
arm_flavor[BASE2]=${arm_flavor[${labels[0]}]}
arm_env[BASE2]=${arm_env[${labels[0]}]}
labels+=(BASE2)

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add sga netns "$NS1" type veth peer name sgb netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev sga
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev sgb
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set sga up
sudo -n ip -n "$NS2" link set sgb up
if [ -n "$RATE" ]; then
    # The rig's NIC, in both directions: tbf (not netem) holds what the wire has not taken, so the sender's socket buffer
    # fills and a blocking send waits, as on the rig (netem takes the whole burst into its queue and never does).
    sudo -n ip netns exec "$NS1" tc qdisc add dev sga root tbf rate "$RATE" burst 64kb latency 500ms || exit 1
    sudo -n ip netns exec "$NS2" tc qdisc add dev sgb root tbf rate "$RATE" burst 64kb latency 500ms || exit 1
fi
line="=== srv_gap_pc $(date -Is) arms:"
for l in "${labels[@]}"; do line+=" $l=${arm_sha[$l]}:${arm_flavor[$l]}${arm_env[$l]:+[${arm_env[$l]}]}"; done
echo "$line cells=[$CELLS] blocks=$BLOCKS rate=${RATE:-none} cflags=[${BUILD_CFLAGS:-}] load=$(cut -d' ' -f1-3 /proc/loadavg) ===" | tee -a "$OUT"

count() { # <perf -x, file> <event>
    awk -F, -v e="$2" '$3 ~ "^"e && $1 ~ /^[0-9]+$/ { print $1; exit }' "$1" 2>/dev/null
}

run() { # <arm> <cell> <block> <mode: perf|strace>
    local arm=$1 cell=$2 block=$3 mode=$4 scen args dir
    scen=$(scenario_of "$cell")
    args=$(cell_args "$cell")
    dir=${arm_dir[$arm]}/$scen
    local slog=$WORK/srv.log clog=$WORK/cli.log smeas=$WORK/srv_meas.txt cmeas=$WORK/cli_meas.txt flag=$WORK/client_done
    rm -f "$smeas" "$cmeas" "$flag" "$slog" "$clog"
    local swrap aenv=${arm_env[$arm]}
    local role=${STRACE_ROLE:-server}
    local pfx=$WORK/prof_${arm}_${cell//:/_}
    if [ "$mode" = profile ]; then swrap="perf record -q -e instructions:u -c 1009 -o ${pfx}_srv.data --"
    elif [ "$mode" = strace ] && [ "$role" = server ]; then swrap="strace -f -c -o $smeas"
    elif [ "$mode" = strace ]; then swrap=""
    else swrap="perf stat -x, -o $smeas -e $INSN_EVENTS --"; fi
    # The server: its counter is perf's (or strace's) child, timeout, which forwards the SIGINT sent when the client is
    # done (run_scenario.sh's stop) to the server. The PID is the launch's own ($!), its child read from /proc and
    # checked by exe before the signal (or the launch itself, when nothing counts the server: it is timeout then).
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS2" bash -c "ulimit -v $VMEM_KB; cd $dir || exit 1
        $swrap env $aenv timeout -s INT 90 ./server $args >$slog 2>&1 &
        p=\$!
        until [ -e $flag ] || ! kill -0 \$p 2>/dev/null; do sleep 0.05; done
        for c in \$p \$(cat /proc/\$p/task/*/children 2>/dev/null); do
            case \$(readlink /proc/\$c/exe) in */timeout) kill -INT \$c ;; esac
        done
        wait \$p" &
    local spid=$!
    sleep "$PRE_CLIENT_SLEEP"
    local cwrap="perf stat -x, -o $cmeas -e $INSN_EVENTS${CLI_EVENTS:+,$CLI_EVENTS} --"
    [ "$mode" = strace ] && cwrap=""
    [ "$mode" = strace ] && [ "$role" = client ] && cwrap="strace -f -c -o $cmeas"
    [ "$mode" = profile ] && cwrap="perf record -q -e instructions:u -c 1009 -o ${pfx}_cli.data --"
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS1" bash -c "ulimit -v $VMEM_KB; cd $dir && exec $cwrap env $aenv timeout -s INT 80 ./client $args" \
        >"$clog" 2>&1
    touch "$flag"
    wait "$spid"
    local cres sres sent recv div
    cres=$(grep -m1 '^RESULT' "$clog")
    sres=$(grep -m1 '^RESULT' "$slog")
    sent=$(grep -o ' sent=[0-9]*' <<<"$cres" | head -1 | cut -d= -f2)
    recv=$(grep -o ' recv=[0-9]*' <<<"$sres" | head -1 | cut -d= -f2)
    if [ "${cell%%:*}" = L ]; then div=$sent; else div=$recv; fi
    if [ -z "$cres" ] || [ -z "$sres" ] || [ -z "$sent" ] || [ "$sent" = 0 ] || [ -z "$div" ] || [ "$div" = 0 ]; then
        echo "RUN arm=$arm cell=$cell block=$block mode=$mode VOID (client: $(tail -1 "$clog" | cut -c1-200)) (server: $(tail -1 "$slog" | cut -c1-200))" | tee -a "$OUT"
        return
    fi
    if [ "$mode" = profile ]; then
        echo "client_sent=$sent server_samples=$div" >"$pfx.txt"
        # perf leaves its data root's, mode 600: the reports are made here, by the same root shell that recorded them.
        local r
        for r in cli srv; do
            # shellcheck disable=SC2024
            sudo -n ip netns exec "$NS1" bash -c "perf report -i ${pfx}_$r.data --stdio -q --sort sym -F period,sym \
                --percent-limit 0.05" >"${pfx}_$r.report" 2>&1
        done
        echo "RUN arm=$arm cell=$cell block=$block mode=profile client_sent=$sent server_samples=$div" | tee -a "$OUT"
        return
    fi
    if [ "$mode" = strace ]; then
        local sc sfile=$smeas
        if [ "$role" = client ]; then sfile=$cmeas; div=$sent; fi
        sc=$(awk -v n="$div" '$NF != "total" && $NF ~ /^[a-z_0-9]+$/ && NF >= 5 {
                calls = $4; err = (NF == 6) ? $5 : 0; if (calls + 0 > 0) printf "%s=%.3f/%.3f ", $NF, calls / n, err / n }' "$sfile")
        echo "RUN arm=$arm cell=$cell block=$block mode=strace role=$role samples=$div per_sample: $sc" >>"$OUT"
        echo "RUN arm=$arm cell=$cell block=$block mode=strace samples=$div"
        return
    fi
    local siu sik ciu cik
    siu=$(count "$smeas" instructions:u); sik=$(count "$smeas" instructions:k)
    ciu=$(count "$cmeas" instructions:u); cik=$(count "$cmeas" instructions:k)
    if [ -z "$siu" ] || [ -z "$sik" ] || [ -z "$ciu" ] || [ -z "$cik" ]; then
        echo "RUN arm=$arm cell=$cell block=$block mode=perf VOID (counts: srv '$siu' '$sik' cli '$ciu' '$cik')" | tee -a "$OUT"
        return
    fi
    local extra="" ev
    for ev in ${CLI_EVENTS:+${CLI_EVENTS//,/ }}; do extra+=" cli_${ev##*:}=$(count "$cmeas" "$ev")"; done
    echo "RUN arm=$arm sha=${arm_sha[$arm]} flavor=${arm_flavor[$arm]} cell=$cell block=$block mode=perf client_sent=$sent server_samples=$div srv_insn_u=$siu srv_insn_k=$sik cli_insn_u=$ciu cli_insn_k=$cik$extra | C $cres | S $sres | SRX $(grep -m1 ' rx: ' "$slog" | sed 's/.* rx: //')" >>"$OUT"
    echo "RUN arm=$arm cell=$cell block=$block srv_u/s=$((siu / div)) srv_k/s=$((sik / div)) cli_u/s=$((ciu / sent)) cli_k/s=$((cik / sent))"
}

n=${#labels[@]}
for block in $(seq 1 "$BLOCKS"); do
    for cell in $CELLS; do
        for i in $(seq 0 $((n - 1))); do
            run "${labels[$(((i + block - 1) % n))]}" "$cell" "$block" perf
        done
    done
done
if [ "${PROFILE:-0}" = 1 ]; then
    for cell in $CELLS; do
        for l in "${labels[@]}"; do [ "$l" = BASE2 ] || run "$l" "$cell" p profile; done
    done
fi
if [ "${STRACE:-0}" = 1 ]; then
    for cell in $CELLS; do
        for l in "${labels[@]}"; do [ "$l" = BASE2 ] || run "$l" "$cell" s strace; done
    done
fi
echo "=== done $(date -Is) ===" | tee -a "$OUT"
rc=0
