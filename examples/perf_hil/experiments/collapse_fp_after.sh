#!/bin/bash
# Waits until <verdicts> ends a sweep (its CFP_SWEEP_DONE line), at most <max_s> seconds, then runs
# collapse_fp_queue.sh <jobs>: the next queue starts on the same core when the last one ends, not on a guess.
#
#   collapse_fp_after.sh <verdicts> <jobs> <max_s>
set -u
here=$(dirname "$0")
deadline=$(( $(date +%s) + $3 ))
until grep -q CFP_SWEEP_DONE "$1" 2>/dev/null; do
    if [ "$(date +%s)" -ge "$deadline" ]; then
        echo "CFP_AFTER_TIMEOUT $1"
        echo CFP_QUEUE_DONE
        exit 1
    fi
    sleep 15
done
exec "$here/collapse_fp_queue.sh" "$2"
