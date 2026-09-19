# rmw_tickle vs rmw_fastrtps_cpp / rmw_cyclonedds_cpp

A living comparison of `rmw_tickle` against the two mature, production DDS-backed `rmw`
implementations ROS 2 ships by default - functional (real `test_rmw_implementation` conformance
results) and, below, performance. Maintained by "TickLE Plan" directly (observation/measurement,
not development work) - see `rmw_tickle/PLAN.md`'s own Milestone 44 for the short pointer entry.

## Functional comparison (2026-09-19)

**Methodology note - not perfectly apples-to-apples, read the caveat before the numbers.**
`rmw_tickle`'s own number below is its real CI's most recent confirmed-green run
(`jazzy` branch of `ros2/rmw_implementation`/`ros2/rcl_interfaces`, with this repo's own
`.github/scripts/rmw-conformance-patches/*.patch` applied - the same patched build
`check-all.yml` already maintains). FastDDS/CycloneDDS's own numbers below are a **pristine,
unpatched** `lyrical`-branch build run locally against this same host's own installed ROS 2
distro (`/opt/ros/lyrical`) - the `jazzy`-branch conformance patch doesn't apply cleanly to
`lyrical`'s own diverged upstream source, and this local host has no `jazzy` install to test
against instead. So: different upstream branch, and `rmw_tickle`'s own run is missing whatever
tests the CMake-level exclusion list has trimmed for it (independent of any per-test
`GTEST_SKIP()`) while FastDDS/CycloneDDS saw the *full* unmodified suite. Good enough to compare
*what kind* of things each implementation is missing; not a precise head-to-head score.

### Executable-level (ctest)

| | Executables | Result | Branch / patch state |
|---|---|---|---|
| `rmw_fastrtps_cpp` | 16/16 | 100% | pristine `lyrical`, local run |
| `rmw_cyclonedds_cpp` | 16/16 | 100% | pristine `lyrical`, local run |
| `rmw_tickle` | 14/14 | 100% | `jazzy` + this repo's own conformance patch, real CI |

### Individual `TEST_F()` case-level

| | OK | SKIPPED | Unique skip reasons |
|---|---|---|---|
| `rmw_fastrtps_cpp` | 135 | 9 | 3 |
| `rmw_cyclonedds_cpp` | 131 | 19 | 7 |
| `rmw_tickle` | 61 | 77 | 32 |

### What FastDDS skips (3, all loan/allocator)

```
TestPublisherAllocator.init_fini_publisher_allocation
TestSerializeDeserialize.rmw_get_serialized_message_size
TestSubscriptionAllocator.init_fini_subscription_allocation
```

### What CycloneDDS skips (7, loan/allocator - a bit wider than FastDDS)

```
TestPublisherAllocator.init_fini_publisher_allocation
TestPublisherUseLoan.borrow_loaned_message_with_bad_arguments
TestSerializeDeserialize.rmw_get_serialized_message_size
TestSubscriptionAllocator.init_fini_subscription_allocation
TestSubscriptionUseLoan.rmw_return_loaned_message_from_subscription
TestSubscriptionUseLoan.rmw_take_loaned_message
TestSubscriptionUseLoan.rmw_take_loaned_message_with_info
```

### What `rmw_tickle` skips (32, much wider)

**Publisher creation/management (9)**
```
TestPublisher.create_and_destroy
TestPublisher.create_and_destroy_native
TestPublisher.create_with_bad_arguments
TestPublisher.create_with_internal_errors
TestPublisher.destroy_with_bad_arguments
TestPublisher.destroy_with_internal_errors
TestPublisher.get_actual_qos_from_system_defaults
TestPublisherUse.publish_message_with_bad_arguments
TestPublisherUse.publish_with_internal_errors
TestPublisherUse.publish_serialized_message_with_bad_arguments
TestPublisherUse.publish_serialized_with_internal_errors
```

**Matched-count / event tracking (8)**
```
TestPublisherUse.count_matched_subscriptions
TestPublisherUse.count_matched_subscriptions_with_bad_args
TestPublisherUse.count_mismatched_subscriptions
TestPublisherUse.get_actual_qos
TestPublisherUse.get_actual_qos_with_bad_arguments
TestPublisherUse.wait_for_all_acked_with_best_effort
TestEvent.basic_publisher_matched_event
TestEvent.basic_subscription_matched_event
TestEvent.one_pub_multi_sub_connect_disconnect
TestEvent.one_sub_multi_pub_matched_unmatched_event
```

**QoS check (2)**
```
TestClient.check_qos
TestService.check_qos
```

