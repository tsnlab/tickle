#!/usr/bin/env bash
# Comparing what a process says it loaded against what it was supposed to load. Sourced by
# check_ros2_interfaces.sh and exercised directly by tests/test_identity_of.sh, which is the point
# of it living in its own file: the version of this guard that read /proc from outside was broken
# for an unknown length of time because nothing ever asked it to fail on purpose.

# Identity, taken from the process's own testimony rather than read from outside. This used to poll
# /proc/<pid>/maps, which only works while that process happens to still be alive: on 2026-09-30, in
# -r mode, the publisher's poll ran its full five seconds first, by which time the subscriber had
# round-tripped and exited, and identity came back "not seen" - the same thing real library
# shadowing produces. A guard whose false alarm cannot be told from its true finding decides
# nothing, so each program now prints the TickLE libraries it has mapped and this reads them out of
# the log, after the fact, with no window to miss. "not reported" is therefore its own answer: the
# process never got far enough to say, which is a different fault from loading the wrong file.
# Usage: identity_of LOGFILE EXPECTED_LIB...
identity_of() {
    local logfile="$1"
    shift
    local mapped
    mapped=$(sed -n 's/^identity-self [^:]*: //p' "$logfile" | sort -u)
    if [ -z "$mapped" ]; then
        echo "not reported by the process itself"
        return
    fi
    local want resolved line found
    local not_mapped=()
    for want in "$@"; do
        resolved=$(readlink -f "$want" 2>/dev/null || echo "$want")
        found=0
        while IFS= read -r line; do
            if [ "$(readlink -f "$line" 2>/dev/null || echo "$line")" = "$resolved" ]; then
                found=1
                break
            fi
        done <<<"$mapped"
        [ "$found" = 1 ] || not_mapped+=("$want")
    done
    if [ ${#not_mapped[@]} -eq 0 ]; then
        echo "OK: $(echo "$mapped" | tr '\n' ' ')"
    else
        echo "WRONG LIBRARY: expected but not mapped: ${not_mapped[*]} - mapped instead: $(echo "$mapped" | tr '\n' ' ')"
    fi
}
