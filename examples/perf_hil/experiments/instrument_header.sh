#!/usr/bin/env bash
# instrument_header.sh - which BenchStats.h a bench build of commit SHA compiles with: the driver checkout's or SHA's own.
#
# WHY. s6_witness_check.sh (and the harnesses copied from it) overwrite the checked-out commit's
# examples/perf_hil/tickle/common/BenchStats.h with the driver's copy, so an old commit prints the instrument fields
# added after it. That is only right when the driver's header is the NEWER one. ab_drain 2026-10-07 14:56 ran its
# driver at ebcb35b1 against arm C = 4c46f7cb, whose bench sources call bench_stats_set_shm_diagnostics() with the
# fifth argument (skipped_superseded) its own BenchStats.h adds: the driver's older header went over it, every C build
# failed on the rig in 2 s, and C-B read NO VERDICT with n=0 - while the PC preflight, which builds SHA's own tree,
# had passed. A header is not a measurement setting that may be made uniform across arms; it is source the arm's code
# was written against.
#
# RULE (printed as the first word):
#   driver  the driver's header is byte-identical to SHA's, or SHA's last change to it is an ancestor of the driver's
#           HEAD and the driver's copy is committed - the driver's header is SHA's or a later version of it
#   arm     anything else (SHA changed the header after the driver's HEAD, or on another branch, or the driver's copy
#           is uncommitted and differs): build with SHA's own header
# rig_preflight.sh applies the same rule to its export, so a commit whose bench does not compile against the header
# the rig will give it fails on the PC, before the rig lock.
#
# Usage: instrument_header.sh <repo> <sha>    exit 0 with "driver ..." or "arm ...", 2 if it cannot tell
set -uo pipefail
REPO=${1:?repo}
SHA=${2:?sha}
P=examples/perf_hil/tickle/common/BenchStats.h
arm_blob=$(git -C "$REPO" rev-parse -q --verify "$SHA:$P") || { echo "unknown: $SHA has no $P"; exit 2; }
drv_blob=$(git -C "$REPO" hash-object "$REPO/$P") || { echo "unknown: cannot hash the driver's $P"; exit 2; }
if [ "$arm_blob" = "$drv_blob" ]; then
    echo "driver identical to ${SHA:0:8}'s (blob ${drv_blob:0:8})"
    exit 0
fi
head_blob=$(git -C "$REPO" rev-parse -q --verify "HEAD:$P")
arm_last=$(git -C "$REPO" log -1 --format=%H "$SHA" -- "$P")
if [ "$head_blob" = "$drv_blob" ] && [ -n "$arm_last" ] && git -C "$REPO" merge-base --is-ancestor "$arm_last" HEAD; then
    echo "driver newer than ${SHA:0:8}'s (its last change ${arm_last:0:8} is in the driver's HEAD; blob ${drv_blob:0:8})"
    exit 0
fi
echo "arm ${SHA:0:8}'s own (blob ${arm_blob:0:8}; last change ${arm_last:0:8} is not in the driver's HEAD, or the" \
    "driver's copy is uncommitted)"
