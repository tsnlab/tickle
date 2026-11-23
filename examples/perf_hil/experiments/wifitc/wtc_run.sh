#!/usr/bin/env bash
# shellcheck disable=SC1091,SC2029,SC2016,SC2086,SC2129,SC2012,SC2024,SC2034,SC2209  # experiment driver: ssh-side expansion, unquoted word lists and sudo redirects are deliberate
# Wi-Fi-like link emulated with tc netem: cross-framework comparison runner (PLAN.md; cells, arms and rules as the earlier Pi comparison,
# the link is tc netem in two namespaces joined by a GRE tap over the rig's Ethernet). Runs on the PC, drives both Pis over ssh.
# One cell / profile / round / framework / copy per run: fresh namespaces, the far side (echo / sub) in wtc_srv on rpi-0, the near side
# (ping / pub) in wtc_sta on rpi-1, raw output under $OUT/<cell>/<profile>/r<round>_<fw>_<copy>.{topo,meta,near,far,lat.*,done}.
#   CELLS="be_small rel_small" | "rel_large" ARMS="M1 M2 S1 S2" ROUNDS=2 FWS="tickle tickle-pace cyclonedds fastdds" COPIES="A A2"
#   PACE_M1=22 PACE_S1=3 ...  (Mbit/s of the "tickle-pace" arm per profile, from the calibration and the DESIGN.md guideline)
#   OUT=$HOME/rig_results_safe/wtc_<set>  LARGE_SZ=131072  DUR_SMALL=30 DUR_LARGE=60  FIRST_ROUND=1
#   MODE=cal runs the calibration of every profile instead (PLAN.md section 4).
#   ADAPTIVE_PLAN.md: FWS may name "tickle-lg" (the small-sample node as a large-samples build: the control of "tickle-ad" in the small cells), "tickle-ad" (tt_LinkOptions.rate_control = ADAPTIVE; TT_RATE_CONTROL=adaptive) or
#   "tickle-ad-c<Mbit/s>" (the same, capped), RATE_STEP="<second>:<Mbit/s> ..." steps the link's rate that many seconds
#   after the near side starts (both taps, other netem settings kept), READERS=<n> runs n reader processes of a rel_large
#   cell in the far namespace (TT_CONTEXT_ID 4, 5, ...; their outputs in <run>.far2, .far3).
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$S/../../../.." && pwd)
if [ -z "${WTC_LOCKED:-}" ]; then
    WTC_LOCKED=1 RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-600} exec flock /tmp/rig.lock "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi
SET=${SET:-wtc}
OUT=${OUT:-$HOME/rig_results_safe/wtc_$(date +%Y%m%d)}
mkdir -p "$OUT"
# shellcheck source=wtc_lib.sh
. "$S/wtc_lib.sh"
CELLS=${CELLS:-"be_small rel_small rel_large"}
ARMS=${ARMS:-"M1 M2 S1 S2"}
ROUNDS=${ROUNDS:-2}
FIRST_ROUND=${FIRST_ROUND:-1}
FWS=${FWS:-"tickle tickle-pace cyclonedds fastdds"}
COPIES=${COPIES:-"A A2"}
DUR_SMALL=${DUR_SMALL:-30}
DUR_LARGE=${DUR_LARGE:-60}
DUR=$DUR_SMALL
LARGE_SZ=${LARGE_SZ:-131072}
LARGE_HZ=${LARGE_HZ:-0.25}
ROTATE=${ROTATE:-3}
IP_NEAR=10.90.0.2
IP_FAR=10.90.0.3
BCAST=10.90.0.255
IF_NEAR=wtcgc
IF_FAR=wtcgs
NS_NEAR=wtc_sta
NS_FAR=wtc_srv
PB='$HOME/'$BIN # remote path of the binaries, expanded on the Pi
ROSLIB=/opt/ros/jazzy/lib:/opt/ros/jazzy/lib/aarch64-linux-gnu # libddsc's RUNPATH does not reach libiceoryx: the ROS install is found by LD_LIBRARY_PATH
RD='$HOME/wtc_run/fw' # remote scratch directory

fdds_xml() { # remote file contents of the Fast DDS 2.14 profile (UDPv4 only, maxMessageSize 1472; no interfaceWhiteList, as the PC plan's
    # amendment: each namespace holds only its tap/wlan and lo). Root and layout of examples/perf_hil/fastdds/fastdds_eth0_only_mms1472.xml
    cat <<XML
<?xml version="1.0" encoding="UTF-8"?>
<profiles xmlns="http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles">
  <transport_descriptors>
    <transport_descriptor>
      <transport_id>t1_udp</transport_id>
      <type>UDPv4</type>
      <maxMessageSize>${FDDS_MMS:-1472}</maxMessageSize>
    </transport_descriptor>
  </transport_descriptors>
  <participant profile_name="t1_udp_participant" is_default_profile="true">
    <rtps>
      <userTransports>
        <transport_id>t1_udp</transport_id>
      </userTransports>
      <useBuiltinTransports>false</useBuiltinTransports>
    </rtps>
  </participant>
</profiles>
XML
}

