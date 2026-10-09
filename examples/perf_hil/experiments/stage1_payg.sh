#!/usr/bin/env bash
# stage1_payg.sh - does the shared-memory module cost anything where it is not used? (ROADMAP "Wired work": stage 1 / S1
# against the WIRE 10.4 floors; MODULE_PLAN 3 criterion 3; SHM_PLAN 5 stage 0's "CPU and binary size indistinguishable").
#
# TWO CLAIMS, both of the user's decision 6 ("using a module must cost no performance"):
#   OFF   - a build with the module compiled out (-Dtt_SEGMENT_ENABLED=0, and lending off too) costs nothing against a
#           build that never had it.
#   UNUSED - a default build (segment and lending compiled in) whose peers are all on other hosts costs nothing either:
#           the module is on, and every datagram still has to go by UDP.
#
# ARMS, all from ONE commit, so nothing but the flags differs (no day-to-day drift in src/):
#   on        the default build                                       - the UNUSED claim's treated arm
#   segoff    -Dtt_SEGMENT_ENABLED=0 (lending still compiled in)       - lending in, unused (no segment, bench never retains)
#   alloff    -Dtt_SEGMENT_ENABLED=0 -Dtt_SAMPLE_LENDING=0             - the reference: the nearest build to "never had it"
#   on_a32    on     + -falign-functions=32                           - the placement control (WIRE 10.4): the flag changes
#   alloff_a32 alloff + -falign-functions=32                          -   no instruction's meaning, only where code lands
# What alloff still carries that a build which never had the module would not is NOT measurable by running it - it is
# compiled in on both sides of any comparison this commit can make - so it is inventoried statically instead (STATIC
# below: the module's fields that stay in struct tt_Context with the module off, with their byte count).
#
# TRAFFIC: cross-host shaped. Client and server in two private network namespaces joined by a veth pair, each process
# with its OWN tmpfs on /dev/shm (as rig_preflight.sh does), so the on arm's segment attach finds nothing - exactly what
# it finds on a peer across a real link - and every datagram crosses the link. reliable_throughput, RELIABLE KEEP_ALL
# (-Q -N <campaign bound> -B 100, campaign cells 1-4), p1-p4, client and server pinned to their own CPUs.
#
# HOW TO READ IT, written before the first run and implemented in stage1_payg.py (not only here):
#   Run VOID: no RESULT line from either side; drained != acked; recv != sent or lost != 0; window != ok;
#     instrument != ok; tx_shm or rx_shm != 0 on any run (the samples did not cross the link, so the run is not
#     cross-host shaped); the client's tx_udp disagreeing with the veth's own tx_packets by more than 1% + 64 (a
#     datagram left by a path the seam does not count - g15's shape, checked end to end); an off arm with
#     shm_attach_attempts != 0 (the flag never reached the compiler); an on arm with shm_attach_attempts == 0 (the arm
#     did not apply its treatment: the on-but-unused path was never exercised).
#   Overall VOID: fewer than 5 valid runs of any (arm, shape); two arms with the same client binary (a flag that did
#     not reach the compiler reads exactly like a flag that changed nothing); the POSITIVE CONTROL failing - one on run
#     with both processes in one namespace sharing /dev/shm, which must show tx_shm > 0. Without it, tx_shm == 0 on
#     every measured run would be consistent with a witness that cannot see the segment at all.
#   Per (shape, metric), each comparison X vs REF is paired by repetition (arms are rotated inside each repetition, so
#     drift lands on all of them): d = mean(X/REF - 1), SE over repetitions, and
#       FLOOR = max(1%, |on_a32/on - 1|, |alloff_a32/alloff - 1|) for CPU per sample and rate (WIRE 10.4: a difference
#               below ~1% between two builds whose code differs in size is not evidence of a cost; the flag-alone
#               effects re-measure that floor on this PC), and 10 KB for peak RSS (10.4's RSS floor).
#       HELD     |d| <= 2 SE.
#       FLOOR    beyond 2 SE but inside FLOOR - not evidence of a cost.
#       COST / BETTER  beyond both, in the worse / better direction. For on vs alloff a COST must also survive the
#               placement control - on_a32 vs alloff_a32 the same sign beyond its own 2 SE - or it is LAYOUT (WIRE
#               10.4's reversal: a gap whose sign a neutral flag flips is placement, not work). segoff has no aligned
#               twin, so its COST is a CANDIDATE.
#     Worse means: higher sched CPU per sample (client and server), lower window send rate, higher peak RSS.
#   Claim verdicts (WIRE 8.3: a single WORSE cell among many is a candidate, not a finding):
#     UNUSED costs something   iff on vs alloff is COST in at least half the shapes for one metric.
#     lending-in costs         iff segoff vs alloff is COST/CANDIDATE in at least half the shapes for one metric.
#     Falsification: any of these firing refutes "no cost" for that claim, and the cost is then to be found and fixed.
#   AMENDMENT (2026-10-09 22:15, after the first full run and before the second; nothing above is changed by it). The
#     first run could not resolve CPU on this PC: load average 14 on 16 cores (other sessions' VMs), the flag-alone
#     effect on CPU per sample up to 19% (client) and 63% (server), 2 SE of 4-35%. Every CPU and rate cell was HELD,
#     which is "could not look", not "no cost". So two instruments are added (PERF=1, RSS_SPLIT=1, both default on):
#       instructions per sample, user + kernel (perf stat), as the CPU-cost metric - work rather than time, so neither
#       the load nor the clock moves it, and placement changes a function's cycles but not its instruction count. The
#       same rules apply (FLOOR = max(1%, the flag-alone effect on instructions)), and the means table prints the
#       absolute instructions per sample beside them so a residual below the floor is still stated in instructions.
#       the RSS split at exit (rss_split_preload.c): the executable's own resident pages, the stack (where the bench
#       keeps struct tt_Context) and anonymous memory - so the RSS COST the first run found (+41-45 KB client, +28-58
#       KB server, on vs alloff, surviving the placement control) can be attributed to code or to data.
#   AMENDMENT 2 (2026-10-09 22:40, after the second run and two fixes, before the third). The second run found the
#     module on and unused COSTING, beyond 2 SE and the floor at p1-p4: +790-950 user instructions a sample on the
#     publisher (+2.4-6.7% of all its instructions), +16-21 KB of stack and +24 KB of the executable's resident code.
#     Found: try_publish_into_slot() ran its size and KEEP_ALL checks and a peer_segment() for every publish before
#     finding no segment, then the staging path ran them again; and reset zeroed the whole 14 KB peer table. Fixed in
#     the working tree (encode_in_slot_destination() asks the peer's entry first; the table is zeroed an entry at a time
#     on first use). Arm on_base (BASE_SHA = the parent) is added, and run 3 is read as follows:
#       the fix holds        on vs on_base BETTER on client instructions at >= half the shapes, and on vs alloff no
#                            longer COST on client instructions (|d| inside the floor, or HELD) at >= half the shapes.
#       the table fix holds  on vs alloff client and server stack within the 10 KB floor at >= half the shapes, where
#                            run 2 had +16-21 KB at all four.
#       still owed           whatever on vs alloff still shows as COST is reported with its size and not called closed;
#                            the executable's code (+24 KB resident) is the module's text, reported as code size.
#     Falsification: on vs on_base not BETTER on client instructions means the slot-path check was not the cost.
#   AMENDMENT 3 (2026-10-09 23:05, after the third run, before the fourth). Run 3: the fix was BETTER on client
#     instructions at p1-p3 (-1.8 to -2.2%) - and on vs alloff was still COST there (+2.8-4.9%) and at p4 (+2.8%). Exact
#     call counts (-finstrument-functions, p1) put about 20 extra calls a publish in the slot path's destination work
#     (unicast_destinations_for, tx_destinations, link_destinations, link_count x4, link_of_ip x2), which the first fix
#     still ran before its check; the check now comes first (any_peer_on_segment()), leaving ~10 calls, all of them the
#     seam's own per-datagram question. Run 4 is read by amendment 2's rules, with on_base still the parent.
#     And the stack rows are re-read, not re-run: the bench keeps struct tt_Context on main()'s stack, and this PC's
#     gcc probes a large frame page by page (-fstack-clash-protection, on by default), so the whole struct is resident
#     whatever the code writes. The stack delta therefore measures sizeof(struct tt_Context) (+15.4 KB on) and cannot
#     show the lazily zeroed peer table; that is shown on zero-allocated memory instead, by mincore()
#     (tests/test_transport_seam.c, test_a_zero_allocated_context_keeps_the_peer_table_out_of_memory).
#   AMENDMENT 4 (2026-10-09 23:28, after the fourth run, before the fifth). Run 4: the fix BETTER at p1-p3 (-2.9 to
#     -3.5%), on vs alloff still COST (+2.2-3.9%). Exact user instructions per sample (perf stat, p1, two reps each,
#     +-2): on 3131, the same with the slot path compiled out 2958, alloff 2794. So the gate became one load
#     (segment_slot_ceiling == 0: this context has never attached a segment), and an unthrottled perf record (-c
#     1000003) put the rest in the seam's per-datagram question (peer_segment(), segment_deliver_ringing(): ~95 a
#     sample), whole_record_limit_for() (~15), and functions gcc no longer inlines once the module gives them a second
#     caller (keep_all_refused_record_bytes(), maybe_solicit_ack_at_watermark()). Run 5 measures that final code by
#     amendment 2's rules; what it still shows as COST is reported as the residual, with that composition.
#   Bytes are reported, not judged by a floor: text/data/bss of libtickle.a and of both bench binaries, and sizeof of
#   the context, per arm. A byte is a byte; whether it shows up in RSS is the RSS row's question.
#
# Usage: stage1_payg.sh            (this checkout's tracked files as they are; SHA=<commit> to measure a commit)
#   env: REPS (5) DUR (5) EDGE_S (2) SHAPES ("p1 p2 p3 p4") ARMS (all five) CPU_CLIENT (12) CPU_SERVER (13)
#        OUT (default ~/rig_results_safe/stage1_payg/<stamp>_<label>) KEEP_WORK=1 keeps the build trees.
# PC only: no rig, no rig lock. Needs passwordless `sudo -n ip`. Long: launch detached (setsid nohup) and read $OUT.
set -uo pipefail

