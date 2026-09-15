# rmw performance comparison runner setup (`tickle-perf`)

[`rmw-perf.yml`](../workflows/rmw-perf.yml) compares `rmw_tickle` against `rmw_fastrtps_cpp` and
`rmw_cyclonedds_cpp` using [`ros2/buildfarm_perf_tests`](https://github.com/ros2/buildfarm_perf_tests)
(which wraps [`ros2/performance_test`](https://github.com/ros2/performance_test)), on a self-hosted
runner registered with the `tickle-perf` label. This is a **different rig from `tickle-hil`**
([`README.md`](README.md)): one machine, not a pair of Raspberry Pis, and it runs a full ROS 2
stack rather than TickLE's own plain Makefile build. Results here are a same-host, two-process
comparison between `rmw` implementations - useful for relative/regression tracking between the
three, not a real-target-network-medium measurement the way `tickle-hil`'s own numbers are (see
`rmw_tickle/PLAN.md`'s benchmark plan for the full reasoning).

This only needs to be set up once per runner; a normal contributor never runs any of this by hand,
and `rmw-perf.yml` itself never provisions anything - it only rebuilds `rmw_tickle`'s own two
packages against the workspace this doc sets up ahead of time.

## What's needed

- **A self-hosted GitHub Actions runner**, registered with the `tickle-perf` label (that's what
  `rmw-perf.yml`'s `runs-on: [self-hosted, tickle-perf]` matches on) - see GitHub's own
  [Adding self-hosted runners](https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/adding-self-hosted-runners)
  guide. Per `DESIGN.md`'s own "Security: no `pull_request` trigger, ever" rule (written for
  `tickle-hil` but stated as applying to *any* workflow using a self-hosted label on this public
  repo) - `rmw-perf.yml` only triggers on `workflow_dispatch`, never `pull_request`.
- **A machine running Ubuntu**, with whichever ROS 2 distro packages.ros.org actually ships for
  *that exact* Ubuntu release - these are tied together (each ROS 2 distro officially targets one
  specific Ubuntu release; there's no single distro name that's "the" answer across every Ubuntu
  version). **Do not assume Jazzy/Ubuntu 24.04** - the runner this was first provisioned on turned
  out to be Ubuntu 26.04 ("resolute"), which has no Jazzy binaries at all (only up to "noble"/
  24.04) - `ros-lyrical-*` is what's actually published for resolute. Check before installing
  anything:
  ```sh
  . /etc/os-release; echo "$VERSION_CODENAME"
  curl -s "http://packages.ros.org/ros2/ubuntu/dists/$VERSION_CODENAME/main/binary-amd64/Packages" \
    | grep "^Package: ros-.*-ros-base$"
  ```
  Whatever distro name that last command prints is `$ROS_DISTRO` for every command below - this
  doc uses `lyrical` throughout since that's what resolute actually has, but substitute your own
  runner's real answer if it differs. With enough disk for a full ROS 2 install plus two DDS
  vendors plus a `performance_test` build - a few GB free is not enough, budget double digits.

## One-time provisioning (as yourself, needs sudo - not the runner's own service account)

1. Install ROS 2 from the official apt repository - follow
   [the official Ubuntu install guide](https://docs.ros.org/en/rolling/Installation/Ubuntu-Install-Debs.html)
   (the generic/rolling version of the instructions, since the distro-specific guide pages don't
   necessarily exist yet for a very new Ubuntu release either), substituting the real `$ROS_DISTRO`
   from above wherever the guide says e.g. `jazzy`. Install at least `ros-$ROS_DISTRO-ros-base`,
   plus explicitly:
   ```sh
   sudo apt install ros-lyrical-rmw-cyclonedds-cpp python3-colcon-common-extensions python3-rosdep python3-vcstool
   sudo rosdep init   # only if this machine has never run rosdep before
   rosdep update
   ```
   `rmw_fastrtps_cpp` ships as part of `ros-base` already (it's the default `rmw`); `rmw_
   cyclonedds_cpp` needs installing explicitly.

2. Build the `buildfarm_perf_tests`/`performance_test` underlay workspace - kept on disk
   permanently, rebuilt by hand only when you want to pick up upstream changes, never by CI:
   ```sh
   mkdir -p ~/rmw_perf_ws/src
   cd ~/rmw_perf_ws
   source /opt/ros/lyrical/setup.bash
   wget https://raw.githubusercontent.com/ros2/buildfarm_perf_tests/master/tools/ros2_dependencies.repos
   vcs import src < ros2_dependencies.repos
   rosdep install --from-paths src --ignore-src -y
   colcon build --cmake-args -DCMAKE_BUILD_TYPE=Release
   ```
   This is the same recipe as
   [buildfarm_perf_tests' own README](https://github.com/ros2/buildfarm_perf_tests#build) - nothing
   TickLE-specific yet. `rmw-perf.yml` expects this workspace at exactly `~/rmw_perf_ws` (override
   via the workflow's own `RMW_PERF_WS` env var if it needs to live elsewhere). If `git clone`s of
   `ros2_dependencies.repos`' two entries already exist under `~/rmw_perf_ws/src` (e.g. done by
   hand ahead of `vcs`/`rosdep`/`colcon` themselves being installed yet), `vcs import` on top is
   still safe - it no-ops on an already-up-to-date checkout.

3. Confirm both DDS vendors are actually visible before moving on:
   ```sh
   source ~/rmw_perf_ws/install/setup.bash
   ros2 pkg list | grep rmw_
   # expect to see rmw_fastrtps_cpp and rmw_cyclonedds_cpp (at minimum)
   ```

4. **Four local, TickLE-specific patches to the underlay's own sources** - found the hard way
   getting `rmw_tickle` through a real `buildfarm_perf_tests` run for the first time. None of
   these are upstreamed; they live only in this checkout of `~/rmw_perf_ws/src`, applied once here
   during provisioning (not by `rmw-perf.yml` on every run - that workflow only rebuilds
   `rmw_tickle`'s own packages and reconfigures `buildfarm_perf_tests`, per its own comments).

   a. **`performance_test`'s own `Array1k.msg`/`Struct16.msg` need a standard-layout symlink.**
      `performance_test`'s `CMakeLists.txt` uses `rosidl_generate_interfaces()`'s colon-prefixed
      custom-base-directory syntax (`"${CMAKE_CURRENT_SOURCE_DIR}/src:msg/Array1k.msg"`, real file
      at `src/msg/Array1k.msg`) - `rosidl_typesupport_tickle_c_generate_interfaces.cmake`'s own
      path-derivation only understands the standard `<pkg>/msg/<Name>.msg` layout yet (a real,
      not-yet-fixed limitation - not fixed here since a symlink workaround costs nothing and
      touches nothing upstream):
      ```sh
      cd ~/rmw_perf_ws/src/performance_test/performance_test
      mkdir -p msg
      ln -s ../src/msg/Array1k.msg msg/Array1k.msg
      ln -s ../src/msg/Struct16.msg msg/Struct16.msg
      ```
      (`Struct256.msg` deliberately has no symlink here - it embeds a nested `Struct16` field,
      which `rosidl_typesupport_tickle_c` doesn't support yet - see `rmw-perf.yml`'s own
      `PERF_TEST_TOPICS`, which excludes it for exactly this reason.)

   b. **`performance_test`'s own nodes need several rclcpp internals disabled.** A real
      `rclcpp::Node` unconditionally creates a `/rosout` publisher and parameter services/event
      publisher, all at `RELIABLE` QoS - `rmw_tickle` only accepts `BEST_EFFORT` for topics (see
      `rmw_tickle/PLAN.md`'s QoS roadmap). Edit
      `~/rmw_perf_ws/src/performance_test/performance_test/src/communication_abstractions/resource_manager.cpp`,
      changing:
      ```cpp
      auto options = rclcpp::NodeOptions();
      ```
      to:
      ```cpp
      auto options = rclcpp::NodeOptions().start_parameter_services(false).start_parameter_event_publisher(false).enable_rosout(false);
      ```

   c. **`buildfarm_perf_tests`' own launch template needs one more internal service disabled.**
      The `~/get_type_description` service is *also* mandatory and `RELIABLE`, but - unlike (b) -
      there's no `NodeOptions` flag for it, only a ROS **parameter** (`start_type_description_
      service`, see `rclcpp`'s own `node_type_descriptions.cpp`). Edit
      `~/rmw_perf_ws/src/buildfarm_perf_tests/test/test_performance.py.in`'s
      `generate_test_description()` function: add, right after the existing `arguments = [...]`
      assignment,
      ```python
      tickle_ros_args = ([
          '--ros-args', '--param', 'start_type_description_service:=false',
      ] if '@COMM@' == 'ROS2' and '@RMW_IMPLEMENTATION@' == 'rmw_tickle' else [])
      ```
      then append `+ tickle_ros_args` as the **last** element of both `node_pub`'s and
      `node_under_test`'s own `arguments=...` lists (after `-s 0`/`-p 0`/`-l ...` respectively) -
      it must come last, or `perf_test`'s own CLI parser misreads whatever follows `--ros-args` as
      more ROS arguments instead of its own flags (`boost::program_options::invalid_option_value`).
      This file is a template (`configure_file()`/`file(GENERATE)`-processed at CMake configure
      time, one instance per rmw/topic/sync-mode combination) - the `@COMM@`/`@RMW_IMPLEMENTATION@`
      placeholders are substituted per-instance, so this guard only adds the extra args for
      `rmw_tickle` combinations, leaving FastDDS/CycloneDDS runs untouched.

   d. **The two-process case needs each side given a distinct TickLE node id.** A tickle node's
      own id defaults to the last octet of its host's own address (see `include/tickle/config.h`'s
      own doc comment on `_tt_CONFIG.node_id`) - correct for two real separate hosts, but the
      "two-process" test topology runs *both* sides on this one runner, so without an override
      both sides derive the identical id and each silently discards the other's every packet as
      "self sent" (`process_datagram()`, `tickle.c`) - not a hang or a crash, `perf_test` reports a
      completed run with `received_messages: 0` for the whole duration. `TICKLE_NODE_ID` (an
      environment variable read once in `rmw_init()`, mirroring `TICKLE_BROADCAST_ADDR` just above
      it) is the override. In the same `generate_test_description()` function edited in (c), add
      right after `tickle_ros_args`:
      ```python
      tickle_env_pub = (
          {'TICKLE_NODE_ID': '101'}
          if '@COMM@' == 'ROS2' and '@RMW_IMPLEMENTATION@' == 'rmw_tickle' else {})
      tickle_env_sub = (
          {'TICKLE_NODE_ID': '102'}
          if '@COMM@' == 'ROS2' and '@RMW_IMPLEMENTATION@' == 'rmw_tickle' else {})
      ```
      then add `additional_env=tickle_env_pub` to `node_pub`'s own `Node(...)` call and
      `additional_env=tickle_env_sub` to `node_under_test`'s - the specific numbers don't matter,
      only that the two nodes get different ones. (The single-process case needs nothing here -
      one process, one node, no id to collide with.)

   Re-running `colcon build` for `performance_test`/`buildfarm_perf_tests` (step 2's own command)
   after any of these picks the patches up - `rmw-perf.yml`'s own "Rebuild buildfarm_perf_tests
   with rmw_tickle now visible" step forces a reconfigure every run regardless (`--cmake-force-
   configure`), so (c) in particular always takes effect even without a manual rebuild first.

`rmw-perf.yml` itself (run on every manual dispatch) builds `rmw_tickle`/`rosidl_typesupport_
tickle_c`/`rosidl_typesupport_tickle_cpp` fresh on top of this underlay, then *does* reconfigure and rebuild `buildfarm_perf_tests`
itself every run too - that package's own `get_available_rmw_implementations()` call runs at its
CMake configure time, so it has to be re-run once `rmw_tickle` is newly visible in `AMENT_PREFIX_
PATH`, with this run's own `PERF_TEST_TOPICS`/`PERF_TEST_RMW_IMPLEMENTATIONS` cache overrides. It
never touches ROS 2, the DDS vendors, or `performance_test` itself, or `buildfarm_perf_tests`' own
other dependencies (`test_msgs`, `rmw_dds_common`, `osrf_testing_tools_cpp`, ...) - only step 2's
`buildfarm_perf_tests` package specifically gets reconfigured, and that's a thin CMake layer, not a
real rebuild cost. Re-run steps 1-3 by hand whenever you want to pick up a new ROS 2 patch release
or a `buildfarm_perf_tests`/`performance_test` upstream change.

## Re-provisioning after an OS/ROS upgrade

Steps 1-3 above are idempotent - re-running them after e.g. an Ubuntu point release is the whole
procedure. A fresh `ros2_dependencies.repos` upstream update (step 2) will overwrite step 4's own
local patches to `performance_test`/`buildfarm_perf_tests` sources (they were never upstreamed) -
re-apply step 4 after any `vcs import`/re-clone of `~/rmw_perf_ws/src`. If the runner ever moves to
a different machine, register the new one with the same `tickle-perf` label (no workflow change
needed) and repeat provisioning there.

## Troubleshooting

- **`colcon build --packages-select rmw_tickle` can't find ROS 2 packages at all**: confirm
  `~/rmw_perf_ws/install/setup.bash` was actually sourced before `rmw-perf.yml`'s own build step -
  see that workflow file's own comment on why it sources both the base ROS 2 `setup.bash` and this
  underlay's `setup.bash`, in that order.
- **`apt install ros-$ROS_DISTRO-...` says the package doesn't exist**: your Ubuntu release almost
  certainly isn't the one you assumed - re-run this doc's own "What's needed" section's `curl`
  check to find the actual `$ROS_DISTRO` name for this machine's real codename, don't guess from
  another machine's setup.
- **A DDS vendor is "available" per `get_available_rmw_implementations()` but every run against it
  fails**: re-run step 3 above to confirm it's genuinely installed and importable, not just present
  as a stale `ament_index` marker from a partially-removed package.
