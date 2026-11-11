#!/usr/bin/env bash
# Build the benchmark binaries the T1 scripts run (recipe of t1_depth.sh / t1_s0.sh), from this repository's tree.
#   t1_build_node.sh [OUTDIR]   (default T1_DIR = $HOME/tmp_build/wm/t1)
# node_d100: wifi_link_node.c with writer history depth 100 (the sed inserts one line before `return qos;`).
# node_base: the same source unmodified (depth 10). large: large_sample_bench.c with 64 KiB-capable buffers (rel_large cell).
# A/B arms (e.g. node_evict_d100) are built the same way from the candidate's tree: W=/path/to/tree T1_BIN_SUFFIX=_evict_d100.
set -eu
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
W=${W:-$(cd "$S/../../../.." && pwd)}
T=${1:-${T1_DIR:-$HOME/tmp_build/wm/t1}}
SUF=${T1_BIN_SUFFIX:-_d100}
E=$W/examples/perf_hil/experiments
mkdir -p "$T"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
SRC="$W/src/tickle.c $W/src/encoding.c $W/src/log.c $W/src/hal_linux.c"
sed 's/^    return qos;/    qos.history.depth = 100;\n    return qos;/' "$E/wifi_link_node.c" >"$tmp/wifi_link_node_d100.c"
cmp -s "$E/wifi_link_node.c" "$tmp/wifi_link_node_d100.c" && { echo "VOID: sed changed nothing"; exit 1; }
# SRC is a list of paths without spaces, split on purpose.
# shellcheck disable=SC2086
cc -O2 -DNDEBUG -Dtt_SEGMENT_ENABLED=0 -Dtt_RELIABLE_STATS -I"$W/include" -I"$W/src" -I"$W/examples/perf_hil/tickle/common" \
    -o "$T/node$SUF" "$tmp/wifi_link_node_d100.c" $SRC -lpthread -lm
if [ "$SUF" = _d100 ]; then
    # shellcheck disable=SC2086
    cc -O2 -DNDEBUG -Dtt_SEGMENT_ENABLED=0 -Dtt_RELIABLE_STATS -I"$W/include" -I"$W/src" -I"$W/examples/perf_hil/tickle/common" \
        -o "$T/node_base" "$E/wifi_link_node.c" $SRC -lpthread -lm
fi
# shellcheck disable=SC2086
cc -O2 -DNDEBUG -Wall -Wextra -Dtt_MAX_BUFFER_LENGTH=65507 -Dtt_LARGE_SAMPLES=1 -Dtt_SEGMENT_ENABLED=0 -Dtt_RELIABLE_STATS \
    -I"$W/include" -I"$W/src" -o "$T/large" "$E/large_sample_bench.c" $SRC -lpthread -lm
echo "BUILD_OK $T (tree $W)"
