#!/usr/bin/env bash
# clang-tidy over rmw_tickle's C sources and every tracked ROS C++ file, the way CI's cpp-linter checks them.
#
# Why (2026-09-24): `make lint` excludes rmw_tickle/ from clang-tidy entirely
# (LINT_TIDY_EXCLUDE_PATHS in platform/linux/Makefile), while CI's cpp-linter runs clang-tidy on
# every changed file, rmw_tickle/ included. So the local gate reported clean for files it never
# analysed. b1c00e7a turned `Check all` red on a variable name nobody's local run could have seen,
# and running CI's command by hand then found two more findings sitting in rmw_subscription.c since
# an earlier commit - latent, because cpp-linter only checks files changed in the push being linted.
#
# rmw_perf_pingpong too (2026-09-27): CI's cpp-linter lints its changed .cpp/.hpp as well, and this gate did not.
# The ping's new phase probe turned 499763db's Check all red on 8 findings while check-gates said clean. A clang-tidy
# run by hand had shown "'algorithm' file not found" - lintenv's clang does not find libstdc++ on its own, so it is
# pointed at the installed g++'s GCC directory - and a run that cannot parse its file is no run at all. The parse
# error is a finding here, on the file's own line, so it fails the gate rather than passing for silence.
#
# Every other tracked ROS C++ file too (2026-09-27), found by `git ls-files`, each against its own package's compile
# database: conv_cost.cpp, built by hand with no package, turned af54d45e's Check all red on "'rclcpp/rclcpp.hpp'
# file not found", and the typesupport tests and rmw_tickle_interfaces_check had never been linted here at all. A
# tracked C++ file with no package.xml above it fails the gate instead of being skipped. Only the Fast DDS harnesses
# are left out: they build against Fast DDS, not ROS (reference: lint them against Fast DDS 2.14.7's headers).
#
# It needs a compile database for rmw_tickle and each ROS C++ package, which needs ROS, so it builds both. A configure
# alone would not do for the ping: its sources include headers rosidl generates at build time. It refuses rather
# than passes when it cannot - a check that quietly does nothing reports success, which is the
# failure it exists to prevent.
# Seeing what CI itself found: cpp-linter reports findings as GitHub annotations, so `gh run view
# --log` shows only "N clang-tidy-checks-failed" and never the finding. Re-run clang-tidy locally
# over the files that push changed - this script for rmw_tickle, `make lint` for the rest, or
# `make check-gates` for both.
set -uo pipefail

REPO="$(git rev-parse --show-toplevel)" || exit 1
cd "$REPO" || exit 1

TIDY="${CLANG_TIDY:-$(command -v clang-tidy-19 || command -v clang-tidy || true)}"
if [ -z "$TIDY" ]; then
    echo "lint-rmw: no clang-tidy found - refusing to report a pass" >&2
    exit 1
fi

