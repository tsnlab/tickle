#!/usr/bin/env bash
# rmw_samehost.sh - the rmw comparison on ONE Pi: rmw_tickle against rmw_fastrtps_cpp and rmw_cyclonedds_cpp, with
# rmw_zenoh_cpp as reference, each on its own shipped same-host transport (work list item 10). docs/RESULTS.md's rmw
# rows (48-75) are all cross-host; the native same-host rows (S1-S16) never went through rclcpp.
#
# Shape (rmw_samehost_cell.sh runs each one; this drives, collects and reads):
#   rtt   rmw_perf_pingpong's ping_node and pong_node on one Pi, the binaries rmw_crosshost_rtt.sh uses, block wait and
#         poll wait (POLL_ARGS: 100 us sleep, random phase, RTT at the callback - the `J` rows' method), msgs bench and
#         array1k, BEST_EFFORT and RELIABLE (the ping's QoS: KEEP_LAST 8 both ways, the same for every rmw).
#   tput  apex perf_test publisher and subscriber on one Pi, -r 0, Array1k and Array4k, RELIABLE + KEEP_ALL (depth
#         1000 echoed) and BEST_EFFORT + KEEP_LAST 1 (docs/TESTING.md section 5 rule 1), the flags rmw_keepall_rig.sh
#         uses otherwise.
#
# Fairness (docs/TESTING.md sections 4 and 5), enforced here or in the cell script, not assumed:
#   - SHIPPED DEFAULTS, DDS defaults first (the user, 2026-10-06). No profile, URI, session config or interface
#     restriction for any vendor: Fast DDS 2.14's builtin transports (SHM and
#     UDPv4, data-sharing as rmw_fastrtps_cpp decides), CycloneDDS 0.10.5 as jazzy ships it, rmw_zenoh_cpp through
#     its router (rmw_zenohd, shipped config, its CPU and memory counted with zenoh's). Each process's middleware
#     environment is scrubbed and read back from /proc/PID/environ; each starts in an empty directory.
#   - rmw_tickle twice. Arm `tickle` sets TICKLE_BROADCAST_ADDR to the rig's eth0 link (TICKLE_BCAST), as every rig
#     rmw harness does; it is the arm that is scored. Arm `tickle_shipped` sets nothing. Found by this harness's PC
#     preflight (2026-10-06): with the shipped broadcast, 255.255.255.255, tt_resolve_link() finds no interface that
#     owns it, so the context's own address stays 0, note_same_host_peer() never sees a peer at its own address, and
#     no segment is ever built (shm_segments_created=0, every datagram tx_udp_broadcast). rmw_tickle as shipped runs
#     same-host traffic over UDP; tickle_shipped measures exactly that and is printed beside, never instead.
#     It is NOT in the default ARMS on the rig: with no own address every datagram is a limited broadcast, which on a
#     rig Pi leaves by the default route - wlan0, the management WiFi - so a max-rate tput cell would flood the lab
#     network the orchestration runs over. The dry run checks it with one 3 s rtt run; ARMS can add it deliberately.
#   - Nothing pinned, governor as the distribution leaves it (ondemand on the rig).
#   - The same warm-up and cool-down for every arm: rtt drops the first WARM_RT and last COOL_RT round trips (4096,
#     1 ms apart, as the native latency cells), by sequence number from the ping's own stamps; tput drops the first
#     WARM_S and last COOL_S delivered seconds. CPU is taken over the same window (rtt: schedstat sampled on the
#     stamps' clock; tput: perf_test's getrusage rows).
#   - rmw_tickle built as the jazzy debs are (CMake type None, -g -O2, no NDEBUG; RMW_TICKLE_BUILD below), every
#     process's /proc/PID/maps must show the arm's rmw library (rmw_tickle: the path just built) and only that one.
#   - Arm order rotated per repetition and per cell; the rig lock taken once for the whole run.
#   - A transport witness per run: every interface's bytes and packets (/proc/net/dev, lo + eth0 + wlan0) per
#     sample. A kernel-path sample is counted leaving and arriving, so it costs at least 2 x its size; the summary
#     reads bytes / (2 x payload) <= 0.25 as shared memory, >= 0.75 as kernel, between as mixed (only for payloads of
#     512 B and more; at 64 B the headers decide). rmw_tickle must also show its own tx_shm share >= 0.5, and the two
#     must agree, or the run is VOID (s6_transport_cells.sh's cross-check). For the vendors the witness is the
#     finding: which transport their default really uses on one host.
#
# What jazzy's vendors do on one host by default, read on the rig 2026-10-06 (the witness is what decides it):
#   - CycloneDDS 0.10.5 (libddsc links libiceoryx_binding_c, so shared memory is compiled in) leaves
#     SharedMemory/Enable at its default, false, and no iox-roudi runs: it uses its UDP path over loopback. The cell
#     records how many iox-roudi processes were running (roudi_procs=) so a run that met one says so.
#   - rmw_zenoh_cpp 0.2.9's shipped session config has shared_memory enabled: false ("disabled by default until fully
#     tested") and connects to tcp/localhost:7447: TCP through the router.
#   - Fast DDS 2.14.6 enables its SHM transport by default beside UDPv4; same-host samples are expected off the wire.
#
# HOW TO READ IT, written before running and enforced in rmw_samehost_summary.py (its docstring has every rule):
#   - VOID runs are listed, never averaged: identity, leftovers, missing RESULT/stamps/rows, a window with fewer than
#     MIN_MEASURED round trips or any unanswered one, a QoS echo that is not the cell's, rmw_tickle off its segment or
#     its two instruments disagreeing, the zenoh router not listening.
#   - Per cell and metric (RTT p50/p99/mean, CPU per round trip; delivered msg/s, CPU per delivered sample; peak RSS):
#     WIN when rmw_tickle's reps' range lies wholly on the better side of both scored vendors' ranges, LOSE when
#     either vendor's range lies wholly on the better side of rmw_tickle's, DRAW when a range overlaps (docs/TESTING.md
#     section 4's win rule; a 2 x SE test until 2026-10-08, which a vendor's scatter defeated); zenoh is printed and
#     never scored. RELIABLE tput: a vendor that loses samples after the
#     first delivered second is DELIVERY FAILED and excluded; rmw_tickle doing so LOSES every metric of that cell.
#   - Expectation, and what would falsify it: the native same-host rows have TickLE ahead of both DDS on every cell
#     (S1-S16). If rmw_tickle LOSES an RTT or delivered-rate cell to a vendor here, the cost is in the rmw layer, not
#     the transport, and that is the finding; if a vendor's witness reads shm where this header says kernel (or the
#     reverse), the header's reading of the defaults was wrong and is corrected from the run.
#
# Preflight: rmw on this PC is ROS lyrical, not jazzy, with no rmw_zenoh_cpp, and the PC's rmw_tickle and perf_test
# are whatever ~/tickle/install and ~/rmw_perf_ws/install hold, not $SHA. So the PC preflight (PREFLIGHT=1, before the
# lock) runs the cell script and the summary for rmw_tickle, Fast DDS and CycloneDDS in a private network namespace
# with a private /dev/shm - it tests the harness, not the build - and zenoh is printed as SKIP. The rig's own build is
# then checked by a dry run under the same lock (DRY=1): every arm, one rtt and one tput run of 2-3 s, read by the
# summary in --preflight mode; any VOID stops the job before the real run.
#
# Usage: SHA=<pushed commit> setsid nohup examples/perf_hil/experiments/rmw_samehost.sh > <log> 2>&1 < /dev/null &
#   REPS=3 ARMS="tickle fastdds cyclonedds zenoh" RTT_MSGS="bench array1k" TPUT_TOPICS="Array1k Array4k"
#   QOSES="best_effort reliable" WAITS="block poll" RTT_DUR=20 TPUT_DUR=20 HOST=10.1.1.213 PREFLIGHT=1 DRY=1
#   PREFLIGHT_ONLY=1 runs the PC preflight alone (no SHA, no lock, no rig).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
CELL="$HERE/rmw_samehost_cell.sh"
SUMMARY="$HERE/rmw_samehost_summary.py"
REPS=${REPS:-3}
ARMS=${ARMS:-"tickle fastdds cyclonedds zenoh"}
RTT_MSGS=${RTT_MSGS:-"bench array1k"}
TPUT_TOPICS=${TPUT_TOPICS:-"Array1k Array4k"}
QOSES=${QOSES:-"best_effort reliable"}
WAITS=${WAITS:-"block poll"}
POLL_ARGS=${POLL_ARGS:-"--poll-sleep-us 100 --phase jitter --rtt-at callback"}
RTT_DUR=${RTT_DUR:-20}
RTT_I=${RTT_I:-0.001}
TPUT_DUR=${TPUT_DUR:-20}
WARM_RT=${WARM_RT:-4096}
COOL_RT=${COOL_RT:-4096}
MIN_MEASURED=${MIN_MEASURED:-2000}
WARM_S=${WARM_S:-2}
COOL_S=${COOL_S:-2}
DOMAIN=${DOMAIN:-79}
HOST=${HOST:-10.1.1.213}
TICKLE_BCAST=${TICKLE_BCAST:-192.168.10.255} # arm tickle's link: the rig's eth0, as every rig rmw harness sets it
# The PC preflight's rmw stack: lyrical, this user's ~/tickle install and a perf_test whose TickLE typesupport matches
# that rmw_tickle (a perf_test generated by an older typesupport is refused by rmw_tickle at create_subscription).
# On 2026-10-06 ~/rmw_perf_ws's perf_test (typesupport of 2026-09-25) was refused and /tmp/keepall_evict/perf's
# (2026-10-05) was not, so that one is the default while it exists.
if [ -z "${PF_PERF_WS:-}" ]; then
    PF_PERF_WS=$HOME/rmw_perf_ws/install
    [ -x /tmp/keepall_evict/perf/install/performance_test/lib/performance_test/perf_test ] &&
        PF_PERF_WS=/tmp/keepall_evict/perf/install