**Unique identifier / GID (6)**
```
TestUniqueIdentifierAPI.compare_gids
TestUniqueIdentifierAPI.compare_gids_with_bad_args
TestUniqueIdentifierAPI.get_client_gid_with_bad_args
TestUniqueIdentifierAPI.get_pub_gid_with_bad_args
TestUniqueIdentifiersForMultipleClients.different_clis
TestUniqueIdentifiersForMultiplePublishers.different_pubs
```

**Other (4)**
```
TestDurationInfinite.create_publisher
TestWaitSetUse.rmw_wait
TestPublisherAllocator.init_fini_publisher_allocation   (shared with both DDS vendors)
TestPublisherUseLoan.borrow_loaned_message_with_bad_arguments   (shared with both DDS vendors)
```

### Reading

- Loan/allocator skips are common to all three implementations - not a `rmw_tickle`-specific gap,
  this whole API family is inherently optional/niche.
- `rmw_tickle`'s own real gaps cluster around **matched-count/event tracking** and **detailed
  publisher/subscriber creation error handling** - overlaps with already-tracked backlog items
  (`rmw_publisher_wait_for_all_acked()`'s real implementation, the `RMW_EVENT_*_MATCHED` event
  family).
- `TestUniqueIdentifierAPI`'s GID-comparison skips are not yet root-caused against anything already
  tracked - worth a closer look.
- `TestClient`/`TestService.check_qos` plausibly the same "needs deadline+lifespan+liveliness all
  at once" gap the conformance patch documented historically - not re-verified this pass whether
  it's still the real cause now that DEADLINE/LIFESPAN/LIVELINESS have all since landed.

### Follow-up needed for a true apples-to-apples pass

Rebuild `test_msgs`/`test_rmw_implementation` fresh against the **same** upstream branch for all
three (either get a `jazzy` ROS 2 install to test FastDDS/CycloneDDS against, or find/adapt the
conformance patch for `lyrical` so `rmw_tickle` can run there too) once useful - not blocking,
this pass already tells us what *kind* of gaps remain.

## Performance comparison

Not yet run. See `rmw_tickle/PLAN.md`'s own Milestone 14 for the hard-won lesson this has to
respect: `buildfarm_perf_tests`' own same-host two-process topology gives FastDDS/CycloneDDS an
unfair advantage via same-host shared-memory transport, invisible to `rmw_tickle` (no such
transport exists for it).

**Deliberately not a GitHub Actions workflow** - this comparison is intermittent enough that a
standalone script, run by hand and copied into this file afterward, is a better fit than a
standing CI job (the user's own call, 2026-09-19, after an initial `rmw-perf.yml` integration
attempt was started and then backed out - see this file's own git history if the abandoned
`workflow_dispatch`-based approach is ever worth reviving).

**How to run it**: `.github/scripts/compare_rmw_perf.sh [runtime_seconds]`, executed directly on a
box with `buildfarm_perf_tests` already provisioned per `.github/scripts/README-rmw-perf.md`
(today, that's the `tickle-perf` self-hosted runner's own `~/rmw_perf_ws` - SSH there and run it
from a checkout of this repo). It:

1. Builds `rmw_tickle` fresh and rebuilds `buildfarm_perf_tests` with all three
   `RMW_IMPLEMENTATION`s visible (`rmw_tickle;rmw_fastrtps_cpp;rmw_cyclonedds_cpp`).
2. Forces FastDDS and CycloneDDS onto real UDPv4 - no shared-memory transport - via their own
   supported configuration: FastDDS through `FASTRTPS_DEFAULT_PROFILES_FILE` pointing at
   `.github/scripts/fastdds_udp_only.xml` (`<use_builtin_transports>false</use_builtin_transports>`
   + an explicit UDPv4-only `<transport_descriptors>`); CycloneDDS through `CYCLONEDDS_URI`
   pointing at `.github/scripts/cyclonedds_no_shm.xml`
   (`//CycloneDDS/Domain/SharedMemory/Enable` = `false` - this install's own real default is still
   unconfirmed, so the file sets it explicitly either way).
3. Prints `/dev/shm` contents and the resolved transport config as a sanity check - **read this by
   hand before trusting any timing number**, it's a coarse proxy, not a hard guarantee that either
   vendor actually dropped shared memory for this specific run.
4. Runs `buildfarm_perf_tests`' own two-process/`rmw` benchmark (`-R two_process_rmw_`, the same
   filter `rmw-perf.yml`'s own standing job uses) across all three implementations, then prints a
   Markdown summary table (`rmw_perf_summary.py`) to copy into this section by hand.

No results recorded here yet - update this section (replacing this paragraph) the next time the
script actually runs.
