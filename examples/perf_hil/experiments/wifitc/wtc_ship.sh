#!/usr/bin/env bash
# shellcheck disable=SC2086
# Copies the topology script and the calibration probe of this directory to both Pis (~/wtc_scripts); the node programs are those already in ~/t1pi_bin.
set -eu
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
KEY=${PI_KEY:-$HOME/.ssh/tickle_ci_ed25519}
SSHO="-i $KEY -o StrictHostKeyChecking=no -o BatchMode=yes"
for h in ${CLI_SSH:-ci@10.1.1.214} ${SRV_SSH:-ci@10.1.1.213}; do
    ssh $SSHO "$h" "mkdir -p wtc_scripts wtc_run" </dev/null
    tar -C "$S" -c wtc_topo.sh wtc_probe.py wtc_netem_player.py | ssh $SSHO "$h" "tar -x -C wtc_scripts"
done
echo WTC_SHIP_OK
