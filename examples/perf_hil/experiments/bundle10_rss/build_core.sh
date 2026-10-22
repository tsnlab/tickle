#!/usr/bin/env bash
# build_core.sh - the Linux default core build bundle10_rss measures (core_host, -O2 -DNDEBUG as `make library`
# BUILD_TYPE=release, plus -g for layout.py), the LD_PRELOAD instrument, and both layouts:
#   OUT/core_host, OUT/layout_core.txt     tt_Context, tt_Server, tt_Client, tt_Publisher, tt_Subscriber (core)
#   OUT/rss_tables_preload.so
#   OUT/layout_rmw.txt                     rmw_tickle_context_impl_t from WS's librmw_tickle.so (build_rmw.sh)
# Usage: [OUT=<dir>] [WS=<dir>] build_core.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../../.." && pwd)"
OUT=${OUT:-/home/semih/tmp_build/b10rss}
WS=${WS:-$OUT/ws}
mkdir -p "$OUT"
export TMPDIR=/home/semih/tmp_build
nice -n 10 gcc -O2 -DNDEBUG -g -Wall -Wextra -I"$REPO/include" -I"$REPO/src" -I"$REPO/examples/uint64" \
    -I"$REPO/examples/set_bool" -I"$HERE" -o "$OUT/core_host" "$HERE/core_host.c" "$REPO/src/tickle.c" \
    "$REPO/src/encoding.c" "$REPO/src/log.c" "$REPO/src/hal_linux.c" "$REPO/examples/uint64/UInt64.c" \
    "$REPO/examples/set_bool/SetBool.c" -lm -lpthread
nice -n 10 gcc -O2 -g -Wall -shared -fPIC -o "$OUT/rss_tables_preload.so" "$HERE/rss_tables_preload.c" -lpthread
RT_TYPES="struct tt_Context;struct tt_Server;struct tt_Client;struct tt_Publisher;struct tt_Subscriber" \
    RT_EXPAND="struct tt_hal" RT_OUT="$OUT/layout_core.txt" gdb -batch -nx -x "$HERE/layout.py" "$OUT/core_host"
if [ -f "$WS/install/rmw_tickle/lib/librmw_tickle.so" ]; then
    RT_TYPES="struct rmw_tickle_context_impl_t" RT_EXPAND="struct tt_Context;struct tt_Discovery;struct tt_hal" \
        RT_OUT="$OUT/layout_rmw.txt" gdb -batch -nx -x "$HERE/layout.py" "$WS/install/rmw_tickle/lib/librmw_tickle.so"
fi
echo BUILD_CORE_DONE
