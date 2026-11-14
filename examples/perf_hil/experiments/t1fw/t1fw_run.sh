#!/usr/bin/env bash
# shellcheck disable=SC2024 # the redirects (>"$OUT/...") run as the invoking user; only `ip netns exec` needs root
# T1 framework comparison runner (plan: FRAMEWORK_CMP_PLAN.md). One cell / arm / round / framework / copy per run: fresh T1
# topology, the framework's echo (or sub) in t1_0_2 and its ping (or pub) in t1_0_1, raw output under $OUT/<cell>/<arm>/.
# SIMULATION ONLY (mac80211_hwsim + wmediumd): no real Wi-Fi, no rig, no Pi. Run one at a time, nothing else on the PC.
#
#   CELLS="be_small rel_small rel_large" ARMS="ctl o60 o200" ROUNDS=3 FWS="tickle cyclonedds fastdds zenohpico" COPIES="A A2"
#   OUT=$HOME/rig_results_safe/t1_fwcmp_20261114  BIN=$HOME/tmp_build/t1fw/bin  T1_DIR=$HOME/tmp_build/t1fw  LARGE_SZ=1048576
# Order inside a (cell, arm) round: the list [fw.A for each fw, fw.A2 for each fw] rotated left by 3*(round-1).
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
T1S=$S/../t1
BIN=${BIN:-$HOME/tmp_build/t1fw/bin}
export T1_DIR=${T1_DIR:-$HOME/tmp_build/t1fw}
export PHY_BASE=${PHY_BASE:-6} RETRY=1 SEED=7 PIN_WM=2 TMPDIR=${T1_TMPDIR:-$HOME/tmp_build}
CELLS=${CELLS:-"be_small rel_small rel_large"}
ARMS=${ARMS:-"ctl o60 o200"}
ROUNDS=${ROUNDS:-3}
FWS=${FWS:-"tickle cyclonedds fastdds zenohpico"}
COPIES=${COPIES:-"A A2"}
DUR=${DUR:-60}
LARGE_SZ=${LARGE_SZ:-1048576}
LARGE_HZ=${LARGE_HZ:-0.25}
ROTATE=${ROTATE:-3}
OUT=${OUT:-$HOME/rig_results_safe/t1_fwcmp_$(date +%Y%m%d)}
IP_PING=10.80.0.2
IP_ECHO=10.80.0.3
BCAST=10.80.0.255
WL_PING=wlan$((PHY_BASE + 1))
WL_ECHO=wlan$((PHY_BASE + 2))
NS_PING=t1_0_1
NS_ECHO=t1_0_2
declare -A PROF_BADP=([ctl]=0.0 [o60]=0.9 [o200]=0.9)
declare -A PROF_GE=([ctl]="1000000,1,0.0" [o60]="1140,60,0.01" [o200]="3800,200,0.01")
mkdir -p "$OUT"

fdds_xml() { # fdds_xml <file>: the rig's campaign-T profile (UDPv4 only, maxMessageSize 1472), v3 root. No interfaceWhiteList: in Fast DDS 3.6 it stopped discovery here (pilot), and each T1 namespace has only its wlan and lo
    cat >"$1" <<XML
<?xml version="1.0" encoding="UTF-8"?>
<dds xmlns="http://www.eprosima.com">
  <profiles>
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
</dds>
XML
}

# cmd_for <fw> <cell> <side: far|near> <ip> <iface> -> prints the command line (env prefix included) for that side.
# far = echo / sub (t1_0_2), near = ping / pub (t1_0_1).
cmd_for() {
    local fw=$1 cell=$2 side=$3 ip=$4 wl=$5 mode role args
    case $cell in be_small) mode=be ;; *) mode=rel ;; esac
    if [ "$cell" = rel_large ]; then
        [ "$side" = far ] && role=sub || role=pub
    else
        [ "$side" = far ] && role=echo || role=ping
    fi
    local common stop_arg=""
    [ "$side" = far ] && stop_arg="-q $STOP"
    case $fw in
    tickle)
        if [ "$cell" = rel_large ]; then
            if [ $role = pub ]; then
                echo "env TT_UDP_OFFLOAD=0 $BIN/large pub -s $LARGE_SZ -r $LARGE_HZ -d $DUR -R -b $BCAST -P wifi"
            else
                echo "env TT_UDP_OFFLOAD=0 $BIN/large sub -s $LARGE_SZ -d $((DUR + 8)) -R -b $BCAST -P wifi"
            fi
        else
            local ctx=1
            [ $role = echo ] && ctx=2
            local d=$DUR
            [ $role = echo ] && d=$((DUR + 40))
            echo "$BIN/node_d100 $role -c $ctx -b $BCAST -d $d -m $mode -s 100 -r 100 -p wifi $stop_arg -l $LAT.$role"
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
        args="$args $stop_arg -l $LAT.$role -i $ip -I $wl"
        case $fw in
        cyclonedds)
            local cx=${STOP%.stop}.cyclone.$ip.xml
            printf '<CycloneDDS><Domain><General><Interfaces><NetworkInterface address="%s"/></Interfaces></General><Discovery><SPDPInterval>1s</SPDPInterval></Discovery></Domain></CycloneDDS>\n' "$ip" >"$cx"
            echo "env CYCLONEDDS_URI=file://$cx $BIN/cyclonedds_node $args"
            ;;
        fastdds)
            echo "env FASTDDS_DEFAULT_PROFILES_FILE=$XML FASTRTPS_DEFAULT_PROFILES_FILE=$XML $BIN/fastdds_node $args"
            ;;
        zenohpico)
            local zenv=""
            if [ "$mode" = rel ]; then
                [ "$side" = far ] && zenv="T1_ZENOH_LISTEN=tcp/$IP_ECHO:7447" || zenv="T1_ZENOH_CONNECT=tcp/$IP_ECHO:7447"
            fi
            echo "env $zenv $BIN/zenohpico_node $args"
            ;;
        esac
        ;;
    esac
}