# ------------------------------------------------------------------------------------------------ one run, as root
# Re-entered through `sudo -n ip netns exec <client ns> env J_*=... bash stage1_payg.sh --job`, inside the client's
# namespace and a mount namespace of its own. Starts the server (its PID from $!, never by name), runs the client under
# a timeout, stops the server with SIGINT, and records the exit statuses and the client veth's tx_packets delta.
if [ "${1:-}" = "--job" ]; then
    mount -t tmpfs -o size=512m tmpfs /dev/shm || { echo "note=no_private_shm" >"$J_STATUS"; exit 1; }
    # What runs each side: pinned, optionally counted by perf (instructions, user and kernel), optionally with the RSS
    # split instrument preloaded. perf, when used, is the PARENT of the bench process.
    pre_s=(taskset -c "$J_SCPU")
    pre_c=(taskset -c "$J_CCPU")
    # shellcheck disable=SC2153 # J_SPERF and J_CPERF come from the env given to `ip netns exec ... env`
    if [ -n "$J_PERF" ]; then
        events="instructions:u,instructions:k"
        pre_s+=(perf stat -x "," -e "$events" -o "$J_SPERF" --)
        pre_c+=(perf stat -x "," -e "$events" -o "$J_CPERF" --)
    fi
    env_s=(env "BENCH_IFACE=$J_SBENCH")
    env_c=(env "BENCH_IFACE=$J_CBENCH")
    if [ -n "$J_PRELOAD" ]; then
        env_s+=("LD_PRELOAD=$J_PRELOAD")
        env_c+=("LD_PRELOAD=$J_PRELOAD")
    fi
    if [ -n "$J_SNS" ]; then
        # shellcheck disable=SC2016,SC2086 # $1/$@ are the inner sh's; J_ARGS is deliberately word-split into argv
        ip netns exec "$J_SNS" sh -c 'mount -t tmpfs -o size=512m tmpfs /dev/shm && cd "$1" && shift && exec "$@"' \
            sh "$J_DIR" "${pre_s[@]}" "${env_s[@]}" ./server $J_ARGS >"$J_SLOG" 2>&1 </dev/null &
    else
        # shellcheck disable=SC2086 # J_ARGS is deliberately word-split into argv
        (cd "$J_DIR" && exec "${pre_s[@]}" "${env_s[@]}" ./server $J_ARGS) >"$J_SLOG" 2>&1 </dev/null &
    fi
    spid=$!
    sleep 1
    tx0=$(cat "/sys/class/net/$J_CIFACE/statistics/tx_packets")
    # shellcheck disable=SC2086 # J_ARGS is deliberately word-split into argv
    (cd "$J_DIR" && exec timeout -s INT -k 5 "$J_CTIMEOUT" "${pre_c[@]}" "${env_c[@]}" ./client $J_ARGS) \
        >"$J_CLOG" 2>&1 </dev/null
    crc=$?
    tx1=$(cat "/sys/class/net/$J_CIFACE/statistics/tx_packets")
    # The server is $! itself, or perf's child when perf counts it. Either way it is confirmed by /proc/PID/exe before
    # the signal, never found by name.
    target=$spid
    if [ -n "$J_PERF" ]; then
        target=$(cat "/proc/$spid/task/$spid/children" 2>/dev/null | awk '{print $1}')
    fi
    stop=sigint
    case "$(readlink "/proc/${target:-0}/exe" 2>/dev/null)" in
    */server) kill -INT "$target" ;;
    *) stop=gone_before_sigint ;;
    esac
    for _ in $(seq 1 100); do
        kill -0 "$spid" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 "$spid" 2>/dev/null; then
        stop=killed_after_10s
        [ -n "$target" ] && kill -KILL "$target"
        kill -KILL "$spid"
    fi
    wait "$spid"
    src=$?
    echo "server_rc=$src client_rc=$crc server_stop=$stop veth_tx_packets=$((tx1 - tx0))" >"$J_STATUS"
    exit 0
