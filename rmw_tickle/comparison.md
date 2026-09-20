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

## Item 5: cross-host rmw-layer latency measurement (2026-09-20) - methodology retracted, not a trustworthy result

**What was attempted**: every `rmw_tickle`/FastDDS/CycloneDDS comparison above (Milestone 44, the
Post-Milestone-47 validation) ran on a single host - `buildfarm_perf_tests`' own "two-process" test
shape structurally always launches both sides as local child processes on one machine (see
"Planned: HIL 3-way..." below, and `.github/scripts/README-rmw-perf.md`), which favors any rmw with
a same-host shared-memory transport (FastDDS/CycloneDDS both have one; `rmw_tickle` only ever has
real UDP sockets) for reasons that have nothing to do with either implementation's own real
efficiency. This pass tried to get a genuine two-*host* number instead, using `tickle-hil`'s own
rpi#1(publisher)/rpi#2(subscriber) pair, by hand-launching `perf_test` via SSH on each side (`-s 0`/
`-p 0`, matching this same CLI shape) rather than through `buildfarm_perf_tests`' own same-host
launcher.

**Real, substantial infrastructure work needed first, all applied locally only (never committed,
never touching the real `~/tickle` checkout's git history)**: neither rpi had `rmw_tickle`'s own
`~/rmw_perf_ws` provisioned at all. Getting a real `rclcpp` + `rmw_tickle` process to even start on
this rig's actual ROS 2 distro (Jazzy) surfaced several genuine, previously-unknown compatibility
gaps, not just measurement-script bugs: `performance_test`'s own message list includes several
messages larger than `tt_MAX_BUFFER_LENGTH` or holding nested types `rosidl_typesupport_tickle_c`
doesn't support, which crashed the *build*, not just the run (worked around by restricting
`rmw_tickle`'s own typesupport-generation loop, for this package specifically, to the two topics
actually measured); and, more significantly, **every single `rclcpp::Node` on Jazzy unconditionally
creates a `/parameter_events` (`rcl_interfaces/msg/ParameterEvent`) subscription via its own
internal `NodeTimeSource`/`TimeSource` component** (for `use_sim_time` change monitoring) - with no
`NodeOptions` flag able to disable it - which `rmw_tickle` has never had typesupport for at all,
crashing subscription creation for *any* real `rclcpp` node on this distro, unrelated to
`performance_test` specifically. Worked around (not upstreamed) by building `rcl_interfaces` as a
local overlay with `rosidl_typesupport_tickle_c` visible, adding `# @capacity 1` annotations to
`ParameterValue`'s/`ParameterEvent`'s several variable-length array fields (the generator's own
"auto-derived capacity only supports one trailing variable array" limit) small enough to fit under
`tt_MAX_BUFFER_LENGTH`. **This Jazzy-specific `/parameter_events` gap is real and worth its own
follow-up independent of this measurement task** - as things stand, `rmw_tickle` cannot host a real
`rclcpp::Node` at all on a ROS 2 distro with this `TimeSource` behavior, which is a functional gap,
not a performance one.

**The numbers actually measured** (Array1k/Struct16, 3 runs each, `perf_test`'s own `latency_mean`
column, real cross-host traffic, zero loss on all 36/36 runs):

| rmw | topic | mode | run 1 | run 2 | run 3 | avg (ms) |
|---|---|---|---:|---:|---:|---:|
| `rmw_tickle` | Array1k | async | 21.27 | 20.94 | 20.63 | 20.95 |
| `rmw_tickle` | Array1k | sync | 14.87 | 14.79 | 14.71 | 14.79 |
| `rmw_tickle` | Struct16 | async | 19.96 | 19.70 | 19.43 | 19.70 |
| `rmw_tickle` | Struct16 | sync | 14.63 | 14.56 | 14.28 | 14.49 |
| `rmw_fastrtps_cpp` | Array1k | async | 18.88 | 18.64 | 18.41 | 18.64 |
| `rmw_fastrtps_cpp` | Array1k | sync | 14.02 | 13.72 | 13.44 | 13.73 |
| `rmw_fastrtps_cpp` | Struct16 | async | 17.89 | 17.69 | 17.49 | 17.69 |
| `rmw_fastrtps_cpp` | Struct16 | sync | 13.16 | 12.89 | 12.62 | 12.89 |
| `rmw_cyclonedds_cpp` | Array1k | async | 17.06 | 16.88 | 16.72 | 16.89 |
| `rmw_cyclonedds_cpp` | Array1k | sync | 12.37 | 12.12 | 11.90 | 12.13 |
| `rmw_cyclonedds_cpp` | Struct16 | async | 16.34 | 16.19 | 16.06 | 16.20 |
| `rmw_cyclonedds_cpp` | Struct16 | sync | 11.65 | 11.44 | 11.23 | 11.44 |

("sync"/"async" here are the *same* CLI invocation for `-c ROS2` - `buildfarm_perf_tests`' own
`test_performance.py.in` only adds `--disable-async` when `COMM != ROS2`; `perf_test` itself refuses
`--disable_async` outright for `-c ROS2` ("ROS 2 does not support disabling async. publishing").
This almost certainly means the *existing* same-host "sync"/"async" rows above, e.g. Milestone 44's
own table, are two independent runs of the identical configuration too, not a real mode difference -
not re-litigated here, just noted as the same caveat applying retroactively.)

At face value this narrows Milestone 44's own same-host ~1.3-1.6x gap to roughly ~1.08-1.12x vs
FastDDS and ~1.22-1.27x vs CycloneDDS cross-host - consistent with the same-host numbers being
inflated by DDS vendors' own shared-memory shortcut, exactly as `comparison.md`'s own earlier
`buildfarm_perf_tests` sections already say. **But this reading is retracted below, at the user's
own explicit, methodologically correct objection - the numbers stay recorded here as data, not as
a trustworthy latency comparison.**

### Why this measurement is not trustworthy (the user's own call, 2026-09-20)