fi
PF_OVERLAYS=${PF_OVERLAYS:-"$HOME/tickle/install/local_setup.bash $PF_PERF_WS/local_setup.bash"}
PF_PERF_TEST=${PF_PERF_TEST:-$PF_PERF_WS/performance_test/lib/performance_test/perf_test}
K=$HOME/.ssh/tickle_ci_ed25519
STAMP=$(date +%Y%m%d-%H%M%S)

# ------------------------------------------------------------------ PC preflight: the harness, in a private netns
preflight_pc() {
    local out ns arms="" a cells c rc
    out=${PREFLIGHT_OUT:-$HOME/rig_results_safe/preflight/rmw_samehost_$STAMP}
    mkdir -p "$out.runs" || return 1
    for a in $ARMS; do
        case "$a" in
        zenoh) [ -f /opt/ros/lyrical/lib/librmw_zenoh_cpp.so ] && arms="$arms zenoh" ||
            echo "PREFLIGHT SKIP zenoh: no rmw_zenoh_cpp in /opt/ros/lyrical on this PC (checked on the rig by the dry run)" ;;
        *) arms="$arms $a" ;;
        esac
    done
    local tickle_lib=$HOME/tickle/install/rmw_tickle/lib/librmw_tickle.so
    echo "warm_rt=256 cool_rt=256 min_measured=500 warm_s=1 cool_s=1 tickle_lib=$tickle_lib host=pc-netns" >"$out.runs/params.txt"
    ns=rmwsh_pf_$$
    sudo -n ip netns add "$ns" || { echo "PREFLIGHT FAIL: cannot create netns $ns"; return 1; }
    sudo -n ip -n "$ns" link set lo up multicast on
    sudo -n ip -n "$ns" route add default dev lo
    # One cell of each code path: rtt block and poll, both QoS, both message sizes; tput both QoS and both topics.
    cells="rtt:bench:reliable:block rtt:array1k:best_effort:poll tput:Array1k:reliable tput:Array4k:best_effort"
    for c in $cells; do
        IFS=: read -r kind msg qos wait <<<"$c"
        for a in $arms; do
            local dir="$out.runs/${kind}_${msg}_${qos}${wait:+_$wait}_${a}_r1" dur=3
            [ "$kind" = tput ] && dur=8
            # Root inside the namespace (sudo permits only ip): a private /dev/shm, then the cell, then the files
            # handed back to this user.
            sudo -n ip netns exec "$ns" env -i PATH="$PATH" HOME="$HOME" ARM="$a" KIND="$kind" MSG="$msg" QOS="$qos" \
                WAIT="${wait:-block}" DUR="$dur" RTT_I="$RTT_I" POLL_ARGS="$POLL_ARGS" CELL_DIR="$dir" DOMAIN="$DOMAIN" \
                ROS_SETUP=/opt/ros/lyrical/setup.bash \
                OVERLAYS="$PF_OVERLAYS" TICKLE_BCAST=127.255.255.255 \
                PINGPONG="$HOME/tickle/install/rmw_perf_pingpong/lib/rmw_perf_pingpong" \
                PERF_TEST="$PF_PERF_TEST" \
                ZENOHD=/opt/ros/lyrical/lib/rmw_zenoh_cpp/rmw_zenohd OWNER="$(id -u):$(id -g)" \
                bash -c 'mount -t tmpfs -o size=512m tmpfs /dev/shm && bash "$0"; rc=$?; chown -R "$OWNER" "$CELL_DIR"; exit $rc' "$CELL"
            echo "preflight $kind $msg $qos ${wait:-} $a: cell exit $?"
        done
    done
    sudo -n ip netns del "$ns"
    python3 "$SUMMARY" "$out.runs" --preflight | tee "$out.txt"
    rc=${PIPESTATUS[0]}
    echo "preflight output: $out.txt"
    return "$rc"
}

