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
# directions (CONTRIBUTING.md). Use a 19 if one is on PATH or in the venv CONTRIBUTING suggests.
CI_CLANG_MAJOR=19
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

# Whether the clang tools we are about to use are CI's version. If they are not, the two lint
# gates still run - a newer clang-tidy finds real things, and found two on this script's first run -
# but their result is ADVISORY and does not fail the run. Reporting FAIL for a check CI does not
# have is how a gate teaches people to ignore it, which is the failure this script exists to
# prevent (Plan's review, 2026-09-25).
clang_major() {
    [ -x "$1" ] || { echo ""; return; }
    "$1" --version 2>/dev/null | sed -n 's/.*version \([0-9][0-9]*\)\..*/\1/p' | head -1
}
tidy_major="$(clang_major "${TIDY:-$(command -v clang-tidy || true)}")"
lint_is_advisory=0
if [ "$tidy_major" != "$CI_CLANG_MAJOR" ]; then
    lint_is_advisory=1
fi

failed=0
results=()

# run_gate <name> <command...>; run_advisory_gate is the same but never fails the run.
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

run_lint_gate() {
    local name="$1"
    shift
    if [ "$lint_is_advisory" = 0 ]; then
        run_gate "$name" "$@"
        return
    fi
    printf '== %s (advisory)\n' "$name"
    if "$@"; then
        results+=("PASS  $name (advisory, clang-tidy ${tidy_major:-?})")
    else
        results+=("ADVS  $name -- findings under clang-tidy ${tidy_major:-?}, which is not CI's $CI_CLANG_MAJOR")
    fi
}

skip_gate() {
    results+=("SKIP  $name -- $1")
}

run_lint_gate "lint (clang-format + clang-tidy)" make lint "${lint_vars[@]}"
run_gate "lint-shell" make lint-shell
run_gate "check-doc-shas" make check-doc-shas
run_gate "check-rig-lock" make check-rig-lock
run_gate "check-bench-shapes" make check-bench-shapes
run_gate "test (unit)" make test
run_gate "tsan (thread safety)" make tsan
# CI lints the FreeRTOS HAL header on its own with include-cleaner on; `make -C platform/freertos lint`
# does not (see lint-headers-ci there). Needs the FreeRTOS/lwIP submodules.
if [ -f third_party/FreeRTOS-Kernel/include/FreeRTOS.h ]; then
    run_lint_gate "lint-freertos-header (as CI)" make -C platform/freertos lint-headers-ci "${lint_vars[@]}"
else
    name="lint-freertos-header (as CI)"
    skip_gate "FreeRTOS/lwIP submodules not checked out"
fi
name="lint-rmw"
if [ -z "$(find /opt/ros -maxdepth 2 -name setup.bash -print -quit 2>/dev/null)" ]; then
    skip_gate "no ROS installation to build rmw_tickle's compile database"
else
    run_lint_gate "lint-rmw" make lint-rmw "${lint_vars[@]}"
fi

echo
echo "== gates"
printf '%s\n' "${results[@]}"
echo "   clang-tidy: ${TIDY:-$(command -v clang-tidy || echo none)} (version ${tidy_major:-?})"
if [ "$lint_is_advisory" = 1 ]; then
    cat <<MSG
   The lint gates above are ADVISORY: this clang-tidy is not CI's $CI_CLANG_MAJOR, so its findings
   may be checks CI does not have - and it can equally miss ones CI does. For a verdict:
     python3 -m venv /tmp/lintenv && /tmp/lintenv/bin/pip install clang-format==19.1.0 clang-tidy==19.1.0
     make check-gates
   (this script picks /tmp/lintenv up automatically, or pass CLANG_TIDY=/CLANG_FORMAT=.)
MSG
fi
exit "$failed"
