#!/usr/bin/env bash
# freshness_rt_pc.sh - the PC dry run of KEEP_LAST freshness by ROUND TRIP (tickle/common/BenchFreshRt.h; the user's
# rule of 2026-10-19: across hosts only round trips are timed). NOT a rig figure: two private network namespaces joined
# by a veth on one PC, never the default namespace, no load generator. It shows the instrument does what it must
# before the rig runs keep_last_freshness_rt.sh, and it is read by the same reader, freshness_rt_summary.py, with
# --pc (which lowers nothing but the rep count it demands: 2).
#
#   netns frtc<pid> (the sender, history_depth_burst_loss/client): eth0 192.168.10.2/24, the --fresh-rt-listen side
#   netns frts<pid> (the reader, history_depth_burst_loss/server): eth0 192.168.10.3/24, --fresh-rt 192.168.10.2:PORT
#   The addresses are the rig's test link (fastdds_eth0_only.xml lists both); each veth end is named eth0 so the
#   CycloneDDS URI of cyclonedds/run_scenario.sh applies unchanged.
#
# CELLS (the rig's, keep_last_freshness_rt.sh, at PC scale):
#   h43   -i 0.05 -n 160 -p 0                      RESULTS row 43 as published, round trip
#   b16   -i 0.05 -n 1600 -p 0 --burst 16          100 bursts of twice the depth
#   dly   h43 with `tc netem delay 1ms` on the sender's eth0 - THE FAULT ARM: the round-trip age and the side channel's
#         RTT must both move by +1 ms (freshness_rt_summary.py R2), the realized qdisc recorded beside each run
#   idn   IDENTITY, not scored: -i 0.05 -n 160 -p 0 with NO freshness flag, run by this checkout's binaries and by
#         BASE's (a checkout without BenchFreshRt.h); the RESULT lines must be equal byte for byte. Printed as
#         IDENTITY lines at the end.
# Frameworks (Fast DDS over UDPv4 only, see FDDS_XML below): tickle and cyclonedds as built by their build.sh (the TickLE prefix under $OUT/home, never the shared
# ~/tickle_local_install); fastdds with a PC-only compatibility copy: this PC has Fast DDS 3.6 (/opt/ros/lyrical)
# where the rig has 2.14, so the copy renames eprosima::fastrtps::Duration_t, compares write() with RETCODE_OK (a
# ReturnCode_t in 3.x, a bool in 2.x) and takes fastddsgen 4's Bench.hpp. Nothing measured is changed; the copy lives
# in $OUT and is never committed.
#
# Usage: [BASE=/path/to/old/checkout] [REPS=2] [CELLS="h43 b16 dly idn"] [FWS="tickle cyclonedds fastdds"]
#        [OUT=/home/semih/tmp_build/frt_pc/<ts>] freshness_rt_pc.sh
# Detached: setsid nohup examples/perf_hil/experiments/freshness_rt_pc.sh > /path/outside/the/session.log 2>&1 < /dev/null &
# Estimate: builds ~4 min, then ~16 s per run: 3 fw x (2 h43 + 2 b16 + 2 dly + 2 idn) = 24 runs, ~7 min; ~11 min.
# The last line is always "FRESHNESS_RT_PC rc=<n> ...".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PH="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$PH/../.." && pwd)"
REPS=${REPS:-2}
CELLS=${CELLS:-h43 b16 dly idn}
FWS=${FWS:-tickle cyclonedds fastdds}
BASE=${BASE:-}
OUT=${OUT:-/home/semih/tmp_build/frt_pc/$(date +%Y%m%d-%H%M%S)}
PORT=${PORT:-47220}
# Named per invocation: two dry runs at once must never share (or delete) each other's namespaces.
NSC=frtc$$
NSS=frts$$
RES=$OUT/results.txt
rc=1
mkdir -p "$OUT" || exit 1
# The one EXIT handler: removes both namespaces (and with them the veth and any netem), then the last line.
finish() {
    sudo -n ip netns del "$NSC" 2>/dev/null
    sudo -n ip netns del "$NSS" 2>/dev/null
    echo "FRESHNESS_RT_PC rc=$rc out=$OUT $(date -Is)"
}
trap finish EXIT
say() { echo "$*" | tee -a "$RES"; }
export TMPDIR=/home/semih/tmp_build

