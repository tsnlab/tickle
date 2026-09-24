#!/usr/bin/env bash
# Builds ROS 2 interface packages (std_msgs, geometry_msgs, ...) from source with TickLE's
# typesupport, into a workspace of your own, so rmw_tickle can carry them.
#
# Why this exists: the interface packages installed with ROS 2 carry the typesupports their build
# knew about - FastDDS, CycloneDDS - and not TickLE's. rmw_tickle needs each package's own
# rosidl_typesupport_tickle_c library, which only a build with rosidl_typesupport_tickle_c present
# produces. The user's decision of 2026-09-24: ship patches and a build script, the user builds the
# packages once in their own workspace (the way CI does); no prebuilt packages.
#
# What it does, per package:
#   - clones the upstream repository at the tag matching the version ROS 2 has installed, so the
#     rebuilt package is the same interface - same fields, same generated C structs - as the one
#     every other installed package was built against. Falls back to the distro branch, with a
#     warning, if that tag does not exist.
#   - applies rmw_tickle's patches for that repository, if it has any (rosidl_typesupport_tickle_c/
#     patches/<repo>/*.patch). Capacities need none: they come from the shipped profiles and your
#     own TICKLE_CAPACITIES_PATH files (rosidl_typesupport_tickle_c.capacity_profile).
#   - builds it with colcon, overriding the installed copy.
#
# Usage:
#   source /opt/ros/$ROS_DISTRO/setup.bash
#   source <rmw_tickle install>/setup.bash      # rosidl_typesupport_tickle_c must be found
#   build_ros2_interfaces.sh [-w WORKSPACE] [PACKAGE...]
#   source WORKSPACE/install/setup.bash          # before running anything that uses them
#
# PACKAGE defaults to builtin_interfaces and std_msgs. List the packages a type you need depends
# on as well - std_msgs/Header nests builtin_interfaces/Time, and a package nesting another's type
# is only generated for TickLE when that other package was.
#
# Changing tt_MAX_BUFFER_LENGTH (TICKLE_MAX_BUFFER_LENGTH, rosidl_typesupport_tickle_c) means
# rebuilding these too: rmw_tickle refuses a type generated for a different value.
set -euo pipefail

usage() {
    sed -n '2,/^set -euo/p' "$0" | sed -e 's/^# \{0,1\}//' -e '$d'
    exit "${1:-0}"
}

WORKSPACE="${HOME}/tickle_ros2_interfaces"
while getopts "w:h" opt; do
    case "$opt" in
    w) WORKSPACE="$OPTARG" ;;
    h) usage 0 ;;
    *) usage 1 ;;
    esac
done
shift $((OPTIND - 1))
PACKAGES=("$@")
[ ${#PACKAGES[@]} -gt 0 ] || PACKAGES=(builtin_interfaces std_msgs)

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCHES="$HERE/../rosidl_typesupport_tickle_c/patches"

fail() {
    echo "build_ros2_interfaces: $*" >&2
    exit 1
}

[ -n "${ROS_DISTRO:-}" ] || fail "no ROS 2 environment - source /opt/ros/<distro>/setup.bash first"
command -v colcon >/dev/null || fail "colcon not found"
command -v git >/dev/null || fail "git not found"
tickle_prefix=""
IFS=: read -r -a prefixes <<<"${AMENT_PREFIX_PATH:-}"
for prefix in "${prefixes[@]}"; do
    if [ -f "$prefix/share/rosidl_typesupport_tickle_c/package.xml" ]; then
        tickle_prefix="$prefix"
        break
    fi
done
[ -n "$tickle_prefix" ] || fail "rosidl_typesupport_tickle_c not found on AMENT_PREFIX_PATH - source rmw_tickle's install first"
echo "rosidl_typesupport_tickle_c: $tickle_prefix"

# Which upstream repository each package lives in.
repo_of() {
    case "$1" in
    builtin_interfaces | rcl_interfaces | action_msgs | lifecycle_msgs | composition_interfaces | rosgraph_msgs | \
        statistics_msgs | type_description_interfaces | service_msgs) echo rcl_interfaces ;;
    std_msgs | geometry_msgs | sensor_msgs | nav_msgs | diagnostic_msgs | shape_msgs | trajectory_msgs | \
        visualization_msgs | std_srvs | actionlib_msgs) echo common_interfaces ;;
    example_interfaces) echo example_interfaces ;;
    unique_identifier_msgs) echo unique_identifier_msgs ;;
    *) fail "don't know which repository '$1' comes from - add it to repo_of()" ;;
    esac
}

# The version ROS 2 has installed, from its package.xml on AMENT_PREFIX_PATH (the first found that
# is not our own workspace).
installed_version() {
    local prefix
    for prefix in "${prefixes[@]}"; do
        case "$prefix" in "$WORKSPACE"/*) continue ;; esac
        if [ -f "$prefix/share/$1/package.xml" ]; then
            sed -n 's:.*<version>\(.*\)</version>.*:\1:p' "$prefix/share/$1/package.xml" | head -1
            return
        fi
    done
}

SRC="$WORKSPACE/src/tickle_ros2_interfaces"
mkdir -p "$SRC"
declare -A cloned=()
for pkg in "${PACKAGES[@]}"; do
    repo=$(repo_of "$pkg")
    [ -z "${cloned[$repo]:-}" ] || continue
    version=$(installed_version "$pkg")
    [ -n "$version" ] || fail "$pkg is not installed with ROS 2 $ROS_DISTRO - nothing to match its version to"
    dest="$SRC/$repo"
    if [ -d "$dest/.git" ] && [ "$(git -C "$dest" describe --tags --exact-match 2>/dev/null || true)" = "$version" ]; then
        echo "$repo: already at $version"
    else
        rm -rf "$dest"
        if git ls-remote --exit-code --tags "https://github.com/ros2/$repo" "refs/tags/$version" >/dev/null 2>&1; then
            echo "$repo: cloning tag $version (the version installed for $pkg)"
            git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$version" "https://github.com/ros2/$repo" "$dest"
        else
            echo "$repo: WARNING no tag $version upstream - cloning branch $ROS_DISTRO instead; its interfaces may" \
                "differ from the installed $pkg" >&2
            git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$ROS_DISTRO" "https://github.com/ros2/$repo" "$dest"
        fi
        if [ -d "$PATCHES/$repo" ]; then
            for patch in "$PATCHES/$repo"/*.patch; do
                [ -f "$patch" ] || continue
                echo "$repo: applying $(basename "$patch")"
                git -C "$dest" apply "$patch"
            done
        fi
    fi
    cloned[$repo]=1
done

override=()
if colcon build --help 2>/dev/null | grep -q -- --allow-overriding; then
    override=(--allow-overriding "${PACKAGES[@]}")
fi
colcon build --base-paths "$SRC" --build-base "$WORKSPACE/build" --install-base "$WORKSPACE/install" \
    "${override[@]}" --packages-select "${PACKAGES[@]}" --cmake-args -DBUILD_SHARED_LIBS=ON

# Proof, not assumption: each package now has TickLE's typesupport library.
missing=0
for pkg in "${PACKAGES[@]}"; do
    lib="$WORKSPACE/install/$pkg/lib/lib${pkg}__rosidl_typesupport_tickle_c.so"
    if [ -f "$lib" ]; then
        echo "  ok  $pkg ($lib)"
    else
        echo "  MISSING  $pkg - no $lib" >&2
        missing=1
    fi
done
[ "$missing" = 0 ] || fail "some packages were built without TickLE typesupport"
echo
echo "Done. Before running anything that uses these, in every shell:"
echo "  source $WORKSPACE/install/setup.bash"
