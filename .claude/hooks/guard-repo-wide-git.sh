#!/usr/bin/env bash
# `git commit -a` and `git stash` act on the whole working tree, and a session cannot see what else is in
# it. In a checkout shared with another session that is not a style preference: a stash+pop with only
# untracked changes once applied an old unrelated stash, and `commit -a` sweeps a peer's work into your
# commit.
#
# deny where the repository opts in with a .claude-shared-checkout marker, ask everywhere else. Ask rather
# than allow because the cost of a wrong `commit -a` is not visible until someone reads the diff.
set -uo pipefail
input=$(cat)
cmd=$(printf '%s' "$input" | jq -r '.tool_input.command // empty')
[ -n "$cmd" ] || exit 0

# `git stash list|show` only read, so they are not in scope: a guard that stops ordinary work is a guard
# someone switches off. `--all` is `-a`'s long form and the first version of this missed it - the `-a` glob
# cannot match `--all`, because what follows "git commit -" there is another dash.
case "$cmd" in
    *"git stash list"*|*"git stash show"*) exit 0 ;;
    *"git stash"*)                         what="git stash" ;;
    *"git commit -a"*|*"git commit --all"*) what="git commit -a" ;;
    *) exit 0 ;;
esac

root=$(git rev-parse --show-toplevel 2>/dev/null || echo "")
if [ -n "$root" ] && [ -e "$root/.claude-shared-checkout" ]; then
    decision=deny
    why="$what is refused here: $root is marked as a shared checkout, so the working tree holds work this session did not make. Stage explicit paths instead."
else
    decision=ask
    why="$what acts on the whole working tree, which this session cannot fully see. Confirm, or stage explicit paths instead."
fi
jq -n --arg d "$decision" --arg r "$why" \
  '{hookSpecificOutput:{hookEventName:"PreToolUse",permissionDecision:$d,permissionDecisionReason:$r}}'
