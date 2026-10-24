#!/bin/sh
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

# Runs garbage_ctx_check.c's two roles, b in one namespace and a in the other, and passes only when both print
# "garbage_ctx_check: PASS - all" (garbage_ctx_check.c says what each checks). Called by test.sh with its namespace pair.
#
# Usage: garbage_ctx_check.sh <ns_a> <ns_b> <broadcast> <binary> <fill> [log prefix]   (a binary named without a
# directory is this one's)
#   GC_ROUND_S (environment) is passed through: the seconds a round lasts; MSAN_OPTIONS too, and GC_WINDOW_WORDS
#   (garbage_ctx_check.c's header: for checking the check, never set by test.sh).
set -u
cd "$(dirname "$0")" || exit 1
NS_A=$1
NS_B=$2
BCAST=$3
BIN=$4
FILL=$5
LOG=${6:-$(basename "$BIN")-$FILL}
case $BIN in
*/*) RUN=$BIN ;;
*) RUN=./$BIN ;;
esac

rm -f "$LOG-a.log" "$LOG-b.log"
# shellcheck disable=SC2024 # the redirect is this shell's, on purpose: the logs are this user's
sudo ip netns exec "$NS_B" env GC_ROUND_S="${GC_ROUND_S:-14}" MSAN_OPTIONS="${MSAN_OPTIONS:-}" \
    GC_WINDOW_WORDS="${GC_WINDOW_WORDS:-0}" "$RUN" b "$FILL" "$BCAST" </dev/null >"$LOG-b.log" 2>&1 &
b_pid=$!
sleep 1
# shellcheck disable=SC2024
sudo ip netns exec "$NS_A" env GC_ROUND_S="${GC_ROUND_S:-14}" MSAN_OPTIONS="${MSAN_OPTIONS:-}" \
    GC_WINDOW_WORDS="${GC_WINDOW_WORDS:-0}" "$RUN" a "$FILL" "$BCAST" </dev/null >"$LOG-a.log" 2>&1
a_rc=$?
wait "$b_pid"
b_rc=$?

status=0
for role in a b; do
    grep '^garbage_ctx_check:' "$LOG-$role.log"
    if ! grep -q '^garbage_ctx_check: PASS - all$' "$LOG-$role.log"; then
        echo "garbage_ctx_check ($BIN, fill $FILL): role $role did not pass - $LOG-$role.log:"
        tail -40 "$LOG-$role.log"
        status=1
    fi
done
echo "garbage_ctx_check ($BIN, fill $FILL): a rc=$a_rc b rc=$b_rc - $([ "$status" -eq 0 ] && echo PASS || echo FAIL)"
exit "$status"
