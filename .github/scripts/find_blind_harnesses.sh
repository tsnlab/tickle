#!/usr/bin/env bash
# Find harnesses that cannot read what the measured processes said.
#
# NOT A GATE, AND DELIBERATELY NOT WIRED INTO check-gates. Run it by hand when you suspect a
# harness is blind to its own evidence. The reason it is not a gate is the number it prints: on
# 2026-10-03 it found 40 instances across the harnesses under examples/perf_hil, and
# a rule that fires 40 times is describing the house convention rather than a defect. Making it a
# gate would mean annotating 40 existing lines, and a gate people learn to annotate past is worse
# than no gate. If you are reading this because you want to enforce it: the count is the argument,
# so re-run it first and see whether the population has actually changed.
#
# WHAT IT LOOKS FOR. Every consumption of a measured process's output is a filter, so nothing can
# ever read the rest. Not "a grep exists" - that is everywhere and fine. An earlier version without
# the narrowing below flagged 95 lines, mostly build logs.
#
#   a build log (build.sh, make, colcon, cmake): the harness wants pass/fail and the text only
#     matters when it fails. Discarding the rest destroys no evidence about the system under test.
#   the log of ./client or ./server: their output IS the measurement. Discarding the rest destroys
#     evidence about the thing being studied.
#
# That distinction is the whole rule, and it is what took 95 down to 40 with every build log
# correctly excluded. So this looks only at the benchmark binaries:
#
#   (a) ./client or ./server invoked and piped straight into a filter
#   (b) a log written by ./client or ./server whose every consumption is a filter
#
# A write (> file) is not a consumption. A whole read (cat, source, cp, scp, a python open()) is,
# and one of them anywhere is enough to stay silent. Comments are not code - an early version
# reported a comment that DOCUMENTED this defect as an instance of it, which is the same false
# positive as a check matching its own checking command.
#
# WHY IT EXISTS. On 2026-10-03 a shared-memory cell delivered 1 sample out of 15,401 perfectly
# received records. The counter that distinguished "dropped" from "held waiting for a seq that
# cannot come" - reorder_held_peak - was printed by the server on the rig and thrown away by the
# capture, which took only `grep '^RESULT'`. The question went unanswered for most of a day
# because the only process that could answer it was speaking into a filter.
#
# THE TWO CASES IT WAS BUILT FROM, so a future reader deciding whether to promote this can see
# them rather than only the count that stopped it. Both were new lines on the day they were
# written, which is why a changed-lines scope was considered and why the count is what ruled it out.
#
#   fe91be3e:84  examples/perf_hil/experiments/s6_witness_check.sh - rule (a)
#                ./client ... 2>&1 | grep '^RESULT'
#                Everything the client said that was not a RESULT line died at the far end of the
#                pipe. Fixed by writing to a file - which left the shape intact, see the next one.
#
#   9d866585:100 examples/perf_hil/experiments/s6_witness_check.sh - rule (b)
#                grep '^RESULT' /tmp/s6wit_server.log, with no statement anywhere reading the file
#                whole. Written directly under a comment describing the first case. This is the one
#                that cost a day: reorder_held_peak was printed by the server on the rig and was
#                unreachable, and it is the counter that separates "dropped" from "held waiting for
#                a seq that cannot come".
#
#
#   A THIRD instance, found by running this by hand on 2026-10-03 after the first two were fixed:
#   fastdds_datasharing_witness.sh:88, added in the same push that fixed the second one.
#       grep -c '^RESULT' /tmp/dsw_client.log 2>/dev/null || echo 0
#   Worse than a filter: `grep -c` keeps only a count, so the client's reason for saying nothing is
#   unreachable, and `|| echo 0` makes "it printed no RESULT" and "I could not ask" the same 0.
#   It was found because the count came out 36 and not the 35 that two fixes predicted, and the
#   odd one was chased rather than assumed. That is the use this script is for: not enforcement,
#   but a number a person can check an expectation against.
#
#   FOUR MORE, found when the scope was widened from experiments/ to all of examples/perf_hil:
#   every vendor's run_scenario.sh - cyclonedds:95, fastdds:66, tickle:67, zenohpico:34 - ends its
#   client invocation in `| grep "^RESULT:"`. Those four scripts run every benchmark this project
#   has, so a client that dies before printing RESULT leaves nothing behind for any vendor. They
#   were invisible to the first version of this script because it looked at one directory, which
#   is the same defect it exists to find, in itself.
# Escape hatch, on the flagged line: # whole-log-not-needed: <reason>
# The reason belongs where the next reader is, not in a list somewhere else.
#
# Usage:  .github/scripts/find_blind_harnesses.sh [dir]     (default: examples/perf_hil/experiments)
# Exit:   1 if anything was found, 0 if not. Nothing is modified.