# ---- builds ----------------------------------------------------------------------------------------------------
FDDS_INC=/opt/ros/lyrical/include/fastdds
FCDR_INC=/opt/ros/lyrical/includefastcdr
FDDS_LIBS="/opt/ros/lyrical/lib:/opt/ros/lyrical/lib/x86_64-linux-gnu"
FDDSGEN=${FDDSGEN:-/home/semih/tmp_build/ctl/perf/src/performance_test/bin/fastddsgen}

# build_fastdds_pc <checkout> <dest dir>: the compatibility copy described in the header, p1 shape.
build_fastdds_pc() {
    local src=$1/examples/perf_hil/fastdds dst=$2 f
    mkdir -p "$dst/gen" "$dst/src" || return 1
    # From a copy of the IDL in the destination: fastddsgen 4 keeps an IDL's path relative to the working directory.
    cp "$1/examples/perf_hil/idl/p1/Bench.idl" "$dst/Bench.idl" || return 1
    (cd "$dst" && "$FDDSGEN" -replace -d gen Bench.idl) >"$dst/gen.log" 2>&1 || return 1
    [ -f "$dst/gen/BenchPubSubTypes.cxx" ] || { echo "fastddsgen wrote no BenchPubSubTypes.cxx"; return 1; }
    printf '#include "Bench.hpp"\n' >"$dst/gen/Bench.h"
    printf '#include "BenchPubSubTypes.hpp"\n' >"$dst/gen/BenchPubSubTypes.h"
    sed -e "s#\"../tickle/common/#\"$1/examples/perf_hil/tickle/common/#" "$src/harness_common.hpp" \
        >"$dst/harness_common.hpp" || return 1
    for f in client server; do
        sed -e 's/eprosima::fastrtps::Duration_t/eprosima::fastdds::dds::Duration_t/g' \
            -e 's/if (writer->write(&msg))/if (writer->write(\&msg) == eprosima::fastdds::dds::RETCODE_OK)/' \
            -e 's/ReturnCode_t::RETCODE_OK/eprosima::fastdds::dds::RETCODE_OK/g' \
            -e 's#fastdds/rtps/common/Time_t.h>#fastdds/rtps/common/Time_t.hpp>#' \
            -e "s#\"../../tickle/common/#\"$1/examples/perf_hil/tickle/common/#" \
            -e 's#"../harness_common.hpp"#"harness_common.hpp"#' \
            "$src/history_depth_burst_loss/$f.cpp" >"$dst/src/$f.cpp" || return 1
        grep -q 'RETCODE_OK)' "$dst/src/$f.cpp" || [ "$f" = server ] || { echo "compat edit missed write() in $f"; return 1; }
        # shellcheck disable=SC2086
        nice g++ -O2 -DBENCH_SAMPLE_BYTES=76 -std=c++17 -I"$dst" -I"$dst/gen" -I"$FDDS_INC" -I"$FCDR_INC" \
            -o "$dst/$f" "$dst/src/$f.cpp" "$dst/gen/BenchPubSubTypes.cxx" "$dst/gen/BenchTypeObjectSupport.cxx" \
            -L/opt/ros/lyrical/lib -L/opt/ros/lyrical/lib/x86_64-linux-gnu -Wl,-rpath,"$FDDS_LIBS" \
            -lfastdds -lfastcdr -lpthread >"$dst/build_$f.log" 2>&1 || { tail -20 "$dst/build_$f.log"; return 1; }
    done
}

# build_all <checkout> <tag>: binaries into $OUT/bin/<tag>/<fw>/{client,server}.
build_all() {
    local co=$1 tag=$2 fw d
    for fw in $FWS; do
        d=$OUT/bin/$tag/$fw
        mkdir -p "$d" || return 1
        case $fw in
        tickle | cyclonedds)
            (cd "$co/examples/perf_hil/$fw" && HOME=$OUT/home PATH=$PATH:/opt/ros/lyrical/bin nice ./build.sh history_depth_burst_loss) \
                >"$d/build.log" 2>&1 || { echo "BUILD FAILED $tag $fw"; tail -20 "$d/build.log"; return 1; }
            cp "$co/examples/perf_hil/$fw/history_depth_burst_loss/client" \
                "$co/examples/perf_hil/$fw/history_depth_burst_loss/server" "$d/" || return 1
            ;;
        fastdds) build_fastdds_pc "$co" "$d" || { echo "BUILD FAILED $tag fastdds"; return 1; } ;;
        esac
        echo "built $tag $fw"
    done
}
mkdir -p "$OUT/home"
if [ -n "${BIN_FROM:-}" ]; then # BIN_FROM=<an earlier OUT>: its binaries, no build
    cp -a "$BIN_FROM/bin" "$OUT/" || exit 1
    echo "binaries from $BIN_FROM"
