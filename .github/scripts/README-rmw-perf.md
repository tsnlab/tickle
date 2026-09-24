# rmw_tickle performance runner setup (`tickle-perf`)

[`rmw-perf.yml`](../workflows/rmw-perf.yml) tracks `rmw_tickle`'s own before/after performance
using [`ros2/buildfarm_perf_tests`](https://github.com/ros2/buildfarm_perf_tests) (which wraps
[`ros2/performance_test`](https://github.com/ros2/performance_test)), on a self-hosted runner
registered with the `tickle-perf` label. This is a **different rig from `tickle-hil`**
([`README.md`](README.md)): one machine, not a pair of Raspberry Pis, and it runs a full ROS 2
stack rather than TickLE's own plain Makefile build.

**Not a cross-vendor comparison** - `PERF_TEST_RMW_IMPLEMENTATIONS` (`rmw-perf.yml`) is
`rmw_tickle` only, deliberately. `buildfarm_perf_tests`' own "two-process" test shape launches
both sides as local child processes via `launch_ros.actions.Node`, which has no remote-host
launch capability at all - "two-process" always means one host, two OS processes, never two real
hosts. That structurally favors any rmw with a same-host shared-memory transport (FastDDS and
CycloneDDS both auto-negotiate one) over `rmw_tickle`, which only ever has real UDP sockets
(`hal_linux.c`) - a byproduct of what TickLE actually targets (a real network medium, e.g.
10Base-T1S), not a fair fight either way. So this rig exists to track `rmw_tickle`'s own
regressions over time (still genuinely useful - it already caught two real wire-level bugs, see
`rmw_tickle/PLAN.md`'s Milestone 12), not to produce an absolute "faster/slower than FastDDS/
CycloneDDS" number - and it's *not* a real-target-network-medium measurement the way `tickle-hil`'s
own numbers are either (see `rmw_tickle/PLAN.md`'s benchmark plan for the full reasoning). Runs on
every push to `main` (plus `workflow_dispatch` for an on-demand run) - each run's own Actions
summary page shows a Markdown table (`.github/scripts/rmw_perf_summary.py`, parsing
`buildfarm_perf_tests`' own per-test `.benchmark.json` output), and the full raw CSV/PNG/JSON
results are uploaded as a build artifact regardless.

This only needs to be set up once per runner; a normal contributor never runs any of this by hand,
and `rmw-perf.yml` itself never provisions anything - it only rebuilds `rmw_tickle`'s own two
packages against the workspace this doc sets up ahead of time.

## What's needed

- **A self-hosted GitHub Actions runner**, registered with the `tickle-perf` label (that's what
  `rmw-perf.yml`'s `runs-on: [self-hosted, tickle-perf]` matches on) - see GitHub's own
  [Adding self-hosted runners](https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/adding-self-hosted-runners)
  guide. Per `DESIGN.md`'s own "Security: no `pull_request` trigger, ever" rule (written for
  `tickle-hil` but stated as applying to *any* workflow using a self-hosted label on this public
  repo) - `rmw-perf.yml` triggers on `push` (to `main` only) and `workflow_dispatch`, never
  `pull_request`.
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
   sudo apt install python3-colcon-common-extensions python3-rosdep python3-vcstool
   sudo rosdep init   # only if this machine has never run rosdep before
   rosdep update
   ```
   `rmw_fastrtps_cpp` ships as part of `ros-base` already (it's the default `rmw`). `ros-$ROS_
   DISTRO-rmw-cyclonedds-cpp` is *not* needed by `rmw-perf.yml` itself (`PERF_TEST_RMW_
   IMPLEMENTATIONS` there is `rmw_tickle` only - see this doc's own intro on why a cross-vendor
   comparison on this same-host rig wouldn't be a fair one) - only install it if you also want to
   run `buildfarm_perf_tests` by hand for some other, DDS-vendor-inclusive purpose.

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

3. Confirm `rmw_fastrtps_cpp` is visible before moving on (it's `buildfarm_perf_tests`' own
   underlay build dependency regardless of which rmws `rmw-perf.yml` actually tests):
   ```sh
   source ~/rmw_perf_ws/install/setup.bash
   ros2 pkg list | grep rmw_
   # expect to see rmw_fastrtps_cpp at minimum
   ```

4. **Six local, TickLE-specific patches to the underlay's own sources** - found the hard way
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

   e. **The test must assert that messages were actually delivered.** `launch_test`'s own gate is
      `assert_wait_for_successful_exit()`, which checks exit codes and nothing else, so a run that
      exchanges *zero* messages for its whole duration passes - observed for real on 2026-09-23,
      an `Array1k`/async cell reporting `received 0 / lost 7902`, throughput 0.0008 Mbit/s, and
      `launch_test` calling it Passed in 16.01 seconds. Nobody would have noticed it from the test
      result; it was found by reading the summary table. This is the same class of hole as (d)'s
      own silent-zero failure mode, and it is what let (d) go unnoticed for as long as it did. In
      `test_performance.py.in`'s own `test_results_@TEST_NAME@`, replace the
      `results_base_path`-guarded block with one that reads the log unconditionally and asserts on
      it first:
      ```python
      performance_logs = glob(performance_log_prefix + '*')
      assert len(performance_logs) == 1, f'expected one performance log, got {performance_logs}'
      performance_data = read_performance_test_csv(performance_logs[0])

      # Exit codes alone pass a run that delivered nothing - see README-rmw-perf.md patch (e).
      total_received = performance_data['received'].sum()
      assert total_received > 0, (
          f'run exited cleanly but delivered no messages at all '
          f'({total_received} received over @PERF_TEST_RUNTIME@s)')

      results_base_path = os.environ.get('PERF_TEST_RESULTS_BASE_PATH')
      if results_base_path:
          _raw_to_jenkins(performance_data, results_base_path + '.csv')
          _raw_to_png(performance_data, results_base_path + '.png')
      else:
          print(
              'No results reports written - set PERF_TEST_RESULTS_BASE_PATH to write reports',
              file=sys.stderr)
      ```
      The threshold is deliberately `> 0` rather than a fraction of what was sent. A zero is
      unambiguous and is the failure actually observed; a fractional bar would need a defensible
      number for every payload and rate combination, and would trade one silent failure for a
      flaky gate. Tightening it later is easy once there is a characterised baseline to tighten
      against.

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

### Patch (f): neither "Data consistency violated" assertion may abort the run (2026-09-24)

Saved as a real patch file, `rmw_perf_patches/f-out-of-order-is-not-an-abort.patch`, rather than as
prose like (a)-(e) - apply with `patch -p1` from `~/rmw_perf_ws/src/performance_test/performance_test`.

`perf_test` threw `std::runtime_error` on two conditions, and `std::terminate()` then ran before any
teardown - discarding the per-Subscriber delivery counters built to diagnose exactly these. The
report of the problem destroyed the evidence for it. Both are now counters that print a
`TICKLE-PATCH-F ...` line and let the process finish.

**The two variants are deliberately not treated alike**, and that distinction is the whole patch:

- The **id** variant (`communicator.cpp`) is the benchmark asserting ordering that its own
  BEST_EFFORT configuration never promises - `experiment_configuration.cpp` selects RELIABLE only
  when `--reliable` is passed, and the `two_process_rmw_` matrix passes neither that nor `--rate`.
  Measured over 554 archived ctest logs on this box: `rmw_fastrtps_cpp` 8/284 cells (**2.82%**),
  `rmw_cyclonedds_cpp` 6/284 (**2.11%**), `rmw_tickle` 23/1751 (**1.31%**). Both reference
  implementations trip it more often than we do. **Reported, never gated.**
- The **timestamp** variant (`ros2_communicator.hpp`) has never been seen under either DDS vendor,
  and fired in 2 of 12 cells on the two-socket build against 5 of 1751 historically. It is treated
  as ours until shown otherwise, and `rmw-perf.yml`'s "Fail on a timestamp-ordering violation" step
  **fails the job** on it. Removing the throw is about keeping the evidence, not forgiving the
  condition.

### Patch (g) is not a patch: `performance_test` cannot currently be rebuilt (2026-09-24, open)

`rosidl_typesupport_tickle_c` emits `_Static_assert(<size> <= tt_MAX_BUFFER_LENGTH, ...)` for every
message type, and `performance_test` declares `Array4k`, `Array32k`, `PointCloud1m` and others that
exceed one datagram. The generator processes every type in `rosidl_generate_interfaces()` - patch
(a)'s symlinks govern path derivation, not type selection - so **the build fails on types nobody
uses**. This workspace was provisioned on 2026-09-15 with an older generator and nothing rebuilt it
for nine days, which is why the regression was invisible until something did.

**Consequence**: `~/rmw_perf_ws/install/performance_test` is empty and the `rmw-perf.yml` job cannot
run until this is fixed. Narrowing the interface list is **not** a workaround - `perf_test`'s own
sources reference the wider type set. The fix belongs in the generator: decline to generate for a
type that cannot fit one datagram, and let `rmw_create_publisher` report it at runtime, which it
already does clearly ("no rmw_tickle typesupport for this message type").

**If you rebuild `performance_test` for any reason**, source `<tickle>/install/setup.bash` *before*
building, so the TickLE typesupport generator runs. Building without it regenerates
`rosidl_typesupport_c` and leaves the TickLE typesupport at its previous date; every rmw_tickle cell
then dies with "no rmw_tickle typesupport for this message type", which looks like a product bug and
is not.

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
- **`rmw_tickle` isn't in `buildfarm_perf_tests`' generated test list at all**: `PERF_TEST_RMW_
  IMPLEMENTATIONS` (a CMake `CACHE` variable) is what actually decides which rmws get test targets,
  overriding `get_available_rmw_implementations()`'s own auto-detected default - confirm `rmw-perf.
  yml`'s own env still has `rmw_tickle` in it, and that "Rebuild buildfarm_perf_tests with rmw_
  tickle now visible" actually ran with `--cmake-force-configure` (a stale cache entry from before
  `rmw_tickle` was installed would otherwise stick).
