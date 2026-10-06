#!/usr/bin/env bash
# The FreeRTOS RISC-V link build, as CI's "Build - FreeRTOS RISC-V" step does it (test-all.yml:
# `make -C platform/freertos all`), so check-gates sees what only a 32-bit bare-metal link can see.
#
# Why this gate exists (2026-10-06): a commit added a 64-bit relaxed atomic store. On Linux x86-64 it is
# one instruction; on RV32 (rv32imac, picolibc, no libatomic) gcc lowers it to a call to __atomic_store_8,
# which nothing provides. Every local gate passed - they all build for the host - and only CI's FreeRTOS
# build failed, at link time. The syntax sweep (sweep_build_configs.sh) cannot see this either: it never
# links, and it compiles for the host.
#
# A from-scratch build every time, into a temporary directory: CI builds a clean checkout, and an object
# cache in the tree could hide a change make did not notice. It costs about two seconds at -j8, and
# writes nothing inside the checkout.
#
# Exit codes, which check_gates.sh maps to distinct verdicts ("I could not look" must not read as "it
# built"):
#   0   compiled and linked
#   77  no riscv64-unknown-elf-gcc / picolibc -> SKIP
#   78  the FreeRTOS/lwIP submodules are not checked out -> SKIP
#   1+  the build or the link failed -> FAIL
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CROSS="${CROSS:-riscv64-unknown-elf-}"

if ! command -v "${CROSS}gcc" >/dev/null 2>&1; then
    echo "build_freertos: ${CROSS}gcc not found - the FreeRTOS link build did NOT run"
    echo "build_freertos: install it as CI does:"
    echo "    sudo apt-get install gcc-riscv64-unknown-elf binutils-riscv64-unknown-elf picolibc-riscv64-unknown-elf"
    exit 77
fi
if ! "${CROSS}gcc" --specs=picolibc.specs -print-file-name=picolibc.specs >/dev/null 2>&1; then
    echo "build_freertos: picolibc for ${CROSS%-} not found - the FreeRTOS link build did NOT run"
    echo "    sudo apt-get install picolibc-riscv64-unknown-elf"
    exit 77
fi
for f in third_party/FreeRTOS-Kernel/include/FreeRTOS.h third_party/lwip/src/include/lwip/init.h; do
    if [ ! -f "$REPO/$f" ]; then
        echo "build_freertos: $f missing - the FreeRTOS link build did NOT run"
        echo "build_freertos: git submodule update --init third_party/FreeRTOS-Kernel third_party/lwip"
        exit 78
    fi
done

OUT="$(mktemp -d "${TMPDIR:-/tmp}/tickle-freertos-gate.XXXXXX")" || exit 90
# shellcheck disable=SC2329  # invoked by the EXIT trap below
cleanup() { rm -rf "$OUT"; }
trap cleanup EXIT # the only EXIT trap here - a second would silently replace it

jobs="$(nproc 2>/dev/null || echo 4)"
if ! make -s -C "$REPO/platform/freertos" -j"$jobs" CROSS="$CROSS" all \
    OBJ_DIR="$OUT/obj" ELF="$OUT/RTOSDemo-selftest-1.elf"; then
    echo "build_freertos: FAILED - CI's 'Build - FreeRTOS RISC-V' would fail the same way"
    exit 1
fi
# The step's own artefact, not make's exit status, is what says it linked.
[ -s "$OUT/RTOSDemo-selftest-1.elf" ] || {
    echo "build_freertos: make exited 0 but produced no ELF"
    exit 1
}
echo "build_freertos: linked $(stat -c %s "$OUT/RTOSDemo-selftest-1.elf") bytes"
exit 0
