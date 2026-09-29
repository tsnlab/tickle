#!/usr/bin/env bash
# S8's control arm: run the Linux integration suite with the shared-memory module COMPILED OUT.
#
# WHY THIS ARM EXISTS. The module is on by default, so CI already runs the suite with it on. What was missing on
# 2026-09-29 was the other half: for thirteen commits the suite was red, and nothing in CI could say whether the
# module was the cause. It was - a 97.6% delivery collapse and an rclcpp segfault, both from the module - and both
# were diagnosed by a human building the same tree with the module out and counting the difference. That is a control
# arm, and a control arm that only exists in someone's terminal is not one.
#
# So this runs the identical suite with -Dtt_SEGMENT_ENABLED=0, and the next time the suite goes red the two arms
# together say immediately whether the module is implicated. RMW_GAPS_PLAN.md S8: "the suite RUNS, not only compiles,
# with the feature on as well as off."
#
# THE IDENTITY CHECK COMES FIRST, and it is the part that makes the arm worth having. A control arm whose flag never
# reached the compiler is indistinguishable from one that ran and found nothing - which is the defect that let a
# ThreadSanitizer gate pass for the shared-memory module's entire life while never once creating a segment, and the
# defect that let a zenoh cell be measured against a subscriber that had already exited. Both were caught by checking
# that the instrument saw something. Here the check is: build the library twice, with and without the flag, and refuse
# to run if the two are byte-identical.
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
FLAG=${FLAG:--Dtt_SEGMENT_ENABLED=0}
LIB=platform/linux/libtickle.a

hash_of() { sha256sum "$LIB" 2>/dev/null | cut -c1-16; }

echo "== $0: identity check before the run"
make -s -C platform/linux clean >/dev/null 2>&1
make -s -C platform/linux library >/dev/null 2>&1 || { echo "FATAL: the default library build failed"; exit 1; }
on=$(hash_of)
make -s -C platform/linux clean >/dev/null 2>&1
make -s -C platform/linux library CPPFLAGS="$FLAG" >/dev/null 2>&1 || { echo "FATAL: the $FLAG library build failed"; exit 1; }
off=$(hash_of)
echo "   module on:  ${on:-<no library>}"
echo "   module off: ${off:-<no library>}"
if [ -z "$on" ] || [ -z "$off" ]; then
    echo "FATAL: one of the builds produced no library, so nothing was compared."
    exit 1
fi
if [ "$on" = "$off" ]; then
    echo "FATAL: the two libraries are byte-identical, so '$FLAG' never reached the compiler."
    echo "  Refusing to run: this arm would report 'the module is not the cause' having never compiled it out,"
    echo "  which is the failure mode it exists to prevent."
    exit 1
fi
echo "   the flag reached the compiler (hashes differ), so this arm is a real control"

echo "== $0: running the suite with the module compiled out"
make -C platform/linux clean >/dev/null 2>&1
make test-linux CPPFLAGS="$FLAG"
suite_rc=$?

# And a check AFTER the suite, because the one above is not sufficient. platform/linux/test.sh re-invokes make for
# each example, and a flag passed to the outer make being silently dropped by an inner one is exactly how
# `CFLAGS=-fsanitize=address` reached nothing on 2026-09-29 - a sanitizer that was never linked, whose silence read
# as a clean run. So: if the library on disk is no longer the one the flag produced, some sub-make rebuilt it without
# the flag and the suite that just "passed with the module off" ran with the module on.
after=$(hash_of)
echo "== $0: library after the suite: ${after:-<no library>} (expected the module-off build, $off)"
if [ "$after" != "$off" ]; then
    echo "FATAL: the library changed during the run, so the suite did not run against the module-off build."
    echo "  A sub-make rebuilt it without '$FLAG'. This arm's result says nothing and is reported as a failure"
    echo "  rather than as a pass, because a control that silently became the experiment is worse than no control."
    exit 1
fi
echo "   unchanged, so the suite ran against the module-off library"
exit "$suite_rc"
