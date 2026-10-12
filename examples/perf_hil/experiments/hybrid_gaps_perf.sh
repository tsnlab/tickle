#!/usr/bin/env bash
# hybrid_gaps_perf.sh - where the hybrid transport's receive and send CPU goes, against Fast DDS's, on the PC (2026-10-12
# rig scorecard: subscriber CPU per MB 13-16% behind Fast DDS at 1 MB and 4 MB, the publisher 1-8% behind).
#
# Two private network namespaces, a veth pair, the publisher's egress shaped to 1 Gbit/s by tbf, buffers 212,992 B, as
# hybrid_gaps_pc.sh. Each arm is profiled at both ends with `perf record -g` (user and kernel, frame pointers in the
# bench build) while it streams 1 MB samples at 30 Hz best effort, then 4 MB at 15 Hz RELIABLE KEEP_LAST 10:
#   tickle_ip45  large_sample_bench, runs of 45 (what the rig ran; -F ip, reader cap off), message encoded by memcpy
#                with the ranged encode (-M -G). The bench has no rmw: its callback only checks every byte (bench_decode),
#                where rmw_tickle decodes into the ROS message - one pass over the data either way.
#   tickle_mtu   the same binary, -F mtu: MTU fragments (main's path), for the per-datagram share.
#   fastdds      apex performance_test (-c ROS2, Array1m / Array4m) over rmw_fastrtps_cpp, UDPv4 only (shared memory
#                off: the namespaces share /dev/shm), whitelisted to the veth - the rig's vendor arm, here on the PC.
#                Its subscriber's cost includes rclcpp and perf_test's own bookkeeping, which the bench does not have;
#                read it by symbol, not by total.
#
# NOT a rig figure. The PC's cost ratios between symbols are what it is for: which share of a TickLE receiver's
# CPU per MB is kernel IP reassembly, the kernel-to-user copy, core's copy into the assembly, the per-unit bookkeeping
# (deliver_large_fragment() and below, once per 1,452-byte unit even inside a run), the drain and the poll loop - and
# whether Fast DDS has the same rows.
#
# HOW TO READ IT, written before the first run (2026-10-12):
#   VOID     an arm whose subscriber received under 98% of what was published (the bench's RESULT; perf_test's
#            received column), or whose perf.data has under 1,000 samples.
#   ROWS     per arm and end: cpu_ms_per_mb (the bench's RESULT; for fastdds, getrusage-equivalent from perf's sample
#            count is not used - the share table is read instead), and the share of samples in: kernel (all [k]
#            symbols), of which IP reassembly (ip_defrag/ip_frag_*/inet_frag_*), copy to user (*copy_to_user*,
#            copyout, skb_copy_datagram_iter), and user: memcpy, core per-unit (deliver_large_fragment, large_place,
#            large_accept_*, update_reliable_ack, find_writer_proxy, large_assembly_find, hybrid_rx_note, drain_reorder),
#            HAL drain (io_uring / rx_take_drained / drain_*), poll loop, bench check.
#   H_UNIT   core's per-unit bookkeeping (everything under process_frag_run_large() except memcpy) is >= 5% of the
#            TickLE ip45 subscriber's samples: then handling a run as one store (one memcpy, one bitmap range, one
#            lookup) is worth a rig A/B. Below 5%: not the gap.
#   H_COPY   the TickLE ip45 subscriber's user-space memcpy share exceeds Fast DDS's by more than the share of one
#            pass over the data (the bench check's share, measured beside it): then TickLE makes one copy more.
#
# Usage: hybrid_gaps_perf.sh [DURATION_S]    reports in $WORK (default ~/largemsg_pc/hybrid_gaps_perf)
# Needs passwordless `sudo ip` and `tc`; perf runs as root inside the namespaces (as the bench does). Detach it.
set -u
DURATION=${1:-20}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
WORK=${WORK:-$HOME/largemsg_pc/hybrid_gaps_perf}
NS1=hgpf-pub-$$
NS2=hgpf-sub-$$
VMEM_KB=${VMEM_KB:-2097152}
ROS_SETUP=${ROS_SETUP:-/opt/ros/lyrical/setup.bash}
PERF_WS=${PERF_WS:-$HOME/rmw_perf_ws/install/setup.bash}
ARMS=${ARMS:-"tickle_ip45 tickle_mtu fastdds"}
CELLS=${CELLS:-"be1m:1048576:30:Array1m rel4m:4194304:15:Array4m"}
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}

