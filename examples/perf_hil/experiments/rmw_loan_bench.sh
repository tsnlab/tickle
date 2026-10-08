#!/usr/bin/env bash
# rmw_loan_bench.sh - loaned messages against plain publish/take, rmw_tickle same-host on the PC (docs/RMW.md,
# "Loaned messages"). A PC figure, never a rig one: nothing here touches the rig, and it runs in a private network
# namespace so no TickLE process reaches the 10.1.1.x management LAN.
#
#   WS=<workspace with rmw/install and rmw/build/rmw_tickle> rmw_loan_bench.sh [OUT_DIR]
#
# Shape: rmw_loan_bench.c (beside this) - one publisher process, one subscriber process, an Array1k-shaped type
# (1040 bytes, the callbacks a generated Array1k has), max rate for SECONDS, the first WARM and last COOL seconds of
# each process's own run dropped. RELIABLE + KEEP_ALL (depth 1000) and BEST_EFFORT + KEEP_LAST 1 (docs/TESTING.md
# section 5). Arms, rotated per repetition:
#   copy      RMW_TICKLE_LOANS=0: rmw_publish / rmw_take, can_loan_messages false - what there was before loans.
#   copy2     the same again: the control. Nothing differs from `copy`, so copy vs copy2 is the noise floor.
#   loan      borrow + publish_loaned / take_loaned + return, the default (ring slots not lent: decoded shells).
#   loan_ring the same with RMW_TICKLE_LOAN_RING_SLOTS=1: samples read in their ring slot.
#
# HOW TO READ IT, written before running and enforced in rmw_loan_bench_summary.py:
#   - VOID (listed, never averaged): a RESULT line missing or ok=0; can_loan not what the arm asks for (copy arms 0,
#     loan arms 1); a loan arm whose subscriber reports no loans at all, or loan_ring with loans_in_place 0 (the
#     treatment was not applied); a window with under 1000 messages.
#   - Per QoS and metric (subscriber delivered/s, subscriber and publisher CPU us per message): an arm is BETTER than
#     `copy` when its range over the repetitions lies wholly on the better side of copy's AND its median moves by more
#     than copy2's median moved from copy's; WORSE the same the other way; otherwise NO DIFFERENCE. A difference the
#     control also shows is not the change's.
#   - What would falsify "loans save CPU": `loan` not BETTER than `copy` on subscriber CPU per message.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WS="${WS:-/home/semih/rmw_loans_ws}"
OUT="${1:-$WS/loan_bench_$(date +%Y%m%d-%H%M%S)}"
REPS="${REPS:-5}"
SECONDS_RUN="${SECONDS_RUN:-6}"
WARM="${WARM:-1}"
COOL="${COOL:-1}"
ARMS=(copy copy2 loan loan_ring)
QOSES=(reliable best_effort)
ROS_SETUP=$(find /opt/ros -maxdepth 2 -name setup.bash -print -quit 2>/dev/null)
FLAGS="$WS/rmw/build/rmw_tickle/CMakeFiles/test_loaned_messages.dir/flags.make"
LIB="$WS/rmw/install/rmw_tickle/lib/librmw_tickle.so"
[ -n "$ROS_SETUP" ] && [ -f "$FLAGS" ] && [ -f "$LIB" ] || { echo "rmw_loan_bench: needs ROS, $FLAGS and $LIB"; exit 2; }
mkdir -p "$OUT" || exit 2

NS="loanbench$$"
cleanup() { sudo -n ip netns del "$NS" 2>/dev/null; }
trap cleanup EXIT # the only EXIT trap here

# The bench compiled as rmw_tickle's own tests are: the same defines and include paths, linked to the library under test.
# An RPATH, not a RUNPATH (--disable-new-dtags): the library's own typesupport dependencies resolve through it too.
# ROS_PACKAGE_NAME is left out: its quoted value does not survive the word split, and the bench does not use it.
defines=$(sed -n 's/^C_DEFINES = //p' "$FLAGS" | tr ' ' '\n' | grep -v '^-DROS_PACKAGE_NAME' | tr '\n' ' ')
includes=$(sed -n 's/^C_INCLUDES = //p' "$FLAGS")
TS_C="$WS/rmw/install/rosidl_typesupport_tickle_c/lib"
TS_CPP="$WS/rmw/install/rosidl_typesupport_tickle_cpp/lib"
ROS_LIB="$(dirname "$ROS_SETUP")/lib"
BIN="$OUT/rmw_loan_bench"
# shellcheck disable=SC2086  # the flag lists are meant to split
cc -O2 -std=gnu17 $defines $includes "$HERE/rmw_loan_bench.c" -o "$BIN" \
    -Wl,--disable-new-dtags,-rpath,"$(dirname "$LIB")":"$TS_C":"$TS_CPP":"$ROS_LIB" "$LIB" \
    -L"$TS_C" -L"$TS_CPP" -lrosidl_typesupport_tickle_c -lrosidl_typesupport_tickle_cpp \
    -L"$ROS_LIB" -lrmw -lrcutils -lrosidl_runtime_c || { echo "rmw_loan_bench: build failed"; exit 2; }
