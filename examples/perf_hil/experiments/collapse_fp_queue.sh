#!/bin/bash
# Runs collapse_fp_sweep.sh once per line of <jobs> (its arguments, whitespace-separated; lines may set environment
# as NAME=value words first), one after the other, and prints "CFP_QUEUE_DONE" last whatever happened.
#
#   collapse_fp_queue.sh <jobs>
set -u
here=$(dirname "$0")
trap 'echo CFP_QUEUE_DONE' EXIT
while read -r -a words; do
    [ "${#words[@]}" -eq 0 ] && continue
    envs=()
    while [[ ${words[0]} == *=* ]]; do
        envs+=("${words[0]}")
        words=("${words[@]:1}")
    done
    echo "CFP_JOB ${envs[*]} ${words[*]} $(date +%T)"
    env "${envs[@]}" "$here/collapse_fp_sweep.sh" "${words[@]}"
done < "$1"
