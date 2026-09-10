#!/usr/bin/env bash
# Merge one section's results into the platform-status table on the gh-pages benchmark page.
#
#   publish_dashboard.sh <section> <fragment.json>
#
# <section> is "buildtest" or "perf" (see dashboard.py). <fragment.json> is that section's
# freshly measured data. This checks out gh-pages, folds the fragment into dev/bench/status.json,
# re-renders the table into dev/bench/index.html, and pushes - retrying against a fresh gh-pages
# each time, since test-all.yml, performance.yml and github-action-benchmark can all be pushing
# there at once on a main push. Meant to run on an ubuntu-latest runner (needs python3 + git; no
# jq). A no-op (no commit, exit 0) when nothing changed.
set -euo pipefail

SECTION="${1:?usage: publish_dashboard.sh <section> <fragment.json>}"
FRAGMENT="${2:?usage: publish_dashboard.sh <section> <fragment.json>}"
FRAGMENT="$(realpath "$FRAGMENT")"

: "${GITHUB_TOKEN:?publish_dashboard.sh needs GITHUB_TOKEN in the environment}"
REPO="${GITHUB_REPOSITORY:-tsnlab/tickle}"
SERVER="${GITHUB_SERVER_URL:-https://github.com}"
GITHUB_SHA="${GITHUB_SHA:-unknown}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

git config --global user.name "github-actions[bot]"
git config --global user.email "41898282+github-actions[bot]@users.noreply.github.com"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
auth_repo="${SERVER#https://}"
git clone --quiet --branch gh-pages --single-branch \
    "https://x-access-token:${GITHUB_TOKEN}@${auth_repo}/${REPO}.git" "$work/gh-pages"

cd "$work/gh-pages"
mkdir -p dev/bench

for attempt in 1 2 3 4 5; do
    git fetch --quiet origin gh-pages
    git reset --hard --quiet origin/gh-pages

    python3 "$SCRIPT_DIR/dashboard.py" merge dev/bench/status.json "$SECTION" "$FRAGMENT"
    if [ ! -f dev/bench/index.html ]; then
        echo "dev/bench/index.html not present yet - github-action-benchmark hasn't run; skipping inject."
    else
        python3 "$SCRIPT_DIR/dashboard.py" inject dev/bench/status.json dev/bench/index.html
    fi

    # The site root would otherwise 404 - the real page github-action-benchmark builds is one
    # level down. Point visitors at it.
    if [ ! -f index.html ]; then
        printf '<!doctype html><meta charset="utf-8"><title>TickLE CI</title>%s\n' \
            '<meta http-equiv="refresh" content="0; url=dev/bench/">' > index.html
    fi

    git add index.html dev/bench/status.json dev/bench/index.html 2>/dev/null \
        || git add index.html dev/bench/status.json
    if git diff --cached --quiet; then
        echo "dashboard: no change for section '$SECTION'."
        exit 0
    fi

    git commit --quiet -m "dashboard: update '$SECTION' (${GITHUB_SHA:0:7})"
    if git push --quiet origin HEAD:gh-pages; then
        echo "dashboard: published section '$SECTION' (attempt $attempt)."
        exit 0
    fi
    echo "dashboard: push race on attempt $attempt, retrying..."
    sleep $((attempt * 3))
done

echo "dashboard: giving up after 5 attempts." >&2
exit 1
