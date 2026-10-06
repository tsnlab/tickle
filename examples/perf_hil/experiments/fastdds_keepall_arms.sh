#!/usr/bin/env bash
# Fast DDS under KEEP_ALL, from its side (docs/ROADMAP.md "Next"): rmw_keepall_rig.sh's shape with four Fast DDS
# QoS arms and CycloneDDS as the control. No rmw_tickle arm, so nothing is built on the Pis.
#
# USAGE (detached, from this checkout; the profiles are copied from it to the Pis, so it need not be pushed):
#   cd <this checkout> && TS=$(date +%Y%m%d-%H%M%S) && OUT=$HOME/rig_results_safe/fastdds_keepall_arms_$TS && \
#   setsid nohup env RIG_LOCK_WAIT=14400 OUT="$OUT" examples/perf_hil/experiments/fastdds_keepall_arms.sh \
#       > "$OUT.launch.log" 2>&1 < /dev/null &
#   Results: $OUT.txt (log and verdicts), $OUT.runs/ (every perf_test log, treatment record, profile copy).
#   Re-read without the rig: python3 examples/perf_hil/experiments/fastdds_keepall_arms_summary.py $OUT.runs 20
# DURATION: 5 arms x 3 cells x 3 reps = 45 runs of ~34 s each (subscriber DUR+8 s, plus ssh/scp) = ~26 min, plus
#   up to RIG_LOCK_WAIT waiting for the rig. Estimate 30 min of rig time; analyse an overrun at 45 min.
#
# Arms (the profile is the only treatment; everything else is the published rmw KEEP_ALL rows' setup, RESULTS `K`):
#   fastdds@F0  fastdds/fastdds_eth0_only.xml (exactly what those rows ran)
#   fastdds@F1  fastdds/fastdds_keepall_F1.xml: F0 + max_blocking_time 5 s
#   fastdds@F2  fastdds/fastdds_keepall_F2.xml: F1 + heartbeatPeriod 50 ms
#   fastdds@F3  fastdds/fastdds_keepall_F3.xml: F0 + max_samples 50,000
#   cyclonedds  control: no profile changes, its rep-to-rep spread is the rig's drift floor
# Each profile reaches rmw_fastrtps_cpp through FASTRTPS_DEFAULT_PROFILES_FILE as a default (is_default_profile)
# data_writer profile; rmw overwrites history, reliability and durability kinds from the ROS QoS and leaves
# max_blocking_time, resource_limits and the writer times alone (the profiles' headers have the detail).
# RMW_FASTRTPS_USE_QOS_FROM_XML and RMW_FASTRTPS_PUBLICATION_MODE are unset for every Fast DDS arm.
#
# Cells: Array1k 0% (the F3 question, and F1/F2's no-loss stall control), Array1k 5% and Array4k 5% (F0..F2).
# Every rep runs all arms in an order rotated per rep; the rig lock is held once for the whole run.
#
# Each run records its realized treatment: the profile's sha256 as copied to both Pis (checked against this
# checkout's file), and per side the middleware environment the launcher exported and /proc/PID/environ of
# perf_test itself at 2 s, with the hash of the profile file it names. The rmw library is identified from
# /proc/PID/maps, as rmw_keepall_rig.sh does. The reading rules, written before the run, are in
# fastdds_keepall_arms_summary.py's header and enforced there.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export RIG_LOCK_SCOPE=hil
if [ "${RIG_LOCK_HELD_HIL:-0}" != "1" ]; then exec "$REPO/examples/perf_hil/rig_lock.sh" "$0" "$@"; fi

export ARMS=${ARMS:-"fastdds@F0 fastdds@F1 fastdds@F2 fastdds@F3 cyclonedds"}
export CELLS=${CELLS:-"Array1k:0 Array1k:5 Array4k:5"}
export REPS=${REPS:-3} DUR=${DUR:-20}
export OUT=${OUT:-$HOME/rig_results_safe/fastdds_keepall_arms_$(date +%Y%m%d-%H%M%S)}
export SUMMARY=$REPO/examples/perf_hil/experiments/fastdds_keepall_arms_summary.py
case " $ARMS " in *" tickle@"*) echo "fastdds_keepall_arms.sh runs no rmw_tickle arm: ARMS=$ARMS" >&2; exit 64 ;; esac
exec "$REPO/examples/perf_hil/experiments/rmw_keepall_rig.sh"