fi

# ------------------------------------------------------------------------------------------------ the driver
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SELF="$HERE/$(basename "${BASH_SOURCE[0]}")"
REPO="$(cd "$HERE/../../.." && pwd)"
REPS=${REPS:-5}
DUR=${DUR:-5}
EDGE_S=${EDGE_S:-2}
SHAPES=${SHAPES:-p1 p2 p3 p4}
ARMS=${ARMS:-on segoff alloff on_a32 alloff_a32}
CPU_CLIENT=${CPU_CLIENT:-12}
CPU_SERVER=${CPU_SERVER:-13}
PERF=${PERF:-1}
RSS_SPLIT=${RSS_SPLIT:-1}
SCEN=reliable_throughput
T0=$(date +%s)

arm_flags() {
    case "$1" in
    on | on_base) echo "" ;;
    segoff) echo "-Dtt_SEGMENT_ENABLED=0" ;;
    alloff) echo "-Dtt_SEGMENT_ENABLED=0 -Dtt_SAMPLE_LENDING=0" ;;
    on_a32) echo "-falign-functions=32" ;;
    alloff_a32) echo "-Dtt_SEGMENT_ENABLED=0 -Dtt_SAMPLE_LENDING=0 -falign-functions=32" ;;
    *) return 1 ;;
    esac
}
# The KEEP_ALL bound per shape: campaign_sweep.sh's keepall_samples_for(), so these cells are campaign cells 1-4.
keepall_for() {
    case "$1" in p1) echo 2048 ;; p2) echo 405 ;; p3) echo 368 ;; p4) echo 187 ;; esac
}
for a in $ARMS; do arm_flags "$a" >/dev/null || { echo "unknown arm $a" >&2; exit 2; }; done
# on_base: the default build of another commit (BASE_SHA), for a before/after of a fix with the same instruments.
BASE_FULL=""
case " $ARMS " in
*" on_base "*)
    BASE_FULL=$(git -C "$REPO" rev-parse --verify -q "${BASE_SHA:?arm on_base needs BASE_SHA}^{commit}") ||
        { echo "no commit $BASE_SHA in $REPO" >&2; exit 1; } ;;