# A TickLE variant of the pacing sweep (PLAN.md): "tickle" is the library default (as the framework comparison ran it);
# "tickle-r<Mbit/s>[-b<bytes>][-f<ip|mtu>]" sets the link's pacing_rate, pacing_burst and large_fragmentation through the bench's
# TT_PACING_BPS / TT_PACING_BURST environment and -F; "-roff" names an explicit unpaced arm (same as "tickle").
# variant_env FW -> TV_ENV (env assignments) and TV_ARGS (extra arguments)
variant_env() {
    local part
    TV_ENV=""
    TV_ARGS=""
    TV_BIN="" # "lg": the small-sample node built with large samples (node_d100lg), which an ADAPTIVE link needs
    for part in $(echo "${1#tickle}" | tr '-' ' '); do
        case $part in
        roff) ;;
        r[0-9]*) TV_ENV="$TV_ENV TT_PACING_BPS=$((${part#r} * 1000000))" ;;
        p[0-9]*) TV_ENV="$TV_ENV TT_PACING_BPS=$((${CARRIED_KBIT:?CARRIED_KBIT unset} * ${part#p} * 10))" ;; # percent of the profile's carried rate (PLAN.md section 10)
        ad) TV_ENV="$TV_ENV TT_RATE_CONTROL=adaptive"; TV_BIN=lg ;; # rate_control ADAPTIVE (ADAPTIVE_PLAN.md)
        lg) TV_BIN=lg ;;                                              # the large-samples small-sample node, no option: ad's control
        c[0-9]*) TV_ENV="$TV_ENV TT_PACING_BPS=$((${part#c} * 1000000))" ;; # with ad: the cap, Mbit/s
        b[0-9]*) TV_ENV="$TV_ENV TT_PACING_BURST=${part#b}" ;;
        fip | fmtu) TV_ARGS="$TV_ARGS -F ${part#f}" ;;
        *) echo "unknown variant part $part in $1" >&2; return 1 ;;
        esac
    done
}

# cmd_for <fw> <cell> <side: far|near> <ip> <iface> <id> -> command line for that side (paths on that Pi)
cmd_for() {
    local fw=$1 cell=$2 side=$3 ip=$4 wl=$5 id=$6 mode role args stop_arg="" stop lat
    [ "$fw" = tickle-pace ] && fw=tickle-r${PACE_MBIT:?PACE_MBIT unset for this profile}
    case $fw in tickle-*) variant_env "$fw" || return 1; fw=tickle ;; *) TV_ENV=""; TV_ARGS=""; TV_BIN="" ;; esac
    stop=$RD/$id.stop
    lat=$RD/$id.lat
    case $cell in be_small) mode=be ;; *) mode=rel ;; esac
    if [ "$cell" = rel_large ]; then
        [ "$side" = far ] && role=sub || role=pub
    else
        [ "$side" = far ] && role=echo || role=ping
    fi
    [ "$side" = far ] && stop_arg="-q $stop"
    case $fw in
    tickle)
        if [ "$cell" = rel_large ]; then
            if [ $role = pub ]; then
                echo "env TT_UDP_OFFLOAD=0$TV_ENV $PB/large pub -s $LARGE_SZ -r $LARGE_HZ -d $DUR -R -b $BCAST -P wifi$TV_ARGS"
            else
                echo "env TT_UDP_OFFLOAD=0$TV_ENV $PB/large sub -s $LARGE_SZ -d $((DUR + 8)) -R -b $BCAST -P wifi$TV_ARGS"
            fi
        else
            local ctx=1 d=$DUR
            [ $role = echo ] && ctx=2 && d=$((DUR + 40))
            echo "env$TV_ENV $PB/node_d100$TV_BIN $role -c $ctx -b $BCAST -d $d -m $mode -s 100 -r 100 -p wifi $stop_arg -l $lat.$role"
        fi
        ;;
    cyclonedds | fastdds | zenohpico)
        if [ "$cell" = rel_large ]; then
            args="$role -m rel -s $LARGE_SZ -r $LARGE_HZ -K 10 -B 10 -Q 3"
            [ $role = pub ] && args="$args -d $DUR" || args="$args -d $((DUR + 8))"
        else
            args="$role -m $mode -s 100 -r 100 -K 100 -B 1"
            [ $role = ping ] && args="$args -d $DUR" || args="$args -d $((DUR + 40))"
        fi
        args="$args $stop_arg -l $lat.$role -i $ip -I $wl"
        case $fw in
        cyclonedds) echo "env LD_LIBRARY_PATH=$ROSLIB CYCLONEDDS_URI=file://$RD/$id.cyclone.xml $PB/cyclonedds_node $args" ;;
        fastdds) echo "env LD_LIBRARY_PATH=$ROSLIB FASTDDS_DEFAULT_PROFILES_FILE=$RD/$id.fastdds.xml FASTRTPS_DEFAULT_PROFILES_FILE=$RD/$id.fastdds.xml $PB/fastdds_node $args" ;;
        zenohpico)
            local zenv=""
            if [ "$mode" = rel ]; then
                [ "$side" = far ] && zenv="T1_ZENOH_LISTEN=tcp/$IP_FAR:7447" || zenv="T1_ZENOH_CONNECT=tcp/$IP_FAR:7447"
            fi
            echo "env $zenv $PB/zenohpico_node $args"
            ;;
        esac
        ;;
    esac
}

