#!/usr/bin/env bash
# largemsg_cpu_profile.sh - where a 1 MB RELIABLE KEEP_LAST 10 30 Hz transfer spends its CPU, per MB, on the socket
# path of large-message step A (docs/DESIGN.md section 8), off the rig: two private namespaces joined by a veth pair,
# large_sample_bench.c at both ends (as large_sample_pc.sh builds it, plus frame pointers and -g).
#
# Why: the rig's A4 stage 2 (I1, Array1m reliable cross-host 0%) measured subscriber 5.85 ms/MB against Fast DDS 2.48
# and CycloneDDS 3.15 (both let the kernel fragment 64 KB / 14.7 KB datagrams), and our like-for-like datagram size
# beat fastdds@mms1472 (6.26). This finds what our per-MB cost is made of.
#
# ARMS (each run is both roles at once, one in each namespace):
#   record  perf record -g (user + kernel cycles) on each role: the profile.
#   stat    perf stat on each role: instructions and cycles split user / kernel, and the syscall tracepoints, so the
#           per-datagram syscall count is counted, not inferred. No perf record in this arm.
#
# HOW TO READ IT (written before the run):
#   Everything is divided by MB delivered (sub) or published (pub), from the bench's own RESULT line.
#   The bench's own payload work - pattern() in bench_encode (pub) and bytes_intact() (sub) - is a stand-in for rmw's
#     serialize / deserialize and is reported apart from core's share, never folded into it.
#   VOID: delivered < 95% of published, torn > 0, or the record arm's delivered count differs from the stat arm's by
#     more than 10% (perf would then be profiling a different run).
#   CONTROL: the stat arm's receive syscalls per received datagram must be >= 1.0 at tt_RX_BATCH 1 (one recvfrom each);
#     below 1.0 the tracepoint counting is not seeing this process.
#   PC cycles are NOT CPU time on the rig and this PC is shared with other sessions: only shares and instruction counts
#     are read from it.
#
# Usage: largemsg_cpu_profile.sh [DURATION_S] [EXTRA_CFLAGS]     results under $WORK (default ~/largemsg_profile)
set -u
DURATION=${1:-20}
EXTRA=${2:-}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../../.." && pwd)}
WORK=${WORK:-$HOME/largemsg_profile}
TAG=${TAG:-$(date +%Y%m%d-%H%M%S)}
OUT=$WORK/$TAG
NS1=lmprof-pub-$$
NS2=lmprof-sub-$$
BIN=$OUT/large_sample_bench
SIZE=${SIZE:-1048576}
RATE=${RATE:-30}

cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    return 0
}
trap cleanup EXIT

mkdir -p "$OUT"
sha=$(git -C "$TREE" rev-parse --short HEAD)
dirty=$(git -C "$TREE" status --porcelain -- src include | wc -l)
# shellcheck disable=SC2086
cc -O2 -g -fno-omit-frame-pointer -DNDEBUG -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 \
    $EXTRA -I"$TREE/include" -I"$TREE/src" -o "$BIN" "$HERE/large_sample_bench.c" "$TREE/src/tickle.c" \
    "$TREE/src/encoding.c" "$TREE/src/log.c" "$TREE/src/hal_linux.c" -lpthread -lm || exit 1

cleanup
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit 1
sudo -n ip link add lmprof1 netns "$NS1" type veth peer name lmprof2 netns "$NS2" || exit 1
sudo -n ip -n "$NS1" addr add 192.168.10.1/24 dev lmprof1
sudo -n ip -n "$NS2" addr add 192.168.10.2/24 dev lmprof2
for ns in "$NS1" "$NS2"; do sudo -n ip -n "$ns" link set lo up; done
sudo -n ip -n "$NS1" link set lmprof1 up
sudo -n ip -n "$NS2" link set lmprof2 up

echo "=== largemsg_cpu_profile $(date -Is) tree=$sha dirty_core_files=$dirty size=$SIZE rate=$RATE duration=${DURATION}s extra='$EXTRA' ===" |
    tee -a "$OUT/summary.txt"

SC='syscalls:sys_enter_recvfrom,syscalls:sys_enter_recvmmsg,syscalls:sys_enter_recvmsg,syscalls:sys_enter_sendmmsg,'
SC+='syscalls:sys_enter_sendto,syscalls:sys_enter_sendmsg,syscalls:sys_enter_ppoll,syscalls:sys_enter_epoll_pwait2,'
SC+='syscalls:sys_enter_io_uring_enter,syscalls:sys_enter_clock_gettime'
EV="instructions:u,instructions:k,cycles:u,cycles:k,$SC"

run_arm() { # ARM
    local arm=$1 pre_sub pre_pub
    if [ "$arm" = record ]; then
        pre_sub="perf record -q -g -F 2999 -o $OUT/sub.perf.data --"
        pre_pub="perf record -q -g -F 2999 -o $OUT/pub.perf.data --"
    else
        pre_sub="perf stat -x, -e $EV -o $OUT/sub.stat.csv --"
        pre_pub="perf stat -x, -e $EV -o $OUT/pub.stat.csv --"
    fi
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS2" $pre_sub "$BIN" sub -s "$SIZE" -d $((DURATION + 8)) -R > "$OUT/sub.$arm.log" 2>&1 &
    local sub_pid=$!
    sleep 1
    # shellcheck disable=SC2024,SC2086
    sudo -n ip netns exec "$NS1" $pre_pub "$BIN" pub -s "$SIZE" -r "$RATE" -d "$DURATION" -R > "$OUT/pub.$arm.log" 2>&1
    wait "$sub_pid"
    echo "$arm pub: $(grep -m1 '^RESULT:' "$OUT/pub.$arm.log")" | tee -a "$OUT/summary.txt"
    echo "$arm sub: $(grep -m1 '^RESULT:' "$OUT/sub.$arm.log")" | tee -a "$OUT/summary.txt"
}

run_arm stat
run_arm record
for role in pub sub; do
    sudo -n ip netns exec "$NS1" perf report -i "$OUT/$role.perf.data" --no-children --sort sym --stdio \
        --percent-limit 0.3 2>/dev/null | grep -v '^$' > "$OUT/$role.self.txt"
    sudo -n ip netns exec "$NS1" perf report -i "$OUT/$role.perf.data" --children --sort sym --stdio -g none \
        --percent-limit 1 2>/dev/null | grep -v '^$' > "$OUT/$role.children.txt"
    sudo -n ip netns exec "$NS1" chmod a+r "$OUT/$role.perf.data"
done
echo "=== done $(date -Is) out=$OUT ===" | tee -a "$OUT/summary.txt"
