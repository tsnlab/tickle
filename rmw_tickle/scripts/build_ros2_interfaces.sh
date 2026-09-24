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
#   build_ros2_interfaces.sh [-w WORKSPACE] [-a | PACKAGE...]
#   source WORKSPACE/install/setup.bash          # before running anything that uses them
#
# PACKAGE defaults to builtin_interfaces and std_msgs. -a builds every package TickLE ships
# capacities for (INVENTORY below) - what a default rclcpp::Node needs is among them
# (rcl_interfaces, type_description_interfaces, rosgraph_msgs, ...).
#
# The interface packages a PACKAGE depends on are added automatically, from its package.xml: a
# type nesting another package's type is only generated for TickLE when that other package was
# (std_msgs/Header nests builtin_interfaces/Time), so building geometry_msgs alone would decline
# every type that carries a Header.
#
# A type TickLE cannot represent - a wstring, such as example_interfaces/WString - is declined, not
# failed: its package builds, and rmw_tickle refuses that one type at creation with the reason.
#
# Changing tt_MAX_BUFFER_LENGTH (TICKLE_MAX_BUFFER_LENGTH, rosidl_typesupport_tickle_c) means
# rebuilding these too: rmw_tickle refuses a type generated for a different value.
set -euo pipefail

usage() {
    sed -n '2,/^set -euo/p' "$0" | sed -e 's/^# \{0,1\}//' -e '$d'
    exit "${1:-0}"
}

# Every jazzy interface package the shipped capacity profiles cover (rmw_tickle/tools/
# p2_inventory.py; rosidl_typesupport_tickle_c/capacities/).
INVENTORY=(
    builtin_interfaces std_msgs
    actionlib_msgs diagnostic_msgs geometry_msgs nav_msgs sensor_msgs shape_msgs std_srvs stereo_msgs
    trajectory_msgs visualization_msgs
    rcl_interfaces action_msgs composition_interfaces lifecycle_msgs rosgraph_msgs service_msgs
    statistics_msgs type_description_interfaces
    unique_identifier_msgs tf2_msgs example_interfaces
)

WORKSPACE="${HOME}/tickle_ros2_interfaces"
ALL=0
while getopts "w:ah" opt; do
    case "$opt" in
    w) WORKSPACE="$OPTARG" ;;
    a) ALL=1 ;;
    h) usage 0 ;;
    *) usage 1 ;;
    esac
done
shift $((OPTIND - 1))
PACKAGES=("$@")
if [ "$ALL" = 1 ]; then
    [ ${#PACKAGES[@]} = 0 ] || usage 1
    PACKAGES=("${INVENTORY[@]}")
fi
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

# Which upstream repository each package lives in; empty for a package this script does not build
# (a dependency such as rosidl_default_runtime, which is not an interface package).
repo_of() {
    case "$1" in
    builtin_interfaces | rcl_interfaces | action_msgs | lifecycle_msgs | composition_interfaces | rosgraph_msgs | \
        statistics_msgs | type_description_interfaces | service_msgs) echo rcl_interfaces ;;
    std_msgs | geometry_msgs | sensor_msgs | nav_msgs | diagnostic_msgs | shape_msgs | trajectory_msgs | \
        visualization_msgs | std_srvs | stereo_msgs | actionlib_msgs) echo common_interfaces ;;
    tf2_msgs) echo geometry2 ;;
    example_interfaces) echo example_interfaces ;;
    unique_identifier_msgs) echo unique_identifier_msgs ;;
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

