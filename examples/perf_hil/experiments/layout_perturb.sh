#!/usr/bin/env bash
# layout_perturb.sh <tree> <name> - apply one layout-only perturbation to an extracted source tree, for the harnesses
# that ask whether a count moves with layout (srv_gap_pc.sh arms "label:commit:flavor:env:<name>"; layout_identity.sh).
# Each perturbation adds bytes the program never executes or reads, so the work it does is unchanged and only where
# code or data sits moves - what an unrelated change does to the code it does not touch.
#   T1  24 bytes of text ahead of every function of src/tickle.c (a top-level asm statement: gcc emits it before the
#       unit's functions; a C function put first is reordered and perturbs nothing - layout_flags_cost.sh).
#   T2  the same ahead of src/hal_linux.c's functions (only the HAL, and what links after it, moves).
#   T3  the same ahead of the bench's own client.c and server.c (linked first: every core function moves).
#   D   24-byte .data and .bss arrays ahead of everything in tickle.c, hal_linux.c and the bench sources: static
#       data moves, code does not (the code's references to it change their displacement only).
#   R   a real C function, never called, in the middle of tickle.c (after the first function that ends past line
#       10000): what a change adding code there does, inlining heuristics included (the unit grows).
# The marker "tt_layout_pert" is in every perturbed file; the caller can grep for it. Exit 1 on an unknown name or a
# perturbation that did not land.
set -euo pipefail
tree=${1:?tree} name=${2:?name}
scen_srcs() { ls "$tree"/examples/perf_hil/tickle/{reliable_latency,reliable_throughput,best_effort_throughput}/{client,server}.c; }
text_pad='__asm__(".text\n\t.skip 24, 0x90"); /* tt_layout_pert */'
data_pad='__attribute__((used)) static volatile unsigned char tt_layout_pert_d[24] = {1};
__attribute__((used)) static unsigned char tt_layout_pert_b[24];'
prepend() { # <file> <text>: put the text first in the file (it needs no header; an #include can sit inside an #if)
    { printf '%s\n' "$2"; cat "$1"; } >"$1.pert" && mv "$1.pert" "$1"
}
case $name in
    T1) prepend "$tree/src/tickle.c" "$text_pad" ;;
    T2) prepend "$tree/src/hal_linux.c" "$text_pad" ;;
    T3) for f in $(scen_srcs); do prepend "$f" "$text_pad"; done ;;
    D)
        for f in "$tree/src/tickle.c" "$tree/src/hal_linux.c" $(scen_srcs); do prepend "$f" "$data_pad"; done
        ;;
    R)
        n=$(awk 'NR > 10000 && /^}$/ { print NR; exit }' "$tree/src/tickle.c")
        [ -n "$n" ] || { echo "no function end past line 10000" >&2; exit 1; }
        awk -v n="$n" '{ print } NR == n {
            print "uint32_t tt_layout_pert_hash(const uint8_t* p, size_t n); /* tt_layout_pert */"
            print "uint32_t tt_layout_pert_hash(const uint8_t* p, size_t n) {"
            print "    uint32_t h = 2166136261U;"
            print "    for (size_t i = 0; i < n; i++) {"
            print "        h = (h ^ p[i]) * 16777619U;"
            print "    }"
            print "    return h;"
            print "}" }' "$tree/src/tickle.c" >"$tree/src/tickle.c.pert" && mv "$tree/src/tickle.c.pert" "$tree/src/tickle.c"
        ;;
    *) echo "unknown perturbation '$name' (T1 T2 T3 D R)" >&2; exit 1 ;;
esac
grep -rqs tt_layout_pert "$tree/src" "$tree/examples/perf_hil/tickle" || { echo "perturbation $name did not land" >&2; exit 1; }
