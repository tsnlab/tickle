#!/bin/bash
# The collapse test's mutants (tests/mutants_hybrid_send.py, MUTANTS filter below), run from the tree this script is in,
# detached: collapse_fp_mutants.sh <log>. Prints "CFP_MUTANTS_DONE <rc>" last, whatever happened.
set -u
log=$1
root=$(cd "$(dirname "$0")/../../.." && pwd)
trap 'echo "CFP_MUTANTS_DONE $rc" >> "$log"' EXIT
rc=99
cd "$root" || exit 1
export TMPDIR=${TMPDIR:-/home/semih/tmp_build}
export MUTANTS='law: collapse without|law: the baseline at|law: significance at|law: the size of the excess|law: the clean test at the 99.9|law: far read|bound:'
(ulimit -v 4194304; timeout 3600 python3 tests/mutants_hybrid_send.py) >> "$log" 2>&1
rc=$?
