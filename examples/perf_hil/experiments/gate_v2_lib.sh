#!/usr/bin/env bash
# gate_v2_lib.sh - sourced by every rig harness that A/Bs TickLE against TickLE or publishes a TickLE figure (gate v2,
# the user's decision of 2026-10-11; TESTING.md section 6.1). One definition of what those harnesses must agree on, so
# a reader cannot apply one arm's rule to another's run.
#
# A1  TICKLE_LAYOUT_CFLAGS: the layout-stable build. -falign-functions=64 puts every function on a 64-byte boundary,
#     so a function's code sits at the same offset in the Cortex-A76's 64-byte i-cache lines (and in the smaller
#     fetch blocks inside them) whatever precedes it, and so does every loop head and branch target inside it
#     (layout_flags_cost.sh Q2: 0 functions change with a perturbation ahead of them, against 2 under the default
#     32:16). Not the product default: it costs core .text +4.6% and the client +6.7% on aarch64 (Q3), and the rule
#     was "only if it costs nothing measurable". It is the harness default; an explicit TICKLE_EXTRA_CFLAGS (even
#     empty, for the plain build) overrides it.
# A1' TICKLE_NOPAD_CFLAGS (gate v3, 2026-10-12): no padding INSIDE a function. Padding before a function entry is
#     never executed; padding at a loop head, jump target or label is, on every pass. The Pis' gcc 13 aligns loops to
#     8 by default (gcc 15: 32:16), so a change that moves a loop head by 4 bytes adds or removes an executed nop per
#     iteration - an instruction count that moved with layout, not with what the code computes. -falign-loops=1
#     removes every in-body nop of the core (tickle.c 75 -> 0, hal_linux.c 7 -> 0, encoding.c 1 -> 0 under gcc 15
#     with the Pis' defaults); -falign-jumps=1 and -falign-labels=1 are no-ops on aarch64 today and keep it so under
#     another tuning. Placement alone (64/128/256) already left every function body identical, the nops included
#     (aarch64_insn/layout_qemu.sh, executed under qemu: 0 nops a sample with these flags, 3-10 without, equal at
#     64/128/256 either way). Added to the layout and the placement flags.
TICKLE_NOPAD_CFLAGS="-falign-loops=1 -falign-jumps=1 -falign-labels=1"
# A2  insn_prefix / insn_fields: executed instructions per process, counted by perf stat as the parent of the bench
#     (perf 6.8 on the Pis, paranoid 1: instructions:u and instructions:k of one's own processes work; tracepoints do
#     not). Appended to the process's RESULT line as insn_u=<n> insn_k=<n> insn=ok, or insn=fail:<why>; the readers
#     divide by the process's samples (sent, recv) or the round trips (latency) and decide on that first. Counting mode
#     only (no sampling), so the instrument costs both arms the same PMU save/restore per context switch.
# A3  TICKLE_PLACEMENT_CFLAGS: arm P (A') - A's commit rebuilt with every function moved by a multiple of 64 bytes and
#     nothing else changed (layout_flags_cost.sh: F128 vs F64 - the same instruction sequences, 0 in-function changes
#     under perturbation). It perturbs exactly what A1 cannot fix and what a real change moves under A1: the address bits
#     above 6 (i-cache set, i-TLB page, branch predictor index). |P - A| is each metric's placement floor (gate_v2.py).
TICKLE_LAYOUT_CFLAGS="-falign-functions=64 $TICKLE_NOPAD_CFLAGS"
# shellcheck disable=SC2034 # read by the scripts that source this file
TICKLE_PLACEMENT_CFLAGS="-falign-functions=128 $TICKLE_NOPAD_CFLAGS"
INSN_EVENTS="instructions:u,instructions:k"

# layout_cflags_default: sets TICKLE_EXTRA_CFLAGS to TICKLE_LAYOUT_CFLAGS when it is UNSET; a set value (also empty)
# is the caller's explicit choice and is kept. Call before the harness records and checks its flags.
layout_cflags_default() {
    if [ -z "${TICKLE_EXTRA_CFLAGS+set}" ]; then TICKLE_EXTRA_CFLAGS=$TICKLE_LAYOUT_CFLAGS; fi
    export TICKLE_EXTRA_CFLAGS
}

# insn_prefix <file>: the command words that count the next command's instructions into <file> (on the Pi).
insn_prefix() { printf 'perf stat -x, -o %s -e %s --' "$1" "$INSN_EVENTS"; }

# insn_fields <perf -x, output>: "insn_u=<n> insn_k=<n> insn=ok", or "insn=fail:<why>" when either count is absent,
# not counted, not supported or zero - a zero instrument is "could not look", never "no work".
insn_fields() {
    local u k
    u=$(printf '%s\n' "$1" | awk -F, '$3 ~ /^instructions:u/ && $1 ~ /^[0-9]+$/ { print $1; exit }')
    k=$(printf '%s\n' "$1" | awk -F, '$3 ~ /^instructions:k/ && $1 ~ /^[0-9]+$/ { print $1; exit }')
    if [ -z "$u" ] && [ -z "$k" ]; then
        if [ -z "$1" ]; then echo "insn=fail:no_output"; else echo "insn=fail:unparsed"; fi
    elif [ -z "$u" ] || [ "$u" = 0 ]; then
        echo "insn=fail:user"
    elif [ -z "$k" ] || [ "$k" = 0 ]; then
        echo "insn=fail:kernel"
    else
        echo "insn_u=$u insn_k=$k insn=ok"
    fi
}

# remote_child_of_perf: a shell fragment (for an ssh command line) defining child_exe <pid> <exe glob>, which prints the
# PID of <pid> itself or of its direct child whose /proc/<pid>/exe matches the glob. Under perf stat the PID a launch
# records is perf's; the bench is perf's child, and is what must be signalled.
remote_child_of_perf() {
    # shellcheck disable=SC2016 # expanded on the Pi
    printf '%s' 'child_exe() { case "$(readlink /proc/$1/exe 2>/dev/null)" in $2) echo "$1"; return 0 ;; esac
for c in $(cat /proc/$1/task/*/children 2>/dev/null); do case "$(readlink /proc/$c/exe 2>/dev/null)" in $2) echo "$c"; return 0 ;; esac; done
return 1; }'
}
