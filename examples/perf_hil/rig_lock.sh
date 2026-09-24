#!/usr/bin/env bash
# Mutual exclusion for the tickle-hil rig (the two rpis), shared by CI's own
# .github/scripts/run_perf.sh and every manual sweep through run_scenario.sh.
#
# Why (2026-09-23, a real collision): CI's self-hosted runners and our manual sweeps both execute
# on this same PC, so a push that lands while someone is measuring runs HIL scenarios against the
# same two rpis at the same time. That produced impossible results (a server counting the other
# run's traffic: recv=18 against sent=1104432, max_seq_seen larger than sent) and ended with CI's
# own processes being killed as if they were leftovers. Remembering to coordinate was the only
# safeguard, and it was not enough.
#
# Usage, either as a wrapper:
#     rig_lock.sh ./my_sweep.sh ...
# or sourced by a script that then runs several scenarios under one lock. Nested calls are cheap:
# rig_lock.sh exports RIG_LOCK_HELD, and an inner invocation (e.g. run_scenario.sh, called in a
# loop by a sweep that already holds the lock) skips acquiring it rather than deadlocking against
# its own parent.
#
# RIG_LOCK_WAIT: seconds to wait for the rig (default 1800). 0 means fail immediately if busy.
#
# RIG_LOCK_SCOPE: which shared resource is being claimed. There are two, and conflating them is
# what this parameter exists to stop (2026-09-24):
#   hil  (default) - the two rpis. Held by run_perf.sh and by every perf_hil sweep.
#   box            - THIS machine. Held by compare_rmw_perf.sh and by CI's own rmw-perf job, both
#                    of which run their benchmarks here rather than on the rpis.
# Until this existed the lock guarded the rpis only, so the one resource the same-host benchmarks
# actually contend for had nothing but "remember to ask the other session" in front of it - which
# is precisely the arrangement the rpi lock exists because it failed. A box-only job taking the hil
# lock is the same error the other way round and blocks the rig for no reason; that happened here
# for most of an afternoon.
#
# The held-flag is per scope (RIG_LOCK_HELD_HIL / RIG_LOCK_HELD_BOX). A single shared flag would
# make a box-scoped call nested inside an hil-scoped one skip acquiring anything at all and report
# success - a lock that silently locks nothing, which is the defect class this repository spent
# 2026-09-23/24 removing.
set -euo pipefail

RIG_LOCK_SCOPE="${RIG_LOCK_SCOPE:-hil}"
case "$RIG_LOCK_SCOPE" in
hil | box) ;;
*)
    echo "rig_lock: unknown RIG_LOCK_SCOPE '$RIG_LOCK_SCOPE' (expected 'hil' or 'box')" >&2
    exit 64 # EX_USAGE - a typo must not silently fall back to locking something else
    ;;
esac
RIG_LOCK_FILE="${RIG_LOCK_FILE:-/tmp/tickle-${RIG_LOCK_SCOPE}.lock}"
RIG_LOCK_WAIT="${RIG_LOCK_WAIT:-1800}"

held_var="RIG_LOCK_HELD_$(echo "$RIG_LOCK_SCOPE" | tr '[:lower:]' '[:upper:]')"
# Indirect expansion rather than eval: same result, and shellcheck can see that the value is read
# from a name rather than conjured, so it needs no suppression to stay quiet about it.
if [ "${!held_var:-0}" = "1" ]; then
    exec "$@" # an outer scope already holds this same lock
fi

# Append-mode, not ">": ">" is O_TRUNC, so a *waiter* would erase the holder's record before it
# even called flock - blanking the one piece of diagnostics this file exists for, for itself and
# for every later waiter (found in review, 2026-09-23). Only the holder truncates, below.
exec 9>>"$RIG_LOCK_FILE"
if ! flock -w "$RIG_LOCK_WAIT" 9; then
    echo "rig_lock: the '$RIG_LOCK_SCOPE' resource is busy (held via $RIG_LOCK_FILE) and did not free within ${RIG_LOCK_WAIT}s" >&2
    echo "rig_lock: whoever holds it: $(cat "$RIG_LOCK_FILE" 2>/dev/null || echo unknown)" >&2
    exit 75 # EX_TEMPFAIL - a retry-later failure, not a measurement failure
fi
printf '%s: %s\n' "${RIG_LOCK_OWNER:-$(id -un)@$(hostname) pid=$$ $(date -Is)}" "$*" >"$RIG_LOCK_FILE"

export "$held_var=1"
# Also exported under the pre-2026-09-24 name so an in-flight sweep, whose run_scenario.sh was read
# from disk before this file gained scopes, still sees a lock it is already inside. Callers check
# the scoped name; this one exists only so renaming the variable cannot deadlock a running job
# against its own parent.
[ "$RIG_LOCK_SCOPE" = hil ] && export RIG_LOCK_HELD=1
exec "$@"