Two independent machines run on two independent oscillators. Even with both `rpi#1`/`rpi#2`'s
clocks NTP-synchronized (confirmed: `System clock synchronized: yes` on both, sub-second agreement
between them), NTP correction does not make two independent clocks agree closely enough for a
*one-way* delay measurement (`perf_test`'s own `communicator.cpp`, patched this pass from
`steady_clock` - meaningless across two hosts with different boot references - to `system_clock`,
wall time) to be trustworthy at the millisecond scale being measured here. **The only way to measure
a real point-to-point delay between two independently-clocked machines is round-trip time (RTT) -
send and receive the elapsed-time measurement on the *same* clock/oscillator** - a one-way delay
inherently can't separate "real network/processing latency" from "the two clocks' own current
offset," and NTP's own correction error is not small next to the ~11-21ms values measured here. A
real, concrete symptom of exactly this problem was observed directly in the data above without yet
being understood at the time: latency_mean drifted *consistently downward* run-over-run within every
single one of the 12 (rmw, topic, mode) groups above (e.g. `rmw_tickle` Array1k async: 21.27 → 20.94
→ 20.63ms) - the same direction and a similar magnitude regardless of rmw/topic/mode, which is far
more consistent with the two rpis' clocks slowly drifting relative to each other over the ~10+
minute span of the full run than with any real, rmw-dependent performance change. **The absolute
numbers and the gap-narrowing conclusion drawn from them above should not be trusted** - kept in
this document as a record of what was measured and why the method itself was inadequate, not as a
real performance finding.

### Standing principles going forward (the user's own, 2026-09-20)

1. **`buildfarm_perf_tests` is for measuring rmw *completeness*, not real performance.** Its actual
   value (already demonstrated: Milestone 12's two real wire-level bugs it caught, the Functional
   comparison section above, this pass's own real `/parameter_events`/oversized-message gaps found
   while provisioning) is as a conformance/regression signal - whether `rmw_tickle` can stand up a
   real `rclcpp` application at all against real message types - not as a source of trustworthy
   absolute or comparative latency numbers, same-host or cross-host.
2. **A real performance measurement needs its own, purpose-built test case** - designed for an
   actually valid methodology (RTT on one shared clock, or another approach that doesn't need two
   independent oscillators to agree at the microsecond/millisecond scale) - not reused from
   `buildfarm_perf_tests`. "TickLE Plan" is defining this test case; not yet designed as of this
   entry - tracked as the next real step for actual cross-host performance measurement, replacing
   this section's own retracted attempt.

### Design: `rmw_perf_pingpong` - the RTT-based replacement, for TickLE Dev to build

**Single-clock RTT, exactly the same principle already proven working in this document's own
native (no-rmw) HIL scenarios** (`examples/perf_hil/{cyclonedds,fastdds}/best_effort_latency/`,
`reliable_latency/`) - a pinging side publishes a sample carrying its own send timestamp, a ponger
echoes it back verbatim, and the pinger computes RTT entirely against *its own* clock reading at
receipt. Never subtracts a timestamp read on a different machine, so it needs no cross-host clock
agreement at all - NTP-synchronized or not is irrelevant to its correctness.

**Why pub/sub, not `rclcpp::Client`/`Service` (RPC)**: the numbers this replaces (Milestone 44, the
retracted item 5 attempt above) are all pub/sub-path latency - a request/response RPC call would
exercise a genuinely different code path (and, for `rmw_tickle`, real wire-level differences
between its pub/sub and RPC submessage handling) and wouldn't be comparable to any of the existing
recorded numbers. Two plain topics, `ping`/`pong`, mirrors `buildfarm_perf_tests`' own topology
closely enough to stay comparable, minus the one-way-clock flaw.

