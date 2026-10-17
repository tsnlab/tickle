#!/usr/bin/env bash
# rx_slot_order_pc.sh - which large buffers an rmw_tickle subscriber actually writes its samples into, in the I4
# best-effort shape (Array4m, BEST_EFFORT KEEP_LAST 5, 15 Hz), on this PC in two private network namespaces joined by a
# veth (never the default namespace). Written 2026-10-17 for the "reassembly slot LIFO" fix
# (~/claude_reports/I4BE_CPU_GAP_2026-10-17.md, fix 1), before deciding what to change.
#
# The question: does the subscriber cycle through several 4 MB buffers (KEEP_LAST 5: up to ~20 MB, every sample written
# to memory no core has touched for several periods), or does it reuse the one it released last? The subscriber's
# perf_test runs under gdb with a printing breakpoint on rmw_tickle's large_acquire() (the bytes asked for; the first
# hit also prints where librmw_tickle.so was loaded from, the identity check) and a dprintf on large_release()
# (the buffer given back) - static functions in librmw_tickle.so, called through core's function pointers, so never
# inlined. The released pointers, in order, are the buffers samples were assembled in.
#
# HOW TO READ IT (written before the first run, enforced at the end):
#   VOID  fewer than 100 acquires of >= 1 MiB (the run did not exercise the path), or gdb's shared-library list for
#         the subscriber does not show $PF_TICKLE_INSTALL's librmw_tickle.so (wrong binary).
#   CYCLES  more than 2 distinct buffers among the releases after the first 10: the allocator rotates; LIFO reuse is a
#           change worth an A/B.
#   ALREADY_LIFO  1 or 2 distinct buffers: the most recently released buffer is reused already; changing the order in
#           which free slots are chosen cannot change which memory is written, so fix 1 has nothing to act on.
# Usage: PF_TICKLE_INSTALL=... PF_PERF_WS=... rx_slot_order_pc.sh [DUR]   (default 30 s; ~1 min in all)
#   OUT (default /home/semih/tmp_build/lifo/slot_order_<ts>). The last line is always "RX_SLOT_ORDER rc=<n> ...".
set -uo pipefail
DUR=${1:-30}
: "${PF_TICKLE_INSTALL:?pc_preflight_build.sh prints it}" "${PF_PERF_WS:?pc_preflight_build.sh prints it}"
OUT=${OUT:-/home/semih/tmp_build/lifo/slot_order_$(date +%Y%m%d-%H%M%S)}
NS1=rso_c_$$
NS2=rso_s_$$
VETH=rso$$
rc=1
verdict=NONE
cleanup() {
    sudo -n ip netns del "$NS1" 2>/dev/null
    sudo -n ip netns del "$NS2" 2>/dev/null
    echo "RX_SLOT_ORDER rc=$rc verdict=$verdict out=$OUT $(date -Is)"
}
trap cleanup EXIT
mkdir -p "$OUT" || exit
read -r load _ < /proc/loadavg
echo "load $load"
sudo -n ip netns add "$NS1" && sudo -n ip netns add "$NS2" || exit
sudo -n ip link add "${VETH}c" type veth peer name "${VETH}s" || exit
for ns in "$NS1" "$NS2"; do
    case "$ns" in "$NS1") peer=${VETH}c ip=192.168.10.2 ;; *) peer=${VETH}s ip=192.168.10.3 ;; esac
    sudo -n ip link set "$peer" netns "$ns" && sudo -n ip -n "$ns" link set "$peer" name eth0 &&
        sudo -n ip -n "$ns" addr add "$ip/24" brd 192.168.10.255 dev eth0 && sudo -n ip -n "$ns" link set lo up &&
        sudo -n ip -n "$ns" link set eth0 up && sudo -n ip -n "$ns" route add default dev eth0 || exit
done
PT=$PF_PERF_WS/performance_test/lib/performance_test/perf_test
cat > "$OUT/env.sh" <<EOF
set +u
source /opt/ros/lyrical/setup.bash
source $PF_TICKLE_INSTALL/local_setup.bash
source $PF_PERF_WS/local_setup.bash
export RMW_IMPLEMENTATION=rmw_tickle TICKLE_BROADCAST_ADDR=192.168.10.255 ROS_DOMAIN_ID=77
EOF
cat > "$OUT/gdb.cmds" <<'EOF'
set pagination off
set breakpoint pending on
handle SIGINT nostop noprint pass
handle SIGPIPE nostop noprint pass
set $shown = 0
break large_acquire
commands
silent
printf "ACQ bytes=%u\n", (unsigned)$rsi
if $shown == 0
info sharedlibrary librmw_tickle
set $shown = 1
end
continue
end
dprintf large_release,"REL buf=%p\n",(void*)$rsi
run
quit
EOF
COMMON="-c ROS2 -t Array4m --keep_last --history_depth 5 --ros-args --param start_type_description_service:=false"
# The subscriber under gdb; the publisher plain. Both as this user inside their namespace; the logs are this user's.
# shellcheck disable=SC2024 # the redirect is meant to run as this user, outside the namespace
sudo -n ip netns exec "$NS2" sudo -n -u "$USER" bash -c "source $OUT/env.sh; exec gdb -q -batch -x $OUT/gdb.cmds \
    --args $PT -p 0 -s 1 --expected_num_pubs 1 --max_runtime $((DUR + 15)) ${COMMON}" > "$OUT/sub.log" 2>&1 < /dev/null &
sub_launcher=$!
sleep 8 # gdb loads the ROS libraries' symbols before perf_test starts
# shellcheck disable=SC2024 # as above
sudo -n ip netns exec "$NS1" sudo -n -u "$USER" bash -c "source $OUT/env.sh; exec $PT -r 15 -p 1 -s 0 \
    --expected_num_subs 1 --max_runtime $DUR ${COMMON}" > "$OUT/pub.log" 2>&1 < /dev/null
wait "$sub_launcher"
grep -E '^(ACQ|REL) ' "$OUT/sub.log" > "$OUT/events.txt"
n_acq=$(grep -c '^ACQ' "$OUT/events.txt")
n_rel=$(grep -c '^REL' "$OUT/events.txt")
big_acq=$(awk '$1 == "ACQ" { split($2, a, "="); if (a[2] + 0 >= 1048576) n++ } END { print n + 0 }' "$OUT/events.txt")
distinct=$(grep '^REL' "$OUT/events.txt" | tail -n +11 | sort -u | wc -l)
lib_ok=0
grep -q "$PF_TICKLE_INSTALL/rmw_tickle/lib/librmw_tickle.so" "$OUT/sub.log" && lib_ok=1
echo "acquires $n_acq (>= 1 MiB: $big_acq), releases $n_rel, distinct released buffers after the first 10: $distinct," \
    "subscriber maps show this build's librmw_tickle.so: $lib_ok"
echo "release order (first 20):"
grep '^REL' "$OUT/events.txt" | head -20 | sed 's/^/  /'
echo "every distinct released buffer, with its count:"
grep '^REL' "$OUT/events.txt" | sort | uniq -c | sed 's/^/  /'
if [ "$big_acq" -lt 100 ] || [ "$lib_ok" != 1 ]; then
    verdict=VOID
elif [ "$distinct" -gt 2 ]; then
    verdict=CYCLES
else
    verdict=ALREADY_LIFO
fi
rc=0
