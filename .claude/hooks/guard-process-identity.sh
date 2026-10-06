#!/usr/bin/env bash
# A pattern match is never evidence about a process, and the escalation always loses: the checking command's
# own text is permanently within reach of a text match. `pgrep -f <thing I just typed>` matches the shell
# running it. The `[c]ommand` bracket idiom - which exists for exactly this - still matched a launcher shell
# whose command line contained the heredoc that creates the script. Six instances in one night.
#
# A warning, not a prompt (the user, 2026-10-06): an "ask" here stopped a night's work for 7 h 40 min on a ps | grep
# nobody was awake to approve. What is forbidden is matching a few keywords; matching the exact, complete command
# line and then using that PID is fine (CLAUDE.md). So the command runs, and the reminder is shown beside it.
set -uo pipefail
input=$(cat)
cmd=$(printf '%s' "$input" | jq -r '.tool_input.command // empty')
[ -n "$cmd" ] || exit 0

hit=""
printf '%s' "$cmd" | grep -qE '(^|[;&|[:space:]])pgrep[[:space:]]+(-[a-zA-Z]*f|[^|]*-f)' && hit="pgrep -f"
printf '%s' "$cmd" | grep -qE 'ps[[:space:]][^|]*\|[[:space:]]*grep' && hit="${hit:+$hit and }ps | grep"
[ -n "$hit" ] || exit 0

jq -n --arg r "$hit identifies a process by its command line, which contains the text of whatever is checking. Ask the subject instead: /proc/<pid>/comm is the executable's name and /proc/<pid>/exe its path, and no shell's arguments can appear in either. If you need a PID, capture it at launch with \$!. A keyword match is not allowed; an exact, complete command-line match followed by its PID is." \
  '{systemMessage:("Warning: " + $r), hookSpecificOutput:{hookEventName:"PreToolUse",additionalContext:$r}}'