cleanup() { # the one EXIT trap
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT
mkdir -p "$WORK"
cc -O2 -g -fno-omit-frame-pointer -DNDEBUG -Wall -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 \
    -Dtt_SEGMENT_ENABLED=0 -Dtt_SOCKET_BUFFER_SIZE=212992 -I"$TREE/include" -I"$TREE/src" -o "$WORK/bench" \
    "$HERE/large_sample_bench.c" "$TREE/src/tickle.c" "$TREE/src/encoding.c" "$TREE/src/log.c" \
    "$TREE/src/hal_linux.c" -lpthread -lm 2>"$WORK/build.log" || { echo "build failed"; exit 1; }
cat >"$WORK/fastdds_veth.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<profiles xmlns="http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles">
  <transport_descriptors>
    <transport_descriptor>
      <transport_id>veth_udp</transport_id>
      <type>UDPv4</type>
      <interfaceWhiteList><address>192.168.84.1</address><address>192.168.84.2</address></interfaceWhiteList>
    </transport_descriptor>
  </transport_descriptors>
  <participant profile_name="veth_only" is_default_profile="true">
    <rtps>
      <builtin><initialPeersList>
        <locator><udpv4><address>192.168.84.1</address></udpv4></locator>
        <locator><udpv4><address>192.168.84.2</address></udpv4></locator>
      </initialPeersList></builtin>
      <userTransports><transport_id>veth_udp</transport_id></userTransports>
      <useBuiltinTransports>false</useBuiltinTransports></rtps>
  </participant>
</profiles>
EOF
# The ROS side of the fastdds arm, as a script each namespace runs (the environment is set inside it).
cat >"$WORK/ros_run.sh" <<EOF
#!/usr/bin/env bash
# shellcheck disable=SC1090,SC1091
source "$ROS_SETUP"; source "$PERF_WS"
# FASTDDS_PROFILE=1 uses the veth-whitelist profile above; by default the builtin UDPv4 transport alone (the namespace
# holds only lo and the veth, so no whitelist is needed, and shared memory is off).
if [ "${FASTDDS_PROFILE:-0}" = 1 ]; then export FASTRTPS_DEFAULT_PROFILES_FILE=$WORK/fastdds_veth.xml; else export FASTDDS_BUILTIN_TRANSPORTS=UDPv4; fi
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp ROS_DOMAIN_ID=77
export ROS_AUTOMATIC_DISCOVERY_RANGE=SUBNET
ulimit -v $VMEM_KB
exec "\$@"
EOF
chmod +x "$WORK/ros_run.sh"
PERF_TEST=$HOME/rmw_perf_ws/install/performance_test/lib/performance_test/perf_test

sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add hgpf1 netns "$NS1" type veth peer name hgpf2 netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.84.1/24 dev hgpf1
sudo -n ip -n "$NS2" addr add 192.168.84.2/24 dev hgpf2
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set hgpf1 up
sudo -n ip -n "$NS2" link set hgpf2 up
sudo -n ip -n "$NS1" route add 224.0.0.0/4 dev hgpf1
sudo -n ip -n "$NS2" route add 224.0.0.0/4 dev hgpf2
sudo -n ip netns exec "$NS1" tc qdisc add dev hgpf1 root tbf rate 1gbit burst 64kb latency 100ms || exit 1
echo "=== hybrid_gaps_perf $(date -Is) head=$(git -C "$TREE" rev-parse --short HEAD) duration=${DURATION}s load=$(cut -d' ' -f1-3 /proc/loadavg) ===" | tee -a "$WORK/results.txt"

PERF="perf record -F 15000 -g -q"
report() { # <tag> : per-symbol shares, kernel and user, from both ends
    for end in sub pub; do
        sudo -n ip netns exec "$NS2" perf report -i "$WORK/$1.$end.data" --no-children --sort dso,sym --stdio \
            --percent-limit 0.3 2>/dev/null | grep -v '^$' | head -120 >"$WORK/$1.$end.report"
        sudo -n ip netns exec "$NS2" perf report -i "$WORK/$1.$end.data" --no-children --sort sym --stdio -g none \
            --percent-limit 0 2>/dev/null | grep -E '^ +[0-9]' >"$WORK/$1.$end.syms"
        sudo -n ip netns exec "$NS2" perf report -i "$WORK/$1.$end.data" --no-children --sort dso,sym --stdio -g caller \
            --percent-limit 2 2>/dev/null | head -400 >"$WORK/$1.$end.callers"
    done
}

for spec in $CELLS; do
    IFS=: read -r cell size rate msg <<<"$spec"
    for arm in $ARMS; do
        tag=${arm}_$cell
        case $arm in
        tickle_*)
            fflag="-F ip"
            [ "$arm" = tickle_mtu ] && fflag="-F mtu"
            qflag=""
            [ "${cell#rel}" != "$cell" ] && qflag="-R"
            # shellcheck disable=SC2024,SC2086
            sudo -n ip netns exec "$NS2" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 40)) $PERF \
                -o $WORK/$tag.sub.data -- $WORK/bench sub -s $size -d $((DURATION + 8)) $qflag -C -b 192.168.84.255" \
                >"$WORK/$tag.sub.log" 2>&1 &
            sub_pid=$!
            sleep 1
            # shellcheck disable=SC2024,SC2086
            sudo -n ip netns exec "$NS1" bash -c "ulimit -v $VMEM_KB; exec timeout $((DURATION + 40)) $PERF \
                -o $WORK/$tag.pub.data -- $WORK/bench pub -s $size -r $rate -d $DURATION $qflag $fflag -M -G \
                -b 192.168.84.255" >"$WORK/$tag.pub.log" 2>&1
            wait "$sub_pid"
            echo "ARM $tag pub: $(grep -m1 '^RESULT' "$WORK/$tag.pub.log" | grep -oE '(published|cpu_ms_per_mb|hybrid_n_hist)=[^ ]*' | tr '\n' ' ')" | tee -a "$WORK/results.txt"
            echo "ARM $tag sub: $(grep -m1 '^RESULT' "$WORK/$tag.sub.log" | grep -oE '(delivered|cpu_ms_per_mb|latency_median_us|drain_starts|drain_datagrams)=[^ ]*' | tr '\n' ' ')" | tee -a "$WORK/results.txt"
            ;;
        fastdds)
            qos="--keep_last --history_depth 5"
            [ "${cell#rel}" != "$cell" ] && qos="--reliable --keep_last --history_depth 10"
            # shellcheck disable=SC2024,SC2086
            sudo -n ip netns exec "$NS2" timeout $((DURATION + 40)) $PERF -o "$WORK/$tag.sub.data" -- \
                "$WORK/ros_run.sh" "$PERF_TEST" -c ROS2 -t "$msg" -p 0 -s 1 --expected_num_pubs 1 \
                --max_runtime $((DURATION + 8)) $qos -l "$WORK/$tag.sub.csv" >"$WORK/$tag.sub.log" 2>&1 &
            sub_pid=$!
            sleep 2
            # shellcheck disable=SC2024,SC2086
            sudo -n ip netns exec "$NS1" timeout $((DURATION + 40)) $PERF -o "$WORK/$tag.pub.data" -- \
                "$WORK/ros_run.sh" "$PERF_TEST" -c ROS2 -t "$msg" -r "$rate" -p 1 -s 0 --expected_num_subs 1 \
                --max_runtime "$DURATION" $qos -l "$WORK/$tag.pub.csv" >"$WORK/$tag.pub.log" 2>&1
            wait "$sub_pid"
            echo "ARM $tag: perf_test logs $WORK/$tag.{sub,pub}.log, csv $WORK/$tag.{sub,pub}.csv" | tee -a "$WORK/results.txt"
            ;;
        esac
        report "$tag"
    done
done
echo "=== done $(date -Is) ===" | tee -a "$WORK/results.txt"
