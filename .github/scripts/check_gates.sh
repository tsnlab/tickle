#!/usr/bin/env bash
# Every local gate CI also runs, in one command, with one line per gate.
#
# Why this exists (2026-09-25): the gates were already right, and a push still went out red three
# times in a day. Each time the gate had found the problem and the answer had been thrown away -
# once by `make -s lint >/dev/null 2>&1 && echo clean`, where the redirect hid the findings and the
# `&&` turned a failure into silence. A gate whose answer nobody reads is the failure mode this
# repository keeps re-deriving (CLAUDE.md's rule 3, and lint_rmw.sh's own header).
#
# So: it never hides output from a failing gate, it says PASS or FAIL for each, and it exits
# non-zero if any failed. A gate that cannot run (no clang-tidy, no ROS) reports SKIP and does not
# pass silently.
#
# What it does NOT cover, because it needs a runner: the conformance suite, the interface-package
# builds, and the rclcpp end-to-end checks in check-all.yml. Green here means "the checks that can
# run on this machine agree", not "CI will be green".
set -uo pipefail

REPO="$(git rev-parse --show-toplevel)" || exit 1
cd "$REPO" || exit 1

# CI pins clang-tidy/clang-format 19 and a current distro ships 21; the two disagree in both
# directions (CONTRIBUTING.md). Use a 19 if one is on PATH or in the venv CONTRIBUTING suggests,
# and say which was used either way - a pass under 21 does not predict CI.
TIDY="${CLANG_TIDY:-}"
FORMAT="${CLANG_FORMAT:-}"
for candidate in "$(command -v clang-tidy-19 || true)" /tmp/lintenv/bin/clang-tidy; do
    [ -n "$TIDY" ] && break
    [ -x "$candidate" ] && TIDY="$candidate"
done
for candidate in "$(command -v clang-format-19 || true)" /tmp/lintenv/bin/clang-format; do
    [ -n "$FORMAT" ] && break
    [ -x "$candidate" ] && FORMAT="$candidate"
done
lint_vars=()
[ -n "$TIDY" ] && lint_vars+=("CLANG_TIDY=$TIDY")
[ -n "$FORMAT" ] && lint_vars+=("CLANG_FORMAT=$FORMAT")

failed=0
results=()

run_gate() {
    local name="$1"
    shift
    printf '== %s\n' "$name"
    if "$@"; then
        results+=("PASS  $name")
    else
        results+=("FAIL  $name")
        failed=1
    fi
}

skip_gate() {
    results+=("SKIP  $name -- $1")
}

run_gate "lint (clang-format + clang-tidy)" make lint "${lint_vars[@]}"
run_gate "lint-shell" make lint-shell
run_gate "check-doc-shas" make check-doc-shas
run_gate "check-rig-lock" make check-rig-lock
run_gate "test (unit)" make test
name="lint-rmw"
if [ -z "$(find /opt/ros -maxdepth 2 -name setup.bash -print -quit 2>/dev/null)" ]; then
    skip_gate "no ROS installation to build rmw_tickle's compile database"
else
    run_gate "lint-rmw" make lint-rmw "${lint_vars[@]}"
fi

echo
echo "== gates"
printf '%s\n' "${results[@]}"
if [ -n "$TIDY" ] || [ -n "$FORMAT" ]; then
    echo "   (clang tools: ${TIDY:-default} / ${FORMAT:-default})"
else
    echo "   (clang tools: whatever make lint found - CI pins 19, see CONTRIBUTING.md)"
fi
exit "$failed"