esac

if [ -n "${SHA:-}" ]; then
    FULL_SHA=$(git -C "$REPO" rev-parse --verify -q "${SHA}^{commit}") || { echo "no commit $SHA in $REPO" >&2; exit 1; }
    LABEL=${FULL_SHA:0:8}
else
    LABEL="tree-$(git -C "$REPO" rev-parse --short HEAD)"
fi
OUTD=${OUT:-$HOME/rig_results_safe/stage1_payg/$(date +%Y%m%d-%H%M%S)_$LABEL}
mkdir -p "$OUTD/runs" || exit 1
SUMMARY="$OUTD/summary.txt"
: >"$SUMMARY"
say() { echo "$*" | tee -a "$SUMMARY"; }

# The work directory: kept in a variable from mktemp, the same command, and removed only by that name.
WORK=$(mktemp -d /tmp/stage1_payg.XXXXXX) || exit 1
NS_LIST="$WORK/namespaces"
: >"$NS_LIST"
# shellcheck disable=SC2329 # invoked by the EXIT trap below
cleanup() {
    local ns pids
    while read -r ns; do
        [ -n "$ns" ] || continue
        pids=$(sudo -n ip netns pids "$ns" 2>/dev/null | tr '\n' ' ')
        # shellcheck disable=SC2086
        [ -n "${pids// /}" ] && sudo -n ip netns exec "$ns" kill -KILL $pids 2>/dev/null
        sudo -n ip netns del "$ns" 2>/dev/null
    done <"$NS_LIST"
    if [ "${KEEP_WORK:-0}" != 1 ] && [ -n "${WORK:-}" ] && [ -d "$WORK" ]; then rm -rf "$WORK"; fi
    return 0
}
trap cleanup EXIT
trap 'exit 130' INT TERM

