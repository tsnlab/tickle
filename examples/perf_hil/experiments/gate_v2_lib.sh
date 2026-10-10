#!/usr/bin/env bash
# gate_v2_lib.sh - sourced by every rig harness that A/Bs TickLE against TickLE or publishes a TickLE figure (gate v2,
# the user's decision of 2026-10-11; TESTING.md section 6.1). One definition of what those harnesses must agree on, so
# a reader cannot apply one arm's rule to another's run.
#
# A1  TICKLE_LAYOUT_CFLAGS: the layout-stable build. -falign-functions=64 puts every function on a 64-byte boundary,
#     so a function's code sits at the same offset in the Cortex-A76's 64-byte i-cache lines (and in the smaller
#     fetch blocks inside them) whatever precedes it, and so does every loop head and branch target inside it
#     (layout_flags_cost.sh Q2: 0 functions change with a perturbation ahead of them, against 2 under the default
#     32:16). -falign-loops is not added: it fixes nothing more (Q2) and adds executed nops (133 -> 1,242 in-body nops),
#     which would put layout into the instruction count gate v2 decides on; -falign-jumps on aarch64 is 4, the
#     instruction size, already a no-op. Not the product default: it costs core .text +4.6% and the client +6.7% on
#     aarch64 (Q3), and the rule was "only if it costs nothing measurable". It is the harness default; an explicit
#     TICKLE_EXTRA_CFLAGS (even empty, for the plain build) overrides it.
TICKLE_LAYOUT_CFLAGS="-falign-functions=64"

# layout_cflags_default: sets TICKLE_EXTRA_CFLAGS to TICKLE_LAYOUT_CFLAGS when it is UNSET; a set value (also empty)
# is the caller's explicit choice and is kept. Call before the harness records and checks its flags.
layout_cflags_default() {
    if [ -z "${TICKLE_EXTRA_CFLAGS+set}" ]; then TICKLE_EXTRA_CFLAGS=$TICKLE_LAYOUT_CFLAGS; fi
    export TICKLE_EXTRA_CFLAGS
}
