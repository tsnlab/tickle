#!/usr/bin/env bash
# Every local gate CI also runs, in one command, with one line per gate.
#
# Why this exists (2026-09-25): the gates were already right, and a push still went out red three
# times in a day. Each time the gate had found the problem and the answer had been thrown away -
# once by `make -s lint >/dev/null 2>&1 && echo clean`, where the redirect hid the findings and the
# `&&` turned a failure into silence. A gate whose answer nobody reads is the failure mode this
# repository keeps re-deriving (CLAUDE.md's rule 3, and lint_rmw.sh's own header).
#
# So: it never hides output from a failing gate (the end of its log is printed, and every row's whole
# log is kept in the directory it names), it says PASS or FAIL for each, and it exits non-zero if any failed. A gate that cannot run (no clang-tidy, no ROS) reports SKIP and does not
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

# --- How the rows run (2026-10-06) ------------------------------------------------------------------------------------
# Every row is started at once, each writing a log of its own, and the verdicts are read afterwards in the fixed order
# of the launches below - so the table has the same rows, names and order it had when they ran one after another, and
# the exit status means what it always did. Run serially the rows took 10-20 minutes.
#
# They can run together because no two write the same tree. lint-rmw builds in $REPO/build; the rmw suite in a base of
# its own under its workspace, with a netns named after its own PID; test-typesupport under build/pytest_basetemp;
# `make test` in platform/linux/obj/debug and `make tsan` in obj/tsan (until 2026-10-06 tsan began with a `make clean`
# of every tree, which would have deleted the unit tests' binaries while they ran); the FreeRTOS build in a mktemp
# directory; the sweep writes nothing. A new row that writes a tree another row also uses must not simply be launched:
# give it a directory of its own, or make the two one command that runs them in order.
#
# Docs-only, opt in: `make check-gates DOCS_ONLY=1` (or --docs-only). When every changed path - staged, modified and
# tracked, and every commit between the upstream (else origin/main) and HEAD - is under docs/, is a *.md, or is under
# examples/perf_hil/results/, only the rows that read those files run: check-doc-shas (README.md, docs/*.md) and
# check-results-provenance (examples/perf_hil/results/). The rest print `SKIP  <name> (docs-only change)`. Anything
# else - a script or source file in one of those places, a path outside them, no changed path at all, or a base that
# does not resolve - runs every row and says which path decided it. CI runs everything regardless.
docs_only_requested=0
case "${1:-}" in
    --docs-only) docs_only_requested=1 ;;
    "") ;;
    *)
        echo "usage: $0 [--docs-only]" >&2
        exit 2
        ;;
esac
[ "${GATES_DOCS_ONLY:-0}" = 1 ] && docs_only_requested=1

# The paths a push of this tree would carry. GATES_BASE_REF overrides the base (the upstream, else origin/main).
changed_paths() {
    # --no-renames: a rename lists both of its paths, so moving a source file into docs/ is not a docs-only change.
    git diff --no-renames --cached --name-only
    git diff --no-renames --name-only
    local base="${GATES_BASE_REF:-}"
    if [ -z "$base" ]; then
        base=$(git rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>/dev/null) || base=origin/main
    fi
    if git rev-parse -q --verify "$base^{commit}" >/dev/null; then
        git diff --no-renames --name-only "$base...HEAD"
    else
        # Not a path anything can match as a document, so it forces the full set.
        printf '(base %s does not resolve)\n' "$base"
    fi
}

