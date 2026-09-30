#!/usr/bin/env bash
# Does the library-identity guard actually decide anything?
#
# The version this replaces read /proc/<pid>/maps from outside the process, and on 2026-09-30 it
# reported "not seen" for a subscriber that had loaded exactly the right libraries and then exited
# before the harness got around to looking. That is the same answer it gives for real library
# shadowing, so the run was failed for the wrong reason - and, worse, the guard would have been just
# as quiet had it never been able to say anything at all. Nothing had ever asked it to fail.
#
# So each case below states what it is for, and the WRONG and ABSENT cases are the controls: if they
# ever come back OK, this guard is decorative and every "identity: OK" ever printed means nothing.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source-path=SCRIPTDIR
. "$HERE/../lib/identity.sh"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
rmw="$work/librmw_tickle.so"
ts="$work/libstd_msgs__rosidl_typesupport_tickle_c.so"
other="$work/other/librmw_tickle.so"
mkdir -p "$work/other"
touch "$rmw" "$ts" "$other"

failures=0
expect() { # NAME EXPECTED_PREFIX ACTUAL
    if [[ "$3" == "$2"* ]]; then
        echo "ok   $1"
    else
        echo "FAIL $1: expected '$2...', got '$3'"
        failures=$((failures + 1))
    fi
}

# The case the guard exists to wave through.
cat >"$work/good.txt" <<LOG
identity-self sub: $rmw
identity-self sub: $rmw
identity-self sub: $ts
interfaces_check sub: PASS
LOG
expect "the libraries under test are the ones mapped" "OK:" "$(identity_of "$work/good.txt" "$rmw" "$ts")"

# Control 1: the shadowing this is for - right name, wrong file, which is what AMENT_PREFIX_PATH
# ordering produces and what a name-pattern check cannot see.
cat >"$work/shadowed.txt" <<LOG
identity-self sub: $other
identity-self sub: $ts
LOG
expect "a same-named library from elsewhere is caught" "WRONG LIBRARY:" \
    "$(identity_of "$work/shadowed.txt" "$rmw" "$ts")"

# Control 2: the expected typesupport simply is not there.
cat >"$work/no_ts.txt" <<LOG
identity-self sub: $rmw
LOG
expect "a missing typesupport is caught" "WRONG LIBRARY:" "$(identity_of "$work/no_ts.txt" "$rmw" "$ts")"

# Control 3: the process never said anything - it died before its entities existed, or the reporting
# was compiled out. Distinct from loading the wrong file, and must not read as OK.
cat >"$work/silent.txt" <<LOG
interfaces_check sub: FAIL
LOG
expect "silence is not agreement" "not reported" "$(identity_of "$work/silent.txt" "$rmw" "$ts")"

# A symlinked install must not read as a different file: colcon installs are full of them.
ln -s "$rmw" "$work/link_to_rmw.so"
cat >"$work/linked.txt" <<LOG
identity-self sub: $work/link_to_rmw.so
identity-self sub: $ts
LOG
expect "a symlink to the expected file still counts" "OK:" "$(identity_of "$work/linked.txt" "$rmw" "$ts")"

if [ "$failures" -eq 0 ]; then
    echo "test_identity_of: PASS"
else
    echo "test_identity_of: FAIL - $failures case(s)"
    exit 1
fi
