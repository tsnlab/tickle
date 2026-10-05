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
    # `make lint` refuses a clang that is not CI's (platform/linux/Makefile, LINT_CLANG_MAJOR); here the
    # mismatch is known and the result already marked advisory, so ask it to run anyway.
    lint_vars+=("LINT_ANY_CLANG=1")
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
run_gate "check-unsupported-list" make check-unsupported-list
run_gate "check-context-reset" make check-context-reset
run_gate "check-results-provenance" make check-results-provenance
run_gate "test (unit)" make test
run_gate "tsan (thread safety)" make tsan
run_gate "test-typesupport (pytest)" make test-typesupport
# CI lints the FreeRTOS HAL files on their own with include-cleaner on; `make -C platform/freertos lint`
# does not (see lint-headers-ci there). Needs the FreeRTOS/lwIP submodules.
if [ -f third_party/FreeRTOS-Kernel/include/FreeRTOS.h ]; then
    run_lint_gate "lint-freertos-hal (as CI)" make -C platform/freertos lint-headers-ci "${lint_vars[@]}"
else
    name="lint-freertos-hal (as CI)"
    skip_gate "FreeRTOS/lwIP submodules not checked out"
fi
name="lint-rmw"
if [ -z "$(find /opt/ros -maxdepth 2 -name setup.bash -print -quit 2>/dev/null)" ]; then
    skip_gate "no ROS installation to build rmw_tickle's compile database"
else
    run_lint_gate "lint-rmw" make lint-rmw "${lint_vars[@]}"
fi

# CI's "Check all" runs rmw_tickle's own ctest suite and this script did not: on 2026-10-02 every
# gate above reported PASS on a commit that broke test_type_checks, and main stayed red for three
# commits. The script reports WHY it could not run separately from a failure, because "I could not
# look" must not read as "it passed".
name="rmw suite (as CI)"
printf '== %s\n' "$name"
./.github/scripts/run_rmw_suite.sh
rmw_suite_rc=$?
case "$rmw_suite_rc" in
    0) results+=("PASS  $name") ;;
    77) skip_gate "no ROS workspace with TickLE typesupport interfaces (set RMW_TEST_WS, or build_ros2_interfaces.sh)" ;;
    78) skip_gate "no ROS installation" ;;
    79) skip_gate "no provable private netns (needs passwordless 'ip') - the suite did NOT run" ;;
    80)
        # The build came from some other checkout. Not a SKIP: the environment is fine and the gate
        # would otherwise report on code nobody asked about, which is the defect it was built to stop.
        results+=("FAIL  $name - NOT THIS CHECKOUT: the build is not provably from $REPO")
        failed=1
        ;;
    *)
        results+=("FAIL  $name")
        failed=1
        ;;
esac

# Every gate above builds ONE configuration. On 2026-10-03 a commit passed 13 of 13 here and broke
# -Dtt_SEGMENT_ENABLED=0, which CI builds in four jobs (two of them FreeRTOS, which also compiles the
# segment out). The rig found it in nine seconds. This sweeps the configurations with -fsyntax-only, so it
# writes no object and cannot disturb a build beside it. Same three-state vocabulary as the gate above: a
# sweep that could not run says so rather than passing quietly.
name="build configs (syntax)"
printf '== %s\n' "$name"
./.github/scripts/sweep_build_configs.sh
sweep_rc=$?
case "$sweep_rc" in
    0) results+=("PASS  $name") ;;
    77) skip_gate "no C compiler to sweep with - the configurations were NOT checked" ;;
    *)
        results+=("FAIL  $name")
        failed=1
        ;;
esac

echo
echo "== gates"
printf '%s\n' "${results[@]}"
# What this script does NOT run, said every time rather than left to be discovered. On 2026-09-29 both CI workflows
# were red for twelve hours while check-gates was green on every commit, and the two tiers that were failing are
# exactly the two below: a green run here was read as "CI will pass" by two sessions in a row. The gate cannot run
# them cheaply - they need a netns, a built rmw and several minutes - but it can stop implying it did.
cat <<'NOTCOVERED'

   NOT COVERED HERE - a pass above does not predict CI:
     make test-linux      two nodes over the Linux HAL in a netns: the service round trip (set_bool),
                          the perf tier's loss and throughput floors, DURABILITY/HISTORY/LIFESPAN.
   It ran red all day on 2026-09-29 while this script reported every gate PASS. Run it before trusting a push:
     make test-linux
   (rmw_tickle's own suite WAS in this list until 2026-10-03; it is now the "rmw suite (as CI)" gate
   above, which SKIPs with its reason when it cannot run rather than passing silently.)
NOTCOVERED
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
