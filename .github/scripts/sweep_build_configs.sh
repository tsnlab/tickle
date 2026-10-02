#!/usr/bin/env bash
# Does src/tickle.c still compile in every configuration this repository supports?
#
# check-gates builds ONE configuration. On 2026-10-03 a commit passed 13 of 13 gates and broke
# -Dtt_SEGMENT_ENABLED=0: a diagnostic helper and its four call sites were written outside the
# tt_SEGMENT_ENABLED guard while the field they touch stays inside it. The rig found it in nine
# seconds and CI failed four jobs on it - Build - FreeRTOS RISC-V, Integration - Linux HAL over
# netns with shared memory OFF, and Integration - FreeRTOS HAL over QEMU - because FreeRTOS builds
# with segments off too, so a change that read as segment-local was not. A green table had said
# nothing whatever about the other configurations.
#
# -fsyntax-only on purpose: it writes no object, so this cannot disturb a build running beside it
# and cannot leave a stale artefact for the next one. It catches the class that bit us - code
# referring to things a preprocessor branch removed - and makes no claim about behaviour.
#
# Exit 0 all clean, 1 any configuration failed, 77 no compiler to sweep with (SKIP, not PASS: "I
# could not look" must not read as "they all build").
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CC_BIN="${CC:-gcc}"
command -v "$CC_BIN" >/dev/null 2>&1 || exit 77

# Each entry is one configuration something in this repository actually builds. Keep the comment
# saying WHO builds it, so a configuration that stops being used can be removed rather than guessed at.
CONFIGS=(
    ""                                              # the default library, and every unit test
    "-Dtt_SEGMENT_ENABLED=0"                        # CI "shared memory OFF (control)"; all FreeRTOS builds
    "-Dtt_FRAG_ENABLED=0"                           # builds whose samples all fit one datagram
    "-Dtt_MAX_SAMPLE_LENGTH=4096"                   # perf_hil p4, sample_path=frag
    "-Dtt_MAX_BUFFER_LENGTH=4096 -Dtt_FRAG_ENABLED=0" # perf_hil p4, sample_path=ipfrag
    "-Dtt_SEGMENT_SLOT_BYTES=4096"                  # whole_record_refusal.sh and the s6 slot arms
    "-Dtt_LOCAL_DELIVERY=1"                         # rmw_tickle
    "-Dtt_CONTEXT_ID_CLAIM=1"                       # rmw_tickle
)

rc=0
for cfg in "${CONFIGS[@]}"; do
    label="${cfg:-(defaults)}"
    # Unquoted on purpose: each entry is a list of flags, not one argument.
    # shellcheck disable=SC2086
    if out=$("$CC_BIN" -fsyntax-only -I"$REPO/include" -I"$REPO/src" -Wall -Wextra $cfg "$REPO/src/tickle.c" 2>&1); then
        printf 'config-sweep: OK    %s\n' "$label"
    else
        printf 'config-sweep: BREAK %s\n' "$label"
        printf '%s\n' "$out" | head -6 | sed 's/^/    /'
        rc=1
    fi
done
exit "$rc"
