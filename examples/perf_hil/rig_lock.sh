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
set -euo pipefail

RIG_LOCK_FILE="${RIG_LOCK_FILE:-/tmp/tickle-hil.lock}"
RIG_LOCK_WAIT="${RIG_LOCK_WAIT:-1800}"

if [ "${RIG_LOCK_HELD:-0}" = "1" ]; then
    exec "$@" # an outer scope already holds it
fi

# Append-mode, not ">": ">" is O_TRUNC, so a *waiter* would erase the holder's record before it
# even called flock - blanking the one piece of diagnostics this file exists for, for itself and
# for every later waiter (found in review, 2026-09-23). Only the holder truncates, below.
exec 9>>"$RIG_LOCK_FILE"
if ! flock -w "$RIG_LOCK_WAIT" 9; then
    echo "rig_lock: the tickle-hil rig is busy (held via $RIG_LOCK_FILE) and did not free within ${RIG_LOCK_WAIT}s" >&2
    echo "rig_lock: whoever holds it: $(cat "$RIG_LOCK_FILE" 2>/dev/null || echo unknown)" >&2
    exit 75 # EX_TEMPFAIL - a retry-later failure, not a measurement failure
fi
printf '%s: %s\n' "${RIG_LOCK_OWNER:-$(id -un)@$(hostname) pid=$$ $(date -Is)}" "$*" >"$RIG_LOCK_FILE"

export RIG_LOCK_HELD=1
exec "$@"
