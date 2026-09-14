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
- **A machine running Ubuntu** (24.04, matching ROS 2 Jazzy's own officially supported platform -
  Jazzy has no official Raspberry Pi OS build, which is why this rig exists separately from
  `tickle-hil`'s own Raspberry Pi pair in the first place) with enough disk for a full ROS 2
  install plus two DDS vendors plus a `performance_test` build - a few GB free is not enough,
  budget double digits.

## One-time provisioning (as yourself, needs sudo - not the runner's own service account)

1. Install ROS 2 Jazzy from the official apt repository - follow
   [the official Ubuntu install guide](https://docs.ros.org/en/jazzy/Installation/Ubuntu-Install-Debs.html).
   Install at least `ros-jazzy-ros-base`, plus explicitly:
   ```sh
   sudo apt install ros-jazzy-rmw-cyclonedds-cpp python3-colcon-common-extensions python3-rosdep python3-vcstool
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
   source /opt/ros/jazzy/setup.bash
   wget https://raw.githubusercontent.com/ros2/buildfarm_perf_tests/master/tools/ros2_dependencies.repos
   vcs import src < ros2_dependencies.repos
   rosdep install --from-paths src --ignore-src -y
   colcon build --cmake-args -DCMAKE_BUILD_TYPE=Release
   ```
   This is the same recipe as
   [buildfarm_perf_tests' own README](https://github.com/ros2/buildfarm_perf_tests#build) - nothing
   TickLE-specific yet. `rmw-perf.yml` expects this workspace at exactly `~/rmw_perf_ws` (override
   via the workflow's own `RMW_PERF_WS` env var if it needs to live elsewhere).

3. Confirm both DDS vendors are actually visible before moving on:
   ```sh
   source ~/rmw_perf_ws/install/setup.bash
   ros2 pkg list | grep rmw_
   # expect to see rmw_fastrtps_cpp and rmw_cyclonedds_cpp (at minimum)
   ```

`rmw-perf.yml` itself (run on every manual dispatch) builds `rmw_tickle`/`rosidl_typesupport_
tickle_c` fresh on top of this underlay, then *does* reconfigure and rebuild `buildfarm_perf_tests`
itself every run too - that package's own `get_available_rmw_implementations()` call runs at its
CMake configure time, so it has to be re-run once `rmw_tickle` is newly visible in `AMENT_PREFIX_
PATH`, with this run's own `PERF_TEST_TOPICS`/`PERF_TEST_RMW_IMPLEMENTATIONS` cache overrides. It
never touches ROS 2, the DDS vendors, or `performance_test` itself, or `buildfarm_perf_tests`' own
other dependencies (`test_msgs`, `rmw_dds_common`, `osrf_testing_tools_cpp`, ...) - only step 2's
`buildfarm_perf_tests` package specifically gets reconfigured, and that's a thin CMake layer, not a
real rebuild cost. Re-run steps 1-3 by hand whenever you want to pick up a new ROS 2 patch release
or a `buildfarm_perf_tests`/`performance_test` upstream change.

## Re-provisioning after an OS/ROS upgrade

Steps 1-3 above are idempotent - re-running them after e.g. an Ubuntu point release or a fresh
`ros2_dependencies.repos` upstream update is the whole procedure. If the runner ever moves to a
different machine, register the new one with the same `tickle-perf` label (no workflow change
needed) and repeat provisioning there.

## Troubleshooting

- **`colcon build --packages-select rmw_tickle` can't find ROS 2 packages at all**: confirm
  `~/rmw_perf_ws/install/setup.bash` was actually sourced before `rmw-perf.yml`'s own build step -
  see that workflow file's own comment on why it sources both `/opt/ros/jazzy/setup.bash` and this
  underlay's `setup.bash`, in that order.
- **A DDS vendor is "available" per `get_available_rmw_implementations()` but every run against it
  fails**: re-run step 3 above to confirm it's genuinely installed and importable, not just present
  as a stale `ament_index` marker from a partially-removed package.
