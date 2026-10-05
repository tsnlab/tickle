This document is a declaration of software quality for the `rmw_tickle` package, based on the
guidelines in [REP-2004](https://www.ros.org/reps/rep-2004.html).

# `rmw_tickle` Quality Declaration

The package `rmw_tickle` claims to be in the **Quality Level 4** category.

Below are the rationales, notes, and caveats for this claim, organized by each requirement listed
in the [Package Requirements for Quality Level 4 in REP-2004](https://www.ros.org/reps/rep-2004.html).

## Version Policy [1]

### Version Scheme [1.i]

`rmw_tickle` uses `semver` according to the recommendation for ROS Core packages in the
[ROS 2 Developer Guide](https://docs.ros.org/en/rolling/Contributing/Developer-Guide.html#versioning).

### Version Stability [1.ii]

`rmw_tickle` is at version `0.0.1` (`package.xml`), not yet a stable release (`>= 1.0.0`). This is
the primary reason this package does not currently claim a higher quality level.

### Public API Declaration [1.iii]

Public API for `rmw_tickle` is declared through `rmw` - like `rmw_cyclonedds_cpp`, `rmw_tickle`
adds no public headers of its own beyond what `rmw` itself already declares.

### API Stability Within a Released ROS Distribution [1.iv]

Given `rmw_tickle` is not yet at a stable version, changes to its API are not currently
guaranteed to be stable within a ROS distribution.

### ABI Stability Within a Released ROS Distribution [1.v]

`rmw_tickle` contains C and C++ code and therefore must be concerned with ABI stability, but at
`0.0.1` no ABI stability guarantee is currently made.

## Change Control Process [2]

`rmw_tickle` follows the change control process used across the [tsnlab/tickle](https://github.com/tsnlab/tickle)
repository it lives in:

### Change Requests [2.i]

Not met today. Changes are pushed to `main` directly after the repository's local gates (`make check-gates`) pass,
and CI (`check-all.yml`, `test-all.yml`) runs on every push. Moving every change to a pull request is the first step
to Quality Level 3. (Corrected 2026-10-05: this section used to say that all changes go through reviewed pull
requests, which was not true.)

### Contributor Origin [2.ii]

Not met today. [CONTRIBUTING](../../docs/CONTRIBUTING.md) names DCO as the policy, but commits do not carry a
`Signed-off-by` line and nothing checks for one. (Corrected 2026-10-05.)

### Peer Review Policy [2.iii]

Not met today: there is no pull-request review, since changes do not go through pull requests (2.i).
(Corrected 2026-10-05.)

### Continuous Integration [2.iv]

Every push must pass CI (`check-all.yml`): build, lint (clang-format/clang-tidy), the
package's own test suite, and (as of this document) the upstream `test_rmw_implementation`
conformance suite - see [Testing](#testing-4) below. `rmw_tickle` is currently tested on Linux
only (see [Platform Support](#platform-support-6)), not all REP-2000 Tier 1 platforms.

### Documentation Quality [2.v]

Quality declaration documents are linked in the [README](../../README.md) of this repository.

## Documentation [3]

### Feature Documentation [3.i]

`rmw_tickle`'s own feature set and design decisions are documented in this repository's
[docs/DESIGN.md](../../docs/DESIGN.md) and [docs/RMW.md](../../docs/RMW.md) (the latter tracking, per
milestone, what's implemented and what's explicitly deferred - e.g. the QoS roadmap, currently
supported message-type subset, and known limitations like single-node-per-process).

### Public API Documentation [3.ii]

`rmw_tickle`'s public API is declared and documented by `rmw` itself.

### Population of Changelog [3.iii]

`rmw_tickle`'s changes are recorded alongside the rest of this repository's in a single, shared
[CHANGELOG.md](../../docs/CHANGELOG.md), not (yet) a REP-132-format `CHANGELOG.rst` of its own.

### Documentation of Quality Level [3.iv]

This document itself declares the quality level and is linked from the package's own README.

## Testing [4]

### Feature Testing [4.i]

All of `rmw_tickle`'s public features are ROS middleware features.

Unit, integration, and system tests higher up in the stack - in particular
[`test_rmw_implementation`](https://github.com/ros2/rmw_implementation/tree/rolling/test_rmw_implementation)
(wired into `check-all.yml` as of the former `rmw_tickle/PLAN.md`'s Milestone 15, readable with
`git show 3c0c505b:rmw_tickle/PLAN.md`) - provide feature coverage,
the same package `rmw_fastrtps_cpp`'s and `rmw_cyclonedds_cpp`'s own quality declarations cite for
this exact purpose. Coverage is partial: a documented fraction of that upstream suite is skipped for `rmw_tickle` (what is
supported and what is not is in [docs/RMW.md](../../docs/RMW.md), "What works, and what does not").
(Corrected 2026-10-05: this used to say that only BEST_EFFORT was accepted for topics; all six QoS policies are.)

`rmw_tickle`'s own scoped test suite (`test/test_qos.c`, `test/test_node_lifecycle.c`,
`test/test_guard_condition_wait.c`, `rmw_tickle/PLAN.md`'s Milestone 10) additionally covers
QoS-rejection logic and end-to-end `rmw_init()`/`rmw_create_node()`/.../`rmw_context_fini()`
lifecycle behavior directly.

### Public API Testing [4.ii]

`rmw_tickle` implements the ROS middleware public API. The same tests named above (`4.i`) provide
this coverage; new additions or changes to this package's own behavior require tests before being
added.

### Coverage [4.iii]

Coverage is tracked, not enforced: no threshold fails a build.

- **TickLE core** (the library `rmw_tickle` wraps): line and branch coverage of `src/` (HALs excluded) by the unit
  tests. `make coverage` builds `make test` with `--coverage` into its own `platform/linux/obj/coverage/` tree and
  reads it with gcovr; the reports (HTML per line, Cobertura XML, JSON summary) land in
  `platform/linux/obj/coverage/report/`. CI runs it on every push and PR in `test-all.yml` ("Coverage (tracked, not
  enforced)"), writes the per-file table into the job summary and uploads the reports as the `coverage-report`
  artifact. On 2026-10-05 it read about 90.6% of lines and 72.6% of branches (tickle.c, encoding.c, log.c); the
  multi-threaded tests make the count vary by a few lines between runs.
- **`rmw_tickle` itself**: not tracked yet. CI builds it with colcon and runs its tests in `check-all.yml`; adding
  `--coverage` there means a second colcon build in an already long job, so it is left as follow-up work. The
  recipe, not yet run: `colcon build --packages-select rmw_tickle --cmake-args -DCMAKE_C_FLAGS=--coverage -DCMAKE_CXX_FLAGS=--coverage`,
  `colcon test --packages-select rmw_tickle` (in a private network namespace), then
  `gcovr --root rmw_tickle/rmw_tickle build/rmw_tickle`.

This is still short of `rmw_fastrtps_cpp`'s/`rmw_cyclonedds_cpp`'s own branch coverage tracking, which covers the
rmw layer as well as the library under it.

### Performance [4.iv]

`rmw_tickle` has performance-tracking CI (`.github/workflows/rmw-perf.yml`, `rmw_tickle`'s own before/after
latency and throughput), run by hand (`workflow_dispatch`) rather than on every push, because it needs the
dedicated rig - something neither `rmw_fastrtps_cpp` nor `rmw_cyclonedds_cpp` currently has ("does not currently
have performance tests," their own quality declarations' own words). It is a regression tracker for `rmw_tickle`
itself, not a cross-vendor comparison. Cross-vendor figures, taken on TickLE core's two-Raspberry-Pi rig
(`.github/workflows/performance.yml`, also dispatch-only), are in `docs/RESULTS.md`.

### Linters and Static Analysis [4.v]

`rmw_tickle` uses and passes clang-format and clang-tidy, run repository-wide in `check-all.yml`
(`cpp-linter-action`) on every push - the same linting infrastructure the rest of this
repository already uses, not a separate ROS-specific linter set (`ament_lint_common` et al.).

## Dependencies [5]

### Direct Runtime ROS Dependencies [5.i]/[5.ii]

`rmw_tickle` has the following runtime ROS dependencies, per its own `package.xml`:

* `rcutils`
* `rmw`
* `rosidl_runtime_c`
* `rosidl_typesupport_tickle_c` (part of this same repository - no separate quality declaration
  yet)
* `rosidl_typesupport_tickle_cpp` (part of this same repository - no separate quality declaration
  yet)

None of these has a Quality Level claim of its own yet either; this is a known, honest gap rather
than an omission.

### Direct Runtime Non-ROS Dependencies [5.iii]

`rmw_tickle`'s only non-ROS runtime dependency is TickLE itself (this repository's own core
library) - not a separately versioned/released package, and without a quality declaration of its
own yet.

## Platform Support [6]

`rmw_tickle` currently supports Linux only (`check-all.yml`, `test-all` target) - not all
REP-2000 Tier 1 platforms. TickLE core (the underlying library `rmw_tickle` wraps) additionally
targets FreeRTOS, but that target is not something `rmw_tickle`/ROS 2 itself runs on.

## Security [7]

### Vulnerability Disclosure Policy [7.i]

This repository does not yet have a dedicated security vulnerability disclosure policy.

# Current Status Summary

The chart below compares the requirements in REP-2004 with the current state of the `rmw_tickle`
package.

| Number | Requirement | Status |
|--------|-------------|--------|
| 1 | **Version policy** | --- |
| 1.i | Uses semantic versioning | ✓ |
| 1.ii | Stable version | ☓ |
| 1.iii | Declared public API | ✓ |
| 1.iv | API stability policy | ☓ |
| 1.v | ABI stability policy | ☓ |
| 2 | **Change control process** | --- |
| 2.i | All changes occur on change request | ☓ |
| 2.ii | Contributor origin (DCO, CLA, etc) | ☓ |
| 2.iii | Peer review policy | ☓ |
| 2.iv | CI policy | ✓ |
| 2.v | Documentation policy | ✓ |
| 3 | **Documentation** | --- |
| 3.i | Per feature documentation | ✓ |
| 3.ii | Per public API item documentation | * |
| 3.iii | Declared usage of any features from other quality levels | N/A |
| 3.iv | Population of changelog | * |
| 3.v | Rationale, quality level, and workarounds documented | ✓ |
| 4 | **Testing** | --- |
| 4.i | Feature items tests | * |
| 4.ii | Public API tests | * |
| 4.iii | Coverage | * |
| 4.iv | Performance tests (if applicable) | ✓ |
| 4.v | Linters and static analysis | ✓ |
| 5 | **Dependencies** | --- |
| 5.i | Must not have ROS dependencies with lower quality level | * |
| 5.ii | Justifies quality use of non-direct ROS dependencies | N/A |
| 5.iii | Justifies quality use of non-ROS dependencies | * |
| 6 | **Platform support** | * |
| 7 | **Security** | --- |
| 7.i | Vulnerability disclosure policy | ☓ |