ldd "$BIN" | grep -E "rmw_tickle|librmw\.so" > "$OUT/ldd.txt"
grep -qF "$LIB" "$OUT/ldd.txt" || { echo "rmw_loan_bench: the bench does not load $LIB"; cat "$OUT/ldd.txt"; exit 2; }

if ! sudo -n ip netns add "$NS" || ! sudo -n ip netns exec "$NS" ip link set lo up ||
    ! sudo -n ip netns exec "$NS" ip route add default dev lo; then
    echo "rmw_loan_bench: no netns"
    exit 2
fi
default_ns=$(readlink /proc/self/ns/net)
proof=$(sudo -n ip netns exec "$NS" sudo -n -u "$USER" bash -c \
    'echo "$(readlink /proc/self/ns/net) addrs=$(ip -o addr show | grep -c 10\\.1\\.1\\.)"')
case "$proof" in
    "$default_ns"*) echo "rmw_loan_bench: would run in the default netns; refusing"; exit 2 ;;
    *" addrs=0") : ;;
    *) echo "rmw_loan_bench: a 10.1.1.x address is visible in $NS; refusing"; exit 2 ;;
esac
{
    echo "lib=$LIB md5=$(md5sum "$LIB" | cut -c1-12) head=$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null)"
    echo "netns=$proof reps=$REPS seconds=$SECONDS_RUN warm=$WARM cool=$COOL load=$(cut -d' ' -f1-3 /proc/loadavg)"
} > "$OUT/provenance.txt"
cat "$OUT/provenance.txt"

# One process in the netns, as the invoking user, with the arm's environment.
in_ns() {
    local env_args="$1"
    shift
    # shellcheck disable=SC2086  # env_args is a list of VAR=value words
    sudo -n ip netns exec "$NS" sudo -n -u "$USER" env -u RMW_TICKLE_LOANS -u RMW_TICKLE_LOAN_RING_SLOTS \
        $env_args "$@"
}

for rep in $(seq 1 "$REPS"); do
    # Rotate the arm order each repetition.
    order=()
    for i in "${!ARMS[@]}"; do
        order+=("${ARMS[$(((i + rep) % ${#ARMS[@]}))]}")
    done
    for qos in "${QOSES[@]}"; do
        for arm in "${order[@]}"; do
            case "$arm" in
                copy | copy2) mode=copy env_args="RMW_TICKLE_LOANS=0" ;;
                loan) mode=loan env_args="RMW_TICKLE_LOANS=1" ;;
                loan_ring) mode=loan env_args="RMW_TICKLE_LOANS=1 RMW_TICKLE_LOAN_RING_SLOTS=1" ;;
            esac
            tag="$OUT/r${rep}_${qos}_${arm}"
            in_ns "$env_args" "$BIN" sub "$mode" "$qos" "$SECONDS_RUN" "$WARM" "$COOL" > "$tag.sub" 2> "$tag.sub.err" &
            sub_pid=$!
            sleep 0.3
            in_ns "$env_args" "$BIN" pub "$mode" "$qos" "$SECONDS_RUN" "$WARM" "$COOL" > "$tag.pub" 2> "$tag.pub.err"
            wait "$sub_pid"
            loans=$(grep -o 'loans_in_place=[0-9]* loans_copied=[0-9]*' "$tag.sub.err" | tail -1)
            echo "rep=$rep qos=$qos arm=$arm $(grep -h RESULT "$tag.sub" | head -1) | $(grep -h RESULT "$tag.pub" | head -1) | ${loans:-loans_in_place=0 loans_copied=0}" |
                tee -a "$OUT/runs.txt"
        done
    done
done
python3 "$HERE/rmw_loan_bench_summary.py" "$OUT/runs.txt" | tee "$OUT/summary.txt"
echo "rmw_loan_bench: DONE $OUT"
