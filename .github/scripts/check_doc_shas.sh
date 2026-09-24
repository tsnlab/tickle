#!/usr/bin/env bash
# Every commit sha cited in the documentation must exist and be reachable from HEAD.
#
# Why (2026-09-24): six shas across PLAN.md and COMPARISON.MD named commits that are not in the
# repository at all. None of it was careless - each was recorded at the moment the work was done,
# from the sha in front of whoever did it, and a rebase before the push changed it. No step in
# anyone's workflow compared the local sha to the published one.
#
# That is why this is a check and not a rule. "Record the sha with every figure" does nothing if
# the sha recorded is the pre-rebase one, and the person recording it cannot tell the difference at
# the time. Only something that looks afterwards can.
#
# What it cost: one of the six was under COMPARISON.MD section 3b's TRANSIENT_LOCAL result, so a
# load-bearing claim cited a commit nobody could check out; another sent an investigation looking
# for a measurement at a commit that does not exist.
#
# Reachable from HEAD rather than from origin/main, deliberately: a document on a branch may
# legitimately cite a commit on that same branch before it is merged. What must never happen is a
# citation to a commit that is nowhere.
set -uo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 1

DOCS=(rmw_tickle/PLAN.md rmw_tickle/COMPARISON.MD README.md)

fail=0
checked=0

for doc in "${DOCS[@]}"; do
    [ -f "$doc" ] || continue
    # Backtick-quoted hex runs of 7-40 characters. Two things that match this and are not broken
    # citations, both found by running it:
    #
    #   - CI run ids. `35054681478` is eleven decimal digits, which is a subset of hex. A string
    #     that does not resolve AND contains no a-f is not a sha anyone could have meant, so it is
    #     skipped silently rather than reported. A genuinely all-decimal sha still resolves and is
    #     still checked, so this costs no coverage.
    #   - Shas quoted *as* broken. The audit note that prompted this check names the six it found,
    #     and a check that fails on the record of what it found is not much of a check. A line
    #     carrying the marker below opts out, which is explicit and greppable, unlike inferring it
    #     from the prose around it.
    # shellcheck disable=SC2016 # the backtick in the pattern below is a literal character being
    # matched by grep, not a command substitution - single quotes are what keeps it literal.
    while IFS= read -r candidate; do
        sha="${candidate//\`/}"
        if grep -n -- "$sha" "$doc" | grep -q 'doc-shas-ignore'; then
            continue
        fi
        checked=$((checked + 1))
        if ! git cat-file -e "${sha}^{commit}" 2>/dev/null; then
            case "$sha" in
            *[a-f]*) ;;
            *)
                checked=$((checked - 1))
                continue
                ;; # all digits and not a commit: a run id, not a citation
            esac
            echo "$doc: '$sha' is not a commit in this repository" >&2
            grep -n -- "$sha" "$doc" | head -2 | sed 's/^/    /' >&2
            fail=1
            continue
        fi
        if ! git merge-base --is-ancestor "$sha" HEAD 2>/dev/null; then
            echo "$doc: '$sha' exists but is not reachable from HEAD" >&2
            grep -n -- "$sha" "$doc" | head -2 | sed 's/^/    /' >&2
            fail=1
        fi
    done < <(grep -ohE '`[0-9a-f]{7,40}`' "$doc" | sort -u)
done

if [ "$fail" = 0 ]; then
    echo "doc-shas: $checked cited sha(s) all exist and are reachable from HEAD"
fi
exit "$fail"