kill_ns_procs() { # every process still in the two namespaces (ip netns pids lists exactly those)
    local ns p
    for ns in $NS_PING $NS_ECHO; do
        for p in $(sudo -n ip netns pids "$ns" 2>/dev/null); do
            case $(readlink "/proc/$p/exe" 2>/dev/null) in
            */wpa_supplicant | */wmediumd) ;;
            *) sudo -n ip netns exec "$ns" kill -KILL "$p" 2>/dev/null ;;
            esac
        done
    done
}

run_one() { # run_one <cell> <arm> <round> <fw> <copy> <idx>
    local cell=$1 arm=$2 round=$3 fw=$4 copy=$5 idx=$6
    local dir=$OUT/$cell/$arm run=r${round}_${fw}_${copy}
    mkdir -p "$dir"
    [ -f "$dir/$run.done" ] && { echo "SKIP $cell/$arm/$run (done)"; return 0; }
    local attempt
    for attempt in 1 2 3; do
        STOP=$dir/$run.stop LAT=$dir/$run.lat XML=$dir/$run.fastdds.xml
        rm -f "$STOP"
        "$T1S/t1_topo.sh" down >/dev/null 2>&1
        sleep 1
        BAD_P=${PROF_BADP[$arm]} "$T1S/t1_topo.sh" up 1 "${PROF_GE[$arm]}" >"$dir/$run.topo" 2>&1
        if ! grep -q "^T1_UP lanes" "$dir/$run.topo"; then
            echo "HARNESS_FAULT $cell/$arm/$run attempt $attempt: topology did not come up"
            continue
        fi
        # lo up in both stations (every framework, same setup): Fast DDS sets IP_MULTICAST_IF to 127.0.0.1 and finds no peer without it
        sudo -n ip -n $NS_PING link set lo up
        sudo -n ip -n $NS_ECHO link set lo up
        [ "$fw" = fastdds ] && fdds_xml "$XML"
        local far near
        far=$(cmd_for "$fw" "$cell" far $IP_ECHO "$WL_ECHO")
        near=$(cmd_for "$fw" "$cell" near $IP_PING "$WL_PING")
        printf 'cell=%s arm=%s round=%s fw=%s copy=%s idx=%s attempt=%s start=%s\nfar: %s\nnear: %s\nprofile: bad_p=%s ge=%s retry=1 seed=7 phy_base=%s\n' \
            "$cell" "$arm" "$round" "$fw" "$copy" "$idx" "$attempt" "$(date -Is)" "$far" "$near" "${PROF_BADP[$arm]}" "${PROF_GE[$arm]}" "$PHY_BASE" >"$dir/$run.meta"
        # shellcheck disable=SC2086
        sudo -n ip netns exec $NS_ECHO timeout -k 5 $((DUR + 60)) $far >"$dir/$run.far" 2>&1 </dev/null &
        local fp=$!
        sleep 1
        # shellcheck disable=SC2086
        sudo -n ip netns exec $NS_PING timeout -k 5 $((DUR + 80)) $near >"$dir/$run.near" 2>&1 </dev/null
        touch "$STOP"
        local i
        for i in $(seq 1 40); do kill -0 $fp 2>/dev/null || break; sleep 0.5; done
        kill_ns_procs
        wait $fp 2>/dev/null
        rm -f "$STOP"
        "$T1S/t1_topo.sh" down >/dev/null 2>&1
        if grep -q '^RESULT' "$dir/$run.near" "$dir/$run.far" 2>/dev/null; then
            local sc
            sc=$(grep -h '^RESULT' "$dir/$run.near" | grep -o 'scored=[0-9]*\|published=[0-9]*\|matched=[0-9]\|p50_us=[0-9.]*\|p99_us=[0-9.]*\|delivered=[0-9]*\|error=[a-z_]*' | tr '\n' ' ')
            echo "$cell/$arm/$run near: $sc"
            date -Is >"$dir/$run.done"
            return 0
        fi
        echo "HARNESS_FAULT $cell/$arm/$run attempt $attempt: no RESULT line"
    done
    echo "RUN_LOST $cell/$arm/$run (3 attempts, no RESULT)"
    echo lost >"$dir/$run.done"
    return 1
}

order_for_round() { # prints "fw.copy" items in the rotated order for a round
    local round=$1 items=() fw c n i off
    for c in $COPIES; do for fw in $FWS; do items+=("$fw.$c"); done; done
    n=${#items[@]}
    off=$(((round - 1) * ROTATE % n))
    for ((i = 0; i < n; i++)); do echo "${items[$(((i + off) % n))]}"; done
}

echo "T1FW_START $(date -Is) out=$OUT cells='$CELLS' arms='$ARMS' rounds=$ROUNDS fws='$FWS' dur=$DUR large=${LARGE_SZ}B@${LARGE_HZ}Hz"
for cell in $CELLS; do
    for arm in $ARMS; do
        for round in $(seq 1 "$ROUNDS"); do
            idx=0
            for it in $(order_for_round "$round"); do
                idx=$((idx + 1))
                run_one "$cell" "$arm" "$round" "${it%%.*}" "${it#*.}" "$idx"
            done
        done
    done
done
"$T1S/t1_topo.sh" down >/dev/null 2>&1
echo "T1FW_END $(date -Is)"