if [ "${PREFLIGHT_ONLY:-0}" = 1 ]; then
    preflight_pc
    exit $?
fi
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != 1 ]; then
    SHA=${SHA:?set SHA to the pushed commit to build and measure}
    if [ "${PREFLIGHT:-1}" != 0 ]; then
        preflight_pc || { echo "REFUSING TO TAKE THE RIG: the PC preflight failed (above). PREFLIGHT=0 overrides." >&2; exit 1; }
    fi
    exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi

# ------------------------------------------------------------------ under the lock
SHA=${SHA:?set SHA to the pushed commit to build and measure}
OUT=${OUT:-$HOME/rig_results_safe/rmw_samehost_${SHA:0:8}_$STAMP}
mkdir -p "$OUT.runs" "$OUT.dry" || exit 1
LOG="$OUT.txt"
say() { echo "$*" | tee -a "$LOG"; }
sh_() { ssh -i "$K" -o BatchMode=yes -o ConnectTimeout=10 "ci@$HOST" "$@"; }
REMOTE=/home/ci/rmw_samehost
cleanup() { # one EXIT trap: stop anything of ours still running on the Pi, by /proc/PID/exe
    # shellcheck disable=SC2016 # expanded on the Pi
    sh_ 'for f in /home/ci/rmw_samehost/run/*.pid; do [ -f "$f" ] || continue; p=$(cat "$f")
        case "$(readlink /proc/$p/exe 2>/dev/null)" in */ping_node|*/pong_node|*/perf_test|*/rmw_zenohd) kill -INT "$p";; esac; done' \
        </dev/null >/dev/null 2>&1 || true
}
trap cleanup EXIT
say "=== rmw same-host on $HOST, $(date -Is), SHA $SHA, $REPS reps, arms: $ARMS; rtt msgs: $RTT_MSGS, waits: $WAITS," \
    "poll: $POLL_ARGS, -d $RTT_DUR -i $RTT_I, window -$WARM_RT/-$COOL_RT rt (min $MIN_MEASURED); tput: $TPUT_TOPICS," \
    "$TPUT_DUR s, window -$WARM_S/-$COOL_S s; qos: $QOSES; domain $DOMAIN; out $OUT ==="

