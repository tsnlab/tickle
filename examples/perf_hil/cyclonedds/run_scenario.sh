#!/usr/bin/env bash
# Orchestrates one CycloneDDS HIL scenario across the two rpis (rpi#1=client, rpi#2=server),
# mirroring .github/scripts/run_perf.sh's own run_paired_test() pattern for TickLE's own HIL
# binaries, standalone (not touching that established script - comparison.md's own design note:
# this whole exercise is a separate track from TickLE Dev's own core/rmw_tickle work).
#
# LD_LIBRARY_PATH set explicitly rather than relying on the built binary's own rpath - a real,
# confirmed glibc ld.so quirk found the hard way: libddsc.so.0 carries its own DT_RUNPATH, which
# shadows the executable's DT_RPATH for libddsc's *own* transitive dependencies (libiceoryx_
# binding_c.so) - no rpath flag combination on the executable itself reliably covers this, but
# LD_LIBRARY_PATH always wins regardless of that shadowing rule.
#
# CYCLONEDDS_URI shortens SPDPInterval from its own 30s default - a real, confirmed root cause
# (not tuned blind): CycloneDDS's default participant-discovery announce interval is 30s, and each
# participant only otherwise announces once at its own startup - common.h's wait_for_*_match()
# helpers make every scenario correctly *wait* for a real match instead of guessing a sleep
# duration, but without this, an unlucky timing miss on the one-shot startup announce would still
# mean waiting out the full 30s before the next one, not a code bug on either end.
set -euo pipefail

SCENARIO="${1:?usage: run_scenario.sh <scenario> [client_args...]}"
shift
CLIENT_ARGS="${*:--i 0.1 -d 10}"

SSH_KEY="$HOME/.ssh/tickle_ci_ed25519"
RPI_CLIENT="10.1.1.214"
RPI_SERVER="10.1.1.213"
LIB_PATH="/opt/ros/jazzy/lib/aarch64-linux-gnu"
CDDS_URI='<CycloneDDS><Domain><Discovery><SPDPInterval>1s</SPDPInterval></Discovery></Domain></CycloneDDS>'
REMOTE_DIR="tickle/examples/perf_hil/cyclonedds/$SCENARIO"

ssh_run() {
    local host="$1"
    shift
    # shellcheck disable=SC2029
    ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=5 "ci@$host" "$@"
}

ssh_run "$RPI_SERVER" "export LD_LIBRARY_PATH=$LIB_PATH; export CYCLONEDDS_URI='$CDDS_URI'; cd ~/$REMOTE_DIR && nohup ./server > /tmp/cdds_${SCENARIO}_server.log 2>&1 &"
sleep 2
ssh_run "$RPI_CLIENT" "export LD_LIBRARY_PATH=$LIB_PATH; export CYCLONEDDS_URI='$CDDS_URI'; cd ~/$REMOTE_DIR && ./client $CLIENT_ARGS" | grep '^RESULT:'
ssh_run "$RPI_SERVER" "pkill -INT -x server" || true
