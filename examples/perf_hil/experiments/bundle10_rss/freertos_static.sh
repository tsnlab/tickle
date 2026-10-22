#!/usr/bin/env bash
# freertos_static.sh - bundle10_rss on FreeRTOS: everything linked into an MCU image is resident, so the measure is
# static RAM. Builds the four example roles (publisher, subscriber, server, client) for RV32 as CI's
# build_freertos.sh does (make -C platform/freertos, default flags), then per image:
#   OUT/<role>.syms     riscv64-unknown-elf-nm -S of every .bss/.data object of at least 1 KiB
#   OUT/<role>.layout   layout.py over the ELF: tt_Context, tt_Server, tt_Client, tt_Publisher, tt_Subscriber fields
#   OUT/<role>.size     section totals (size -A)
# freertos_static.py reads them, with the 4 KiB rule.
#
# The worktree has no FreeRTOS/lwIP submodules checked out, so the build runs from a `git archive` of HEAD with
# third_party/FreeRTOS-Kernel and third_party/lwip linked from SUBMODULES (default: the main checkout's). They only
# supply the kernel and the IP stack; every TickLE size comes from HEAD.
#
# Usage: OUT=<dir> [SUBMODULES=/home/semih/tickle/third_party] freertos_static.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../../.." && pwd)"
OUT=${OUT:?output directory}
SUBMODULES=${SUBMODULES:-/home/semih/tickle/third_party}
CROSS=riscv64-unknown-elf-
mkdir -p "$OUT"
SRC=$OUT/src
rm -rf "$SRC"
mkdir -p "$SRC"
git -C "$REPO" archive HEAD | tar -x -C "$SRC"
for m in FreeRTOS-Kernel lwip; do
    rm -rf "${SRC:?}/third_party/$m"
    ln -s "$SUBMODULES/$m" "$SRC/third_party/$m"
done
git -C "$REPO" rev-parse HEAD >"$OUT/head.txt"
for role in publisher subscriber server client; do
    elf=$OUT/$role.elf
    TMPDIR=/home/semih/tmp_build nice -n 10 make -s -C "$SRC/platform/freertos" -j2 CROSS="$CROSS" ROLE="$role" all \
        OBJ_DIR="$OUT/obj-$role" ELF="$elf"
    "${CROSS}nm" -S --size-sort --radix=d "$elf" | awk '$3 ~ /^[bBdD]$/ && $2 + 0 >= 1024 {print $2 + 0, $3, $4}' \
        >"$OUT/$role.syms"
    "${CROSS}size" -A "$elf" >"$OUT/$role.size"
    RT_TYPES="struct tt_Context;struct tt_Server;struct tt_Client;struct tt_Publisher;struct tt_Subscriber" \
        RT_EXPAND="struct tt_hal" RT_OUT="$OUT/$role.layout" gdb -batch -nx -x "$HERE/layout.py" "$elf"
done
echo FREERTOS_STATIC_DONE
