#!/usr/bin/env bash
# s6_transport_cells.sh - S6's paired transport cells: for each framework, does shared memory actually carry the
# samples, and does the framework's own account of that agree with an instrument it does not control?
#
# WHY PAIRED ARMS. RMW_GAPS_PLAN's S6 settled on the loopback interface's packet counter as the witness, because a
# framework's introspection reports what it was CONFIGURED with and not what it DID. The validation run
# (s6_witness_check.sh, 2026-09-30) then refused the absolute bands that design had guessed, for two reasons worth
# more than the thresholds were: on `lo` a datagram is counted leaving AND arriving, so the kernel-path baseline is
# 2.0 packets per sample rather than 1.0; and the shared-memory arm is not 0, because a transport that moves data
# through a ring still signals over the socket (the doorbell). So the witness is a RATIO against the same cell's own
# kernel-path arm, and a cell without that arm has no denominator and is not a measurement.
#
#   ratio = median(ON  wire_packets_per_sample) / median(OFF wire_packets_per_sample)
#     <= 0.25  the data went through shared memory; what remains on the interface is control traffic
#     >= 0.75  the kernel carried the data, whatever the configuration claimed
#     between  the arm did not use one transport -> VOID, and the row says that instead of averaging two paths
#
# IDENTITY, MATCHED TO WHAT CAN ACTUALLY FAIL ON EACH FRAMEWORK. These are not the same check twice:
#   tickle  - the arms differ by -Dtt_SEGMENT_ENABLED=0, so the two BINARIES must differ. Identical binaries mean
#             the flag never reached the compiler, and "no difference" would be true for the wrong reason.
#   fastdds - the arms differ by an XML profile and come out of ONE binary, so the binary says nothing. The RESULT
#             line's transport_profile= must name the file that arm intended. If the environment did not arrive,
#             both arms run the same configuration and a null result reads as "shared memory did nothing".
# The identity check asks "did the configuration I intended apply?". The witness asks "which transport carried the
# samples?". A framework can pass the first and fail the second - that is the whole reason S6 exists.
#
# CROSS-CHECK, AND IT APPLIES ONLY TO US. tx_shm is a counter we wrote; the loopback counter is not. On the tickle
# cell the two are independent measurements of one fact and must agree:
#   tx_shm share high  AND witness ratio >= 0.75  ->  our counter is lying     -> VOID, naming the counter
#   tx_shm share low   AND witness ratio <= 0.25  ->  the witness is lying     -> VOID, naming the witness
# Either disagreement is a finding about an instrument, not about a transport. This is the only part of S6 that can
# catch a defect in something this project built, which is why it is here and not optional.
#
# CYCLONEDDS IS REFUSED, NOT SKIPPED. Its shared-memory path needs iox-roudi running (/opt/ros/jazzy/bin/iox-roudi
# is present on the rig). Without the daemon CycloneDDS does not fail - it uses its network path and its config still
# says SharedMemory is enabled, which is exactly the configuration-read this design exists to avoid. So this script
# refuses that cell with a message rather than measuring the network path and labelling it shared memory. Building
# the daemon's lifecycle is the named next step in RMW_GAPS_PLAN's S6 section.
#
# Usage: s6_transport_cells.sh            Output: $OUT (default ~/rig_results_safe/s6_transport_cells.txt)
#   FRAMEWORKS="tickle fastdds"  REPS=3  DUR=5  SCEN=reliable_throughput  SIZE=p1  SHA=<sha>
#
# OUT defaults to a path on DISK, not /tmp: /tmp here is tmpfs, and the reboot of 2026-09-30 23:54 took a running
# measurement's output with it. A location is not a durability property.
#
# S6_ANALYSE_ONLY=1 runs the verdict block against an $OUT that already exists and touches no host and no lock. It is
# here so the verdicts can be shown to DECIDE - fed an arm pair that must pass and pairs that must each be refused,
# they have to disagree. The same code path, not a copy of it: a copy drifts and then tests itself.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
ANALYSE_ONLY=${S6_ANALYSE_ONLY:-0}
if [ "$ANALYSE_ONLY" != 1 ]; then
    export RIG_LOCK_SCOPE=hil
    if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi
    export RIG_LOCK_HELD_HIL=1   # so the tickle cell's own harness does not take the lock again
fi

FRAMEWORKS=${FRAMEWORKS:-"tickle fastdds"}
REPS=${REPS:-3}
DUR=${DUR:-5}
SCEN=${SCEN:-reliable_throughput}
SIZE=${SIZE:-p1}
SHA=${SHA:-$(git -C "$REPO" rev-parse --short HEAD)}
OUT=${OUT:-$HOME/rig_results_safe/s6_transport_cells.txt}
HOST=${HOST:-10.1.1.214}     # both roles on one Pi, so a same-host transport is possible at all
K=$HOME/.ssh/tickle_ci_ed25519
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$1" "${@:2}"; }
mkdir -p "$(dirname "$OUT")"
[ "$ANALYSE_ONLY" = 1 ] || : >"$OUT"
say() { echo "$*" | tee -a "$OUT"; }

