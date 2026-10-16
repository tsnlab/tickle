# shellcheck shell=bash
# shellcheck disable=SC2016 # every single-quoted $ here is meant for the Pi's shell, not this one
# pi_checkout.sh - sourced by the rig harnesses: the text of a remote command that puts a Pi's ~/tickle at exactly the
# commit asked for, or stops the remote shell with a reason.
#
# Why (2026-10-16): rmw_keepall_rig.sh built on the Pis with
#     set -e; cd ~/tickle && git fetch -q origin && git reset -q --hard $HEAD_SHA && git clean ...
#     colcon build ...
# and origin had grown a branch `rmw/...` while the Pi still held a remote-tracking `origin/rmw`, so the fetch failed on
# the ref conflict. A failure inside an && list does not trigger set -e: the reset was skipped, the next line built the
# PREVIOUS checkout, and two rig runs measured a commit nobody had asked for. The same line was in some forty harnesses.
#
# What the text it prints does, in the remote shell (sh or bash, set -e or not, alone or inside an && list - it exits
# the shell itself rather than returning a status someone may ignore):
#   - fetches exactly the commits named, by full sha (`git fetch origin <sha>`: no remote-tracking ref is written, so no
#     stale or conflicting ref can stop it), falling back to `git fetch --prune origin` when the server refuses a sha;
#   - checks every named commit is then present, resets ~/tickle hard to the first, and compares `git rev-parse HEAD`
#     with its full sha;
#   - on any failure prints "CHECKOUT FAILED on <host>: <what>" to stderr and runs `exit 1`.
# Shas are resolved to full ones HERE, in the checkout the harness runs from, so a short sha or a branch name names one
# commit and the Pi is compared against that one. A name this checkout cannot resolve does not run at all: the printed
# text is itself a refusal that exits the remote shell.
#
# Use it inside a double-quoted remote command or an unquoted here-document - the output of $(...) is not expanded
# again, so its own $ reach the Pi as written:
#     . "$REPO/examples/perf_hil/experiments/pi_checkout.sh"
#     sh_ "$h" "set -e; cd ~/tickle && $(pi_checkout "$SHA") && git clean -fdqx -e install -e build -e log ..."
# and after a build, against every tree it built (the directory is shell text, expanded on the Pi):
#     ... colcon build ...; $(pi_verify_head "\$HOME/rmw_variants/pre/src" "$PRE_SHA")
#
# pi_full_sha <rev>: the full sha of <rev> in this checkout (stdout), or a message on stderr and status 1.
# pi_checkout <rev> [<rev>...]: fetch every <rev>, reset hard to the first, verify HEAD.
# pi_fetch <rev>...: fetch every <rev> and check each is present; no reset.
# pi_verify_head <dir> <rev>: refuse unless `git -C <dir> rev-parse HEAD` is <rev>'s full sha.

PI_CHECKOUT_REPO="${PI_CHECKOUT_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"

pi_full_sha() {
    local full
    full=$(git -C "$PI_CHECKOUT_REPO" rev-parse --verify --quiet "$1^{commit}" 2>/dev/null) || {
        echo "pi_checkout: '$1' does not name a commit in $PI_CHECKOUT_REPO (fetch it, or push it and fetch)" >&2
        return 1
    }
    echo "$full"
}

# The refusal printed in place of a command when a name does not resolve here: the remote shell stops with the reason.
pi_refuse_text() {
    printf '{ echo "CHECKOUT FAILED on $(hostname): %s" >&2; exit 1; }' "$1"
}

# pi_fetch_text <full>...: fetch, by sha, with the --prune fallback; then every sha must be a commit on the Pi.
pi_fetch_text() {
    local s present=""
    for s in "$@"; do present="$present git cat-file -e \"$s^{commit}\" 2>/dev/null &&"; done
    printf '{ git fetch -q origin %s || git fetch -q --prune origin || { echo "CHECKOUT FAILED on $(hostname): git fetch origin" >&2; exit 1; }; %s true || { echo "CHECKOUT FAILED on $(hostname): after the fetch a commit is still missing of: %s" >&2; exit 1; }; }' \
        "$*" "$present" "$*"
}

pi_fetch() {
    local r full fulls=()
    for r in "$@"; do
        full=$(pi_full_sha "$r") || { pi_refuse_text "'$r' does not resolve on the PC"; return 0; }
        fulls+=("$full")
    done
    pi_fetch_text "${fulls[@]}"
}

pi_checkout() {
    local r full fulls=()
    for r in "$@"; do
        full=$(pi_full_sha "$r") || { pi_refuse_text "'$r' does not resolve on the PC"; return 0; }
        fulls+=("$full")
    done
    printf '{ %s; git reset -q --hard %s || { echo "CHECKOUT FAILED on $(hostname): git reset --hard %s" >&2; exit 1; }; %s; }' \
        "$(pi_fetch_text "${fulls[@]}")" "${fulls[0]}" "${fulls[0]}" "$(pi_verify_text . "${fulls[0]}")"
}

# pi_verify_text <dir> <full>
pi_verify_text() {
    printf '{ tt_head=$(git -C %s rev-parse HEAD 2>/dev/null || true); [ "$tt_head" = %s ] || { echo "CHECKOUT FAILED on $(hostname): %s is at ${tt_head:-no commit}, not %s" >&2; exit 1; }; }' \
        "$1" "$2" "$1" "$2"
}

pi_verify_head() {
    local full
    full=$(pi_full_sha "$2") || { pi_refuse_text "'$2' does not resolve on the PC"; return 0; }
    pi_verify_text "$1" "$full"
}
