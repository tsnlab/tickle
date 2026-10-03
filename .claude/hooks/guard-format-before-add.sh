#!/usr/bin/env bash
# Formatting after staging means the gates find it, and three night-shift gate runs died on comment
# alignment alone. Check the C/C++ files this `git add` would stage, with the SAME formatter the gates use.
#
# "I could not look" gets its own answer: if the pinned formatter is absent, this asks rather than allowing,
# because a silent pass here is indistinguishable from a clean tree.
set -uo pipefail
input=$(cat)
cmd=$(printf '%s' "$input" | jq -r '.tool_input.command // empty')
printf '%s' "$cmd" | grep -qE '(^|[;&|[:space:]])git[[:space:]]+add([[:space:]]|$)' || exit 0

root=$(git rev-parse --show-toplevel 2>/dev/null || exit 0)
[ -f "$root/.clang-format" ] || exit 0

fmt="${CLAUDE_CLANG_FORMAT:-/tmp/lintenv/bin/clang-format}"
if [ ! -x "$fmt" ]; then
    jq -n --arg r "The pinned formatter is not at $fmt, so formatting could not be checked before staging. That is 'I could not look', not 'the tree is clean' - the gates use that exact binary and will disagree with a different one. Set CLAUDE_CLANG_FORMAT or restore it, then retry." \
      '{hookSpecificOutput:{hookEventName:"PreToolUse",permissionDecision:"ask",permissionDecisionReason:$r}}'
    exit 0
fi

# Only the paths this command names, resolved against the repo, and only ones the formatter owns.
bad=""
for tok in $(printf '%s' "$cmd" | sed -E 's/.*git[[:space:]]+add[[:space:]]+//' | tr ' ' '\n'); do
    case "$tok" in -*|'') continue ;; esac
    case "$tok" in *.c|*.h|*.cpp|*.hpp|*.cc) ;; *) continue ;; esac
    [ -f "$tok" ] || continue
    "$fmt" --dry-run --Werror "$tok" >/dev/null 2>&1 || bad="$bad $tok"
done
[ -n "$bad" ] || exit 0

jq -n --arg r "Not formatted with the gates' own clang-format:$bad. Run '$fmt -i$bad' first - staging now means check-gates finds it, and a gate run costs twenty minutes to tell you about whitespace." \
  '{hookSpecificOutput:{hookEventName:"PreToolUse",permissionDecision:"deny",permissionDecisionReason:$r}}'
