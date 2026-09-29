#!/usr/bin/env python3
"""Mutants for .github/scripts/check_context_reset.py - does the gate actually fail?

A gate nobody has watched fail is not a gate. This one exists because of `perf_server received N (need >= 5)`, which
could not fail against a run that normally delivers 5.3 million, and because of a pre-registered VOID rule that
required `transport=tcp` and was satisfied by a dead session that had printed `transport=tcp`. Both were written by
people who believed they had added a check. So this file mutates the sources the gate reads and asserts that each
mutant is caught - and, just as importantly, that the two controls are NOT flagged, since a check that fires on
everything is as useless as one that fires on nothing.

Run: python3 tests/mutants_check_context_reset.py
"""
import importlib.util
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("ccr", ROOT / ".github/scripts/check_context_reset.py")
ccr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ccr)

HDR = (ROOT / "include/tickle/tickle.h").read_text()
SRC = (ROOT / "src/tickle.c").read_text()
KNOWN = set(ccr.SURVIVES_RESET) | ccr.UNREVIEWED


def missing(header, source):
    touched = ccr.reset_touches(source)
    return [f for f in ccr.context_fields(header) if f not in touched and f not in KNOWN]


def main():
    failures = []

    def check(name, condition, detail=""):
        print("  %-62s %s" % (name, "ok" if condition else "FAILED " + detail))
        if not condition:
            failures.append(name)

    print("check_context_reset mutants:")

    # 1. The bug itself, twice over: a field added to the struct and to nothing else.
    mutated = HDR.replace("struct tt_Context {", "struct tt_Context {\n    uint64_t mutant_never_reset;", 1)
    check("new struct field, never reset -> caught", "mutant_never_reset" in missing(mutated, SRC))

    # 2. A field that IS reset today loses its assignment.
    touched = ccr.reset_touches(SRC)
    victim = next(f for f in ccr.context_fields(HDR) if f in touched and "segment" not in f and not f.startswith("rx_"))
    stripped = re.sub(r"^\s*node->%s\s*=.*$" % re.escape(victim), "", SRC, flags=re.M)
    check("existing field loses its reset -> caught (%s)" % victim, victim in missing(HDR, stripped))

    # 3. Control: the same field on unmutated source must NOT be flagged.
    check("control: same field unmutated -> not flagged", victim not in missing(HDR, SRC))

    # 4. The guard rule: written only behind a flag, read when the flag is off.
    check("written under #if, read outside it -> caught", bool(ccr.guard_mismatches("""
#if tt_SOME_FLAG
static void setit(struct tt_Context* node) { node->guarded_demo = 1; }
#endif
static void useit(struct tt_Context* node) { if (node->guarded_demo) { return; } }
""", {"guarded_demo"})))

    # 5. Control for the guard rule. This is the case Dev's first formulation ("fail on any flag-gated assignment")
    # would have rejected, and it is correct code: the tt_CONTEXT_ID_CLAIM family is written by claim_initial_id()
    # under the flag and every read of it is under the same flag, so with the flag off it is neither written nor read.
    check("written and read under the same #if -> not flagged", not ccr.guard_mismatches("""
#if tt_SOME_FLAG
static void setit(struct tt_Context* node) { node->guarded_ok = 1; }
static void useit(struct tt_Context* node) { if (node->guarded_ok) { return; } }
#endif
""", {"guarded_ok"}))

    # 6. Control: the real tt_CONTEXT_ID_CLAIM family in the real source must not trip the guard rule.
    real = ccr.guard_mismatches(SRC, set(ccr.context_fields(HDR)))
    check("control: real source has no guard mismatch", not real, str(real))

    if failures:
        print("\n%d mutant(s) survived: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("\nall mutants died, both controls held")
    return 0


if __name__ == "__main__":
    sys.exit(main())