# ---- build at $SHA on the one Pi: rmw_tickle at the debs' level, then the interface packages and perf_test with
# TickLE typesupport, then the ping/pong (the order rmw_crosshost_rtt.sh and rmw_keepall_rig.sh established).
RMW_TICKLE_BUILD="-DCMAKE_BUILD_TYPE=None '-DCMAKE_C_FLAGS=-g -O2' '-DCMAKE_CXX_FLAGS=-g -O2'"
if [ "${SKIP_BUILD:-0}" = 1 ]; then
    at=$(sh_ "cd ~/tickle && git rev-parse HEAD" </dev/null)
    case "$at" in "$SHA"*) say "--- SKIP_BUILD: $HOST already at $at ---" ;; *) say "SKIP_BUILD refused: $HOST is at $at"; exit 1 ;; esac
else
    say "--- building at $SHA on $HOST ---"
    sh_ "set -e
cd ~/tickle && git fetch -q origin && git reset -q --hard $SHA && git clean -fdqx -e install -e build -e log
export PYTHONPATH=\$HOME/tickle/tools/typesupport\${PYTHONPATH:+:\$PYTHONPATH}
set +u; source /opt/ros/jazzy/setup.bash; set -u
colcon build --packages-select rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp rmw_tickle \
  --cmake-args -DBUILD_SHARED_LIBS=ON $RMW_TICKLE_BUILD > /tmp/rmwsh_build1.log 2>&1 \
  || { echo \"STAGE 1 FAILED\"; tail -25 /tmp/rmwsh_build1.log; exit 1; }
set +u; source \$HOME/tickle/install/local_setup.bash; set -u
colcon build --base-paths \$HOME/rmw_perf_ws --build-base \$HOME/rmw_perf_ws/build --install-base \$HOME/rmw_perf_ws/install \
  --packages-select builtin_interfaces rcl_interfaces performance_test --cmake-args -DCMAKE_BUILD_TYPE=Release \
  --cmake-force-configure > /tmp/rmwsh_build2.log 2>&1 \
  || { echo \"STAGE 2 FAILED\"; tail -25 /tmp/rmwsh_build2.log; exit 1; }
set +u; source \$HOME/rmw_perf_ws/install/local_setup.bash; source \$HOME/tickle/install/local_setup.bash; set -u
colcon build --packages-select rmw_perf_pingpong --cmake-args -DCMAKE_BUILD_TYPE=Release > /tmp/rmwsh_build3.log 2>&1 \
  || { echo \"STAGE 3 FAILED\"; tail -25 /tmp/rmwsh_build3.log; exit 1; }
test -x \$HOME/tickle/install/rmw_perf_pingpong/lib/rmw_perf_pingpong/ping_node
test -x \$HOME/rmw_perf_ws/install/performance_test/lib/performance_test/perf_test
echo \"built on \$(hostname) at \$(git rev-parse --short HEAD)\"
sha256sum \$HOME/tickle/install/rmw_tickle/lib/librmw_tickle.so" </dev/null 2>&1 | tee -a "$LOG"
    [ "${PIPESTATUS[0]}" = 0 ] || { say "BUILD FAILED - not running"; exit 1; }
fi
# The application binaries must not link an rmw implementation: RMW_IMPLEMENTATION alone selects it.
links=$(sh_ "cd \$HOME/tickle/install/rmw_perf_pingpong/lib/rmw_perf_pingpong && for b in ping_node pong_node \$HOME/rmw_perf_ws/install/performance_test/lib/performance_test/perf_test; do ldd \$b | grep -oE 'librmw_(tickle|fastrtps_cpp|cyclonedds_cpp|zenoh_cpp)\.so' | sed \"s|^|\$(basename \$b)-links-|\"; done" </dev/null)
[ -z "$links" ] || { say "an application binary links an rmw implementation directly ($links) - not running"; exit 1; }
sh_ "mkdir -p $REMOTE && cat > $REMOTE/cell.sh && chmod +x $REMOTE/cell.sh" <"$CELL" || { say "cell copy failed"; exit 1; }
want=$(sha256sum <"$CELL" | cut -c1-64)
got=$(sh_ "sha256sum < $REMOTE/cell.sh" </dev/null | cut -c1-64)
[ "$want" = "$got" ] || { say "cell script on $HOST hashes $got, not $want"; exit 1; }
say "cell script sha256 $want on $HOST; governor $(sh_ 'cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor' </dev/null)"

TICKLE_LIB=/home/ci/tickle/install/rmw_tickle/lib/librmw_tickle.so
run_one() { # run_one <runs dir> <kind> <msg> <qos> <wait|-> <arm> <rep> <dur> <warm_rt> ...
    local runs=$1 kind=$2 msg=$3 qos=$4 wait=$5 arm=$6 rep=$7 dur=$8 stem
    stem="${kind}_${msg}_${qos}$([ "$wait" != - ] && echo "_$wait")_${arm}_r${rep}"
    sh_ "env ARM=$arm KIND=$kind MSG=$msg QOS=$qos WAIT=${wait/-/block} DUR=$dur RTT_I=$RTT_I POLL_ARGS='$POLL_ARGS' \
CELL_DIR=$REMOTE/run DOMAIN=$DOMAIN ROS_SETUP=/opt/ros/jazzy/setup.bash \
OVERLAYS='/home/ci/tickle/install/local_setup.bash /home/ci/rmw_perf_ws/install/local_setup.bash' \
PINGPONG=/home/ci/tickle/install/rmw_perf_pingpong/lib/rmw_perf_pingpong \
PERF_TEST=/home/ci/rmw_perf_ws/install/performance_test/lib/performance_test/perf_test \
TICKLE_BCAST=$TICKLE_BCAST ZENOHD=/opt/ros/jazzy/lib/rmw_zenoh_cpp/rmw_zenohd bash $REMOTE/cell.sh" </dev/null >/dev/null 2>&1
    mkdir -p "$runs/$stem"
    sh_ "tar -C $REMOTE/run -cf - ." </dev/null | tar -C "$runs/$stem" -xf - ||
        say "  (could not fetch $stem)"
    say "  $(date +%T) $stem $(grep -c . "$runs/$stem/meta.txt" 2>/dev/null) meta lines$(grep -q '^done=1' "$runs/$stem/meta.txt" 2>/dev/null || echo ' - UNFINISHED')"
}
cells_list() { # every cell, one per line: kind msg qos wait
    local m q w
    for q in $QOSES; do
        for m in $RTT_MSGS; do for w in $WAITS; do echo "rtt $m $q $w"; done; done
        for m in $TPUT_TOPICS; do echo "tput $m $q -"; done
    done
}

# ---- dry run, same lock: every arm, one short rtt and one short tput; any VOID stops the job here
if [ "${DRY:-1}" != 0 ]; then
    say "--- dry run: every arm, rtt bench reliable block 3 s and tput Array1k reliable 8 s ---"
    echo "warm_rt=256 cool_rt=256 min_measured=500 warm_s=1 cool_s=1 tickle_lib=$TICKLE_LIB host=$HOST" >"$OUT.dry/params.txt"
    for arm in $ARMS; do
        run_one "$OUT.dry" rtt bench reliable block "$arm" 1 3
        run_one "$OUT.dry" tput Array1k reliable - "$arm" 1 8
    done
    # rmw_tickle as shipped, rtt only (3 s of one ping in flight): does the Pi agree with the PC preflight that it
    # never builds its segment? Not in the real run by default - see the header.
    case " $ARMS " in *" tickle_shipped "*) ;; *) run_one "$OUT.dry" rtt bench reliable block tickle_shipped 1 3 ;; esac
    python3 "$SUMMARY" "$OUT.dry" --preflight | tee -a "$LOG"
    [ "${PIPESTATUS[0]}" = 0 ] || { say "DRY RUN FAILED - the real run was not started"; exit 1; }
fi

# ---- the run: every rep runs every cell; within a cell the arms' order rotates by rep and by cell
echo "warm_rt=$WARM_RT cool_rt=$COOL_RT min_measured=$MIN_MEASURED warm_s=$WARM_S cool_s=$COOL_S tickle_lib=$TICKLE_LIB host=$HOST" >"$OUT.runs/params.txt"
read -r -a arms <<<"$ARMS"
n=${#arms[@]}
for rep in $(seq 1 "$REPS"); do
    say "--- rep $rep/$REPS $(date -Is) ---"
    ci=0
    while read -r kind msg qos wait; do
        s=$(((rep - 1 + ci) % n))
        for i in $(seq 0 $((n - 1))); do
            arm=${arms[$(((i + s) % n))]}
            dur=$RTT_DUR
            [ "$kind" = tput ] && dur=$TPUT_DUR
            run_one "$OUT.runs" "$kind" "$msg" "$qos" "$wait" "$arm" "$rep" "$dur"
        done
        ci=$((ci + 1))
    done < <(cells_list)
done
say "=== runs done $(date -Is) ==="
python3 "$SUMMARY" "$OUT.runs" | tee -a "$LOG"
