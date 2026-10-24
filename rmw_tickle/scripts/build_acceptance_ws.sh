#!/usr/bin/env bash
# Provisions the workspace rmw_gap_acceptance.sh needs, in the one shape CI and a local run share:
#
#   WS/rmw/install     rmw_tickle and its typesupports, from this checkout
#   WS/ifaces/install  the standard ROS 2 interfaces rebuilt WITH TickLE's typesupport (build_ros2_interfaces.sh -a),
#                      which the installed copies do not carry
#
# Usage: build_acceptance_ws.sh -w WS
#   ROS_DISTRO_DIR (default /opt/ros/lyrical) is the ROS 2 install both stages build against; CI sets /opt/ros/jazzy.
#
# BUILD_SHARED_LIBS=ON is not optional: rosidl_typesupport_c dlopens a message's typesupport by convention name, which
# a static archive cannot satisfy. Stage 2 needs stage 1 sourced, because it looks for rosidl_typesupport_tickle_c, and
# it clones each interface repository at the tag matching the installed version.
set -euo pipefail
WS=""
while getopts "w:" o; do case "$o" in w) WS=$OPTARG ;; *) exit 2 ;; esac; done
[ -n "$WS" ] || { echo "usage: $0 -w WS" >&2; exit 2; }
DISTRO=${ROS_DISTRO_DIR:-/opt/ros/lyrical}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$WS"
WS=$(cd "$WS" && pwd)
log() { echo "[$(date -Is)] $*"; }

log "stage 1/2: rmw_tickle + typesupports from $REPO -> $WS/rmw/install ($DISTRO)"
set +u
# shellcheck disable=SC1091
source "$DISTRO/setup.bash"
set -u
(cd "$REPO" && colcon build \
    --packages-select rmw_tickle rosidl_typesupport_tickle_c rosidl_typesupport_tickle_cpp \
    --build-base "$WS/rmw/build" --install-base "$WS/rmw/install" \
    --cmake-args -DBUILD_SHARED_LIBS=ON)

log "stage 2/2: standard interfaces with TickLE typesupport -> $WS/ifaces/install"
set +u
# shellcheck disable=SC1091
source "$WS/rmw/install/setup.bash"
set -u
"$REPO/rmw_tickle/scripts/build_ros2_interfaces.sh" -w "$WS/ifaces" -a
log "done: $WS"
