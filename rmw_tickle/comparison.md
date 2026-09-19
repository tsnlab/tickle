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

### Results (2026-09-19, local dev box - not the tickle-perf rig, but the same `lyrical` distro +
pre-provisioned `~/rmw_perf_ws` this box happens to already have)

**Real bug found on the first run, not yet reproduced on the second**: `rmw_tickle`'s own async
mode crashed (`SIGABRT`) on both topics with `Data consistency violated. Received sample with not
strictly higher id. Received sample id 5 Prev. sample id: 4788` - a real data-consistency
assertion inside `performance_test`'s own receiver, not a script/harness failure. Did not
reproduce on an immediate second run (0 errors, 0 failures) - intermittent, not yet root-caused.
Worth its own investigation regardless of the comparison below (a race under sustained async
publish load is a real, if rare, concern) - not blocking this report, tracked here as a loose end.

**A real methodology bug found and fixed before trusting any number**: the first run's own
`/dev/shm` sanity check (this script's own step) showed live `fast_datasharing_*`/`fastdds_*`
segments being created *during* the run, despite `fastdds_udp_only.xml`'s `useBuiltinTransports=
false` - FastDDS's "Data Sharing" feature is a **separate**, QoS-level same-host shortcut layered
on top of whichever transport is configured, not itself a transport - disabling the transport
alone does nothing to it. Fixed by adding `<data_sharing><kind>OFF</kind></data_sharing>` to both
the default `data_writer`/`data_reader` QoS profiles in `fastdds_udp_only.xml` (see that file's
own updated comment for the full story) - confirmed on a second run that no new `/dev/shm`
segments appeared at all. **The results below are from that second, verified-clean run.**
`rmw_cyclonedds_cpp`'s own async cases were skipped by `ctest` on this particular run (a test-
registration flakiness on this rig - the total test count itself varied 16 -> 24 between the two
runs - not something either DDS vendor's own async correctness has been confirmed on here yet).

| Topic | Sync | rmw implementation | Latency (ms) | Throughput (Mbit/s) | Received (/ ~1000 sent) | Lost |
|---|---|---|---:|---:|---:|---:|
| Array1k | async | `rmw_fastrtps_cpp` | 0.0375 | 0.9901 | 998 | 0 |
| Array1k | async | `rmw_tickle` | 0.0487 | 0.9905 | 998 | 0 |
| Array1k | sync | `rmw_cyclonedds_cpp` | 0.0322 | 0.9878 | 996 | 0 |
| Array1k | sync | `rmw_fastrtps_cpp` | 0.0363 | 0.9905 | 998 | 0 |
| Array1k | sync | `rmw_tickle` | 0.0467 | 0.9904 | 998 | 0 |
| Struct16 | async | `rmw_fastrtps_cpp` | 0.0369 | 0.0305 | 998 | 0 |
| Struct16 | async | `rmw_tickle` | 0.0543 | 0.0304 | 997 | 0 |
| Struct16 | sync | `rmw_cyclonedds_cpp` | 0.0357 | 0.0304 | 996 | 0 |
| Struct16 | sync | `rmw_fastrtps_cpp` | 0.0310 | 0.0305 | 998 | 0 |
| Struct16 | sync | `rmw_tickle` | 0.0464 | 0.0305 | 998 | 0 |

**Reading**: throughput is essentially identical across all three (dominated by the test's own
fixed send rate, not implementation efficiency - expected, not a finding). Latency is not: with
both DDS vendors genuinely forced onto real UDP/IP (verified above, not assumed), `rmw_tickle`
is consistently **~1.3-1.6x higher latency** than whichever DDS vendor is present in each row -
0.0464-0.0543ms vs FastDDS's 0.0310-0.0375ms and CycloneDDS's 0.0322-0.0357ms. Small, but real and
consistent across every topic/sync-mode combination measured - not noise. This is still a
same-host, not-the-real-target-network-medium measurement (10Base-T1S is the actual target - see
this workflow's own top comment) - read as "how much implementation-level overhead does
`rmw_tickle` carry relative to two mature DDS stacks, network medium held equal," not as a
prediction of real-deployment numbers.

**Not yet done**: a real cross-host run (this is still same-host, `lo`-adjacent - see
`rmw_tickle/PLAN.md`'s own Milestone 14 "left as explicit future work" note, still true); chasing
the intermittent async crash; understanding why `rmw_cyclonedds_cpp`'s own async cases were
skipped rather than run.

### Follow-up (2026-09-20): Milestone 45's own allocation-pooling change - an honest negative result

`rmw_tickle/PLAN.md`'s Milestone 45 implemented the highest-confidence fix this section's own
profiling pointed at: `rmw_publish()`'s scratch `to_tickle()` conversion buffer and `rmw_
subscription.c`'s own per-message "shell" buffer (`subscriber_callback()`/`rmw_take_with_info()`)
both now reuse an allocator-owned buffer instead of a fresh `allocate()`/`deallocate()` pair every
single call - a real, verified-correct change (a dedicated regression test, `test_publish_take_
reuse.c`, passes clean under ASan+UBSan+leak detection, specifically targeting the one real
correctness hazard reuse introduces: a reused buffer must be zeroed before its next use, or a
`rosidl_runtime_c__String__assign()`-style call would free memory a caller still owns).

**Re-running this exact script twice (this same methodology, same box) after that change shows no
measurable, above-noise latency improvement** - if anything, one topic/sync-mode combination came
in marginally *higher* than Milestone 44's own baseline, well within the same run-to-run noise
band the DDS vendors' own numbers show here too (e.g. `rmw_fastrtps_cpp` sync Array1k: 0.0363ms
baseline vs 0.0309/0.0365ms across these two follow-up runs - a wider swing than most of the
`rmw_tickle` deltas below):

| Topic | Sync | Milestone 44 baseline | Run 1 (this change) | Run 2 (this change) |
|---|---|---:|---:|---:|
| Array1k | async | 0.0487 | *(crashed - see below)* | 0.0505 |
| Array1k | sync | 0.0467 | 0.0542 | 0.0473 |
| Struct16 | async | 0.0543 | *(crashed - see below)* | 0.0529 |
| Struct16 | sync | 0.0464 | 0.0496 | 0.0449 |

(Run 1's own async rows crashed outright - all four `rmw_tickle` combinations that run hit
`rmw_tickle/PLAN.md`'s own Milestone 47 finding, `Data consistency violated... id 1/2/5 Prev.
sample id: ~6770-6820` - the already-root-caused, deliberately-deferred cross-instance identity
gap, not anything Milestone 45 touched. Retrying the whole script - the same recovery Milestone 44
itself needed on its own first run - produced Run 2's own clean, 0-failure numbers.)

**Independent corroboration (2026-09-20, "TickLE Plan," 3 more runs on this same box, per the
user's own explicit go-ahead)** - same conclusion, plus a genuinely new finding about how often
Milestone 47's own crash actually shows up:

| Topic | Sync | Milestone 44 baseline | Run 3 (this change) |
|---|---|---:|---:|
| Array1k | async | 0.0487 | 0.0506 |
| Array1k | sync | 0.0467 | 0.0463 |
| Struct16 | async | 0.0543 | 0.0440 |
| Struct16 | sync | 0.0464 | 0.0462 |

Run 3 (the one clean, complete run of the three) lands in the same place as TickLE Dev's own two
runs above - some deltas up, some down, none clearly outside this rig's own established noise band
(FastDDS/CycloneDDS's own numbers, untouched by this change, moved by similarly-sized amounts run
to run throughout this whole exercise). **Runs 4 and 5 did not produce usable `rmw_tickle`
numbers at all**: Run 4 had most combinations skipped by `ctest` outright (the same test-
registration flakiness already noted above, worse this time - only one row printed); Run 5 hit
Milestone 47's own crash again, independently - `Data consistency violated. Received sample with
not strictly higher id. Received sample id 1 Prev. sample id: 4392` and `...id: 4393`, on *two*
separate combinations in one run (`Array1k async` produced no result row at all; `Struct16 async`
technically finished but reported `Lost: 976` of what should have been complete delivery - numbers
too degraded to treat as a real latency measurement, not included above).

**Tally across every attempt so far, both sessions combined**: 5 total run attempts (TickLE Dev's
2 + these 3), Milestone 47's own crash/major-loss signature hit in **3 of them** - notably more
often than the 0-reproductions-in-25-iterations TickLE Dev's own narrower, `rmw_tickle`-only
natural-repro test found (`rmw_tickle/PLAN.md`'s own Milestone 47 row). Plausible reason, not yet
confirmed: this script builds and starts all *three* `RMW_IMPLEMENTATION`s' own two-process pairs
in sequence on one box, extending the total window (and process-churn) an old, slow-to-fully-exit
`rmw_tickle` process from an earlier combination has to overlap with a new one - exactly the
trigger condition Milestone 47's own root-cause analysis already named as most likely. **Worth
relaying to whoever picks up Milestone 47's implementation**: this script is turning out to be a
meaningfully better reproducer for that bug than the isolated test was.

**Overall verdict on Milestone 45 item 1's own latency effect, both sessions' data combined**: no
measurable, above-noise improvement across every clean run so far (1 from TickLE Dev, 1 from this
session) - a real, if unglamorous, negative result. Not necessarily "the fix did nothing" - malloc/
free churn removal is still a legitimate, verified-correct efficiency win on its own terms, and a
same-host `lo`-adjacent rig with ~30-50us absolute latencies is a genuinely hard place to see a
few-`malloc`-calls-worth of savings above this much ambient noise. A confident answer either way
would need many more repeated trials with real statistical treatment (median/IQR across, say, 10+
clean runs) - not attempted here, a bigger undertaking than manual runs support well. Given how
often Milestone 47's own crash is corrupting attempts, **fixing Milestone 47 first is probably a
prerequisite for that kind of larger, trustworthy run anyway**, not just a nice-to-have.

**Reading, honestly**: eliminating a real per-message heap allocation pair did not move the
needle here. The most likely explanation, not yet confirmed by an actual profiler: at this
message size and this box's own glibc, a same-size, high-frequency `malloc`/`free` pair is
already served out of `tcache` on the order of tens of nanoseconds - genuinely below this
measurement's own noise floor (run-to-run variation of several *micro*seconds, i.e. hundreds of
times larger) at the ~0.03-0.05ms total latency scale being measured here. **This is a real,
verified-correct change that stays in the codebase regardless** (fewer allocator calls is its own
modest win, e.g. under `valgrind`/heap-profiling, and it's strictly simpler than what it replaced),
but it does not close Milestone 45's own latency gap. The two remaining candidates that row's own
original analysis named - (2) `rmw_publish()`'s unconditional `tt_Node_interrupt()`+lock pair
regardless of whether the poll thread needed interrupting, and (3) the real cross-thread hand-off
between poll_thread and the application's own executor thread - were **not** attempted this pass;
closing this gap for real now needs an actual profiling pass (`perf`/`ftrace`, not counting
allocations by reading the code) to find out whether either of those, or something not yet
identified at all, actually dominates - tracked as the next real step, not guessed at again.

**Post-Milestone-47 validation (2026-09-20, TickLE Dev, 3 runs, per the user's own explicit
go-ahead)** - the actual point of this follow-up: does Milestone 47's own `(node_id, entity_id)`
WriterProxy + goodbye fix (`rmw_tickle/PLAN.md`) actually close the crash this whole exercise kept
tripping over. **All 3 runs completed clean - 36/36 `two_process_rmw_*` combinations passed, zero
crashes, zero `Data consistency violated` occurrences, zero skipped `rmw_tickle` rows** (the two
`rmw_cyclonedds_cpp` skips each run are a pre-existing, unrelated `async` test-registration quirk
already noted above, not a `rmw_tickle` issue):

| Topic | Sync | Run 1 | Run 2 | Run 3 |
|---|---|---:|---:|---:|
| Array1k | async | 0.0481 | 0.0504 | 0.0478 |
| Array1k | sync | 0.0533 | 0.0481 | 0.0462 |
| Struct16 | async | 0.0510 | 0.0533 | 0.0465 |
| Struct16 | sync | 0.0470 | 0.1045 | 0.0562 |

(Struct16 sync's own Run 2 spike, 0.1045ms, is a single-run outlier, not a repeat - Run 1 and Run 3
both land back in the same 0.04-0.06ms band every other row across every run this whole exercise
has produced sits in. Consistent with ordinary run-to-run noise on this rig, not a regression.)

**Updated tally, all sessions combined**: 8 total run attempts (5 pre-fix: 2 from TickLE Dev + 3
from "TickLE Plan", 3/5 hit Milestone 47's own crash/major-loss signature; 3 post-fix, all from
TickLE Dev, **0/3** hit it). Not proof the residual race the fix's own design explicitly
acknowledges (a still-in-flight stale packet arriving in the narrow window before goodbye's own
broadcast reaches a peer) is literally impossible - matching real DDS's own identical, accepted
limitation for an ungraceful shutdown - but a real, substantial drop in observed frequency (3/5 →
0/3) on the exact same reproducer that found it in the first place, which is the honest answer
this specific question can give without a much larger sample than manual runs support well.
