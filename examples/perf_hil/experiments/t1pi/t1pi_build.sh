#!/usr/bin/env bash
# Build the T1 node programs (node_d100, node_base, large) natively on the client Pi from this tree, then copy the scripts and
# binaries to both Pis. Runs on the dev PC (ssh). Needs the rig lock only if a measurement is running on the Pis (it is not
# a measurement, but it loads rpi-1's CPUs: take the lock).
#   t1pi_build.sh            (CLI_SSH / SRV_SSH override the ssh targets)
set -eu
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$S/../../../.." && pwd)
KEY=${PI_KEY:-$HOME/.ssh/tickle_ci_ed25519}
SSHO="-i $KEY -o StrictHostKeyChecking=no -o BatchMode=yes"
CLI=${CLI_SSH:-ci@10.1.1.214}
SRV=${SRV_SSH:-ci@10.1.1.213}
SRC=t1pi_src
BIN=t1pi_bin
HEADREV=$(git -C "$REPO" rev-parse --short HEAD)
for h in "$CLI" "$SRV"; do
    # shellcheck disable=SC2086
    ssh $SSHO "$h" "rm -rf $SRC && mkdir -p $SRC $BIN t1pi_scripts" </dev/null
done
# shellcheck disable=SC2086
git -C "$REPO" archive HEAD | ssh $SSHO "$CLI" "tar -x -C $SRC"
# shellcheck disable=SC2086
ssh $SSHO "$CLI" "cd $SRC/examples/perf_hil/experiments/t1 && T1_DIR=\$HOME/$BIN ./t1_build_node.sh \$HOME/$BIN"
# scripts and binaries to both Pis
for h in "$CLI" "$SRV"; do
    # shellcheck disable=SC2086
    tar -C "$S" -c t1pi_topo.sh t1pi_run.sh | ssh $SSHO "$h" "tar -x -C t1pi_scripts"
done
# shellcheck disable=SC2086
ssh $SSHO "$CLI" "tar -C $BIN -c node_d100 node_base large" | ssh $SSHO "$SRV" "tar -x -C $BIN"
echo "T1PI_BUILD_OK tree=$HEADREV"
