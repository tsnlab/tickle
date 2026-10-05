#!/usr/bin/env bash
# Keeps test_unsupported_entry_points.c honest about what rmw_tickle still refuses, and keeps
# rmw_unsupported.c's own reasons naming the right function.
#
# Why this gate exists (2026-09-28). g1 implemented rmw_publish_serialized_message and
# rmw_get_serialized_message_size, but the test that asserts entry points answer RMW_RET_UNSUPPORTED
# still named them, so `Check all` was red for three commits and nobody's local gate could see it:
# rmw_tickle's ctest suite needs the whole colcon workspace and a netns, so check-gates is core-only
# by design. That workspace is not coming to check-gates - but the defect class does not need it.
# "A test asserts an entry point is unsupported while the implementation no longer refuses it" is
# answerable from the two files' text, in a second, with no build at all.
#
# Two directions, because the mistake has two shapes:
#   1. every name passed to expect_unsupported() in the test is a function rmw_unsupported.c still
#      defines. A name that has left that file is an entry point now implemented, and the assertion
#      is stale - the failure this gate was written for.
#   2. every UNSUPPORTED("name") reason inside rmw_unsupported.c names the function it sits in. The
#      test matches the reason with strstr(), so a copy-pasted reason naming the neighbouring
#      function would make the test pass while the error message misleads a user.
#
# What it deliberately does not check: whether an entry point that is implemented actually works.
# That is the ctest suite's job, in CI. This gate only refuses a contradiction between the two files.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$HERE/rmw_tickle/rmw_tickle/src/rmw_unsupported.c"
TEST="$HERE/rmw_tickle/rmw_tickle/test/test_unsupported_entry_points.c"
status=0

for f in "$SRC" "$TEST"; do
    if [ ! -f "$f" ]; then
        echo "check-unsupported-list: FAIL missing $f"
        exit 1
    fi
done

# The functions rmw_unsupported.c defines, and the reason each one gives.
# sed, not a second grep for rmw_*: `rmw_ret_t rmw_set_log_severity(` contains two such names, and
# taking both put the return type `rmw_ret_t` into this list - which then matched the same stray name
# on the other side and made direction 1 pass whatever the test said. Checked by printing both lists.
defined=$(grep -oE '^[a-z_]+ rmw_[a-z0-9_]+\(' "$SRC" | sed -E 's/^[a-z_]+ //; s/\($//' | sort -u)
if [ -z "$defined" ]; then
    echo "check-unsupported-list: FAIL parsed no function definitions out of rmw_unsupported.c"
    exit 1 # a check that cannot fail is worse than no check: an empty list would pass direction 1 always
fi

# Direction 1: the test may only assert names this file still refuses.
# The name must be followed by '(' so this matches a call and not expect_unsupported()'s own
# declaration, whose first parameter is `rmw_ret_t ret`.
asserted=$(grep -oE 'expect_unsupported\(rmw_[a-z0-9_]+\(' "$TEST" | sed -E 's/^expect_unsupported\(//; s/\($//' | sort -u)
if [ -z "$asserted" ]; then
    echo "check-unsupported-list: FAIL parsed no expect_unsupported() calls out of the test"
    exit 1
fi
for name in $asserted; do
    if ! printf '%s\n' "$defined" | grep -qx "$name"; then
        echo "check-unsupported-list: FAIL $name is asserted UNSUPPORTED by the test but rmw_unsupported.c"
        echo "  no longer defines it - the entry point is implemented, so it must leave the test's list too,"
        echo "  and be counted as supported in docs/RMW.md's support table."
        status=1
    fi
done

# Direction 2: each UNSUPPORTED() reason names the function it is inside. awk walks the file keeping
# the last function name it saw, which is the enclosing one for every UNSUPPORTED() below it.
mismatch=$(awk '
    match($0, /^[a-z_]+ rmw_[a-z0-9_]+\(/) {
        line = $0
        sub(/^[a-z_]+ /, "", line)
        sub(/\(.*$/, "", line)
        current = line
    }
    match($0, /UNSUPPORTED\("rmw_[a-z0-9_]+"\)/) {
        reason = substr($0, RSTART, RLENGTH)
        gsub(/UNSUPPORTED\("|"\)/, "", reason)
        if (reason != current) { printf "  %s: reason says %s\n", current, reason }
    }
' "$SRC")
if [ -n "$mismatch" ]; then
    echo "check-unsupported-list: FAIL an UNSUPPORTED() reason names a different function than its own:"
    echo "$mismatch"
    echo "  The test matches the reason with strstr(), so this passes the test while misleading a user."
    status=1
fi

if [ "$status" = 0 ]; then
    echo "check-unsupported-list: $(printf '%s\n' "$asserted" | wc -l) asserted, $(printf '%s\n' "$defined" | wc -l) refused by rmw_unsupported.c, reasons all match"
fi
exit "$status"
