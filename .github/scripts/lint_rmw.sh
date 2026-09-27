#!/usr/bin/env bash
# clang-tidy over rmw_tickle's C sources and rmw_perf_pingpong's C++ ones, the way CI's cpp-linter checks them.
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
# It needs a compile database for rmw_tickle and rmw_perf_pingpong, which needs ROS, so it builds both. A configure
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
    colcon build --packages-select rmw_tickle rmw_perf_pingpong \
        --cmake-args -DBUILD_SHARED_LIBS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null 2>&1
); then
    echo "lint-rmw: rmw_tickle or rmw_perf_pingpong did not build - refusing to report a pass" >&2
    exit 1
fi
for db in build/rmw_tickle/compile_commands.json build/rmw_perf_pingpong/compile_commands.json; do
    if [ ! -f "$db" ]; then
        echo "lint-rmw: no compile database at $db - refusing to report a pass" >&2
        exit 1
    fi
done
GCC_DIR="$(dirname "$(g++ -print-libgcc-file-name 2>/dev/null)")"
if [ ! -d "$GCC_DIR" ]; then
    echo "lint-rmw: no g++ to give clang-tidy its C++ headers - refusing to report a pass" >&2
    exit 1
fi

fail=0
checked=0
# Tests too, not only src/ (2026-09-25): cpp-linter checks every file a push CHANGED, tests
# included, so a gate that skipped them reported clean for files CI was about to fail on - which is
# exactly how this script's own existence came about, one directory further in. Four findings in
# test_storage_budget.c turned Check all red this way. And headers (2026-09-26): cpp-linter lints a
# changed header on its own, and an include-cleaner finding in rmw_tickle.h turned af87e150 red while
# this gate - linting only .c files, where the header's own includes are never judged - said clean.
for f in rmw_tickle/rmw_tickle/src/*.c rmw_tickle/rmw_tickle/test/*.c rmw_tickle/rmw_tickle/include/rmw_tickle_c/*.h; do
    checked=$((checked + 1))
    out=$("$TIDY" -p build/rmw_tickle "$f" 2>&1 | grep -E "$(basename "$f"):[0-9]+:[0-9]+: (warning|error)")
    if [ -n "$out" ]; then
        echo "$out" >&2
        fail=1
    fi
done
for f in rmw_tickle/rmw_perf_pingpong/src/*.cpp rmw_tickle/rmw_perf_pingpong/src/*.hpp; do
    checked=$((checked + 1))
    out=$("$TIDY" -p build/rmw_perf_pingpong --extra-arg=--gcc-install-dir="$GCC_DIR" "$f" 2>&1 |
        grep -E "$(basename "$f"):[0-9]+:[0-9]+: (warning|error)")
    if [ -n "$out" ]; then
        echo "$out" >&2
        fail=1
    fi
done
if [ "$checked" -eq 0 ]; then
    echo "lint-rmw: found no rmw_tickle sources - this check is not reading what it thinks it is" >&2
    exit 1
fi
[ "$fail" = 0 ] && echo "lint-rmw: $checked rmw_tickle and rmw_perf_pingpong source(s), no clang-tidy findings ($("$TIDY" --version | grep -i 'LLVM version' | tr -s ' '))"
exit "$fail"
