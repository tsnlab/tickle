#!/usr/bin/env bash
# pi_checkout_selftest.sh - pi_checkout.sh against a stand-in Pi on this PC: no rig, no ssh, no network.
#
# A "PC" repository with three commits, an "origin" bare repository, and a "Pi" clone of it whose remote-tracking
# `origin/rmw` goes stale when origin replaces branch `rmw` with `rmw/next` - the ref conflict that, on 2026-10-16, made
# `git fetch -q origin` fail on the rig and the checkout silently keep the previous commit. Each case runs the remote
# text the way a harness does (`bash -c "set -e; cd ~/tickle && <text> && ..."`, HOME = the stand-in Pi home) and
# checks what it printed and how it exited. The first case runs the OLD line and must reproduce the defect, or the
# fixture is not the world the fix is for.
#
# Usage: pi_checkout_selftest.sh        prints one line per case and exits non-zero if any case did not behave.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
W=$(mktemp -d "${TMPDIR:-/tmp}/pi_checkout_selftest.XXXXXX") || exit 2
trap 'rm -rf "$W"' EXIT
g() { git -c user.name=selftest -c user.email=selftest@localhost -c init.defaultBranch=main "$@"; }

# The PC: commits A, B and C on main; only A and B are pushed at first.
g init -q "$W/pc" || exit 2
for c in A B C; do echo "$c" >"$W/pc/f"; g -C "$W/pc" add f; g -C "$W/pc" commit -q -m "$c"; done
A=$(g -C "$W/pc" rev-parse HEAD~2); B=$(g -C "$W/pc" rev-parse HEAD~1); C=$(g -C "$W/pc" rev-parse HEAD)
g init -q --bare "$W/origin.git" || exit 2
g -C "$W/pc" push -q "$W/origin.git" "$B:refs/heads/main" "$A:refs/heads/rmw" || exit 2
# The Pi: a clone at A, holding origin/rmw. A second one (pi2), untouched until case 5, for the fallback.
for pi in pi pi2; do
    mkdir -p "$W/$pi" && g clone -q "$W/origin.git" "$W/$pi/tickle" && g -C "$W/$pi/tickle" reset -q --hard "$A" || exit 2
done
# Origin moves on: C is pushed, and branch rmw is replaced by rmw/next - a path the Pi's stale origin/rmw blocks.
g -C "$W/pc" push -q "$W/origin.git" "$C:refs/heads/main" ":refs/heads/rmw" "$C:refs/heads/rmw/next" || exit 2

# shellcheck disable=SC2034 # read by pi_checkout.sh
PI_CHECKOUT_REPO="$W/pc"
# shellcheck source=examples/perf_hil/experiments/pi_checkout.sh
# shellcheck disable=SC1091 # found from the script's own path at runtime; shellcheck -x follows the source= above
. "$HERE/pi_checkout.sh"

fail=0
# remote <text>: run <text> as a harness does on the stand-in Pi; prints its output, returns its status.
remote() { HOME="$W/${PI:-pi}" bash -c "set -e; $1" 2>&1; }
# expect <name> <want status: 0|nonzero> <pattern in the output> <pattern NOT in the output> <text> [<also in output>]
expect() {
    local name=$1 want=$2 has=$3 hasnt=$4 also=${6:-} out rc ok=1
    out=$(remote "$5"); rc=$?
    case "$want:$rc" in 0:0 | nonzero:[1-9]*) ;; *) ok=0 ;; esac
    grep -qE -- "$has" <<<"$out" || ok=0
    [ -n "$also" ] && { grep -qE -- "$also" <<<"$out" || ok=0; }
    [ -n "$hasnt" ] && grep -qE -- "$hasnt" <<<"$out" && ok=0
    if [ "$ok" = 1 ]; then echo "ok    $name (exit $rc)"; else
        echo "FAIL  $name (exit $rc, wanted $want; output:)"; while IFS= read -r l; do echo "        $l"; done <<<"$out"; fail=1
    fi
}
# shellcheck disable=SC2016 # expanded on the stand-in Pi
BUILD='echo "BUILD at $(git rev-parse HEAD)"'