set -uo pipefail

ROOT="${1:-examples/perf_hil}"
FILTERS='grep|egrep|fgrep|sed|awk|head|tail|cut|wc'
WHOLE='cat|source|\.|cp|scp|less|more|tee|python3?|mv|rsync'
PRODUCER='\./(client|server)\b'

findings=0
hatched=0

# Lines that are wholly a comment. A file that documents this very defect in prose must not be
# reported as committing it - the first version flagged zenoh_cells.sh for a comment explaining
# what went wrong in another harness.
code_lines() { grep -nv '^[[:space:]]*#' "$1"; }

flag() { # file line kind message
    if sed -n "${2}p" "$1" | grep -q 'whole-log-not-needed:'; then
        hatched=$((hatched + 1))
        return
    fi
    printf '%s:%s: %s: %s\n' "$1" "$2" "$3" "$4"
    findings=$((findings + 1))
}

# Recursive, and the default is the whole of examples/perf_hil rather than experiments/
# alone. The first version looked only at experiments/ and so could not see that all three
# vendors' run_scenario.sh - the scripts that run EVERY benchmark - end their client invocation
# in `| grep "^RESULT:"`. A tool for finding blind spots that is itself pointed at one directory
# has one.
while IFS= read -r f; do
    [ -f "$f" ] || continue

    # (a) a benchmark binary piped straight into a filter.
    while IFS=: read -r ln rest; do
        [ -n "${ln:-}" ] || continue
        printf '%s' "$rest" | grep -qE "${PRODUCER}[^|]*\|[[:space:]]*($FILTERS)\b" || continue
        flag "$f" "$ln" "a" "a measured process's output goes straight into a filter; the rest is unreachable"
    done < <(code_lines "$f")

    # (b) a log WRITTEN BY a benchmark binary whose every consumption filters it.
    #     Gathered as literals; a literal assigned to a variable brings that variable's uses in with
    #     it, or a harness that says LOG=/tmp/x.log and greps "$LOG" reads as one occurrence and
    #     slips through - which is how a textual rule goes quiet on the next person's code.
    paths=$(code_lines "$f" | grep -oE "${PRODUCER}.*[0-9]*>>?[[:space:]]*/tmp/[A-Za-z0-9_.-]+\.(log|txt|out)" |
        grep -oE '/tmp/[A-Za-z0-9_.-]+\.(log|txt|out)' | sort -u)
    for p in $paths; do
        var=$(code_lines "$f" | grep -oE '[A-Za-z_][A-Za-z0-9_]*=("?)'"$p" | grep -oE '^[A-Za-z_][A-Za-z0-9_]*' | head -1)
        pat="$p"
        [ -n "$var" ] && pat="$p|\\\$\{?$var\}?"

        whole_seen=0
        filter_lines=""
        while IFS=: read -r ln text; do
            [ -n "${ln:-}" ] || continue
            printf '%s' "$text" | grep -qE "($pat)" || continue
            # A redirect target is a write, not a consumption.
            stripped=$(printf '%s' "$text" | sed -E "s#[0-9]*>>?[[:space:]]*($pat)##g")
            printf '%s' "$stripped" | grep -qE "($pat)" || continue
            if printf '%s' "$stripped" | grep -qE "\b($WHOLE)\b[^|;]*($pat)"; then
                whole_seen=1
                break
            fi
            if printf '%s' "$stripped" | grep -qE "\b($FILTERS)\b[^|;]*($pat)"; then
                filter_lines="$filter_lines $ln"
            fi
        done < <(code_lines "$f")

        if [ "$whole_seen" = 0 ] && [ -n "$filter_lines" ]; then
            first=$(printf '%s' "$filter_lines" | awk '{print $1}')
            flag "$f" "$first" "b" "every consumption of $p (written by a measured process) is a filter"
        fi
    done
done < <(find "$ROOT" -type f -name '*.sh' | sort)

echo "---"
echo "findings=$findings hatched=$hatched"
exit $((findings > 0 ? 1 : 0))