srv_pid=""
cleanup() {   # ONE EXIT trap for the whole script: a second one silently replaces the first
    [ -n "${srv_pid:-}" ] && sh_ "$HOST" "case \"\$(readlink /proc/$srv_pid/exe 2>/dev/null)\" in
        */server) kill -TERM $srv_pid;; esac" </dev/null >/dev/null 2>&1
    srv_pid=""
}
trap cleanup EXIT

if [ "$ANALYSE_ONLY" = 1 ]; then
    echo "=== S6 verdicts only, against the existing $OUT - no host touched ==="
else
say "=== S6 transport cells $(date -Is) sha=$SHA scen=$SCEN size=$SIZE dur=${DUR}s reps=$REPS host=$HOST iface=lo ==="
say "    frameworks: $FRAMEWORKS"
fi

# ---------------------------------------------------------------- tickle: delegate, do not re-implement
run_tickle_cell() {
    local sub="$OUT.tickle"
    say "### tickle cell: delegating to s6_witness_check.sh (validated 2026-09-30) rather than copying its arms ==="
    if ! OUT="$sub" DUR="$DUR" SCEN="$SCEN" SIZE="$SIZE" \
         "$REPO/examples/perf_hil/experiments/s6_witness_check.sh" "$SHA" "$REPS" >/dev/null 2>&1; then
        say "  tickle cell FAILED to run - see $sub"
    fi
    if [ ! -s "$sub" ]; then
        say "  tickle cell produced no output at $sub. That is 'could not look', not 'no difference'."
        return 0
    fi
    grep -E '^(arm=|FATAL|  arm )' "$sub" | sed 's/^/  /' | tee -a "$OUT" >/dev/null
    grep -E '^(arm=|FATAL)' "$sub" | sed 's/^/  /'
}

# ---------------------------------------------------------------- fastdds: two profiles, one binary
fdds_build() {
    local out
    out=$(sh_ "$HOST" "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA
cd examples/perf_hil/fastdds && ./build.sh $SCEN $SIZE >/tmp/s6_fdds_build.log 2>&1 || { echo BUILD_FAILED; cat /tmp/s6_fdds_build.log; exit 0; }
sha256sum ${SCEN}_${SIZE}/client | cut -c1-16" </dev/null 2>&1)
    case "$out" in *BUILD_FAILED*|*error:*) say "FATAL fastdds build failed:"; say "$out"; return 1;; esac
    say "  fastdds built at $SHA, client sha256=$(printf '%s' "$out" | tail -1)"
}

fdds_run() {  # fdds_run <arm> <profile-basename>
    local arm=$1 prof=$2 dir=/home/ci/tickle/examples/perf_hil/fastdds p line
    p="$dir/$prof"
    sh_ "$HOST" "test -f $p" </dev/null || { say "  FATAL arm $arm: profile $prof is not on $HOST"; return 1; }
    cleanup
    srv_pid=$(sh_ "$HOST" "cd $dir/${SCEN}_${SIZE} && rm -f /tmp/s6_fdds.pid
(setsid sh -c 'echo \$\$ >/tmp/s6_fdds.pid; exec env BENCH_IFACE=lo FASTRTPS_DEFAULT_PROFILES_FILE=$p taskset -c 1 ./server -d $((DUR + 40))' >/tmp/s6_fdds_server.log 2>&1 </dev/null &); sleep 3; cat /tmp/s6_fdds.pid" </dev/null)
    line=$(sh_ "$HOST" "cd $dir/${SCEN}_${SIZE} && env BENCH_IFACE=lo FASTRTPS_DEFAULT_PROFILES_FILE=$p taskset -c 2 ./client -d $DUR 2>&1 | grep '^RESULT'" </dev/null)
    cleanup
    if [ -z "$line" ]; then say "  arm=$arm produced no RESULT line"; return 0; fi
    # Identity: the arm must report the profile it was given. Checked per rep, because the environment can arrive
    # for one rep and not the next, and an arm that silently reverted would otherwise be averaged in.
    case "$line" in
    *"transport_profile=$prof"*) say "arm=$arm $line" ;;
    *) say "  arm=$arm IDENTITY FAILED: its RESULT line does not say transport_profile=$prof, so this rep ran a"
       say "       configuration it was not asked for. Not recorded."
       say "       $line" ;;
    esac
}

