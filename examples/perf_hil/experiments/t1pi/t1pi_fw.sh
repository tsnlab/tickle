#!/usr/bin/env bash
# T1-Pi cross-framework comparison runner (PLAN.md section 6; cells, arms, rules from the PC's T1_FRAMEWORK_CMP_PLAN.md ported
# unchanged to the Pi topology). Runs on the PC, drives both Pis over ssh. SIMULATION ONLY (mac80211_hwsim + wmediumd).
# One cell / arm / round / framework / copy per run: fresh topology, the far side (echo / sub) in t1p_srv on rpi-0, the near side
# (ping / pub) in t1p_sta on rpi-1, raw output under $OUT/<cell>/<arm>/r<round>_<fw>_<copy>.{topo,meta,near,far,lat.*,done}.
#   CELLS="be_small rel_small rel_large" ARMS="ctl o60 o200" ROUNDS=3 FWS="tickle cyclonedds fastdds zenohpico" COPIES="A A2"
#   OUT=$HOME/rig_results_safe/t1pi_fwcmp_20261114  LARGE_SZ=131072   (SET=fwcmp; PILOT=1 for unscored pilot runs under $OUT/pilot_*)
# Order inside a (cell, arm) round: the list [fw.A for each fw, fw.A2 for each fw] rotated left by ROTATE*(round-1).
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$S/../../../.." && pwd)
if [ -z "${T1PI_LOCKED:-}" ]; then
    T1PI_LOCKED=1 RIG_LOCK_WAIT=${RIG_LOCK_WAIT:-600} exec flock /tmp/rig.lock "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"
fi
SET=${SET:-fwcmp}
OUT=${OUT:-$HOME/rig_results_safe/t1pi_fwcmp_$(date +%Y%m%d)}
mkdir -p "$OUT"
# shellcheck source=t1pi_lib.sh
. "$S/t1pi_lib.sh"
CELLS=${CELLS:-"be_small rel_small rel_large"}
ARMS=${ARMS:-"ctl o60 o200"}
ROUNDS=${ROUNDS:-3}
FWS=${FWS:-"tickle cyclonedds fastdds zenohpico"}
COPIES=${COPIES:-"A A2"}
DUR=${DUR:-60}
LARGE_SZ=${LARGE_SZ:-131072}
LARGE_HZ=${LARGE_HZ:-0.25}
ROTATE=${ROTATE:-3}
IP_NEAR=10.90.0.2
IP_FAR=10.90.0.3
BCAST=10.90.0.255
IF_NEAR=hwsta
IF_FAR=t1pgs
NS_NEAR=t1p_sta
NS_FAR=t1p_srv
PB='$HOME/'$BIN # remote path of the binaries, expanded on the Pi
ROSLIB=/opt/ros/jazzy/lib:/opt/ros/jazzy/lib/aarch64-linux-gnu # libddsc's RUNPATH does not reach libiceoryx: the ROS install is found by LD_LIBRARY_PATH
RD='$HOME/t1pi_run/fw' # remote scratch directory

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

# cmd_for <fw> <cell> <side: far|near> <ip> <iface> <id> -> command line for that side (paths on that Pi)
cmd_for() {
    local fw=$1 cell=$2 side=$3 ip=$4 wl=$5 id=$6 mode role args stop_arg="" stop lat
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
                echo "env TT_UDP_OFFLOAD=0 $PB/large pub -s $LARGE_SZ -r $LARGE_HZ -d $DUR -R -b $BCAST -P wifi"
            else
                echo "env TT_UDP_OFFLOAD=0 $PB/large sub -s $LARGE_SZ -d $((DUR + 8)) -R -b $BCAST -P wifi"
            fi
        else
            local ctx=1 d=$DUR
            [ $role = echo ] && ctx=2 && d=$((DUR + 40))
            echo "$PB/node_d100 $role -c $ctx -b $BCAST -d $d -m $mode -s 100 -r 100 -p wifi $stop_arg -l $lat.$role"
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

kill_ns_procs() { # every process of ours still in the namespaces (never the wpa_supplicants / wmediumd)
    srv "for p in \$(sudo -n ip netns pids $NS_FAR 2>/dev/null); do sudo -n kill -KILL \$p 2>/dev/null; done"
    cli "for p in \$(sudo -n ip netns pids $NS_NEAR 2>/dev/null); do case \$(sudo -n readlink /proc/\$p/exe 2>/dev/null) in */wpa_supplicant) ;; *) sudo -n kill -KILL \$p 2>/dev/null ;; esac; done"
}

clock_offset() { # run: measures CLOCK_MONOTONIC(rpi-0) - CLOCK_MONOTONIC(rpi-1) over the wired link, microseconds, into $1
    srv "setsid nohup python3 \$HOME/t1pi_scripts/t1pi_clock.py serve $SRV_ETH 47911 >/dev/null 2>&1 </dev/null &" || true
    sleep 1
    cli "python3 \$HOME/t1pi_scripts/t1pi_clock.py probe $SRV_ETH 47911" >"$1" 2>&1
}

run_one() { # run_one <cell> <arm> <round> <fw> <copy> <idx>
    local cell=$1 arm=$2 round=$3 fw=$4 copy=$5 idx=$6
    local dir=$OUT/$cell/$arm run=r${round}_${fw}_${copy} id i
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
        printf 'cell=%s arm=%s round=%s fw=%s copy=%s idx=%s attempt=%s start=%s\nfar: %s\nnear: %s\nprofile: %s retry=1 seed=%s topology=t1pi\ncli %s\nsrv %s\n' \
            "$cell" "$arm" "$round" "$fw" "$copy" "$idx" "$attempt" "$(date -Is)" "$far" "$near" "$ENVS ge=$GE_ARG" "${SEED:-7}" \
            "$(cpu_state cli)" "$(cpu_state srv)" >>"$dir/$run.meta"
        local fpid
        fpid=$(srv "setsid nohup sudo -n ip netns exec $NS_FAR timeout -k 5 $((DUR + 60)) $far >$RD/$id.far 2>&1 </dev/null & echo \$!")
        sleep 1
        cli "sudo -n ip netns exec $NS_NEAR timeout -k 5 $((DUR + 80)) $near" >"$dir/$run.near" 2>&1
        srv "touch $RD/$id.stop"
        for i in $(seq 1 40); do srv "[ -d /proc/$fpid ]" || break; sleep 0.5; done
        kill_ns_procs
        srv "cat $RD/$id.far" >"$dir/$run.far" 2>&1
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

trap session_end EXIT
session_start
log "T1PIFW_START cells='$CELLS' arms='$ARMS' rounds=$ROUNDS fws='$FWS' dur=$DUR large=${LARGE_SZ}B@${LARGE_HZ}Hz"
for cell in $CELLS; do
    for arm in $ARMS; do
        for round in $(seq 1 "$ROUNDS"); do
            idx=0
            for it in $(order_for_round "$round"); do
                idx=$((idx + 1))
                run_one "$cell" "$arm" "$round" "${it%%.*}" "${it#*.}" "$idx"
                [ $? = 2 ] && exit 2
            done
        done
    done
done
log "T1PIFW_END"