ROS_SETUP=""
for candidate in /opt/ros/*/setup.bash; do
    [ -f "$candidate" ] && ROS_SETUP="$candidate" && break
done
if [ -z "$ROS_SETUP" ]; then
    echo "lint-rmw: no ROS installation to build rmw_tickle's compile database - refusing to report a pass" >&2
    exit 1
fi

# ROS's own setup.bash reads unbound variables, so it is sourced in a subshell without -u.
if ! (
    set +u
    # shellcheck disable=SC1090
    . "$ROS_SETUP"
    # shellcheck disable=SC1091
    [ -f "$HOME/rmw_perf_ws/install/setup.bash" ] && . "$HOME/rmw_perf_ws/install/setup.bash"
    colcon build --packages-up-to rmw_tickle rmw_perf_pingpong rmw_tickle_interfaces_check \
        rosidl_typesupport_tickle_c_tests conv_cost \
        --cmake-args -DBUILD_SHARED_LIBS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE= >/dev/null 2>&1
); then
    echo "lint-rmw: rmw_tickle or a ROS C++ package did not build - refusing to report a pass" >&2
    exit 1
fi
if [ ! -f build/rmw_tickle/compile_commands.json ]; then
    echo "lint-rmw: no compile database at build/rmw_tickle/compile_commands.json - refusing to report a pass" >&2
    exit 1
fi
GCC_DIR="$(dirname "$(g++ -print-libgcc-file-name 2>/dev/null)")"
if [ ! -d "$GCC_DIR" ]; then
    echo "lint-rmw: no g++ to give clang-tidy its C++ headers - refusing to report a pass" >&2
    exit 1
fi

# One clang-tidy per file, several at once (2026-10-06): one after another the 66 files took five and a half minutes,
# the longest row of check-gates by far, since each C++ file parses rclcpp's headers on its own. Every file still gets
# exactly the check it got before; each one's output goes to a file of its own and is printed in the order the files
# are listed, so a finding reads as it did. LINT_RMW_JOBS sets how many run at once.
JOBS="${LINT_RMW_JOBS:-$(($(nproc 2>/dev/null || echo 2) / 2))}"
[ "$JOBS" -ge 1 ] 2>/dev/null || JOBS=1
OUT="$(mktemp -d "${TMPDIR:-/tmp}/lint-rmw.XXXXXX")" || exit 1
# shellcheck disable=SC2329  # invoked by the EXIT trap below
cleanup() { rm -rf "$OUT"; }
trap cleanup EXIT # the only EXIT trap here - a second would silently replace it

# A C source or header, against rmw_tickle's database. Prints findings; non-zero if there were any.
# shellcheck disable=SC2329  # invoked through start() below
check_c() {
    local f="$1" out
    out=$("$TIDY" -p build/rmw_tickle "$f" 2>&1 | grep -E "$(basename "$f"):[0-9]+:[0-9]+: (warning|error)")
    if [ -n "$out" ]; then
        echo "$out"
        return 1
    fi
    return 0
}

# check_c_db <package> <file>: a C file outside rmw_tickle that a ROS package builds (its own database entry required).
# shellcheck disable=SC2329  # invoked through check_loan_bench() below
check_c_db() {
    local package="$1" f="$2" out
    if ! grep -q "\"file\": \"$REPO/$f\"" "build/$package/compile_commands.json" 2>/dev/null; then
        echo "$f: not in build/$package/compile_commands.json - clang-tidy would not parse it as built"
        return 1
    fi
    out=$("$TIDY" -p "build/$package" "$f" 2>&1 | grep -E "$(basename "$f"):[0-9]+:[0-9]+: (warning|error)")
    if [ -n "$out" ]; then
        echo "$out"
        return 1
    fi
    return 0
}
# shellcheck disable=SC2329  # invoked through start() below
check_loan_bench() { check_c_db conv_cost "$1"; }

# A tracked C++ file, against its own package's database, then compiled at C++17. Prints findings; non-zero if there
# were any, and 3 if the file is one this machine cannot build (counted as not checked).
# shellcheck disable=SC2329  # invoked through start() below
check_cpp() {
    local f="$1" package_dir package out compile std_dir std_cmd
    package_dir=$(dirname "$f")
    while [ "$package_dir" != "." ] && [ ! -f "$package_dir/package.xml" ]; do
        package_dir=$(dirname "$package_dir")
    done
    if [ "$package_dir" = "." ]; then
        echo "$f: no package.xml above it, so no compile database - clang-tidy cannot parse it"
        return 1
    fi
    package=$(sed -n 's:.*<name>\(.*\)</name>.*:\1:p' "$package_dir/package.xml" | head -1)
    if [ ! -f "build/$package/compile_commands.json" ]; then
        echo "$f: no compile database at build/$package - is $package in the colcon build above?"
        return 1
    fi
    # A header is never in the database itself; clang-tidy borrows a neighbouring source's flags for it.
    if [ "${f%.hpp}" = "$f" ] && ! grep -q "\"file\": \"$REPO/$f\"" "build/$package/compile_commands.json"; then
        # action_check.cpp is built only where rclcpp_action, example_interfaces and action_msgs are installed (its
        # CMakeLists finds them QUIET). Anything else missing from its database is a failure, not a skip.
        if [ "$f" = rmw_tickle/rmw_tickle_interfaces_check/src/action_check.cpp ]; then
            echo "lint-rmw: SKIPPED $f - not built on this machine (no rclcpp_action/action_msgs); CI lints it"
            return 3
        fi
        echo "$f: not in build/$package/compile_commands.json - clang-tidy would not parse it as built"
        return 1
    fi
    local rc=0
    out=$("$TIDY" -p "build/$package" --extra-arg=--gcc-install-dir="$GCC_DIR" "$f" 2>&1 |
        grep -E "$(basename "$f"):[0-9]+:[0-9]+: (warning|error)")
    if [ -n "$out" ]; then
        echo "$out"
        rc=1
    fi
    # And the same file compiled at CI's C++ standard, which is not necessarily this machine's.
    # CXX_STANDARD is a minimum: a ROS 2 install whose own targets require C++20 raises ours to
    # C++20, so a C++20 construct builds on a developer box and fails on CI's jazzy, where rclcpp
    # leaves the standard at 17. That has now happened three times (47ddcb75's conv_cost, then
    # 12b4f06e's direct_codec_identity, which clang-tidy's own --fix wrote), every time in CI
    # rather than here. Taking the file's real compile command and lowering only -std reproduces
    # CI's dialect exactly, without needing jazzy. A target that deliberately requires C++20 goes
    # in CXX20_TARGETS below, where CI compiles it at 20 too - forgetting to list one fails here,
    # which is the safe direction.
    case " $CXX20_TARGETS " in
    *" $f "*) return "$rc" ;;
    esac
    compile=$(python3 - "build/$package/compile_commands.json" "$REPO/$f" <<'PY'
import json, sys
db = json.load(open(sys.argv[1]))
for entry in db:
    if entry["file"] == sys.argv[2]:
        command = entry.get("command") or " ".join(entry["arguments"])
        print(json.dumps({"dir": entry["directory"], "command": command}))
        break
PY
    )
    if [ -z "$compile" ]; then
        return "$rc" # a header, or a file whose own database entry the check above already reported
    fi
    std_dir=$(printf '%s' "$compile" | python3 -c 'import json,sys; print(json.load(sys.stdin)["dir"])')
    std_cmd=$(printf '%s' "$compile" | python3 -c 'import json,sys; print(json.load(sys.stdin)["command"])' |
        sed -E 's/-std=(gnu|c)\+\+[0-9a-z]+/-std=gnu++17/g; s| -o [^ ]+| -fsyntax-only|')
    case "$std_cmd" in
    *-std=gnu++17*) ;;
    *) std_cmd="$std_cmd -std=gnu++17" ;;
    esac
    if ! out=$(cd "$std_dir" && eval "$std_cmd" 2>&1); then
        echo "$out" | grep -E "error:"
        echo "$f: does not compile at C++17, which is what CI compiles it as - see lint_rmw.sh"
        rc=1
    fi
    return "$rc"
}

# start <n> <function> <file>: one file's check in the background, its output in $OUT/<n>.out and status in $OUT/<n>.rc.
start() {
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do
        wait -n
    done
    (
        "$2" "$3" >"$OUT/$1.out" 2>&1
        echo $? >"$OUT/$1.rc"
    ) &
}

# Tests too, not only src/ (2026-09-25): cpp-linter checks every file a push CHANGED, tests
# included, so a gate that skipped them reported clean for files CI was about to fail on - which is
# exactly how this script's own existence came about, one directory further in. Four findings in
# test_storage_budget.c turned Check all red this way. And headers (2026-09-26): cpp-linter lints a
# changed header on its own, and an include-cleaner finding in rmw_tickle.h turned af87e150 red while
# this gate - linting only .c files, where the header's own includes are never judged - said clean.
#
# And core's own sources as rmw_tickle compiles them (2026-09-28): `make lint` checks src/*.c with core's defines, but
# code behind a flag only rmw_tickle's build sets (tt_CONTEXT_ID_CLAIM, tt_LOCAL_DELIVERY) is compiled out there, while
# CI's cpp-linter lints the same files with rmw_tickle's database. The g8 host registry in hal_linux.c turned
# 958e909e's Check all red on six findings this way (a short `fd`, a magic 0666) while check-gates said clean.
c_files=(rmw_tickle/rmw_tickle/src/*.c rmw_tickle/rmw_tickle/test/*.c rmw_tickle/rmw_tickle/include/rmw_tickle_c/*.h
    src/tickle.c src/hal_linux.c src/encoding.c src/log.c)
mapfile -t cpp_files < <(git ls-files '*.cpp' '*.hpp' '*.cc' | grep -Ev '^(third_party|examples/perf_hil/fastdds)/')
# Files whose own CMake requires C++20, so CI compiles them at 20 as well - exempt from the C++17
# compile below. Keep in step with every `CMAKE_CXX_STANDARD 20` / `cxx_std_20` in the tree.
CXX20_TARGETS="examples/perf_hil/experiments/conv_cost/conv_cost.cpp"

files=()
n=0
for f in "${c_files[@]}"; do
    files+=("$f")
    start "$n" check_c "$f"
    n=$((n + 1))
done
for f in "${cpp_files[@]}"; do
    files+=("$f")
    start "$n" check_cpp "$f"
    n=$((n + 1))
done
# C outside rmw_tickle that needs ROS (2026-10-08): the loaned-messages bench, built in conv_cost for its database entry
# and excluded from `make lint`, which has no ROS include path and stops at 'rcutils/allocator.h' file not found.
files+=(examples/perf_hil/experiments/rmw_loan_bench.c)
start "$n" check_loan_bench examples/perf_hil/experiments/rmw_loan_bench.c
n=$((n + 1))
wait

fail=0
checked=0
for i in "${!files[@]}"; do
    cat "$OUT/$i.out" >&2 2>/dev/null
    rc=$(cat "$OUT/$i.rc" 2>/dev/null || echo none)
    case "$rc" in
    0) checked=$((checked + 1)) ;;
    3) ;;
    1)
        checked=$((checked + 1))
        fail=1
        ;;
    *)
        echo "${files[$i]}: its check did not finish (status '$rc') - refusing to report a pass" >&2
        checked=$((checked + 1))
        fail=1
        ;;
    esac
done
if [ "$checked" -eq 0 ]; then
    echo "lint-rmw: found no rmw_tickle or ROS C++ sources - this check is not reading what it thinks it is" >&2
    exit 1
fi
[ "$fail" = 0 ] && echo "lint-rmw: $checked rmw_tickle, core-as-rmw and ROS C++ source(s), no clang-tidy findings ($("$TIDY" --version | grep -i 'LLVM version' | tr -s ' '), $JOBS at once)"
exit "$fail"