run_fastdds_cell() {
    say "### fastdds cell: ON=fastdds_shm_and_eth0.xml  OFF=fastdds_eth0_only.xml (one binary, profile is the arm) ==="
    fdds_build || return 1
    for r in $(seq 1 "$REPS"); do
        say "--- fastdds rep $r/$REPS $(date -Is) ---"
        fdds_run ON  fastdds_shm_and_eth0.xml
        fdds_run OFF fastdds_eth0_only.xml
    done
}

[ "$ANALYSE_ONLY" = 1 ] && FRAMEWORKS=""   # analyse an existing $OUT: run no cells
for fw in $FRAMEWORKS; do
    case $fw in
    tickle)  run_tickle_cell ;;
    fastdds) run_fastdds_cell ;;
    cyclonedds)
        say "### cyclonedds cell: REFUSED ==="
        say "  Its shared-memory path needs iox-roudi, and without the daemon CycloneDDS does not fail - it uses"
        say "  its network path while its configuration still reports SharedMemory enabled. Measuring that and"
        say "  calling it shared memory is the configuration-read this design exists to avoid, so this cell is"
        say "  refused until the daemon's lifecycle is built (RMW_GAPS_PLAN S6's named next step)." ;;
    *) say "### unknown framework '$fw' - ignored, and said so rather than passed over silently" ;;
    esac
done
[ "$ANALYSE_ONLY" = 1 ] || say "=== done $(date -Is) ==="

# Not `A && cat || tee`: if cat failed, tee would run and append the verdicts to the file being analysed.
if [ "$ANALYSE_ONLY" = 1 ]; then SINK=(cat); else SINK=(tee -a "$OUT"); fi
python3 - "$OUT" <<'PYEOF' | "${SINK[@]}"
import re, statistics as st, sys, collections
rows = collections.defaultdict(list)
for line in open(sys.argv[1]):
    m = re.match(r"^\s*arm=(\S+) .*RESULT:(.*)$", line)
    if not m: continue
    arm, rest = m.group(1), m.group(2)
    w = re.search(r"wire_packets_per_sample=([0-9.]+)", rest)
    shm = re.search(r"\btx_shm=([0-9]+)", rest)
    udp = re.search(r"\btx_udp=([0-9]+)", rest)
    fw = "tickle" if shm else "fastdds"
    if w: rows[(fw, arm)].append((float(w.group(1)),
                                 int(shm.group(1)) if shm else None,
                                 int(udp.group(1)) if udp else None))
print()
print("=== S6 verdicts: the witness as a ratio against the same cell's own kernel-path arm ===")
for fw in sorted({k[0] for k in rows}):
    on, off = rows.get((fw,"ON"), []), rows.get((fw,"OFF"), [])
    print(f"  {fw}: ON n={len(on)}  OFF n={len(off)}")
    if len(on) < 3 or len(off) < 3:
        print(f"    VOID {fw}: fewer than 3 usable reps on an arm - a ratio from one or two is not a measurement.")
        continue
    mon, moff = st.median([x[0] for x in on]), st.median([x[0] for x in off])
    print(f"    ON  wire_packets_per_sample median {mon:.3f}   OFF median {moff:.3f}")
    if moff <= 0:
        print(f"    VOID {fw}: the kernel-path arm shows {moff:.3f} packets per sample, so there is no denominator.")
        continue
    ratio = mon/moff
    band = "shared memory carried the data" if ratio <= 0.25 else \
           "the kernel carried the data, whatever the configuration said" if ratio >= 0.75 else None
    print(f"    ratio {ratio:.3f}  ->  {band if band else 'MIXED: the arm did not use one transport'}")
    if band is None:
        print(f"    VOID {fw}: between 0.25 and 0.75. Reported as a mixed path rather than averaged over two.")
        continue
    if fw == "tickle":
        shm_tot = sum(x[1] for x in on if x[1] is not None)
        udp_tot = sum(x[2] for x in on if x[2] is not None)
        share = shm_tot/(shm_tot+udp_tot) if (shm_tot+udp_tot) else 0.0
        print(f"    cross-check: our own tx_shm share on the ON arm = {share:.3f} ({shm_tot} shm, {udp_tot} udp)")
        if share >= 0.5 and ratio >= 0.75:
            print("    VOID tickle: tx_shm says shared memory, the loopback witness says kernel. Our counter is")
            print("         the suspect - it is the one we wrote. This is a finding about the instrument.")
        elif share < 0.5 and ratio <= 0.25:
            print("    VOID tickle: tx_shm says kernel, the witness says shared memory. The witness is the")
            print("         suspect here. Either way S6 cannot use either number until it is known which.")
        else:
            print("    the two independent instruments agree, so neither is currently suspect.")
PYEOF
