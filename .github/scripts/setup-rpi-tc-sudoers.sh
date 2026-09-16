#!/usr/bin/env bash
# One-time setup for run_perf.sh's tc/netem loss-injection scenarios (QoS roadmap #5,
# rmw_tickle/PLAN.md) - see .github/scripts/README.md's "What's needed" section.
#
# Run this ON rpi#1 itself (the sender - the only Pi run_perf.sh ever runs `tc` on), as root:
#
#   sudo bash setup-rpi-tc-sudoers.sh
#
# or, from wherever you can already SSH into rpi#1 as a user with sudo, without copying the file
# over first:
#
#   ssh <user>@<rpi#1> 'sudo bash -s' < .github/scripts/setup-rpi-tc-sudoers.sh
#
# Grants the `ci` user (the account run_perf.sh's SSH key logs in as) passwordless sudo for the
# `tc` binary only - not a blanket NOPASSWD:ALL - so probe_loss_testing()'s own `sudo -n tc ...`
# can succeed non-interactively over SSH. Idempotent: re-running just overwrites the same file
# with the same content.
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this as root, e.g.: sudo bash $0" >&2
    exit 1
fi

CI_USER="ci"
if ! id "$CI_USER" >/dev/null 2>&1; then
    echo "User '$CI_USER' doesn't exist on this host - .github/scripts/README.md's own \"What's" >&2
    echo "needed\" section expects it (the account run_perf.sh SSHes in as); create it first." >&2
    exit 1
fi

TC_PATH="$(command -v tc || true)"
if [ -z "$TC_PATH" ]; then
    echo "tc not found on PATH - install it first: sudo apt install iproute2" >&2
    exit 1
fi

SUDOERS_FILE=/etc/sudoers.d/tickle-ci-tc
TMP_FILE="$(mktemp)"
trap 'rm -f "$TMP_FILE"' EXIT

printf '%s ALL=(root) NOPASSWD: %s\n' "$CI_USER" "$TC_PATH" > "$TMP_FILE"

# Validated *before* installing - a malformed sudoers file breaks sudo system-wide for everyone,
# not just this one entry, so this never touches /etc/sudoers.d/ with something visudo rejects.
if ! visudo -c -f "$TMP_FILE"; then
    echo "Generated sudoers snippet failed validation - not installing anything." >&2
    exit 1
fi

install -o root -g root -m 0440 "$TMP_FILE" "$SUDOERS_FILE"
echo "Installed $SUDOERS_FILE:"
cat "$SUDOERS_FILE"

echo
echo "Verifying as $CI_USER..."
if su - "$CI_USER" -c "sudo -n '$TC_PATH' qdisc show" >/dev/null 2>&1; then
    echo "OK: $CI_USER can run 'sudo tc' non-interactively - run_perf.sh's loss-injection"
    echo "scenarios will run on the next push to main."
else
    echo "WARNING: verification failed even after installing the sudoers entry." >&2
    echo "Check manually: su - $CI_USER -c \"sudo -n $TC_PATH qdisc show\"" >&2
    exit 1
fi