kill_ns_procs() { # every process of ours still in the namespaces
    srv "for p in \$(sudo -n ip netns pids $NS_FAR 2>/dev/null); do sudo -n kill -KILL \$p 2>/dev/null; done"
    cli "for p in \$(sudo -n ip netns pids $NS_NEAR 2>/dev/null); do sudo -n kill -KILL \$p 2>/dev/null; done"
}

clock_offset() { # t1pi_clock.py is shipped with the t1pi scripts (both Pis; removed from the tree, last at cb9c8606 under experiments/t1pi/)
    # run: measures CLOCK_MONOTONIC(rpi-0) - CLOCK_MONOTONIC(rpi-1) over the wired link, microseconds, into $1
    srv "setsid nohup python3 \$HOME/t1pi_scripts/t1pi_clock.py serve $SRV_ETH 47911 >/dev/null 2>&1 </dev/null &" || true
    sleep 1
    cli "python3 \$HOME/t1pi_scripts/t1pi_clock.py probe $SRV_ETH 47911" >"$1" 2>&1
}

# rate_step_driver <run dir/run>: at each "<second>:<Mbit/s>" of RATE_STEP (seconds after it was started) the link's rate
# becomes that, on both taps, the profile's other netem settings kept. Logged to <run>.steps.
rate_step_driver() {
    local base=$1 t0 item at mbit now n
    t0=$(date +%s.%N)
    for item in $RATE_STEP; do
        at=${item%%:*}
        mbit=${item##*:}
        now=$(date +%s.%N)
        n=$(awk -v a="$at" -v t0="$t0" -v now="$now" 'BEGIN { d = a - (now - t0); print (d > 0 ? d : 0) }')
        sleep "$n"
        # shellcheck disable=SC2001
        NETEM=$(echo "$NETEM_BASE" | sed "s/rate [0-9a-z]*/rate ${mbit}mbit/")
        srv "NETEM='$NETEM' $T srv setnetem" >>"$base.steps" 2>&1
        cli "NETEM='$NETEM' $T cli setnetem" >>"$base.steps" 2>&1
        echo "$(date -Is) step at=${at}s rate=${mbit}mbit" >>"$base.steps"
    done
}

run_one() { # run_one <cell> <arm> <round> <fw> <copy> <idx>
    local cell=$1 arm=$2 round=$3 fw=$4 copy=$5 idx=$6
    local dir=$OUT/$cell/$arm run=r${round}_${fw}_${copy} id i
    case $cell in rel_large) DUR=$DUR_LARGE ;; *) DUR=$DUR_SMALL ;; esac
    case $arm in M*) CARRIED_KBIT=29730 ;; S*) CARRIED_KBIT=3970 ;; esac # carried rates of the calibration, kbit/s
    eval "PACE_MBIT=\${PACE_$arm:-}"
    mkdir -p "$dir"
    [ -f "$dir/$run.done" ] && { echo "SKIP $cell/$arm/$run (done)"; return 0; }
    id=${cell}_${arm}_$run
    local attempt
    for attempt in 1 2 3; do
        bring_up "$dir/$run" "$arm" || { log "HARNESS_FAULT $cell/$arm/$run attempt $attempt: no topology"; continue; }
        srv "rm -rf $RD; mkdir -p $RD"
        cli "rm -rf $RD; mkdir -p $RD"
        # lo up in both namespaces (every framework, same setup: Fast DDS sets IP_MULTICAST_IF to 127.0.0.1)
        srv "sudo -n ip -n $NS_FAR link set lo up"
        cli "sudo -n ip -n $NS_NEAR link set lo up"
        [ "$fw" = fastdds ] && { fdds_xml | srv_in "cat > $RD/$id.fastdds.xml"; fdds_xml | cli_in "cat > $RD/$id.fastdds.xml"; }
        if [ "$fw" = cyclonedds ]; then
            cyc_xml() { printf '<CycloneDDS><Domain><General><Interfaces><NetworkInterface address="%s"/></Interfaces></General><Discovery><SPDPInterval>1s</SPDPInterval></Discovery></Domain></CycloneDDS>\n' "$1"; }
            cyc_xml $IP_FAR | srv_in "cat > $RD/$id.cyclone.xml"
            cyc_xml $IP_NEAR | cli_in "cat > $RD/$id.cyclone.xml"
        fi
        local far near
        far=$(cmd_for "$fw" "$cell" far $IP_FAR $IF_FAR "$id")
        near=$(cmd_for "$fw" "$cell" near $IP_NEAR $IF_NEAR "$id")
        [ "$cell" = rel_large ] && clock_offset "$dir/$run.clockoff"
        printf 'cell=%s arm=%s round=%s fw=%s copy=%s idx=%s attempt=%s start=%s\nfar: %s\nnear: %s\nlink: %s pace_mbit=%s topology=wtc\ncli %s\nsrv %s\n' \
            "$cell" "$arm" "$round" "$fw" "$copy" "$idx" "$attempt" "$(date -Is)" "$far" "$near" "$NETEM" "${PACE_MBIT:-}" \
            "$(cpu_state cli)" "$(cpu_state srv)" >>"$dir/$run.meta"
        local fpid
        if [ "${PCAP:-0}" = 1 ]; then # diagnosis runs (PACE_PLAN.md section 4): headers of every frame on the STA radio and on the server's tap
            cli "setsid nohup sudo -n ip netns exec $NS_NEAR timeout -k 5 $((DUR + 80)) tcpdump -i $IF_NEAR -s 96 -B 8192 -w $RD/$id.cli.pcap >/dev/null 2>&1 </dev/null &"
            srv "setsid nohup sudo -n ip netns exec $NS_FAR timeout -k 5 $((DUR + 80)) tcpdump -i $IF_FAR -s 96 -B 8192 -w $RD/$id.srv.pcap >/dev/null 2>&1 </dev/null &"
            sleep 1
        fi
        fpid=$(srv "setsid nohup sudo -n ip netns exec $NS_FAR timeout -k 5 $((DUR + 60)) $far >$RD/$id.far 2>&1 </dev/null & echo \$!")
        local extra
        for ((extra = 2; extra <= ${READERS:-1}; extra++)); do # more readers on the same link (rel_large): their own context ids
            srv "setsid nohup sudo -n ip netns exec $NS_FAR timeout -k 5 $((DUR + 60)) ${far/env /env TT_CONTEXT_ID=$((extra + 2)) } >$RD/$id.far$extra 2>&1 </dev/null &"
        done
        sleep 1
        local steppid=""
        if [ -n "${RATE_STEP:-}" ]; then
            NETEM_BASE=$NETEM rate_step_driver "$dir/$run" &
            steppid=$!
        fi
        cli "sudo -n ip netns exec $NS_NEAR timeout -k 5 $((DUR + 80)) $near" >"$dir/$run.near" 2>&1
        [ -n "$steppid" ] && kill "$steppid" 2>/dev/null
        srv "touch $RD/$id.stop"
        for i in $(seq 1 40); do srv "[ -d /proc/$fpid ]" || break; sleep 0.5; done
        if [ "${PCAP:-0}" = 1 ]; then
            for h in cli srv; do
                [ $h = cli ] && nsn=$NS_NEAR || nsn=$NS_FAR
                $h "for p in \$(sudo -n ip netns pids $nsn 2>/dev/null); do [ \"\$(sudo -n cat /proc/\$p/comm 2>/dev/null)\" = tcpdump ] && sudo -n kill -INT \$p; done; sleep 2"
            done
            cli "cat $RD/$id.cli.pcap" >"$dir/$run.cli.pcap" 2>/dev/null
            srv "cat $RD/$id.srv.pcap" >"$dir/$run.srv.pcap" 2>/dev/null
        fi
        kill_ns_procs
        srv "cat $RD/$id.far" >"$dir/$run.far" 2>&1
        for ((extra = 2; extra <= ${READERS:-1}; extra++)); do srv "cat $RD/$id.far$extra" >"$dir/$run.far$extra" 2>&1; done
        for role in echo sub; do srv "cat $RD/$id.lat.$role 2>/dev/null" >"$dir/$run.lat.$role" 2>/dev/null; done
        for role in ping pub; do cli "cat $RD/$id.lat.$role 2>/dev/null" >"$dir/$run.lat.$role" 2>/dev/null; done
        srv "rm -rf $RD"
        cli "rm -rf $RD"
        { echo "end=$(date -Is)"; echo "cli $(cpu_state cli)"; echo "srv $(cpu_state srv)"; } >>"$dir/$run.meta"
        [ "$cell" = rel_large ] && clock_offset "$dir/$run.clockoff2"
        down_both
        if grep -q '^RESULT' "$dir/$run.near" "$dir/$run.far" 2>/dev/null; then
            local sc
            sc=$(grep -h '^RESULT' "$dir/$run.near" | grep -o 'scored=[0-9]*\|published=[0-9]*\|matched=[0-9]\|p50_us=[0-9.]*\|p99_us=[0-9.]*\|delivered=[0-9]*\|error=[a-z_]*' | tr '\n' ' ')
            log "$cell/$arm/$run near: $sc"
            date -Is >"$dir/$run.done"
            check_state "$cell/$arm/$run" || { log "ABORT: wlan0/eth0/modules state changed"; return 2; }
            return 0
        fi
        log "HARNESS_FAULT $cell/$arm/$run attempt $attempt: no RESULT line"
    done
    log "RUN_LOST $cell/$arm/$run (3 attempts, no RESULT)"
    echo lost >"$dir/$run.done"
    check_state "$cell/$arm/$run" || return 2
    return 1
}

