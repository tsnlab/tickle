#!/usr/bin/env bash
# clang-tidy over rmw_tickle's C sources, the way CI's cpp-linter checks them.
#
# Why (2026-09-24): `make lint` excludes rmw_tickle/ from clang-tidy entirely
# (LINT_TIDY_EXCLUDE_PATHS in platform/linux/Makefile), while CI's cpp-linter runs clang-tidy on
# every changed file, rmw_tickle/ included. So the local gate reported clean for files it never
# analysed. b1c00e7a turned `Check all` red on a variable name nobody's local run could have seen,
# and running CI's command by hand then found two more findings sitting in rmw_subscription.c since
# an earlier commit - latent, because cpp-linter only checks files changed in the push being linted.
#
# It needs a compile database for rmw_tickle, which needs ROS, so it builds one. It refuses rather
# than passes when it cannot - a check that quietly does nothing reports success, which is the
# failure it exists to prevent.
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
    colcon build --packages-select rmw_tickle --cmake-args -DBUILD_SHARED_LIBS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        >/dev/null 2>&1
); then
    echo "lint-rmw: rmw_tickle did not build - refusing to report a pass" >&2
    exit 1
fi
if [ ! -f build/rmw_tickle/compile_commands.json ]; then
    echo "lint-rmw: no compile database was produced - refusing to report a pass" >&2
    exit 1
fi

fail=0
checked=0
for f in rmw_tickle/rmw_tickle/src/*.c; do
    checked=$((checked + 1))
    out=$("$TIDY" -p build/rmw_tickle "$f" 2>&1 | grep -E "$(basename "$f"):[0-9]+:[0-9]+: (warning|error)")
    if [ -n "$out" ]; then
        echo "$out" >&2
        fail=1
    fi
done
if [ "$checked" -eq 0 ]; then
    echo "lint-rmw: found no rmw_tickle sources - this check is not reading what it thinks it is" >&2
    exit 1
fi
[ "$fail" = 0 ] && echo "lint-rmw: $checked rmw_tickle source(s), no clang-tidy findings ($("$TIDY" --version | grep -i 'LLVM version' | tr -s ' '))"
exit "$fail"
