#!/usr/bin/env bash
# Does the network-only model for the hybrid large-sample transport hold? (user question 2026-10-11)
#
# Model under test: a UDP datagram carrying n units of u = 1446 B survives frame loss p with
# q(n) = (1-p)^n; kernel IP reassembly (ipfrag_time, ipfrag_high_thresh) can push q below that at
# high rate and loss; the RELIABLE wire cost per delivered unit is
#   cost(n) = (u + h/n) + (1 - q(n)) * (u + h),  h = 24 B,  minimum near n* = sqrt(h / (u p)).
#
# No TickLE code runs here. frag_reasm_probe.c is a paced UDP sender (netns A) and a per-sequence
# receiver (netns B) over one veth, with netem loss on A's egress. Both namespaces are created fresh
# for every cell (so no reassembly queue carries over) with their DEFAULT ipfrag sysctls, which are
# read and recorded, never changed. Every witness is read harness-side from the namespaces' /proc.
#
# The pre-registered predictions P0-P4, the VOID rules, the sample-size rule and the cell reductions
# are in the header of frag_reasm_model.py and implemented in its code. Read that header first.
#
# Usage: frag_reasm_model.sh [--plan | --smoke]   (no argument = the full campaign)
#   --plan   calibrate fragment memory, print the cell plan, sample sizes and time estimate, exit
#   --smoke  a handful of short cells to check the plumbing (results are not a measurement)
# Results: $FQ_OUT_DIR (default ~/rig_results_safe/frag_reasm_model_<timestamp>/results.txt).
# Long: run it detached, e.g.
#   setsid nohup examples/perf_hil/experiments/frag_reasm_model.sh > ~/rig_results_safe/fq.log 2>&1 < /dev/null &
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TS=$(date +%Y%m%d_%H%M%S)
export FQ_OUT_DIR=${FQ_OUT_DIR:-$HOME/rig_results_safe/frag_reasm_model_$TS}
export FQ_NS_A=fqA_$$ FQ_NS_B=fqB_$$
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}
mkdir -p "$FQ_OUT_DIR" "$TMPDIR"

cleanup() {
    sudo -n ip netns del "$FQ_NS_A" 2>/dev/null
    sudo -n ip netns del "$FQ_NS_B" 2>/dev/null
    # Check the world, not the exit status: sudo -n can fail silently.
    if ip netns list | awk '{print $1}' | grep -Fx -e "$FQ_NS_A" -e "$FQ_NS_B"; then
        echo "CLEANUP FAILED: namespace(s) above still exist"
    else
        echo "cleanup: $FQ_NS_A and $FQ_NS_B are gone"
    fi
}
trap cleanup EXIT

export FQ_PROBE=$FQ_OUT_DIR/frag_reasm_probe
cc -O2 -Wall -Wextra -o "$FQ_PROBE" "$HERE/frag_reasm_probe.c" || { echo "BUILD FAILED"; exit 1; }
echo "harness_sha=$(git -C "$HERE" rev-parse HEAD) dirty=$(git -C "$HERE" status --porcelain -- . | wc -l)"
python3 -u "$HERE/frag_reasm_model.py" "$@"
echo "DRIVER_EXIT $?"
