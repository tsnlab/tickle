#!/usr/bin/env bash
# The typesupport generator's pytest suites, the same way in CI and in `make check-gates` (2026-09-27).
#
# The generator runs clang-format on what it writes, and clang-format takes its style from the nearest
# .clang-format above the output - or LLVM's when there is none, which is what CI's pytest (in /tmp, empty above)
# has always seen, and what test_ros2_cpp_adapter.py's expectations are written against. So pytest's temp dirs
# get a .clang-format of their own that states exactly that. Without it, whatever lies above decides: a stray
# /tmp/.clang-format on this machine (2026-09-26, origin unknown) turned `char *` into `char*` and failed the
# suite on an untouched tree, and under build/ the repository's own .clang-format does the same.
#
# Two runs, not one session: both directories have a conftest.py, and pytest cannot import two `conftest` modules.
set -uo pipefail
REPO="$(git rev-parse --show-toplevel)" || exit 1
cd "$REPO" || exit 1
if ! python3 -c 'import pytest' 2>/dev/null; then
    echo "test-typesupport: no pytest for python3 - refusing to report a pass" >&2
    exit 1
fi
base="$REPO/build/pytest_basetemp"
mkdir -p "$base"
printf 'BasedOnStyle: LLVM\n' >"$base/.clang-format"
status=0
python3 -m pytest -q --basetemp="$base/tools" tools/typesupport/tests/ || status=1
python3 -m pytest -q --basetemp="$base/rosidl" rmw_tickle/rosidl_typesupport_tickle_c/test/ || status=1
exit "$status"
