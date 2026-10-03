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

FRAMEWORKS=${FRAMEWORKS:-"tickle fastdds cyclonedds"}
REPS=${REPS:-3}
DUR=${DUR:-5}
SCEN=${SCEN:-reliable_throughput}
# Extra client arguments, passed through to every framework's client so all three get the SAME ones.
# Why this exists (2026-10-02): DUR means different things in the two scenarios. In reliable_throughput, DUR=5 is
# five seconds of millions of samples; in reliable_latency the client pings once a second, so DUR=5 is FIVE round
# trips - and the first S6 latency run produced exactly that, with TickLE's segment never attaching and every
# framework's witness unusable. A latency cell therefore needs -i to set the ping interval, which the harness had
# no way to pass.
CLI_ARGS=${CLI_ARGS:-}
# Forwarded to the tickle cell, which is the only one whose geometry we compile.
BUILD_FLAGS=${BUILD_FLAGS:-}
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
cleanup() {
    [ -n "${srv_pid:-}" ] && sh_ "$HOST" "case \"\$(readlink /proc/$srv_pid/exe 2>/dev/null)\" in
        */server) kill -TERM $srv_pid;; esac" </dev/null >/dev/null 2>&1
    srv_pid=""
}
# ONE EXIT trap for the whole script - a second `trap ... EXIT` silently replaces the first, which once left the
# rig shaped at delay 10ms after a clean exit. The daemon is stopped here too, not by a trap of its own.
# roudi_off is defined further down with the cyclonedds cell; if the script exits before reaching that definition
# this would be a "command not found" inside the trap, so it is called only when it exists.
on_exit() { cleanup; declare -f roudi_off >/dev/null && roudi_off; }
trap on_exit EXIT

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
    if ! OUT="$sub" DUR="$DUR" SCEN="$SCEN" SIZE="$SIZE" CLI_ARGS="$CLI_ARGS" BUILD_FLAGS="$BUILD_FLAGS" \
         TICKLE_DATAGRAM_BYTES="${TICKLE_DATAGRAM_BYTES:-}" TICKLE_RELIABLE_STATS="${TICKLE_RELIABLE_STATS:-}" \
         TICKLE_FRAG_SLOTS="${TICKLE_FRAG_SLOTS:-}" \
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
# FastDDS's binaries do not carry an rpath to their own libraries - run_scenario.sh exports this, and a client
# started without it dies on `libfastrtps.so.2.14: cannot open shared object file` before printing anything. The
# first version of this cell omitted it and lost six runs to it.
FDDS_LIB_PATH=${FDDS_LIB_PATH:-/opt/ros/jazzy/lib}

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
    local env_common="BENCH_IFACE=lo LD_LIBRARY_PATH=$FDDS_LIB_PATH FASTRTPS_DEFAULT_PROFILES_FILE=$p"
    srv_pid=$(sh_ "$HOST" "cd $dir/${SCEN}_${SIZE} && rm -f /tmp/s6_fdds.pid
(setsid sh -c 'echo \$\$ >/tmp/s6_fdds.pid; exec env $env_common taskset -c 1 ./server -d $((DUR + 40))' >/tmp/s6_fdds_server.log 2>&1 </dev/null &); sleep 3; cat /tmp/s6_fdds.pid" </dev/null)
    # The WHOLE output, kept on the Pi and then read, rather than piped through grep '^RESULT' at the far end. A
    # client that cannot load its libraries says so on stderr and prints no RESULT line at all; the first version of
    # this cell discarded that sentence and reported "produced no RESULT line" six times without the reason.
    local all
    all=$(sh_ "$HOST" "cd $dir/${SCEN}_${SIZE} && env $env_common taskset -c 2 ./client -d $DUR $CLI_ARGS >/tmp/s6_fdds_client.log 2>&1; cat /tmp/s6_fdds_client.log" </dev/null)
    line=$(printf '%s\n' "$all" | grep '^RESULT' | head -1)
    cleanup
    if [ -z "$line" ]; then
        say "  arm=$arm produced no RESULT line. What it did say:"
        printf '%s\n' "$all" | grep -v '^RESULT' | tail -6 | sed 's/^/       | /' | tee -a "$OUT"
        say "  server's last lines:"
        sh_ "$HOST" "tail -4 /tmp/s6_fdds_server.log" </dev/null 2>/dev/null | sed 's/^/       | /' | tee -a "$OUT"
        return 0
    fi
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
# ---------------------------------------------------------------- cyclonedds: a daemon, and a weaker identity
# Checked on the rig before this was written, because two of the three facts were not what the file names implied:
#   - libddsc here is 0.10.5 at /opt/ros/jazzy/lib/aarch64-linux-gnu/, NOT /opt/ros/jazzy/lib/ - an ldd of the
#     latter says "No such file or directory", which looks exactly like "no iceoryx dependency" if the line is
#     counted rather than read.
#   - it DOES have shared-memory support compiled in: the SharedMemory config element,
#     dds_is_shared_memory_available, dds_loan_shared_memory_buffer, 52 iceoryx strings.
#   - iox-roudi starts, reserves its segments, prints "RouDi is ready for clients", and stops on TERM.
#
# THE IDENTITY PROBLEM IS REAL HERE AND WEAKER THAN FASTDDS'S. FastDDS prints transport_profile= on its RESULT
# line, so each repetition testifies to the configuration it ran. CycloneDDS prints nothing of the kind: both arms
# are the same binary with a different CYCLONEDDS_URI, and nothing in the output says which. So the ON arm's
# identity rests on bracketing it with the daemon's liveness - ready before, still alive after - plus the witness.
# "Still alive after" is not decoration: if RouDi dies mid-run CycloneDDS does not fail, it falls back to the
# network, and the arm would be a kernel-path run wearing a shared-memory label.
# DERIVED FROM THE BINARY, NOT HARDCODED, and that is the whole point. Two prefixes are installed on the rig:
# jazzy's CycloneDDS 0.10.5 and rolling's 11.0.1. build.sh searches /opt/ros/rolling FIRST, so the campaign's
# client links libddsc.so.11 from rolling through a baked-in DT_RPATH - every CycloneDDS figure in COMPARISON is
# 11.0.1, which nothing in that document currently says.
#
# The first version of this cell copied run_scenario.sh's LIB_PATH (jazzy) and the ON arm failed with
# "Failed to load PSMX library 'psmx_iox'" and then "dds_create_writer failed": DT_RPATH resolved libddsc from
# rolling while LD_LIBRARY_PATH pointed the plugin search at jazzy, which ships no psmx_iox at all. Taking the
# prefix from the binary's own RPATH makes the daemon, the plugin and the library match by construction instead
# of by my getting three paths right.
#
# Worth recording from that failure: CycloneDDS did NOT silently fall back to the network when the plugin was
# missing. It refused to create the writer. The design note above assumed the opposite - "without the daemon
# CycloneDDS does not fail, it uses its network path" - and at least in this failure mode that is not what it
# does. Whether it falls back when the plugin loads but RouDi is absent is a different question and still untested,
# so the refusal below stays.
CDDS_LIB_PATH=${CDDS_LIB_PATH:-}
CDDS_URI_OFF='<CycloneDDS><Domain><General><Interfaces><NetworkInterface name="eth0"/></Interfaces></General><Discovery><SPDPInterval>1s</SPDPInterval></Discovery></Domain></CycloneDDS>'
CDDS_URI_ON='<CycloneDDS><Domain><General><Interfaces><NetworkInterface name="eth0"/></Interfaces></General><Discovery><SPDPInterval>1s</SPDPInterval></Discovery><SharedMemory><Enable>true</Enable></SharedMemory></Domain></CycloneDDS>'
roudi_pid=""

# The prefix the client's own RPATH names, and the RouDi that lives beside it.
cdds_paths() {
    local dir=/home/ci/tickle/examples/perf_hil/cyclonedds/${SCEN}_${SIZE}
    CDDS_LIB_PATH=$(sh_ "$HOST" "readelf -d $dir/client 2>/dev/null | sed -n 's/.*RPATH.*\[\(.*\)\]/\1/p' | head -1" </dev/null)
    [ -n "$CDDS_LIB_PATH" ] || { say "  FATAL the client has no RPATH, so the matching RouDi cannot be derived"; return 1; }
    CDDS_ROUDI=${CDDS_LIB_PATH%/lib/*}/bin/iox-roudi
    sh_ "$HOST" "test -x $CDDS_ROUDI" </dev/null || { say "  FATAL no iox-roudi at $CDDS_ROUDI, beside the library the client links"; return 1; }
    say "  client links $CDDS_LIB_PATH; using $CDDS_ROUDI"
}

roudi_on() {
    roudi_pid=$(sh_ "$HOST" "rm -f /tmp/s6_roudi.pid /tmp/s6_roudi.log
export LD_LIBRARY_PATH=$CDDS_LIB_PATH
(setsid sh -c 'echo \$\$ >/tmp/s6_roudi.pid; exec $CDDS_ROUDI' >/tmp/s6_roudi.log 2>&1 </dev/null &)
for i in 1 2 3 4 5 6 7 8 9 10; do grep -q 'RouDi is ready for clients' /tmp/s6_roudi.log 2>/dev/null && break; sleep 1; done
cat /tmp/s6_roudi.pid" </dev/null)
    if ! sh_ "$HOST" "grep -q 'RouDi is ready for clients' /tmp/s6_roudi.log" </dev/null; then
        say "  FATAL RouDi did not report ready. Refusing the ON arm rather than measuring CycloneDDS's network"
        say "        path and labelling it shared memory. Its log said:"
        sh_ "$HOST" "tail -5 /tmp/s6_roudi.log" </dev/null 2>/dev/null | sed 's/^/        | /' | tee -a "$OUT"
        roudi_off; return 1
    fi
    say "  RouDi ready, pid $roudi_pid"
}

roudi_off() {
    [ -z "${roudi_pid:-}" ] && return 0
    sh_ "$HOST" "case \"\$(readlink /proc/$roudi_pid/exe 2>/dev/null)\" in */iox-roudi) kill -TERM $roudi_pid;; esac
for i in 1 2 3 4 5; do [ -d /proc/$roudi_pid ] || break; sleep 1; done; true" </dev/null >/dev/null 2>&1
    roudi_pid=""
}

cdds_run() {  # cdds_run <arm> <uri>
    # ${SCEN}_${SIZE}, not $SCEN: build.sh puts the binaries in "$HERE/${SCENARIO}_${PAYLOAD}" when a payload is
    # given, same as fastdds. I read the source directory as the output one because at the time I looked the
    # built directory did not exist yet - the listing was of a tree that had not been built.
    local arm=$1 uri=$2 dir=/home/ci/tickle/examples/perf_hil/cyclonedds/${SCEN}_${SIZE} all line
    cleanup
    srv_pid=$(sh_ "$HOST" "cd $dir && rm -f /tmp/s6_cdds.pid
(setsid sh -c 'echo \$\$ >/tmp/s6_cdds.pid; exec env BENCH_IFACE=lo LD_LIBRARY_PATH=$CDDS_LIB_PATH CYCLONEDDS_URI='\''$uri'\'' taskset -c 1 ./server -d $((DUR + 40))' >/tmp/s6_cdds_server.log 2>&1 </dev/null &); sleep 3; cat /tmp/s6_cdds.pid" </dev/null)
    all=$(sh_ "$HOST" "cd $dir && env BENCH_IFACE=lo LD_LIBRARY_PATH=$CDDS_LIB_PATH CYCLONEDDS_URI='$uri' taskset -c 2 ./client -d $DUR $CLI_ARGS >/tmp/s6_cdds_client.log 2>&1; cat /tmp/s6_cdds_client.log" </dev/null)
    line=$(printf '%s\n' "$all" | grep '^RESULT' | head -1)
    cleanup
    if [ -z "$line" ]; then
        say "  arm=$arm produced no RESULT line. What it did say:"
        printf '%s\n' "$all" | grep -v '^RESULT' | tail -6 | sed 's/^/       | /' | tee -a "$OUT"
        return 0
    fi
    # The ON arm only counts if the daemon outlived it: RouDi dying mid-run is a silent fall back to the network.
    if [ "$arm" = ON ]; then
        if [ -z "$(sh_ "$HOST" "readlink /proc/$roudi_pid/exe 2>/dev/null" </dev/null)" ]; then
            say "  arm=ON DISCARDED: RouDi was not alive at the end of this repetition, so the run may have fallen"
            say "       back to the network partway. Not recorded."
            return 0
        fi
    fi
    say "arm=$arm $line"
}

run_cyclonedds_cell() {
    say "### cyclonedds cell: ON=SharedMemory+RouDi  OFF=the campaign's own URI (same binary, URI is the arm) ==="
    local out
    out=$(sh_ "$HOST" "set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA
cd examples/perf_hil/cyclonedds && ./build.sh $SCEN $SIZE >/tmp/s6_cdds_build.log 2>&1 || { echo BUILD_FAILED; cat /tmp/s6_cdds_build.log; exit 0; }
sha256sum ${SCEN}_${SIZE}/client | cut -c1-16" </dev/null 2>&1)
    # "No such file" from sha256sum means the build wrote nothing where this cell will look, which is a failure
    # even though build.sh exited 0. Checked here rather than discovered three repetitions later.
    case "$out" in *BUILD_FAILED*|*error:*|*"No such file"*) say "FATAL cyclonedds build produced no client at ${SCEN}_${SIZE}/:"; say "$out"; return 1;; esac
    say "  cyclonedds built at $SHA, client sha256=$(printf '%s' "$out" | tail -1)"
    cdds_paths || return 1
    say "  NOTE: CycloneDDS prints no per-run identity field, unlike FastDDS's transport_profile=. This arm's"
    say "        identity is the daemon bracket - ready before, alive after - and is weaker for it."
    for r in $(seq 1 "$REPS"); do
        say "--- cyclonedds rep $r/$REPS $(date -Is) ---"
        if roudi_on; then cdds_run ON "$CDDS_URI_ON"; fi
        roudi_off
        cdds_run OFF "$CDDS_URI_OFF"
    done
}

for fw in $FRAMEWORKS; do
    case $fw in
    tickle)  run_tickle_cell ;;
    fastdds) run_fastdds_cell ;;
    cyclonedds) run_cyclonedds_cell ;;
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
    # Bytes as well as packets, because the packet counter alone cannot see every shared-memory transport.
    # Measured 2026-10-02 on the latency cell: TickLE's packet ratio was 1.000 while tx_shm said 87% of samples
    # went through the segment - and the bytes ratio was 0.147, with bytes-per-packet falling 1,339 -> 197. Both
    # are true. Our doorbell wakes a blocked reader over the socket, so a segment-carried sample still costs one
    # small packet; in the throughput cells batching coalesces many samples into each packet and the packet ratio
    # drops, but a latency cell sends one sample at a time and there is nothing to coalesce. So the packet witness
    # is blind to OUR transport in a per-sample scenario, and a cross-check that reads only packets blames the
    # wrong instrument.
    b = re.search(r"wire_bytes_per_sample=([0-9.]+)", rest)
    shm = re.search(r"\btx_shm=([0-9]+)", rest)
    udp = re.search(r"\btx_udp=([0-9]+)", rest)
    # The framework comes from the RESULT line's own framework= field. An earlier version inferred it as
    # "tickle if tx_shm is present else fastdds", which was true while there were two cells and silently merged
    # cyclonedds into fastdds's arms the moment there were three.
    fwm = re.search(r"framework=(\w+)", rest)
    if not fwm:
        print(f"  skipped a row with no framework= field: {rest[:60]}")
        continue
    fw = fwm.group(1)
    if w: rows[(fw, arm)].append((float(w.group(1)),
                                 int(shm.group(1)) if shm else None,
                                 int(udp.group(1)) if udp else None,
                                 float(b.group(1)) if b else None))
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
    if fw == "tickle":  # the cross-check applies only where we wrote the counter
        shm_tot = sum(x[1] for x in on if x[1] is not None)
        udp_tot = sum(x[2] for x in on if x[2] is not None)
        share = shm_tot/(shm_tot+udp_tot) if (shm_tot+udp_tot) else 0.0
        print(f"    cross-check: our own tx_shm share on the ON arm = {share:.3f} ({shm_tot} shm, {udp_tot} udp)")
        if share >= 0.5 and ratio >= 0.75:
            # Before blaming our own counter, ask the third instrument. Bytes and packets are independent readings
            # of the same interface, and a transport that moves the PAYLOAD off the wire while still sending a
            # per-sample notification shows exactly this: packets unchanged, bytes collapsed. If the bytes agree
            # with tx_shm, two instruments out of three agree and the packet counter is simply blind to this
            # transport in this scenario - which is a fact about the cell's shape, not a defect in the counter.
            bon = [x[3] for x in on if x[3] is not None]
            boff = [x[3] for x in off if x[3] is not None]
            bratio = (st.median(bon)/st.median(boff)) if bon and boff and st.median(boff) > 0 else None
            if bratio is not None and bratio <= 0.25:
                print(f"    witness-by-bytes {bratio:.3f} (packets {ratio:.3f}): the payload left the wire and the")
                print("         per-sample notification did not. tx_shm and the byte counter agree, so the packet")
                print("         ratio is blind to this transport here rather than contradicting them. Cell stands.")
            else:
                print("    VOID tickle: tx_shm says shared memory, and neither the packet nor the byte witness")
                bshow = "n/a" if bratio is None else f"{bratio:.3f}"
                print(f"         agrees (packets {ratio:.3f}, bytes {bshow}). Our counter is the suspect - it is")
                print("         the one we wrote. This is a finding about the instrument.")
        elif share < 0.5 and ratio <= 0.25:
            print("    VOID tickle: tx_shm says kernel, the witness says shared memory. The witness is the")
            print("         suspect here. Either way S6 cannot use either number until it is known which.")
        else:
            print("    the two independent instruments agree, so neither is currently suspect.")
PYEOF
