#!/usr/bin/env bash
# rx_rows_inplace_build.sh - builds one arm of rx_rows_inplace_pc.sh from THIS checkout: best_effort_throughput p1 and
# p2 and reliable_throughput p2, with the shared-memory segment compiled out (-Dtt_SEGMENT_ENABLED=0) so a same-PC pair
# takes the UDP receive path a cross-host pair takes. The install prefixes go under a private HOME, never the shared
# ~/tickle_local_install*, and the binaries are copied to ARM_DIR/<cell>/ so a rebuild cannot change an arm under a run.
#
# Usage: rx_rows_inplace_build.sh ARM_DIR
set -euo pipefail
ARM=${1:?usage: rx_rows_inplace_build.sh ARM_DIR}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PERF="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$PERF/../.." && pwd)"
mkdir -p "$ARM/home"
ARM="$(cd "$ARM" && pwd)"
for cell in best_effort_throughput:p1 best_effort_throughput:p2 reliable_throughput:p2; do
    scen=${cell%%:*} shape=${cell##*:}
    HOME="$ARM/home" TICKLE_EXTRA_CFLAGS="-Dtt_SEGMENT_ENABLED=0" "$PERF/tickle/build.sh" "$scen" "$shape" >"$ARM/build_${scen}_$shape.log" 2>&1
    mkdir -p "$ARM/${scen}_$shape"
    cp "$PERF/tickle/${scen}_$shape/client" "$PERF/tickle/${scen}_$shape/server" "$ARM/${scen}_$shape/"
done
(cd "$REPO" && git rev-parse HEAD && git status --short -- src include platform) >"$ARM/source.txt"
echo "BUILD_DONE $ARM"