say "=== stage1_payg $(date -Is): $LABEL, arms '$ARMS', shapes '$SHAPES', $REPS reps, -d $DUR, edges ${EDGE_S}s, cpus client $CPU_CLIENT server $CPU_SERVER ==="
say "    output: $OUTD"
say "    instruments: perf instructions=$PERF, RSS split=$RSS_SPLIT"
PRELOAD=""
if [ "$RSS_SPLIT" = 1 ]; then
    PRELOAD="$WORK/rss_split_preload.so"
    gcc -O2 -Wall -Wextra -shared -fPIC -o "$PRELOAD" "$HERE/rss_split_preload.c" || { say "FATAL: rss_split_preload"; exit 1; }
fi
J_PERF_ON=""
[ "$PERF" = 1 ] && J_PERF_ON=1

# ------------------------------------------------------------------------------------------------ builds
# One exported tree per arm: build.sh runs `make clean` in the tree's root, so two arms building in one tree would
# clean each other's objects. HOME points into the work directory, so the core prefixes are private to this run.
export_tree() { # $1 destination, $2 commit (optional: default SHA, else the working tree)
    mkdir -p "$1" || return 1
    local sha=${2:-${FULL_SHA:-}}
    if [ -n "$sha" ]; then
        git -C "$REPO" archive "$sha" | tar -x -C "$1"
    else
        (cd "$REPO" && git ls-files -z | tar --null -T - -cf -) | tar -x -C "$1"
    fi
}
build_arm() { # $1 arm
    local arm=$1 flags shape tree log
    flags=$(arm_flags "$arm")
    tree="$WORK/$arm/src"
    log="$OUTD/build_$arm.log"
    local from=""
    [ "$arm" = on_base ] && from=$BASE_FULL
    export_tree "$tree" "$from" >"$log" 2>&1 || { echo "BUILD_FAIL export" >>"$log"; return 1; }
    for shape in $SHAPES; do
        if ! (cd "$tree/examples/perf_hil/tickle" && HOME="$WORK/$arm/home" TICKLE_EXTRA_CFLAGS="$flags" \
            ./build.sh "$SCEN" "$shape") >>"$log" 2>&1; then
            echo "BUILD_FAIL $shape" >>"$log"
            return 1
        fi
        mkdir -p "$WORK/bin/$arm/$shape"
        cp "$tree/examples/perf_hil/tickle/${SCEN}_$shape/client" "$tree/examples/perf_hil/tickle/${SCEN}_$shape/server" \
            "$WORK/bin/$arm/$shape/" || return 1
    done
    echo "BUILD_OK" >>"$log"
}
for arm in $ARMS; do build_arm "$arm" & done
wait
for arm in $ARMS; do
    tail -1 "$OUTD/build_$arm.log" | grep -q '^BUILD_OK$' || { say "FATAL: build of $arm failed - $OUTD/build_$arm.log"; exit 1; }