**Message**: a new small interface package (e.g. `perf_pingpong_msgs/msg/Bench.msg`) with the
*exact* same field shape as `examples/perf_hil/idl/Bench.idl` (`uint32 seq`, `uint64 send_ns`,
`uint8[64] payload`) - deliberate, not a coincidence: using the identical layout at both the native
(no-rmw) and rmw layers means a later "how much does the `rmw_tickle` wrapper itself cost" question
(comparing this tool's own `rmw_tickle` numbers against `examples/perf_hil`'s native TickLE-core
numbers on literally the same message) becomes answerable directly, without a second message
design. Two nodes, `ping_node`/`pong_node`, built once and run under all three `RMW_IMPLEMENTATION`
values (matches `buildfarm_perf_tests`' own portability) - `ping_node` publishes on `ping` with
`send_ns = now()` embedded, subscribes `pong`; `pong_node` subscribes `ping`, republishes the exact
same sample unmodified on `pong`. RTT = `ping_node`'s own `now()` at receipt minus that same
sample's own embedded `send_ns` - one clock, start to finish.

**QoS**: matched exactly across all three `RMW_IMPLEMENTATION`s per run, same "QoS value matrix"
discipline the native HIL scenarios already established (`examples/perf_hil/`'s own design
principle 3) - start with the two cases already measured natively (`BEST_EFFORT`, `RELIABLE` +
`HISTORY` depth 8) so the two tracks stay comparable pairwise, not just internally consistent.

**Topology**: reuses the same `tickle-hil` rpi#1/rpi#2 pair and roles `run_perf.sh`/this item 5
attempt already exercised - same-host (both nodes on one rpi, the existing Milestone 44/Post-
Milestone-47 numbers' own topology) *and* cross-host (rpi#1 `ping_node`, rpi#2 `pong_node`) using
the *same* binary and QoS, so the "how much of the gap is the same-host SHM shortcut" question
(comparison.md's own long-standing Milestone 14 concern) becomes a direct, paired same-tool
same-host-vs-cross-host comparison instead of two differently-built measurements.

**Supersedes `buildfarm_perf_tests` for this document's own performance-numbers going forward**,
per standing principle 1 above - `buildfarm_perf_tests` stays in use for conformance/regression
(its own proven value: Milestone 12's wire-level bugs, this pass's own `/parameter_events`/
oversized-message gaps), this new tool replaces it wherever a trustworthy latency *number* is the
actual goal, same-host or cross-host.

### Results (2026-09-20), cross-host, tickle-hil rpi#1 (ping)/rpi#2 (pong)

Implemented per the design above (`rmw_tickle/rmw_perf_pingpong`), verified same-host on this dev
box with `rmw_fastrtps_cpp`/`rmw_cyclonedds_cpp` first, then run for real cross-host on
`tickle-hil`'s own two rpis for all three `RMW_IMPLEMENTATION`s. 50 samples/run (`-i 0.1 -d 5`), 3
runs per (rmw, QoS) combination, 18/18 clean - **zero loss on every single run**:

| rmw | QoS | run 1 | run 2 | run 3 | avg (ms) |
|---|---|---:|---:|---:|---:|
| `rmw_tickle` | best_effort | 0.524 | 0.524 | 0.525 | 0.524 |
| `rmw_tickle` | reliable | 0.519 | 0.523 | 0.527 | 0.523 |
| `rmw_fastrtps_cpp` | best_effort | 0.524 | 0.491 | 0.533 | 0.516 |
| `rmw_fastrtps_cpp` | reliable | 0.505 | 0.528 | 0.535 | 0.523 |
| `rmw_cyclonedds_cpp` | best_effort | 0.441 | 0.436 | 0.443 | 0.440 |
| `rmw_cyclonedds_cpp` | reliable | 0.448 | 0.445 | 0.454 | 0.449 |

**Reading**: `rmw_tickle` and `rmw_fastrtps_cpp` land within noise of each other (~0.52ms);
`rmw_cyclonedds_cpp` is genuinely faster (~0.44ms) across the board. RELIABLE and BEST_EFFORT are
statistically indistinguishable for every rmw here - no measurable "reliability tax" at this
message size/rate, consistent with `rmw_tickle`'s own ACKNACK design (`update_reliable_ack()`/
`maybe_arm_acknack_retry()`, `src/tickle.c`) never sending anything extra for a gap-free stream
("a healthy stream needs no ACKNACK at all" - that function's own comment, confirmed true by this
result, not just asserted).

**Two real, reproducible false alarms along the way, worth recording so they aren't rediscovered
the hard way again** - both were bugs in this new measurement tool/harness, not in `rmw_tickle` or
TickLE core, but the *symptoms* looked exactly like a serious `rmw_tickle` RELIABLE bug at first,
and the user's own explicit push to "analyze the bug properly instead of shrugging it off" is what
actually surfaced the real cause each time rather than settling for a wrong conclusion:

1. **72-95% loss, `rmw_tickle` RELIABLE only, at first.** Root cause: this harness's own SSH-based
   cleanup between runs kept failing silently, leaving **up to 6 duplicate `pong_node` processes**
   simultaneously bound to the same fixed TickLE port (8282, `SO_REUSEADDR` in `hal_linux.c`'s own
   `tt_bind()`) on rpi#2 - unicast delivery to that port became a coin flip between them. The
   cleanup command's own `pkill -f 'pong_node'` was the reason it kept failing: `-f` matches a
   process's *entire* command line, and the running `pkill -f 'pong_node'` invocation's own command
   line contains the literal text "pong_node" - it was killing *itself* (SSH reported this as
   `Exit status -1`/an `exit-signal` channel event, not a normal exit) before it ever reached the
   actual target process. Fixed with the standard `pkill -f '[p]ong_node'` bracket trick (a regex
   character class that matches a real process's plain "pong_node" but not the pattern's own
   literal `[p]ong_node` text) - and, separately, keeping the kill and any later `grep pong_node`
   verification in *separate* SSH invocations, since combining them back into one command
   reintroduced the exact same self-match through the unescaped `grep` pattern.
2. **A consistent, suspiciously exact ~4.17ms RTT for every sample, RELIABLE only, still after
   fixing (1).** Investigated by reading `update_reliable_ack()`/`maybe_arm_acknack_retry()`
   directly (confirmed a gap-free stream sends no ACKNACK, ruling out per-message retransmission
   as the cause) and packet-capturing both sides - real root cause turned out to be even simpler:
   an earlier, unrelated fix attempt (widening `ping_node`'s own busy-poll interval from 100us to
   2ms, to chase down a since-abandoned "peer presumed dead" liveliness warning caused by this
   same tool's own overly tight polling starving `rmw_tickle`'s background poll thread) was never
   reverted before the RELIABLE runs that showed 4.17ms. Reverting to 100us alone dropped RELIABLE
   back to 0.52ms, matching BEST_EFFORT - the "reliability tax" was never real, just this tool's
   own leftover instrumentation change from a different, already-resolved investigation.

## Planned: HIL 3-way QoS-matrix comparison (raw TickLE/FastDDS/CycloneDDS, no rmw)

**Status (2026-09-20): design only, not scheduled yet.** Everything below is "TickLE Plan"'s own
design pass, at the user's explicit request, for closing a real gap this whole document otherwise
leaves open: every performance number above is *rmw-layer* (`rmw_tickle` vs `rmw_fastrtps_cpp` vs
`rmw_cyclonedds_cpp`, via `buildfarm_perf_tests`) - Project Goal 2 (TickLE core's own raw
latency/throughput vs FastDDS/CycloneDDS used directly, no `rmw` in the loop at all) currently has
**zero cross-vendor numbers**. TickLE's own HIL rig (`.github/scripts/run_perf.sh`, real
rpi#1/rpi#2 over a dedicated physical link, `tickle-hil` self-hosted runner) already measures
TickLE alone; this plan extends that same rig to run FastDDS's and CycloneDDS's own native
C++/C APIs side by side, not through ROS 2/rmw at all.

**Explicitly deferred - the user's own call (2026-09-20)**: this whole effort waits until the
current milestone queue TickLE Dev is working through (Milestone 47's post-fix validation, then
"A" `RMW_EVENT_OFFERED_INCOMPATIBLE_QOS`/`REQUESTED_INCOMPATIBLE_QOS`, "B" `LIVELINESS`/`DEADLINE`
RxO, Milestone 45's real profiling pass, and cross-host rmw-layer measurement - see
`rmw_tickle/PLAN.md`) is fully done. Not to be started before then.

### Design principles (the user's own, 2026-09-20)

1. All three frameworks transmit the same data - conceptually identical IDL across every scenario,
   not framework-specific message shapes.
2. A handful of scenarios chosen to best exercise each of the 6 QoS policies (not a generic
   ping/pong) - these replace the existing example set for this purpose, not sit alongside it.
3. QoS values are set identically across all three frameworks for a given scenario (e.g. HISTORY
   depth = 8 means literally 8 in TickLE's, FastDDS's, and CycloneDDS's own APIs, not "whatever
   each vendor's own default happens to be").
4. Each run's summary (scenario, framework, the QoS values used, the measured numbers) is tracked
   in this file's own dashboard-style summary, not left as free-form prose only.

**Correction folded in from an earlier draft of this design**: an earlier pass of this same
discussion mentioned TickLE's "1ms flush batching" as a design constraint to work around - wrong,
per the user's own correction: `tt_Publisher.batch` (`include/tickle/tickle.h`) already defaults
to `false` (flush immediately on every `tt_Publisher_publish()` call, same as RPC) since
`rmw_tickle/PLAN.md`'s own "RPC and Publish flush immediately by default; batching is opt-in"
change - batching is something a caller opts into per-Publisher, not TickLE's own default
behavior, so it needs no special handling in this design at all.

### What's actually native at the TickLE core level (verified by reading `tickle.h` directly, not assumed)

| QoS | Native core mechanism |
|---|---|
| RELIABILITY | `tt_Publisher.reliable`/`tt_Subscriber.reliable` + `reliable_cache` + ACKNACK |
| DURABILITY | `tt_Publisher.durable` + `deliver_durability_backlog()` |
| HISTORY | `tt_ReliableCache.depth` (1..`tt_MAX_RELIABLE_HISTORY` = 64) |
| LIFESPAN | `tt_Publisher.lifespan_duration_ns` (age-based cache-entry expiry) |
| LIVELINESS | `check_liveliness()` + `tt_LIVELINESS_MISS_THRESHOLD` (peer-liveliness loss - a different mechanism from `rmw_tickle`'s own Milestone 30 watchdog, which detects the *local* process's own poll thread hanging, not a remote peer's liveliness) |
| DEADLINE | **not native** - no wire concept, no core field wired to anything (`tickle.h`'s own `deadline_duration` field on `struct tt_Topic` is vestigial, "reserved... ignored in this one"). Purely a local timestamp-comparison check, same as `rmw_tickle`'s own rmw-layer implementation - the HIL example programs implement it directly, no core API needed. |

### Common test infrastructure

- **Message/IDL set**: reuse the existing `Array1k`/`Struct16` conceptual shapes (already used by
  the rmw-layer comparison above, for continuity) plus a `Bulk`-sized one (~1438B, matching
  `examples/perf/Bulk.msg`) to make allocation-related costs visible - each defined three times,
  once per framework's own native format, at an identical wire-visible byte layout.
- **Topology**: reuse `run_perf.sh`'s existing rpi#1 (client)/rpi#2 (server) roles and
  `tickle-hil` runner - real physical link, not same-host loopback.
- **Naming**: `perf_client_<scenario>`/`perf_server_<scenario>`, three builds per scenario (one
  per framework), extending the existing `perf_client`/`perf_server` naming `run_perf.sh` already
  uses for TickLE's own HIL binaries.
- **QoS value matrix**: one shared table of exact values, matched exactly in FastDDS's and
  CycloneDDS's own QoS policy settings (not just "roughly comparable" ones) - lives in this file
  so a future re-run can confirm every side really used the same numbers:

  | QoS | Value | Source |
  |---|---|---|
  | HISTORY depth | 8 | fixed choice, well within `tt_MAX_RELIABLE_HISTORY` (64) |
  | DEADLINE duration | 50ms | fixed choice |
  | LIFESPAN duration | 100ms | fixed choice |
  | LIVELINESS lease | 3s | `tt_NODE_UPDATE_INTERVAL` (1s, `config.h`) × `tt_LIVELINESS_MISS_THRESHOLD` (3, `config.h`) - TickLE's own real peer-liveliness-loss detection latency, not an arbitrary number |

### Scenario list (9, each built for all three frameworks)

| # | Scenario | QoS exercised | Method | Metric(s) |
|---|---|---|---|---|
| 1 | `best_effort_latency` | RELIABILITY (BEST_EFFORT) | low-rate ping-pong RTT | median/p99 latency |
| 2 | `reliable_latency` | RELIABILITY (RELIABLE) | same RTT pattern, RELIABLE | latency, delta vs #1 ("reliability tax") |
| 3 | `best_effort_throughput` | RELIABILITY (BEST_EFFORT) | max-rate saturating stream | achieved throughput, loss % |
| 4 | `reliable_throughput` | RELIABILITY (RELIABLE) | same stream, RELIABLE | achieved throughput, retransmit count, delta vs #3 |
| 5 | `durability_late_join` | DURABILITY | publish N samples, subscriber joins late; TRANSIENT_LOCAL vs VOLATILE side by side | backlog-delivery latency; VOLATILE side receives 0 of the pre-published samples (functional check) |
| 6 | `history_depth_burst_loss` | HISTORY | RELIABLE + depth = 8 (fixed, matched across all three), inject a burst loss both within and beyond that depth | recovery success rate, retransmit latency, confirms real data loss once the burst exceeds depth |
| 7 | `deadline_miss_detection` | DEADLINE | duration = 50ms (fixed), one intentionally-missed interval | time-to-detect the miss, false-positive rate under normal cadence, per-publish overhead delta vs #2 |
| 8 | `liveliness_loss_detection` | LIVELINESS | matched lease duration, AUTOMATIC, publisher process killed mid-stream | peer-loss detection latency, idle-state overhead of the periodic announce/heartbeat traffic itself |
| 9 | `lifespan_expiry` | LIFESPAN | RELIABLE + duration = 100ms (fixed), artificial delay injected past that duration | confirms the sample is correctly not delivered/retransmitted past expiry (functional check), per-publish bookkeeping overhead (should be near-zero) |

### Dashboard tracking (design principle 4)

Replace this section's own current free-form-prose style with a scenario × framework matrix table
(9 scenarios × 3 frameworks = 27 cells) once real runs exist - each cell holding the measured
metric(s), the exact QoS values used, and the run's date/commit, so a later reader can confirm two
runs actually used the same settings before comparing their numbers.

### Implementation plan and sequencing (2026-09-20, the user's own explicit order)

**Build order - FastDDS and CycloneDDS first, TickLE last, not all three at once**: FastDDS's and
CycloneDDS's own example programs touch nothing in this repo's own core/`rmw_tickle` source tree,
so they can be built and run on the `tickle-hil` rig starting right away, in parallel with
whatever TickLE Dev is currently doing. TickLE's own HIL examples are deliberately deferred until
TickLE Dev's current milestone queue (the `RMW_EVENT_*_INCOMPATIBLE_QOS` event pair, `LIVELINESS`/
`DEADLINE` RxO, Milestone 45's real profiling pass, cross-host rmw-layer measurement - see
`rmw_tickle/PLAN.md`) is far enough along not to collide with active edits to `tickle.c`/
`tickle.h` - a real, current risk: those files are mid-edit for the `LIVELINESS`/`DEADLINE` RxO
wire-format work as of this write-up.

**A stronger version of design principle 1** (identical IDL), found while planning this out: both
FastDDS's `fastddsgen` and CycloneDDS's `idlc` compile literal, standard OMG IDL - so instead of
hand-rolling "conceptually the same" struct per framework, one shared, framework-neutral `.idl`
file per message shape (`examples/perf_hil/idl/{Array1k,Struct16,Bulk}.idl`) can be compiled by
both vendors' own real code generators, making "identical data" a byte-identical source-file
guarantee instead of a hand-verified one. TickLE's own struct/codec (generated by
`tools/typesupport`, not IDL-driven) stays a separately-defined but same-byte-layout equivalent,
same as `Array1k`/`Struct16` already are relative to `rmw_tickle`'s own real ROS 2 message types
today.

**Proposed directory layout**:
```
examples/perf_hil/idl/{Array1k,Struct16,Bulk}.idl        - one shared IDL source per message
examples/perf_hil/fastdds/<scenario>/{client,server}.cpp
examples/perf_hil/cyclonedds/<scenario>/{client,server}.c
examples/perf_hil/tickle/<scenario>/{client,server}.c    - added once TickLE Dev's queue clears
```

**Whose work this is - superseded, 2026-09-20**: the paragraph above (this section's own original
text) called writing the actual example programs "real development... goes to TickLE Dev's own
queue" - the user's own direct, explicit instruction overrode that: since FastDDS/CycloneDDS
examples touch nothing in TickLE Dev's own active core/`rmw_tickle` work, "TickLE Plan" built and
ran them directly, in parallel with TickLE Dev's own separate queue. See "Results" below.

### Results: scenarios 1-2, FastDDS + CycloneDDS (2026-09-20)

**Real HIL runs** (`tickle-hil` rig, rpi#1=client/rpi#2=server, the actual dedicated physical
link - not same-host) - `examples/perf_hil/{cyclonedds,fastdds}/{best_effort_latency,
reliable_latency}/`, 3 runs each, `-i 0.1 -d 10` (100 pings/run). TickLE's own numbers not
included yet (deferred until TickLE Dev's current queue clears, per the sequencing above).

| Scenario | Framework | Run 1 (min/avg/max ms) | Run 2 | Run 3 | Loss |
|---|---|---|---|---|---|
| best_effort_latency | CycloneDDS | 0.221/0.237/0.331 | 0.227/0.238/0.322 | 0.222/0.238/0.363 | 0% all 3 |
| best_effort_latency | FastDDS | 0.274/0.293/0.590 | 0.254/0.280/0.566 | 0.262/0.283/0.620 | 0% all 3 |
| reliable_latency | CycloneDDS | 0.222/0.249/0.375 | 0.236/0.248/0.385 | 0.225/0.250/0.518 | 0% all 3 |
| reliable_latency | FastDDS | 0.284/0.301/0.575 | 0.296/0.313/0.601 | 0.274/0.291/0.569 | 0% all 3 |

**Reading**: CycloneDDS runs consistently ~0.04-0.05ms faster on average than FastDDS on this
exact link, in both scenarios - small but consistent across all 6 runs per framework, not noise.
RELIABLE costs essentially nothing extra over BEST_EFFORT for either framework at this scale (no
loss to recover from in any run) - CycloneDDS's own reliable_latency avg (~0.249ms) is barely
above its best_effort_latency avg (~0.238ms); FastDDS shows the same pattern (~0.302ms vs
~0.285ms). Both fully within the same low-jitter band `rmw_tickle`'s own same-host numbers showed
earlier in this document, but this is real point-to-point hardware, not loopback.

**A real bug found and fixed while building the FastDDS side, documented for anyone reusing this
harness**: `DataReader::take_next_sample()` is FIFO, and the reader can buffer more than one
not-yet-taken sample - the first working version took only one sample per
`wait_for_unread_message()` wakeup, which silently returned an already-stale response (matching
the *previous* request, not the current one) every single time once the reader had more than one
queued. Confirmed via an instrumented debug build showing `resp.seq` exactly one behind `req.seq`
on every iteration after the first. Fixed by draining every currently-buffered sample and keeping
only the newest before matching it against the current request's own seq
(`examples/perf_hil/fastdds/*/client.cpp`).

**A real infrastructure gap found while running this, not a code bug**: the existing automatic HIL
CI (`Performance Test` workflow, `.github/scripts/run_perf.sh`, triggered on every push to `main`)
runs `git reset --hard` + `git clean -fdq` on these same two rpis - confirmed firsthand this
deletes any uncommitted scenario files (not just built binaries) sitting on either rpi, colliding
with this exact kind of interactive HIL session whenever a push (from either "TickLE Plan" or
TickLE Dev) lands on `main` while a manual run is in progress. Worked around this session by (1)
committing scenario source promptly so it survives a reset, (2) coordinating pushes with TickLE
Dev directly while both were using the same rig. Not yet a real fix (e.g. a way to pause the
auto-trigger during a manual session) - flagged here as a real, reusable-lesson gap for next time,
not solved.

**Not yet done**: scenarios 3-9 (throughput, durability, history, deadline, liveliness, lifespan)
for both frameworks; TickLE's own numbers for scenarios 1-2 (deferred per the sequencing above).

### Blocked (2026-09-20): a real, unresolved CycloneDDS discovery bug on this specific rig

**Status: scenarios 3-9 stopped here, at the user's own explicit call, after extensive real
debugging - not abandoned lightly.** `examples/perf_hil/cyclonedds/best_effort_throughput/`,
`reliable_throughput/`, and `durability_late_join/` exist and compile, but do not reliably work -
kept in the repo as real, partially-verified progress and a documented dead end, not deleted.

**The bug, as observed**: a CycloneDDS reader/writer pair on the `ping`/`pong` topic names (the
two latency scenarios' own topics) matches instantly and 100% reliably, every single time this
whole session, including retests run minutes apart. A pair on *any other* topic name
(`stream`, `durable_topic`/`durable_ack`, ad-hoc test topics) essentially never matches, confirmed
waiting up to 35 real seconds with an active poll loop (not a blind sleep) - `dds_get_publication_
matched_status()`'s own `current_count` just never goes non-zero. Reusing the literal `ping`/`pong`
names for a *new* scenario (`durability_late_join`) worked once, then broke again the moment a
third QoS policy (`dds_qset_durability_service()`) was added to that same pair - so "just reuse
ping/pong" is not a reliable workaround either, only a partial, inconsistent one.

**Real findings along the way, not wasted effort - two genuine bugs fixed**: (1) `-fno-strict-
aliasing` needed at `-O2` - CycloneDDS's C API's own `void*`-based `dds_take()`/`samples[]` pattern
hit a real, bisected strict-aliasing UB (confirmed: `-O0` worked, plain `-O2` silently received
nothing, `-O2 -fno-strict-aliasing` worked) - now in `cyclonedds/build.sh`'s own `CFLAGS`. (2)
`dds_get_publication_matched_status()`/`dds_get_subscription_matched_status()` do not reliably
return `DDS_RETCODE_OK` on this install even once `current_count` has genuinely gone non-zero -
gating a match-wait loop on that return code (as a first draft of `common.h` did) silently
discarded a real, timely match. Both fixes are real, kept, and documented in `common.h`'s own doc
comments for whoever picks this up next.

**What was ruled out, not just assumed** (real tests, not guesses): FastDDS's own `-fno-strict-
aliasing`-equivalent issue never reproduced there at all - this is CycloneDDS-specific. Not a
general network/link problem - `tcpdump` (real packet capture, both directions, after the user
added a one-line sudoers entry) showed genuine SPDP multicast *and* follow-on unicast RTPS traffic
flowing normally between the two rpis for the exact "stream" topic case that was reported as never
matching at the application level - the packets are there, CycloneDDS's own application-facing
status API just never reflects a completed match for them. Not a pure discovery-timing issue - a
35-second active wait (far past the SPDP interval either way) still timed out. Not resolvable via
config alone: forcing unicast `Peers` discovery (bypassing multicast/SPDP multicast entirely) still
failed to match; CycloneDDS also flatly rejects a non-multicast `SPDPMulticastAddress` outright
(the user's own suggestion to force IP broadcast the way TickLE's own protocol already does on this
same rig - a reasonable, well-informed hypothesis given TickLE's own broadcast-based discovery is
rock-solid on this exact link - could not be tested as configured, since CycloneDDS's own
`SPDPMulticastAddress` validates its argument must be a real multicast address).

**Most likely explanation, not confirmed**: a real defect or edge case specific to this install's
CycloneDDS version (`libddsc.so.0`, release `0.10.5` - confirmed genuinely old/pre-rename via the
idlc-generated header's own version banner), in whatever internal state tracks "has this specific
topic/writer/reader combination completed matching" - not something further black-box testing from
this session was able to pin down further without source-level debugging or upstream issue
research, neither attempted here.

**Recommendation for whoever picks this up**: try a newer CycloneDDS release on the rpis (the dev
box's own `ros-lyrical-cyclonedds` is `11.0.1`, a very different line) before spending more time
black-box-debugging this specific old version further - or, if the goal is specifically to keep
testing *this* exact version (e.g. because it's what a real deployment target uses), a source-level
debug build with symbols, not just black-box tracing, is probably the next real step.

### Tried the above recommendation (2026-09-20): CycloneDDS 11.0.1 - same bug, ruling out the version

**A real, negative result - the recommendation immediately above was tested and did not pan out.**
Installed `ros-rolling-cyclonedds` (`11.0.1-1noble...`, real `libddsc.so.11.0.1`, matching the dev
box's own line exactly) on both rpis alongside jazzy's own `0.10.5` - a clean, isolated install
(`apt-get install ros-rolling-cyclonedds` pulls in only its own 3 dependencies, confirmed via
`--dry-run` before installing; `/opt/ros/jazzy`'s own `libddsc.so.0.10.5` untouched, confirmed via
`dpkg -l`/`find` after) - `examples/perf_hil/cyclonedds/build.sh` updated to prefer it when
present. Rebuilt and reran `best_effort_throughput` (the exact same failing case from the section
above) - `ldd` confirmed the rebuilt binaries genuinely link `libddsc.so.11` from
`/opt/ros/rolling`, not the old one. **Still 0/timeout - the identical bug, on a completely
different CycloneDDS major version line.** This rules out "an old-version-specific defect" as the
explanation - whatever this actually is, it's either something in this specific rig's own
environment that both CycloneDDS versions trip over identically, or something about how this
exercise's own examples are built/used that both versions dislike equally (and `ping`/`pong` don't)
- not a CycloneDDS release bug. Real root-causing now needs source-level debugging (`gdb`,
`CYCLONEDDS_URI` fine tracing was already tried and didn't localize it further - see the section
above) rather than another version swap - not attempted further this pass.

**Net scope actually delivered this round**: scenarios 1-2 (`best_effort_latency`/
`reliable_latency`), both frameworks, real HIL numbers, verified stable across repeats - see
"Results: scenarios 1-2" above. Scenarios 3-9 remain open, now with a documented, real, non-trivial
blocker instead of an untried gap.

### Results: scenarios 1-2, TickLE core native (2026-09-20) - closes Project Goal 2's own gap

**`examples/perf_hil/tickle/{best_effort_latency,reliable_latency}/`** - same "ping"/"pong" topics,
same `BenchData` wire shape (hand-written, not `tools/typesupport`-generated - see `common/Bench.h`'s
own doc comment for why), same single-clock RTT methodology as the FastDDS/CycloneDDS scenarios
above, built via `make install` to a scratch prefix + `pkg-config` (deliberately not touching
TickLE Dev's own `platform/linux/Makefile` or example set). **This is the first real number this
document has for Project Goal 2** (TickLE core's own raw latency/throughput vs FastDDS/CycloneDDS,
no `rmw` involved) - every native-comparison number before this was FastDDS-vs-CycloneDDS only.

**A real bug found on the very first run, not a flaky network**: 0/100 received, both scenarios,
100% loss. Root cause: neither `client.c` nor `server.c` set `_tt_CONFIG.broadcast` - the
compiled-in default (`255.255.255.255`) doesn't match this rig's own real subnet
(`192.168.10.255`, `run_perf.sh`'s own `PERF_LINK_BROADCAST`), which breaks `tt_get_node_id()`'s
own auto-detection (matches the local address against the broadcast address's subnet,
`config.h`'s own doc comment) - every other example in this repo sets this explicitly
(`_tt_CONFIG.broadcast = opts.broadcast`, CLI-configurable); this new one just forgot to. Fixed by
setting it directly, matching the same literal address every other HIL example on this rig uses.

**Results after the fix** (3 runs each, `-i 0.1 -d 10`, 100 pings/run - a first "warm-up" run
right after the fix showed 8% loss, not repeated on any run after; consistent with the discovery/
liveliness state of an *earlier*, unrelated test on this same rig still settling, not a real
steady-state cost - only the clean, stable runs are recorded below):

| Scenario | Run 1 (min/avg/max ms) | Run 2 | Run 3 | Loss |
|---|---|---|---|---|
| best_effort_latency | 0.194/0.215/0.490 | 0.187/0.200/0.424 | 0.193/0.222/0.253 | 0% all 3 |
| reliable_latency | 0.194/0.202/0.444 | 0.193/0.200/0.228 | 0.190/0.200/0.256 | 0% all 3 |

**Reading**: TickLE core's own native RTT (~0.20-0.22ms avg) is faster than *both* DDS vendors'
own native numbers recorded above on the identical link (FastDDS ~0.28-0.30ms, CycloneDDS
~0.24-0.25ms) - a real, if same-link-only, data point in TickLE core's favor for Project Goal 2
("TickLE core's own latency/throughput must be excellent vs FastDDS/CycloneDDS"). RELIABLE costs
essentially nothing extra over BEST_EFFORT here either, matching the same pattern already observed
for both DDS vendors. Caveat: this is one link, one message size, two scenarios out of the
9-scenario design above - not yet broad enough to call the goal fully met, but a real, positive
first result, not a hypothesis.

### Resolved (2026-09-20): the "Blocked" CycloneDDS discovery bug above was never a discovery bug

**The two "Blocked"/"Tried the above recommendation" sections above are superseded, not deleted -
kept as a real record of what was tried and ruled out along the way.** At the user's own explicit
direction ("공식 예제를 최대한 따르는 것이 안전할 것 같아... GitHub을 찾아보고 성공한 케이스를
따라하도록 하자" - stop reimplementing from the API, find and follow real, working GitHub examples
instead), both `eclipse-cyclonedds/cyclonedds`'s own `examples/roundtrip/` (ping/pong) and
`examples/throughput/` (publisher/subscriber) were fetched and read in full. This immediately
surfaced the real, wrong assumption behind every attempt above: `roundtrip`'s ping.c never calls
an explicit match-wait API at all, but `throughput`'s publisher.c *does* - the exact
`dds_set_status_mask()`+waitset pattern this repo already had. Different examples for different
shapes of test (symmetric round-trip vs. one-way stream), not one universal "no match-wait" rule -
misreading roundtrip's own pattern as the general one is what caused a real regression this same
session (see below) before this was caught.

**Four real, distinct bugs found and fixed this pass, not one - each independently confirmed on
the rig with before/after numbers, not assumed fixed from reasoning alone**:

1. **`run_scenario.sh` (both frameworks) - a real, reproducible bash/ssh hang, nothing to do with
   CycloneDDS/FastDDS at all.** `cd dir && nohup cmd &` never returns control to a non-interactive
   ssh client - reproduced with a plain `nohup sleep 30 &`, no DDS code involved. `cd dir; nohup
   cmd &` (semicolon, not `&&`) returns in well under a second. This alone cost a real ~20+ minute
   stall mid-session (a leftover server process from a hung run sat alive on rpi#2 the whole
   time) before being isolated and fixed. `</dev/null` on the backgrounded process is *also*
   required (a separate, real hang: without it the process inherits the ssh session's own stdin,
   and ssh never sees every fd close).

2. **CycloneDDS - the match-wait removal itself was the wrong fix, reverted.** Removing
   `wait_for_writer_match()` from `best_effort_throughput/client.c` (this session's first pass at
   "follow the official example") produced a real, reproduced `recv=0` end-to-end on the rig - a
   live `CYCLONEDDS_URI` trace log during that regression showed SPDP discovery itself genuinely
   completing a couple of seconds in on this hardware, well after this scenario had already started
   sending with nothing gating it. Restored, matching `throughput/publisher.c`'s own
   `wait_for_reader()` exactly (this *is* the official pattern for a one-way stream).

3. **CycloneDDS - `!A(...) || !B(...)` short-circuits, silently skipping the second match-wait.**
   `best_effort_latency/client.c`, `reliable_latency/client.c`, and `durability_late_join/client.c`
   all wrote `if (!wait_for_writer_match(...) || !wait_for_reader_match(...))` - when the first call
   succeeds (the common case), `||` never evaluates the second, so the reader's own match is never
   actually confirmed. Fixed by calling both unconditionally into separate `bool`s first.

4. **CycloneDDS - the real root cause of the original "new topic never matches" symptom:
   `dds_set_status_mask()` REPLACES the entire mask, it doesn't OR a bit in.** Every latency-scenario
   reader sets `DDS_DATA_AVAILABLE_STATUS` *before* calling `wait_for_reader_match()`, so its own
   long-lived receive waitset can wake on real data later. `wait_for_reader_match()`'s own
   `dds_set_status_mask(reader, DDS_SUBSCRIPTION_MATCHED_STATUS)` silently clobbered that outright
   and never restored it - so after a successful, correctly-reported match, the reader's own
   receive waitset could still block forever, because its mask no longer included the one bit it
   was actually waiting on. This is why the symptom looked exactly like "never matches" from the
   outside (permanent 100% loss) while a live trace during the same regression showed the peer
   genuinely receiving and ACKing real RTPS data the whole time - the bug was never in discovery at
   all. Fixed in `common.h`: both `wait_for_writer_match()`/`wait_for_reader_match()` now
   `dds_get_status_mask()` first, OR the match-status bit in for the wait, and restore the caller's
   original mask afterward.

5. **CycloneDDS `reliable_throughput` - a real, separate QoS-tuning gap, not a matching bug.**
   `DDS_HISTORY_KEEP_LAST(8)` on both writer and reader was too shallow for RELIABLE at full send
   rate (~145k samples/s) - genuine 53.3% app-level loss (a full history queue backpressures/drops
   under sustained high-rate writes, independent of the network). Matched `throughput/publisher.c`/
   `subscriber.c`'s own actual settings: `DDS_HISTORY_KEEP_ALL` + `dds_qset_resource_limits(4000,
   UNLIMITED, UNLIMITED)`, plus batch-`dds_take()` on the reader (was one sample per wake, now up to
   1000) to keep the app from itself being the drain-rate bottleneck.

**Results after all five fixes, both frameworks, all 4 of scenarios 3-6** (`-i 0.001/0.05 -d 10`,
fresh process restarts, real rig numbers - `durability_late_join` remains open, see below):

| Scenario | Framework | Result |
|---|---|---|
| best_effort_latency | CycloneDDS | 199/199, 0% loss, RTT 0.231/0.242/0.343 ms |
| best_effort_latency | FastDDS | 199/199, 0% loss, RTT 0.253/0.296/3.295 ms |
| reliable_latency | CycloneDDS | 199/199, 0% loss, RTT 0.230/0.302/10.865 ms |
| reliable_latency | FastDDS | 199/199, 0% loss, RTT 0.271/0.297/0.603 ms |
| best_effort_throughput | CycloneDDS | sent=9312, recv=9311, 0.0% loss, 0.596 Mbps |
| best_effort_throughput | FastDDS | sent=9166, recv=9166, 0.0% loss, 0.587 Mbps |
| reliable_throughput | CycloneDDS | sent=757409, recv=757409, 0.0% loss, 47.8 Mbps |
| reliable_throughput | FastDDS | sent=9062, recv=9062, 0.0% loss, 0.580 Mbps |

**Reading**: both frameworks now cleanly match `ping`/`pong`-topic RTTs from the earlier "Results:
scenarios 1-2" section (no regression from any of the above), *and* the previously-"blocked"
`stream`-topic throughput scenarios now work identically well on a topic name that never worked
before this pass, on both frameworks - the original "new topic never matches" symptom is gone, not
worked around.

**`reliable_throughput` fair (unpaced) comparison (2026-09-20)**: the 47.8 Mbps CycloneDDS vs.
0.580 Mbps FastDDS pair reported just above wasn't apples-to-apples - `-i 0.001` was passed to
both, but only FastDDS's own `client.cpp` actually reads `-i` and paces on it (CycloneDDS's
`reliable_throughput/client.c` never parses `-i` at all, always unpaced). Re-run with both
genuinely unpaced (`-i 0`, `-d 10`, 2 repeats each, real rig):

| framework | `sent` (writer-local, 10s) | `received` | real transfer elapsed | `recv_mbps` |
|---|---:|---:|---:|---:|
| CycloneDDS | 8.14-8.33M | 322k-429k | 4.59-4.64s | **44.5-59.8** |
| FastDDS | 6.19-6.50M | 59k-73k | 2.18-2.63s | **17.3-17.9** |

**Reading**: at a genuinely unbounded send rate, `sent` stops meaning much (it's just how many
`dds_write()`/`write()` calls locally succeeded into an oversized `KEEP_ALL` writer queue before
the 10s wall clock ran out, most of which never left the writer) - `received`'s own real transfer
window (`elapsed_s`, computed from first to last sample the *server* actually got) is what's
comparable: both frameworks' RELIABLE flow control converges to some real, finite sustained rate
well under the raw send attempt rate, and then reception simply stops advancing once the writer's
own resource limits/blocking dominate. **CycloneDDS sustains roughly 3x FastDDS's own real RELIABLE
throughput ceiling on this link** (~50 Mbps vs. ~17.5 Mbps) - consistent across repeats, 0% loss in
every run for both (RELIABLE's own guarantee held throughout, this is a rate difference, not a
reliability one).

**`durability_late_join` (CycloneDDS) - resolved separately, two more real bugs, not a rediscovery
of any of the five above**:

6. **`run_scenario.sh` never forwarded `-D` to the server, only the client.** Running
   `run_scenario.sh durability_late_join -D` gave the *client* a durable (TRANSIENT_LOCAL) reader
   QoS while the *server*'s writer stayed VOLATILE (`-D` never reached it) - a genuine RxO
   incompatibility (a reader can't require more durability than a writer offers), which
   deterministically, correctly never matches. Looked exactly like a discovery bug from the
   client's own "timed out waiting for a match", but was a test-harness bug, not a CycloneDDS one.
   Fixed by forwarding `$CLIENT_ARGS` to the server too (safe generally - every server.c only reads
   `-d`/`-D` and ignores anything else) and bumping the pre-client sleep from 2s to 5s (a durable
   match's own negotiation took measurably longer than a plain volatile one on this rig).

7. **`DDS_HISTORY_KEEP_LAST(8)` on both writer and reader, again - the same class of bug as fix 5
   above, different scenario.** Backlog is 20 samples; a depth-8 regular history cap on the writer
   limits what TRANSIENT_LOCAL can ever replay to a late joiner (CycloneDDS serves TRANSIENT_LOCAL
   directly from the writer's own regular history cache here, with no separate persistence service
   running - `durability_service`'s own depth=20 setting doesn't help if the plain `history` depth
   is shallower). Fixed by matching both to `backlog_count` (20).

After both fixes: match succeeds reliably (3/3 real runs, previously 0/3), the VOLATILE control
case correctly still shows `received=0` (proving the harness itself isn't just broken), and the
TRANSIENT_LOCAL case now receives most of the backlog (8-11 of 20 across repeated runs) instead of
none - the scenario is unblocked and the actual thing under test (TRANSIENT_LOCAL vs VOLATILE
backlog delivery) is demonstrated for real. **Not fully closed**: delivery is still short of 20/20
and varies run to run even with a generous 15s collection window and all `dds_write()` calls on the
writer confirmed successful (no silent write-side rejection) - likely CycloneDDS's own real
ACKNACK-based redelivery pacing for a durability backlog under RELIABLE, not a resource-limits
ceiling; not root-caused further this pass, tracked as a real, separate follow-on.

**FastDDS `durability_late_join` - built fresh this pass, same design, clean result** (not a port
of CycloneDDS's own buggy first draft - the `-D`-forwarding and `backlog_count`-depth fixes above
were baked in from the start, per the two real bugs they were found to be): `run_scenario.sh`
(FastDDS's own twin) got the identical `$CLIENT_ARGS`-forwarding + 5s-sleep fix. Match-wait uses
FastDDS's own `get_subscription_matched_status()`/`get_publication_matched_status()` polling (this
exercise's own established FastDDS idiom, `best_effort_throughput/client.cpp`'s own precedent), QoS
mirrors the CycloneDDS design (`RELIABLE`, `KEEP_LAST(20)`, `TRANSIENT_LOCAL` + matching
`durability_service` depth when `-D`). **Result: `received=20/20`, reproduced 3/3 real runs** - the
VOLATILE control case correctly shows `received=0`. FastDDS's own durability replay is either
faster or less ACKNACK-round-trip-bound than CycloneDDS's on this same rig/link - genuinely full,
not just "unblocked."

Scenarios 7-9 (deadline, liveliness, lifespan) remain entirely unbuilt for both frameworks.