else
    build_all "$REPO" new || exit
    case " $CELLS " in *" idn "*)
        [ -n "$BASE" ] || { echo "REFUSED: cell idn needs BASE=<checkout without the change>"; exit 1; }
        build_all "$BASE" base || exit
        ;;
    esac
fi

# ---- the namespaces ----------------------------------------------------------------------------------------------
sudo -n ip netns del "$NSC" 2>/dev/null
sudo -n ip netns del "$NSS" 2>/dev/null
sudo -n ip netns add "$NSC" && sudo -n ip netns add "$NSS" || exit 1
sudo -n ip link add "c$$" netns "$NSC" type veth peer name "s$$" netns "$NSS" || exit 1
sudo -n ip -n "$NSC" link set "c$$" name eth0 && sudo -n ip -n "$NSS" link set "s$$" name eth0 || exit 1
sudo -n ip -n "$NSC" addr add 192.168.10.2/24 broadcast 192.168.10.255 dev eth0 || exit 1
sudo -n ip -n "$NSS" addr add 192.168.10.3/24 broadcast 192.168.10.255 dev eth0 || exit 1
for ns in "$NSC" "$NSS"; do
    sudo -n ip -n "$ns" link set lo up && sudo -n ip -n "$ns" link set eth0 up || exit 1
    sudo -n ip -n "$ns" route add default dev eth0 || exit 1
done

set_delay() { # on|off, on the sender's eth0 (as the rig arm: rpi#1, the only Pi with tc)
    if [ "$1" = on ]; then
        sudo -n ip netns exec "$NSC" tc qdisc replace dev eth0 root netem delay 1ms
    else
        sudo -n ip netns exec "$NSC" tc qdisc del dev eth0 root 2>/dev/null || true
    fi
}
qdisc_now() { sudo -n ip netns exec "$NSC" tc qdisc show dev eth0 | head -1; }

CDDS_URI='<CycloneDDS><Domain><General><Interfaces><NetworkInterface name="eth0"/></Interfaces></General><Discovery><SPDPInterval>1s</SPDPInterval></Discovery></Domain></CycloneDDS>'
# Fast DDS on this PC: UDPv4 only, no shared-memory transport (the rig's fastdds_eth0_only.xml does the same with 2.14's
# interfaceWhiteList; with it, 3.6 never matched across the veth on 2026-10-19, and with its default transports the data
# could take shared memory between the namespaces and bypass the link the fault arm shapes). PC only.
if [ -z "${FDDS_XML:-}" ]; then
    FDDS_XML=$OUT/fastdds_pc_udp_v3.xml
    cat >"$FDDS_XML" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<dds xmlns="http://www.eprosima.com">
  <profiles>
    <transport_descriptors>
      <transport_descriptor>
        <transport_id>pc_udp</transport_id>
        <type>UDPv4</type>
      </transport_descriptor>
    </transport_descriptors>
    <participant profile_name="pc_udp_only_participant" is_default_profile="true">
      <rtps>
        <userTransports>
          <transport_id>pc_udp</transport_id>
        </userTransports>
        <useBuiltinTransports>false</useBuiltinTransports>
      </rtps>
    </participant>
  </profiles>
</dds>
EOF
fi