# rmw_tickle's patches for repository $1, applied to the fresh clone at $2.
apply_patches() {
    local patch
    [ -d "$PATCHES/$1" ] || return 0
    for patch in "$PATCHES/$1"/*.patch; do
        [ -f "$patch" ] || continue
        echo "$1: applying $(basename "$patch")"
        git -C "$2" apply "$patch"
    done
}

# Clones $1's repository at the version ROS 2 has installed of $1, once per repository.
clone_repo_of() {
    local pkg="$1" repo version dest patch
    repo=$(repo_of "$pkg")
    [ -n "$repo" ] || fail "don't know which repository '$pkg' comes from - add it to repo_of()"
    [ -z "${cloned[$repo]:-}" ] || return 0
    version=$(installed_version "$pkg")
    # Not installed itself: the repository's version is still fixed by whichever of its other
    # packages is, and the clone serves those too.
    local other
    for other in "${INVENTORY[@]}"; do
        [ -z "$version" ] && [ "$(repo_of "$other")" = "$repo" ] || continue
        version=$(installed_version "$other")
    done
    dest="$SRC/$repo"
    if [ -z "$version" ]; then
        # Not installed, so nothing installed was built against it and there is no version to
        # match - the distro branch is as right as any tag. (example_interfaces on a ros-base
        # install is the usual case.)
        if [ -d "$dest/.git" ]; then
            echo "$repo: already cloned ($pkg is not installed with ROS 2, using what is there)"
        else
            echo "$repo: $pkg is not installed with ROS 2 $ROS_DISTRO - cloning branch $ROS_DISTRO"
            git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$ROS_DISTRO" "https://github.com/ros2/$repo" "$dest" ||
                fail "$repo has no $ROS_DISTRO branch upstream, and $pkg is not installed to take a version from"
            apply_patches "$repo" "$dest"
        fi
    elif [ -d "$dest/.git" ] && [ "$(git -C "$dest" describe --tags --exact-match 2>/dev/null || true)" = "$version" ]; then
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
        apply_patches "$repo" "$dest"
    fi
    cloned[$repo]=1
}

# The directory holding $1's package.xml in its cloned repository, or nothing if it has none.
package_dir() {
    local dir
    dir="$SRC/$(repo_of "$1")"
    [ -f "$dir/$1/package.xml" ] && dir="$dir/$1"
    [ -f "$dir/package.xml" ] && grep -q "<name>$1</name>" "$dir/package.xml" && echo "$dir"
    return 0
}

# The interface packages rosidl makes every interface package use without its package.xml saying
# so (rosidl_default_generators brings them in): builtin_interfaces and service_msgs always, and for
# an action, action_msgs and unique_identifier_msgs too - its implicit interfaces nest a
# unique_identifier_msgs/UUID goal id and a builtin_interfaces/Time. example_interfaces declares none
# of them, and was built against the installed copies, which have no TickLE typesupport.
IMPLICIT=(builtin_interfaces service_msgs unique_identifier_msgs action_msgs)

# The interface packages the package.xml in directory $1 depends on, one per line - declared, and
# the implicit ones above an action needs.
interface_deps_of() {
    if compgen -G "$1/action/*.action" >/dev/null; then
        printf '%s\n' action_msgs unique_identifier_msgs builtin_interfaces
    fi
    sed -n 's:.*<\(depend\|build_depend\|build_export_depend\|exec_depend\)>\([^<]*\)</.*:\2:p' "$1/package.xml" |
        while read -r dep; do
            [ -z "$(repo_of "$dep")" ] || echo "$dep"
        done
}

# The requested packages plus, transitively, every interface package they depend on.
declare -A wanted=()
declare -A absent=()
queue=("${PACKAGES[@]}")
while [ ${#queue[@]} -gt 0 ]; do
    pkg="${queue[0]}"
    queue=("${queue[@]:1}")
    [ -z "${wanted[$pkg]:-}" ] && [ -z "${absent[$pkg]:-}" ] || continue
    clone_repo_of "$pkg"
    dir=$(package_dir "$pkg")
    if [ -z "$dir" ]; then
        # The distros differ: common_interfaces dropped actionlib_msgs after jazzy. With -a that is
        # a package this distro does not have, so skip it; named explicitly, it is a mistake.
        [ "$ALL" = 1 ] || fail "$(repo_of "$pkg") has no package $pkg at the version cloned for ROS 2 $ROS_DISTRO"
        echo "skipping $pkg: $(repo_of "$pkg") has no such package for ROS 2 $ROS_DISTRO"
        absent[$pkg]=1
        continue
    fi
    wanted[$pkg]=1
    while read -r dep; do
        [ -n "${wanted[$dep]:-}" ] || queue+=("$dep")
    done < <(interface_deps_of "$dir")
done
added=()
for pkg in "${!wanted[@]}"; do
    case " ${PACKAGES[*]} " in *" $pkg "*) ;; *) added+=("$pkg") ;; esac
done
[ ${#added[@]} = 0 ] || echo "also building, as dependencies: ${added[*]}"
mapfile -t PACKAGES < <(printf '%s\n' "${!wanted[@]}" | sort)

override=()
if colcon build --help 2>/dev/null | grep -q -- --allow-overriding; then
    override=(--allow-overriding "${PACKAGES[@]}")
fi
build() {
    colcon build --base-paths "$SRC" --build-base "$WORKSPACE/build" --install-base "$WORKSPACE/install" \
        "${override[@]}" --packages-select "$@" --cmake-args -DBUILD_SHARED_LIBS=ON
}
use_workspace() {
    if [ -f "$WORKSPACE/install/setup.bash" ]; then
        set +u
        # shellcheck disable=SC1091
        . "$WORKSPACE/install/setup.bash"
        set -u
    fi
}
# Two passes, because colcon orders packages and exposes each one's dependencies only by what
# package.xml declares, and the implicit ones above are not declared: the first pass builds them,
# and the second, with this workspace sourced, finds them here rather than in the ROS 2 install.
first=()
rest=()
for pkg in "${PACKAGES[@]}"; do
    case " ${IMPLICIT[*]} " in *" $pkg "*) first+=("$pkg") ;; *) rest+=("$pkg") ;; esac
done
use_workspace
[ ${#first[@]} = 0 ] || build "${first[@]}"
use_workspace
[ ${#rest[@]} = 0 ] || build "${rest[@]}"

# Proof, not assumption: each package now has TickLE's typesupport libraries - the C one rmw_tickle
# reads, and the C++ one rclcpp reaches it through (rosidl_typesupport_tickle_cpp delegates to C).
missing=0
for pkg in "${PACKAGES[@]}"; do
    for lang in c cpp; do
        lib="$WORKSPACE/install/$pkg/lib/lib${pkg}__rosidl_typesupport_tickle_${lang}.so"
        if [ -f "$lib" ]; then
            echo "  ok  $pkg ($lib)"
        else
            echo "  MISSING  $pkg - no $lib" >&2
            missing=1
        fi
    done
done
[ "$missing" = 0 ] || fail "some packages were built without TickLE typesupport"

# The types declined rather than generated (ros2_cli.py writes a stub for each, carrying the reason).
# Listed so a decline is seen here, not first as a creation error in someone's node.
declined=$(grep -rl --include='*__type_support.c' -- '__tickle_unsupported_t' "$WORKSPACE/build" 2>/dev/null |
    sed -e 's:.*/::' -e 's:__type_support\.c$::' -e 's:_\(Request\|Response\)$::' | sort -u || true)
if [ -n "$declined" ]; then
    echo "Declined - no TickLE typesupport, rmw_tickle refuses these at creation (reason in each stub):"
    while read -r type_name; do echo "    $type_name"; done <<<"$declined"
fi
echo
echo "Done. Before running anything that uses these, in every shell:"
echo "  source $WORKSPACE/install/setup.bash"
