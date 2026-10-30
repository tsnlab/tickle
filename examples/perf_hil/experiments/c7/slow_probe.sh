#!/usr/bin/env bash
# Builds slow_probe.c (C7) against this checkout's src/tickle.c into $OUT (default /home/semih/tmp_build/c7/slow_probe).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
out=${OUT:-/home/semih/tmp_build/c7/slow_probe}
mkdir -p "$(dirname "$out")"
cc ${CFLAGS_EXTRA:-} -I"$root/include" -I"$root/src" -O2 -g -w -o "$out" "$here/slow_probe.c" "$root/src/encoding.c" "$root/src/log.c" \
    -lpthread -lm