# run_one <run id> <fw> <bin dir> <args...>: server in frts, client in frtc, the server stopped with SIGINT by the PID
# its own shell recorded (checked against /proc/PID/exe inside the namespace) once the client is done.
run_one() {
    local run=$1 fw=$2 dir=$3
    shift 3
    local pidf=$OUT/server_$run.pid slog=$OUT/server_$run.log clog=$OUT/client_$run.log envs
    envs=(env "CYCLONEDDS_URI=$CDDS_URI" "FASTRTPS_DEFAULT_PROFILES_FILE=$FDDS_XML" "FASTDDS_DEFAULT_PROFILES_FILE=$FDDS_XML")
    rm -f "$pidf"
    # The logs are written by this shell, as the invoking user, and not by the sudo'd process: intended.
    # shellcheck disable=SC2024
    sudo -n ip netns exec "$NSS" "${envs[@]}" sh -c 'echo $$ > "$1"; shift; exec "$@"' sh "$pidf" "$dir/server" "$@" \
        >"$slog" 2>&1 </dev/null &
    # shellcheck disable=SC2024 # as above
    sudo -n ip netns exec "$NSC" "${envs[@]}" timeout 120 "$dir/client" "$@" >"$clog" 2>&1 </dev/null
    local pid exe _
    pid=$(cat "$pidf" 2>/dev/null)
    exe=$(sudo -n ip netns exec "$NSS" readlink "/proc/$pid/exe" 2>/dev/null)
    if [ -n "$pid" ] && [ "$exe" = "$dir/server" ]; then
        sudo -n ip netns exec "$NSS" kill -INT "$pid"
    else
        say "FRESH run=$run server pid '$pid' is '$exe', not $dir/server: not signalled"
    fi
    wait
    grep -h '^RESULT:' "$clog" "$slog" | while IFS= read -r line; do say "FRESH run=$run $line"; done
}

cell_args() {
    local listen="--fresh-rt-listen $PORT --fresh-rt 192.168.10.2:$PORT"
    case "$1" in
    h43 | dly) echo "-i 0.05 -n 160 -p 0 --fresh $listen" ;;
    b16) echo "-i 0.05 -n 1600 -p 0 --fresh --burst 16 $listen" ;;
    idn) echo "-i 0.05 -n 160 -p 0" ;;
    esac
}

say "=== freshness_rt_pc $(date -Is) head $(git -C "$REPO" rev-parse --short HEAD) base ${BASE:-none} reps $REPS cells $CELLS fws $FWS ==="
say "PC: $(uptime)"
run=0
read -r -a fw_arr <<<"$FWS"
for rep in $(seq 1 "$REPS"); do
    k=$(((rep - 1) % ${#fw_arr[@]}))
    order=("${fw_arr[@]:$k}" "${fw_arr[@]:0:$k}")
    for c in $CELLS; do
        for fw in "${order[@]}"; do
            if [ "$c" = dly ]; then set_delay on; else set_delay off; fi
            # shellcheck disable=SC2207
            args=($(cell_args "$c"))
            if [ "$c" = idn ]; then
                for tag in base new; do
                    run=$((run + 1))
                    say "FRESH run=$run cell=idn-$tag fw=$fw rep=$rep topo=pc args=${args[*]}"
                    run_one "$run" "$fw" "$OUT/bin/$tag/$fw" "${args[@]}"
                done
                continue
            fi
            run=$((run + 1))
            say "FRESH run=$run cell=$c fw=$fw rep=$rep topo=pc args=${args[*]}"
            say "QDISC run=$run $(qdisc_now)"
            run_one "$run" "$fw" "$OUT/bin/new/$fw" "${args[@]}"
            say "QDISC run=$run after $(qdisc_now)"
        done
    done
done
set_delay off

# The identity arm: each base run's RESULT lines against the new run right after it, byte for byte.
python3 - "$RES" <<'EOF' | tee -a "$RES"
import re, sys
runs, cur = {}, None
for line in open(sys.argv[1]):
    m = re.match(r"FRESH run=(\d+) cell=idn-(base|new) fw=(\w+) rep=(\d+)", line)
    if m:
        cur = (m.group(3), m.group(4), m.group(2)); runs[cur] = []; continue
    m = re.match(r"FRESH run=\d+ (RESULT: .*)", line)
    if m and cur:
        runs[cur].append(m.group(1).rstrip("\n"))
    elif not line.startswith("FRESH run") or "cell=" in line:
        cur = None if "cell=" in line else cur
keys = sorted({k[:2] for k in runs})
for fw, rep in keys:
    b, n = runs.get((fw, rep, "base"), []), runs.get((fw, rep, "new"), [])
    same = len(b) == 2 and b == n
    print(f"IDENTITY fw={fw} rep={rep} lines_base={len(b)} lines_new={len(n)} {'EQUAL' if same else 'DIFFERENT'}")
    if not same:
        for x, y in zip(b, n):
            if x != y:
                print(f"IDENTITY   base: {x}\nIDENTITY   new:  {y}")
EOF
rc=0
say "=== done $(date -Is): $run runs; read with: examples/perf_hil/experiments/freshness_rt_summary.py --pc $RES ==="
