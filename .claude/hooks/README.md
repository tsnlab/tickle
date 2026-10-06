# Hooks

Guards that enforce a few of this repository's rules mechanically, so following them does not depend on
remembering them. They run as `PreToolUse` hooks on `Bash`, deny, ask or warn, and cost nothing in context.

## Why these live here and not in an organization-wide plugin

They were written as a shared plugin and that was wrong. **"The gates" means something different in every
project**: here it is `make check-gates`, fifteen checks; elsewhere it is pytest and ruff, or lint and
simulation. `guard-format-before-add` hardcodes `clang-format`, the `.c/.h/.cpp` extensions and this
repository's pinned formatter path, none of which a Python or Verilog project has.

Two of the three are in fact general - nothing in `guard-process-identity` or `guard-repo-wide-git` is about
C or about TickLE - but they were not separated from the one that is not, and a set that is three quarters
general is not general. A shared version would have to ask the *project* what its formatter and gate command
are, and do **nothing** where a project declares neither, rather than blocking. That is not built.

So: project-scoped, in the repository rather than in somebody's home directory, versioned and reviewable.

## What each one does

| hook | denies | asks | warns | silent on |
|---|---|---|---|---|
| `guard-repo-wide-git` | `git stash`, `git commit -a` where `.claude-shared-checkout` exists | the same elsewhere | - | explicit-path commits, `git status` |
| `guard-process-identity` | - | - | `pgrep -f`, `ps \| grep` (since 2026-10-06; it asked before, and a night's work waited 7 h 40 min on it) | `/proc/<pid>/comm`, `pgrep -x` |
| `guard-format-before-add` | staging an unformatted C/C++ file | when the pinned formatter is missing | - | formatted files, non-C files |

`guard-format-before-add` asking rather than allowing when the formatter is absent is deliberate: a silent
pass there is indistinguishable from a clean tree, and "I could not look" needs its own answer.

**Known gap**: `git add .` and `git add -A` pass straight through, because only explicitly named C files are
checked. This repository's rule is explicit paths anyway, but the hook does not enforce that, and it should
not be read as making a bulk add safe.

## Enabling them

Per developer, in `.claude/settings.local.json` (git-ignored):

```json
{ "hooks": { "PreToolUse": [ { "matcher": "Bash", "hooks": [
  { "type": "command", "command": "${CLAUDE_PROJECT_DIR}/.claude/hooks/guard-repo-wide-git.sh", "timeout": 10 },
  { "type": "command", "command": "${CLAUDE_PROJECT_DIR}/.claude/hooks/guard-process-identity.sh", "timeout": 10 },
  { "type": "command", "command": "${CLAUDE_PROJECT_DIR}/.claude/hooks/guard-format-before-add.sh", "timeout": 30 }
] } ] } }
```

They are **not** in the shared `.claude/settings.json` on purpose. A hook with a bug blocks the other session
immediately, and these have been verified by feeding them input, not yet by running in a loaded session.

## Verifying one

Each hook reads the tool call as JSON on stdin and answers on stdout. Both directions matter - a guard nobody
has seen refuse is the same as no guard, and one nobody has seen allow may be refusing everything:

```bash
probe() { printf '{"tool_input":{"command":%s}}' "$(jq -Rn --arg c "$1" '$c')" | ./"$2" \
          | jq -r '.hookSpecificOutput.permissionDecision // "no decision"'; }
probe 'git stash'                guard-repo-wide-git.sh      # ask or deny
probe 'git commit -m x src/a.c'  guard-repo-wide-git.sh      # no decision
```