order_for_round() {
    local round=$1 items=() fw c n i off
    for c in $COPIES; do for fw in $FWS; do items+=("$fw.$c"); done; done
    n=${#items[@]}
    off=$(((round - 1) * ROTATE % n))
    for ((i = 0; i < n; i++)); do echo "${items[$(((i + off) % n))]}"; done
}


# Calibration (MODE=cal, PLAN.md section 4): what each profile carries, measured with plain UDP between the namespaces, before any framework runs.
calibrate() {
    local prof f
    for prof in ${ARMS}; do
        bring_up "$OUT/cal_$prof" "$prof" || continue
        srv "sudo -n ip netns exec $NS_FAR true" || true
        cli "sudo -n ip netns exec $NS_NEAR ping -c 20 -i 0.2 -W 2 $IP_FAR | tail -3" >"$OUT/cal_$prof.ping" 2>&1
        srv "setsid nohup sudo -n ip netns exec $NS_FAR python3 \$HOME/wtc_scripts/wtc_probe.py serve $IP_FAR 47921 34 >\$HOME/wtc_run/probe.out 2>&1 </dev/null &"
        sleep 2
        for f in "flood $IP_FAR 47921 1472 5" "burst $IP_FAR 47921 1472 91 3" "burst $IP_FAR 47921 1472 256 3" "rate $IP_FAR 47921 1472 40 8"; do
            cli "sudo -n ip netns exec $NS_NEAR python3 \$HOME/wtc_scripts/wtc_probe.py $f" >>"$OUT/cal_$prof.client" 2>&1
            sleep 1
        done
        sleep 10
        srv "cat \$HOME/wtc_run/probe.out" >"$OUT/cal_$prof.server" 2>&1
        srv "rm -f \$HOME/wtc_run/probe.out"
        kill_ns_procs
        down_both
        log "CAL $prof netem='$NETEM': $(tr -d '\n' <"$OUT/cal_$prof.server" | cut -c1-300)"
        check_state "cal_$prof" || { log "ABORT: state changed"; return 2; }
    done
}

trap session_end EXIT
session_start
if [ "${MODE:-}" = cal ]; then calibrate; exit $?; fi
log "WTC_START cells='$CELLS' arms='$ARMS' rounds=$ROUNDS fws='$FWS' dur=$DUR large=${LARGE_SZ}B@${LARGE_HZ}Hz dur_small=$DUR_SMALL dur_large=$DUR_LARGE"
for cell in $CELLS; do
    for arm in $ARMS; do
        for round in $(seq "$FIRST_ROUND" "$ROUNDS"); do
            idx=0
            for it in $(order_for_round "$round"); do
                idx=$((idx + 1))
                run_one "$cell" "$arm" "$round" "${it%%.*}" "${it#*.}" "$idx"
                [ $? = 2 ] && exit 2
            done
        done
    done
done
log "WTC_END"