# A path only the documents' rows read. Code-shaped files are excluded even under docs/ or results/: lint-shell reads
# every *.sh in the tree, lint every *.c/*.h, and the gate cannot know what reads a new kind of file.
is_doc_path() {
    case "$1" in
        *.sh | *.py | *.c | *.h | *.cpp | *.hpp | *.cc | *.yml | *.yaml | *.mk | *Makefile | *CMakeLists.txt | *.cmake | *.json)
            return 1
            ;;
        docs/* | *.md | examples/perf_hil/results/*) return 0 ;;
        *) return 1 ;;
    esac
}

docs_only=0
if [ "$docs_only_requested" = 1 ]; then
    mapfile -t paths < <(changed_paths | sort -u)
    offending=""
    for p in "${paths[@]}"; do
        is_doc_path "$p" || {
            offending="$p"
            break
        }
    done
    if [ "${#paths[@]}" = 0 ]; then
        echo "== docs-only requested, but nothing is changed against the base - running every row"
    elif [ -n "$offending" ]; then
        echo "== docs-only requested, but '$offending' is not a document path - running every row"
    else
        docs_only=1
        echo "== docs-only change (${#paths[@]} paths): running only the rows that read documents"
    fi
fi
# The rows that read docs/, *.md or examples/perf_hil/results/ - the only ones a docs-only change runs.
DOC_ROWS=" docshas prov "

LOGDIR="$(mktemp -d "${TMPDIR:-/tmp}/tickle-gates.XXXXXX")" || exit 1
ids=()
declare -A NAME KIND

# launch <id> <kind> <name> <command...>: start a row in the background, its output in $LOGDIR/<id>.log and
# "<exit> <seconds>" in $LOGDIR/<id>.rc. <kind> says how the exit status is read once it finishes.
launch() {
    local id="$1" kind="$2" name="$3"
    shift 3
    ids+=("$id")
    NAME[$id]="$name"
    KIND[$id]="$kind"
    if [ "$docs_only" = 1 ] && [ "${DOC_ROWS/ $id /}" = "$DOC_ROWS" ]; then
        printf 'SKIP  %s (docs-only change)\n' "$name" >"$LOGDIR/$id.row"
        return
    fi
    (
        start=$(date +%s)
        "$@" >"$LOGDIR/$id.log" 2>&1 </dev/null
        rc=$?
        echo "$rc $(($(date +%s) - start))" >"$LOGDIR/$id.rc"
    ) &
}

# preskip <id> <name> <reason>: a row that cannot run here, decided before launching anything.
preskip() {
    ids+=("$1")
    NAME[$1]="$2"
    KIND[$1]=skip
    if [ "$docs_only" = 1 ]; then
        printf 'SKIP  %s (docs-only change)\n' "$2" >"$LOGDIR/$1.row"
    else
        printf 'SKIP  %s -- %s\n' "$2" "$3" >"$LOGDIR/$1.row"
    fi
}

lint_kind=gate
[ "$lint_is_advisory" = 1 ] && lint_kind=lint

wall_start=$(date +%s)
launch lint "$lint_kind" "lint (clang-format + clang-tidy)" make lint "${lint_vars[@]}"
launch shell gate "lint-shell" make lint-shell
launch docshas gate "check-doc-shas" make check-doc-shas
launch riglock gate "check-rig-lock" make check-rig-lock
launch bench gate "check-bench-shapes" make check-bench-shapes
launch unsup gate "check-unsupported-list" make check-unsupported-list
launch ctxreset gate "check-context-reset" make check-context-reset
launch prov gate "check-results-provenance" make check-results-provenance
launch unit gate "test (unit)" make test
launch tsan gate "tsan (thread safety)" make tsan
launch pytest gate "test-typesupport (pytest)" make test-typesupport
# CI lints the FreeRTOS HAL files on their own with include-cleaner on; `make -C platform/freertos lint`
# does not (see lint-headers-ci there). Needs the FreeRTOS/lwIP submodules.
if [ -f third_party/FreeRTOS-Kernel/include/FreeRTOS.h ]; then
    launch frhal "$lint_kind" "lint-freertos-hal (as CI)" make -C platform/freertos lint-headers-ci "${lint_vars[@]}"
else
    preskip frhal "lint-freertos-hal (as CI)" "FreeRTOS/lwIP submodules not checked out"
fi
if [ -z "$(find /opt/ros -maxdepth 2 -name setup.bash -print -quit 2>/dev/null)" ]; then
    preskip lintrmw "lint-rmw" "no ROS installation to build rmw_tickle's compile database"
else
    launch lintrmw "$lint_kind" "lint-rmw" make lint-rmw "${lint_vars[@]}"
fi
# CI's "Check all" runs rmw_tickle's own ctest suite and this script did not: on 2026-10-02 every
# gate above reported PASS on a commit that broke test_type_checks, and main stayed red for three
# commits. The script reports WHY it could not run separately from a failure, because "I could not
# look" must not read as "it passed". --behaviour (2026-10-08) adds the rmw behaviour checks "Check all" runs
# after the suite - check_ros2_interfaces.sh's pub/sub, default rclcpp::Node and action cases - in the same netns
# against the same build, ~80 s more on a row that finished well inside the table's wall time.
launch rmw rmw "rmw suite (as CI)" ./.github/scripts/run_rmw_suite.sh --behaviour
# Every gate above builds ONE configuration. On 2026-10-03 a commit passed 13 of 13 here and broke
# -Dtt_SEGMENT_ENABLED=0, which CI builds in four jobs (two of them FreeRTOS, which also compiles the
# segment out). The rig found it in nine seconds. This sweeps the configurations with -fsyntax-only, so it
# writes no object and cannot disturb a build beside it. Same three-state vocabulary as the gate above: a
# sweep that could not run says so rather than passing quietly.
launch sweep sweep "build configs (syntax)" ./.github/scripts/sweep_build_configs.sh
# Every gate above compiles for this host, and the sweep above never links. On 2026-10-06 a 64-bit relaxed
# atomic store linked on x86-64 and failed only in CI's "Build - FreeRTOS RISC-V": RV32 lowers it to
# __atomic_store_8, which picolibc does not provide. This is that build, from scratch, when the cross
# compiler is here - and a loud SKIP with how to install it when it is not.
launch freertos freertos "build freertos (link, as CI)" ./.github/scripts/build_freertos.sh
wait
wall=$(($(date +%s) - wall_start))

# verdict <id> <exit>: the table row for a finished row, by its kind.
verdict() {
    local id="$1" rc="$2" name="${NAME[$1]}"
    case "${KIND[$id]}:$rc" in
        gate:0 | rmw:0 | sweep:0 | freertos:0) echo "PASS  $name" ;;
        lint:0) echo "PASS  $name (advisory, clang-tidy ${tidy_major:-?})" ;;
        lint:*) echo "ADVS  $name -- findings under clang-tidy ${tidy_major:-?}, which is not CI's $CI_CLANG_MAJOR" ;;
        rmw:77) echo "SKIP  $name -- no ROS workspace with TickLE typesupport interfaces (set RMW_TEST_WS, or build_ros2_interfaces.sh)" ;;
        rmw:78) echo "SKIP  $name -- no ROS installation" ;;
        rmw:79) echo "SKIP  $name -- no provable private netns (needs passwordless 'ip') - the suite did NOT run" ;;
        # The build came from some other checkout. Not a SKIP: the environment is fine and the gate
        # would otherwise report on code nobody asked about, which is the defect it was built to stop.
        rmw:80) echo "FAIL  $name - NOT THIS CHECKOUT: the build is not provably from $REPO" ;;
        sweep:77) echo "SKIP  $name -- no C compiler to sweep with - the configurations were NOT checked" ;;
        freertos:77) echo "SKIP  $name -- NO RISC-V CROSS COMPILER - the FreeRTOS link was NOT checked; install: sudo apt-get install gcc-riscv64-unknown-elf binutils-riscv64-unknown-elf picolibc-riscv64-unknown-elf" ;;
        freertos:78) echo "SKIP  $name -- FreeRTOS/lwIP submodules not checked out - the FreeRTOS link was NOT checked; git submodule update --init" ;;
        *) echo "FAIL  $name" ;;
    esac
}

# One section per row, in the table's order: a header with the exit status and time, and the end of the log of any
# row that did not pass - the whole log is in $LOGDIR. A row that left no exit status did not finish, and fails.
TAIL_LINES=60
for id in "${ids[@]}"; do
    name="${NAME[$id]}"
    if [ -f "$LOGDIR/$id.row" ]; then
        row=$(cat "$LOGDIR/$id.row")
        printf '== %s: not run\n' "$name"
        results+=("$row")
        continue
    fi
    if ! read -r rc secs <"$LOGDIR/$id.rc" 2>/dev/null; then
        rc="none"
        secs="?"
    fi
    row=$(verdict "$id" "$rc")
    printf '== %s (exit %s, %ss)\n' "$name" "$rc" "$secs"
    case "$row" in
        PASS*) ;;
        *)
            echo "--- last $TAIL_LINES lines of $LOGDIR/$id.log"
            tail -n "$TAIL_LINES" "$LOGDIR/$id.log" 2>/dev/null || echo "(no log)"
            echo "---"
            ;;
    esac
    case "$row" in FAIL*) failed=1 ;; esac
    results+=("$row")
done

echo
echo "== all rows finished in ${wall}s; every row's full output: $LOGDIR"
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
     make test-freertos   the FreeRTOS round trip under QEMU (the "build freertos" gate above only links it).
   It ran red all day on 2026-09-29 while this script reported every gate PASS. Run it before trusting a push:
     make test-linux
     rmw conformance      test_rmw_implementation (patched jazzy clone), the interface controls and the
                          direct-codec identity harness: CI's "Check all" only (run_rmw_suite.sh's header).
   (rmw_tickle's own suite WAS in this list until 2026-10-03; it is now the "rmw suite (as CI)" gate
   above, which SKIPs with its reason when it cannot run rather than passing silently. Since 2026-10-08
   it also runs check_ros2_interfaces.sh's pub/sub, rclcpp and action cases.)
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