done
say "    built in $(($(date +%s) - T0)) s"

# ------------------------------------------------------------------------------------------------ static
# Identity first: every arm's client must differ from every other's, per shape, or a flag did not arrive.
STATIC="$OUTD/static.txt"
: >"$STATIC"
for shape in $SHAPES; do
    dup=$(for arm in $ARMS; do sha256sum "$WORK/bin/$arm/$shape/client" | cut -c1-16; done | sort | uniq -d)
    [ -z "$dup" ] || { say "FATAL: two arms built an identical $shape client ($dup) - a flag never reached the compiler"; exit 1; }
done
PROBE="$WORK/probe.c"
cat >"$PROBE" <<'EOF'
#include <stdio.h>
#include <tickle/tickle.h>
int main(void) {
    printf("sizeof_context=%zu sizeof_publisher=%zu sizeof_subscriber=%zu segment=%d lending=%d frag=%d\n",
           sizeof(struct tt_Context), sizeof(struct tt_Publisher), sizeof(struct tt_Subscriber), tt_SEGMENT_ENABLED,
           tt_SAMPLE_LENDING, tt_FRAG_ENABLED);
    return 0;
}
EOF
for arm in $ARMS; do
    flags=$(arm_flags "$arm")
    for shape in $SHAPES; do
        if [ "$shape" = p4 ]; then
            lib=$(find "$WORK/$arm/home" -path '*sample4096*/lib/libtickle.a' | head -1)
        else
            lib=$(find "$WORK/$arm/home" -path '*/lib/libtickle.a' -not -path '*sample4096*' | head -1)
        fi
        pfx=$(dirname "$(dirname "$lib")")
        extra=""
        [ "$shape" = p4 ] && extra="-Dtt_MAX_SAMPLE_LENGTH=4096"
        # shellcheck disable=SC2086 # flags are deliberately word-split
        gcc $flags $extra -I"$pfx/include" "$PROBE" -o "$WORK/probe_${arm}_$shape" ||
            { say "FATAL: struct probe did not build for $arm $shape"; exit 1; }
        read -r lt ld lb _ < <(size -t "$lib" | tail -1)
        read -r ct cd cb _ < <(size "$WORK/bin/$arm/$shape/client" | tail -1)
        read -r st sd sb _ < <(size "$WORK/bin/$arm/$shape/server" | tail -1)
        echo "STATIC arm=$arm shape=$shape lib_text=$lt lib_data=$ld lib_bss=$lb client_text=$ct client_data=$cd" \
            "client_bss=$cb server_text=$st server_data=$sd server_bss=$sb $("$WORK/probe_${arm}_$shape")" \
            "client_sha=$(sha256sum "$WORK/bin/$arm/$shape/client" | cut -c1-16)" >>"$STATIC"
    done
done
cat "$STATIC" >>"$SUMMARY"
# What the module leaves in struct tt_Context when it is compiled out: every field named for it outside its #if.
python3 "$HERE/stage1_payg.py" --residue "$REPO/include/tickle/tickle.h" >>"$STATIC" 2>&1
grep '^RESIDUE' "$STATIC" | tee -a "$SUMMARY"

