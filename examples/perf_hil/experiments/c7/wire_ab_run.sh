#!/usr/bin/env bash
# C7 round 3 (the wire experiment, config.h tt_ACKNACK_ROOM): the sim sweep and the PC netns A/B against main, each
# under the PC A/B lock and only on a quiet PC. Run it detached:
#   setsid nohup examples/perf_hil/experiments/c7/wire_ab_run.sh > ~/rig_results_safe/large_repair/c7_wire.log 2>&1 < /dev/null &
# Readings (registered before the run, in code): sweep_read_r3.py (sim) and netns_read_r3.py (netns).
# Prints "C7_WIRE_DONE rc=N SIM=file NETNS=dir" last, whatever happened.
# Env: STEPS (default "sim netns"), REPS (6), HEAD_REF (HEAD), BASE (75495519).
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
stamp=$(date +%Y%m%d-%H%M%S)
lock=/home/semih/rig_jobs/pc_ab.lock
quiet=/home/semih/tmp_build/PC_QUIET
results=$HOME/rig_results_safe/large_repair
sim_out=$results/c7_wire_sim_$stamp.txt
netns_out=$results/c7_wire_netns_$stamp
base=${BASE:-75495519}
head_ref=$(git -C "$root" rev-parse --short "${HEAD_REF:-HEAD}")
rc=1
finish() {
    echo "C7_WIRE_DONE rc=$rc SIM=$sim_out NETNS=$netns_out $(date -Is)"
}
trap finish EXIT
mkdir -p "$results"

# Waits (at most 3 h) until PC_QUIET is gone and the 1-minute load is below 4.
wait_quiet() {
    local deadline=$((SECONDS + 10800))
    while [ -e "$quiet" ] || awk '{exit !($1 >= 4)}' /proc/loadavg; do
        if [ "$SECONDS" -ge "$deadline" ]; then
            echo "wait_quiet: gave up after 3 h (PC_QUIET $([ -e "$quiet" ] && echo present || echo absent)," \
                "load $(cut -d' ' -f1 /proc/loadavg))"
            return 1
        fi
        sleep 20
    done
    echo "quiet at $(date -Is): load $(cut -d' ' -f1 /proc/loadavg)"
}

echo "=== C7 wire A/B $(date -Is) base=$base head=$head_ref steps='${STEPS:-sim netns}'"
for step in ${STEPS:-sim netns}; do
    case $step in
    sim)
        wait_quiet || exit
        flock "$lock" env BASE="$base" WORK=/home/semih/tmp_build/c7_wire OUT="$sim_out" "$here/sweep.sh"
        echo "sweep rc=$? at $(date -Is)"
        python3 "$here/sweep_read_r3.py" "$sim_out" | tee "$sim_out.read.txt"
        ;;
    netns)
        wait_quiet || exit
        flock "$lock" env BASE="$base" HEAD_REF="$head_ref" OUT="$netns_out" PHASE=order ROUND=3 REPS="${REPS:-6}" \
            SLOW_W=31000 SLOW_SUB_EXTRA="-S 262144" "$here/netns_slow_ab.sh"
        echo "netns rc=$? at $(date -Is)"
        ;;
    esac
done
rc=0