# 0. The old line, as the harnesses had it: the fetch fails on the ref conflict, the reset is skipped, nothing fails,
#    and the build runs at the Pi's previous commit A. If this does not reproduce, the fixture is wrong.
g -C "$W/pi/tickle" reset -q --hard "$A"
expect "old line: fetch fails, build still runs at the old commit" 0 "BUILD at $A" "" \
    "cd ~/tickle && git fetch -q origin && git reset -q --hard $C && git clean -fdqx
$BUILD" "could not be updated|conflicting"
# 1. The new text, same conflict: the fetch by sha writes no ref, so it succeeds, and HEAD is verified at C.
g -C "$W/pi/tickle" reset -q --hard "$A"
expect "pi_checkout through the ref conflict reaches C" 0 "BUILD at $C" "CHECKOUT FAILED" \
    "cd ~/tickle && $(pi_checkout "$C") && git clean -fdqx
$BUILD"
# 2. A short sha on the PC names the same full commit on the Pi.
g -C "$W/pi/tickle" reset -q --hard "$A"
expect "a short sha is resolved on the PC" 0 "BUILD at $C" "" "cd ~/tickle && $(pi_checkout "${C:0:8}")
$BUILD"
# 3. A commit origin does not have (never pushed): the fetch and its --prune fallback both leave it missing - refused
#    before the build, inside an && list, with the reason.
D=$(g -C "$W/pc" commit-tree -p "$C" -m D "$(g -C "$W/pc" rev-parse "$C^{tree}")")
g -C "$W/pi/tickle" reset -q --hard "$A"
expect "an unpushed commit is refused, nothing built" nonzero "CHECKOUT FAILED on .*: .*$D" "BUILD at" \
    "cd ~/tickle && $(pi_checkout "$D") && git clean -fdqx
$BUILD"
# 4. A name the PC cannot resolve does not run anything on the Pi.
expect "an unknown name is refused on the Pi" nonzero "CHECKOUT FAILED.*does not resolve on the PC" "BUILD at" \
    "cd ~/tickle && $(pi_checkout no-such-branch 2>/dev/null) && git clean -fdqx
$BUILD"
# 5. The server refuses a sha it does not advertise (protocol v0, no allow*SHA1InWant): the --prune fallback fetches
#    every branch, and --prune removes the stale origin/rmw that blocked rmw/next.
#    pi2 has never fetched since origin moved: it holds the stale origin/rmw and not C. Origin's branches move one
#    commit past C, so C is reachable but no longer advertised.
E=$(g -C "$W/pc" commit-tree -p "$C" -m E "$(g -C "$W/pc" rev-parse "$C^{tree}")")
g -C "$W/pc" push -q -f "$W/origin.git" "$E:refs/heads/main" "$E:refs/heads/rmw/next" || exit 2
g -C "$W/pi2/tickle" config protocol.version 0
PI=pi2 expect "fallback: a refused sha want is fetched by --prune" 0 "BUILD at $C" "CHECKOUT FAILED" \
    "cd ~/tickle && $(pi_checkout "$C") && git clean -fdqx
$BUILD" "not our ref|unadvertised|allow"
# 6. After the build: a tree that is not at the commit asked for is refused, and one that is passes.
g -C "$W/pi/tickle" reset -q --hard "$B"
expect "verify after the build: a tree at B is refused for C" nonzero "CHECKOUT FAILED on .*: .* is at $B, not $C" \
    "RUN" "cd ~/tickle && $(pi_verify_head "\$HOME/tickle" "$C") && echo RUN"
expect "verify after the build: a tree at B passes for B" 0 "RUN" "CHECKOUT FAILED" \
    "cd ~/tickle && $(pi_verify_head "\$HOME/tickle" "$B") && echo RUN"
# 7. A variant tree that does not exist is refused too, with its reason - also as a command of its own under set -e,
#    where a failing $(git ...) would otherwise end the shell before the message.
expect "verify: a missing tree is refused" nonzero "is at no commit" "RUN" \
    "$(pi_verify_head "\$HOME/rmw_variants/pre/src" "$B") && echo RUN"
expect "verify: a missing tree is refused, standalone" nonzero "is at no commit" "RUN" \
    "$(pi_verify_head "\$HOME/rmw_variants/pre/src" "$B")
echo RUN"

if [ "$fail" = 0 ]; then echo "pi_checkout_selftest: every case behaved"; else echo "pi_checkout_selftest: FAILED"; fi
exit "$fail"
