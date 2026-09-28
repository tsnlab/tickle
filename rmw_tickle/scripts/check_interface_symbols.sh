#!/usr/bin/env bash
# check_interface_symbols.sh - no two interface packages may define the same symbol.
#
# Why (RMW_GAPS_PLAN.md g12, found 2026-09-28): the generator named a message's TickLE struct and
# its codec after the message alone, so std_msgs/String and example_interfaces/String - and
# action_msgs/GoalStatus and actionlib_msgs/GoalStatus - each defined `StringData_encode`,
# `GoalStatusData_encode` and so on. Whichever library loaded first served both, silently. Where
# the two types happened to have the same layout nothing showed; where they did not, one type's
# messages were encoded by the other type's codec, putting heap pointers on the wire. 144 symbols
# collided across the shipped inventory before the fix.
#
# This checks the property rather than the instance: whatever the generator is renamed to next, two
# packages must not both define a symbol. It needs only `nm`, so it is cheap enough to run on every
# push, and it catches services and actions as well as messages - which one-package-at-a-time
# conformance checks cannot, because a collision needs two packages loaded together.
#
# Usage: check_interface_symbols.sh [-w WORKSPACE]   (default ~/tickle_ros2_interfaces)
set -euo pipefail

WORKSPACE="${HOME}/tickle_ros2_interfaces"
while getopts "w:" opt; do
    case "$opt" in
    w) WORKSPACE="$OPTARG" ;;
    *) exit 2 ;;
    esac
done

fail() {
    echo "check_interface_symbols: $*" >&2
    exit 1
}

[ -d "$WORKSPACE/install" ] || fail "no $WORKSPACE/install - run build_ros2_interfaces.sh first"
command -v nm > /dev/null || fail "no nm"

symbols=$(mktemp)
trap 'rm -f "$symbols"' EXIT

# Collected before the loop: a `for ... done | sort` pipeline runs in a subshell, so a count kept
# inside it is lost - and a count that is always zero would turn this check's own "did it read
# anything" guard into a check that can only fail.
mapfile -t libraries < <(ls "$WORKSPACE"/install/*/lib/lib*__rosidl_typesupport_tickle_c*.so 2> /dev/null || true)
[ "${#libraries[@]}" -gt 0 ] ||
    fail "no interface libraries under $WORKSPACE/install - this check is not reading what it thinks"

for library in "${libraries[@]}"; do
    package=$(basename "$library" | sed 's/^lib//; s/__rosidl.*//')
    # Strong definitions only. A weak one (W) is a C++ template instantiation, which every
    # translation unit that uses it emits and the linker folds - duplicates there are correct.
    # TickLE core's own tt_ symbols are excluded too: each library statically links the same
    # encoding.c and log.c, so those duplicates are the same code, not two meanings of one name.
    nm -D --defined-only "$library" |
        awk -v p="$package" '($2 == "T" || $2 == "D" || $2 == "B") && $3 !~ /^_?tt_/ { print $3, p }'
done | sort -u > "$symbols"

[ -s "$symbols" ] ||
    fail "read ${#libraries[@]} librarie(s) and found no symbols at all - this check is not reading what it thinks"

collisions=$(awk '{ print $1 }' "$symbols" | uniq -d)
if [ -n "$collisions" ]; then
    echo "check_interface_symbols: a symbol defined by more than one interface package:" >&2
    while read -r symbol; do
        echo "  $symbol <- $(grep -E "^$symbol " "$symbols" | awk '{ print $2 }' | tr '\n' ' ')" >&2
    done <<< "$collisions"
    fail "$(wc -l <<< "$collisions") colliding symbol(s) across ${#libraries[@]} librarie(s)"
fi

echo "check_interface_symbols: $(wc -l < "$symbols") symbol(s) across ${#libraries[@]} librarie(s), none defined twice"
