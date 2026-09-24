#!/usr/bin/env bash
# Every colcon invocation that writes a tree shared with other jobs on the box must run under the
# box lock.
#
# Why this is a check and not a rule (2026-09-24): the same defect was found and fixed three times
# in one day, each time in a different step of the same workflow.
#
#   1. `colcon test-result` and the artifact upload read $RMW_PERF_WS/build outside the lock.
#   2. "Rebuild buildfarm_perf_tests" WRITES $RMW_PERF_WS/build and install outside the lock.
#   3. "Build rmw_tickle" writes $REPO_ROOT/install - not in the perf workspace at all, which is
#      why it survived the first two passes.
#
# None of the three was careless. The lock was introduced to protect "the benchmark", which is an
# activity, and each pass asked "is the benchmark locked?" rather than "is this state shared?" -
# so each fixed the instance in front of it and left the next one. A rule saying "take the lock"
# does not help, because everyone believed they had.
#
# What it cost: a twelve-rep measurement whose reps 2, 5 and 6 ran while CI was rewriting the
# binaries underneath them reported 4 failures in 12, against 0 in 12 for an identical batch with
# no CI writing. That produced an entire hypothesis about concurrent load which had to be
# withdrawn - the expensive kind of wrong, because the data looked real.
set -uo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 1

python3 - "$@" <<'PY'
import sys, re

try:
    import yaml
except ImportError:
    # Said explicitly rather than skipped. A check that quietly does nothing when a dependency is
    # missing is worse than no check, because it reports success - which is the failure mode this
    # whole file exists to prevent.
    print("check_rig_lock_coverage: PyYAML is not installed, so this check cannot run. "
          "Install it (pip install pyyaml) - do not treat this as a pass.", file=sys.stderr)
    sys.exit(1)

WORKFLOW = ".github/workflows/rmw-perf.yml"

try:
    doc = yaml.safe_load(open(WORKFLOW))
except FileNotFoundError:
    print(f"{WORKFLOW}: not found", file=sys.stderr)
    sys.exit(1)


def commands(script):
    """Yield (command, covered) for each shell command in a step.

    `covered` is true when rig_lock.sh wraps it. Two shapes count, and the second is the one a
    naive line-based check gets wrong: rig_lock.sh may wrap the colcon call directly, or it may
    wrap a `bash -c '...'` whose body holds several calls - all of which are inside the lock even
    though only the first line mentions it.
    """
    buf, in_block = "", False
    for line in script.split("\n"):
        stripped = line.strip()
        if stripped.startswith("#"):
            continue
        if in_block:
            # The block ends at a line that is just the closing quote.
            if stripped == "'":
                in_block = False
                continue
            yield stripped, True
            continue
        buf += " " + stripped
        if stripped.endswith("\\"):
            buf = buf[:-1]
            continue
        command = buf.strip()
        buf = ""
        if not command:
            continue
        covered = "rig_lock.sh" in command
        # An unterminated single quote after `bash -c` opens a wrapped block.
        if covered and re.search(r"bash\s+-c\s+'", command) and command.count("'") % 2 == 1:
            in_block = True
            continue
        yield command, covered
    if buf.strip():
        yield buf.strip(), False


fail = 0
checked = 0
covered_count = 0

for job in (doc.get("jobs") or {}).values():
    for step in job.get("steps") or []:
        script = step.get("run")
        if not script:
            continue
        name = step.get("name", "<unnamed>")
        for command, covered in commands(script):
            if not re.search(r"\bcolcon\s+(build|test)\b", command):
                continue
            checked += 1
            if covered:
                covered_count += 1
                continue
            print(f"{WORKFLOW}: step '{name}' runs colcon outside the box lock", file=sys.stderr)
            print(f"    {command[:160]}", file=sys.stderr)
            fail = 1

if checked == 0:
    # Nothing matched: either the workflow stopped using colcon, or this check stopped finding it.
    # The second is indistinguishable from a pass and is exactly how a check quietly dies, so it
    # is an error rather than a silent success.
    print(f"{WORKFLOW}: found no colcon invocations at all - this check is not reading what it "
          f"thinks it is", file=sys.stderr)
    sys.exit(1)

if not fail:
    print(f"rig-lock-coverage: {covered_count} of {checked} colcon invocation(s) under the box lock")
sys.exit(fail)
PY