# ------------------------------------------------------------------------------------------------ runs
NSID=0
run_one() { # $1 arm, $2 shape, $3 rep, $4 topology (veth|samens)
    local arm=$1 shape=$2 rep=$3 topo=$4
    NSID=$((NSID + 1))
    local rd="$OUTD/runs/${shape}_${arm}_r${rep}_$topo"
    mkdir -p "$rd"
    local nsa="s1p$$-${NSID}a" nsb="s1p$$-${NSID}b" va="s1p$$v${NSID}a" vb="s1p$$v${NSID}b"
    echo "$nsa" >>"$NS_LIST"
    sudo -n ip netns add "$nsa" || { echo "note=no_namespace" >"$rd/status"; return; }
    sudo -n ip -n "$nsa" link set lo up
    local sns="" ciface siface cbench sbench
    if [ "$topo" = veth ]; then
        echo "$nsb" >>"$NS_LIST"
        if ! { sudo -n ip netns add "$nsb" && sudo -n ip link add "$va" type veth peer name "$vb" &&
            sudo -n ip link set "$va" netns "$nsa" && sudo -n ip link set "$vb" netns "$nsb" &&
            sudo -n ip -n "$nsa" addr add 192.168.10.1/24 brd + dev "$va" &&
            sudo -n ip -n "$nsb" addr add 192.168.10.2/24 brd + dev "$vb" &&
            sudo -n ip -n "$nsb" link set lo up && sudo -n ip -n "$nsa" link set "$va" up &&
            sudo -n ip -n "$nsb" link set "$vb" up; }; then
            echo "note=no_veth" >"$rd/status"
            return
        fi
        sns=$nsb ciface=$va siface=$vb cbench=$va sbench=$vb
    else
        if ! { sudo -n ip -n "$nsa" link add "$va" type dummy && sudo -n ip -n "$nsa" link set "$va" multicast on &&
            sudo -n ip -n "$nsa" addr add 192.168.10.1/24 brd + dev "$va" && sudo -n ip -n "$nsa" link set "$va" up; }; then
            echo "note=no_link" >"$rd/status"
            return
        fi
        ciface=$va siface=$va cbench=lo sbench=lo
    fi
    local args
    args="-Q -N $(keepall_for "$shape") -B 100 -d $DUR --warmup-s $EDGE_S --cooldown-s $EDGE_S"
    echo "arm=$arm shape=$shape rep=$rep topo=$topo flags='$(arm_flags "$arm")' args='$args' started=$(date -Is)" >"$rd/job"
    # The logs are written by this shell, as the invoking user: intended.
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$nsa" env J_STATUS="$rd/status" J_CLOG="$rd/client.log" J_SLOG="$rd/server.log" \
        J_DIR="$WORK/bin/$arm/$shape" J_SNS="$sns" J_CIFACE="$ciface" J_ARGS="$args" J_CBENCH="$cbench" \
        J_SBENCH="$sbench" J_CCPU="$CPU_CLIENT" J_SCPU="$CPU_SERVER" J_CTIMEOUT=90 J_PERF="$J_PERF_ON" \
        J_CPERF="$rd/client.perf" J_SPERF="$rd/server.perf" J_PRELOAD="$PRELOAD" \
        bash "$SELF" --job >"$rd/job.log" 2>&1
    sudo -n ip netns del "$nsa" 2>/dev/null
    [ "$topo" = veth ] && sudo -n ip netns del "$nsb" 2>/dev/null
    : "$siface"
}

# The positive control, first: the witness must be able to see the segment, or tx_shm == 0 below means nothing.
first_shape=${SHAPES%% *}
run_one on "$first_shape" 0 samens
say "    positive control (on, $first_shape, one namespace, shared /dev/shm): $(grep -o 'tx_shm=[0-9]*' "$OUTD/runs/${first_shape}_on_r0_samens/client.log" 2>/dev/null | head -1)"

read -r -a ARM_LIST <<<"$ARMS"
NARMS=${#ARM_LIST[@]}
for rep in $(seq 1 "$REPS"); do
    si=0
    for shape in $SHAPES; do
        # Rotated per repetition and per shape, so every arm takes every position in the order as often as possible.
        for k in $(seq 0 $((NARMS - 1))); do
            arm=${ARM_LIST[$(((k + rep + si) % NARMS))]}
            run_one "$arm" "$shape" "$rep" veth
        done
        si=$((si + 1))
    done
    say "    rep $rep/$REPS done at $(($(date +%s) - T0)) s"
done

# ------------------------------------------------------------------------------------------------ verdict
python3 "$HERE/stage1_payg.py" "$OUTD" | tee -a "$SUMMARY"
say "=== done in $(($(date +%s) - T0)) s: $OUTD ==="
