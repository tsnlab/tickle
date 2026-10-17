# Testing and measurement

How TickLE is tested, how its performance is compared, and the rules every measurement follows.
Wire format and internals: [DESIGN.md](DESIGN.md). Published figures: [RESULTS.md](RESULTS.md). The ROS 2
layer: [RMW.md](RMW.md). Open work: [ROADMAP.md](ROADMAP.md).

All commands run from the repository root.

## 1. Test tiers

| Tier | Command | What it is | Needs |
|---|---|---|---|
| Unit | `make test` | `tests/test_*.c`, whitebox: each includes `src/tickle.c` and a mock HAL (`tests/test_mock.h`). No real I/O. | a C compiler |
| Coverage | `make coverage` | the unit tests built with `--coverage`; line and branch figures for core (`src/`, HALs excluded), reports in `platform/linux/obj/coverage/report/`. Tracked, not enforced. | gcovr |
| Sanitizers | `make sanitize` | the unit tests under ASan + UBSan | |
| Threads | `make tsan` | the thread-safety test under ThreadSanitizer | |
| Fuzz | `make fuzz FUZZ_ARGS='-max_total_time=60'` | libFuzzer on the packet parser, seeded from `tests/fuzz_corpus/` | clang |
| Fuzz seeds | `make fuzz-corpus` | regenerates the seed corpus after a wire-format change | |
| Headers | `make headers-cpp` | public headers compile as C and as C++ | |
| Same host | `make test-samehost` | two nodes on one host over loopback, plus an io_uring-refused pass | no root |
| Linux HAL | `make test-linux` | two nodes in two network namespaces joined by a veth pair | passwordless `sudo ip` |
| FreeRTOS HAL | `make test-freertos` | a single-node selftest on each HAL build (interrupt wake, empty reads per datagram), then round trips under QEMU over emulated virtio-net | RISC-V toolchain, QEMU |
| All self-contained | `make test-all` | `test`, `headers-cpp`, `check-doc-shas`, `check-rig-lock`, `check-bench-shapes`, `test-samehost`, `test-linux`, `test-freertos` | all of the above |
| Typesupport | `make test-typesupport` | the code generator's pytest suites | Python |
| Codec drift | `make regen` | regenerates committed codecs; CI fails on any diff | `tickle-typesupport` installed |

Manual netns helpers (from `platform/linux/netns.mk`): `make createns`, `make deletens`, `make runclient`,
`make runserver`, `make runping`, `make runpong`, `make runperf_client`, `make runperf_server`, `make dump1`, `make dump2`.

**When to run what**
- Every change: `make check-gates` (section 2).
- Touched `src/hal_linux.c`, the transport or the protocol path: also `make test-linux`. The gate table does not run it.
- Touched `platform/freertos/` or the HAL contract (`include/tickle/hal.h`): `make -C platform/freertos lint` and `make test-freertos`.
- New code path: add a unit test. New `tests/test_*.c` files are picked up automatically.

### rmw_tickle tests

| What | How | Notes |
|---|---|---|
| rmw ctest suite | `.github/scripts/run_rmw_suite.sh` (also a `check-gates` row) | runs in a private netns; SKIPs with a reason if no ROS, no workspace or no netns |
| Build + colcon tests in a netns | `examples/perf_hil/experiments/rmw_test_netns.sh` | |
| Gap acceptance | `rmw_tickle/scripts/rmw_gap_acceptance.sh` | each scenario runs on `rmw_cyclonedds_cpp` first as the control; if the control fails, the test is VOID |
| Upstream conformance | CI only (`test_rmw_implementation`, in `check-all.yml`) | |
| Standard interfaces | `rmw_tickle/scripts/build_ros2_interfaces.sh` | needed before a default-options node runs on rmw_tickle |

A ready workspace for the acceptance scripts is `~/rmw_accept_ws` (or set `RMW_TEST_WS`).

## 2. The local gate and CI

### `make check-gates`

Runs every gate that can run on this machine, one row each, and exits non-zero if any row FAILs.

Rows: `lint` (clang-format + clang-tidy), `lint-shell`, `check-doc-shas`, `check-rig-lock`, `check-bench-shapes`,
`check-unsupported-list`, `check-context-reset`, `check-results-provenance`, `test (unit)`, `tsan`,
`test-typesupport`, `lint-freertos-hal`, `lint-rmw`, `rmw suite (as CI)`, `build configs (syntax)`.

Verdicts: `PASS`, `FAIL`, `SKIP` (could not run, with the reason), `ADVS` (lint findings from a clang that is not CI's 19).

**The procedure**

```
/tmp/lintenv/bin/clang-format -i <files>  →  git add <paths>  →  make check-gates  →  read the table  →  push only if it exits 0
```

- Format and lint with clang 19 (`/tmp/lintenv`), not the system clang. The two disagree on the same bytes. Setup:
  `python3 -m venv /tmp/lintenv && /tmp/lintenv/bin/pip install clang-format==19.1.0 clang-tidy==19.1.0`.
- Read the table by shape: `grep -E '^[A-Z]{4}  '`, count the rows, count the non-PASS rows. Do not compare it
  against a list of expected verdicts.
- Never chain `git push` after the gates with `&&`. Read the table yourself.
- ADVS or SKIP is not PASS. A SKIP means the check did not run.

**What the gate does not cover**
- It builds one configuration in full. `build configs (syntax)` only syntax-checks the other configurations.
- `make test-linux`. Run it yourself for HAL or transport changes.
- The interface set CI assembles. A capacity row naming a type that exists only on this machine passes here and fails in CI.
- C++ standard: CI compiles the ROS `.cpp` files as C++17; this machine defaults to C++20 (`lint_rmw.sh` uses 17).
- Flag-gated core code (`tt_CONTEXT_ID_CLAIM`, `tt_LOCAL_DELIVERY`). Lint it with `clang-tidy -p build/rmw_tickle`.

### CI workflows (`.github/workflows/`)

| Workflow | Trigger | Job | What it runs |
|---|---|---|---|
| `test-all.yml` "Test all" | push, PR | `linux-unit`, `coverage`, `sanitize`, `linux-netns`, `linux-netns-shm-off`, `freertos` in parallel; `test` gates on all six | Linux and FreeRTOS builds, FreeRTOS lint, doc SHAs, rig-lock coverage, `headers-cpp`, `test`, `coverage` (job summary + `coverage-report` artifact, not gated), `sanitize`, `tsan`, fuzz smoke (45 s), `test-samehost`, `test-linux`, `test-linux` with shared memory compiled out (control), `test-freertos` |
| `check-all.yml` "Check all" | push, PR | `check-all` | lint and clang-tidy, bench shapes, the unsupported-entry-point list, codec drift, typesupport tests, the rmw_tickle build, its ctest suite, upstream rmw conformance, std_msgs through rmw_tickle, codec identity |
| `performance.yml` "Performance Test" | manual only | `test`, `publish` | the HIL scenarios on the two Pis (`run_perf.sh`); a regression alert can fail the run only for the two throughput groups |
| `rmw-perf.yml` "rmw_tickle performance" | manual only | `compare` | `buildfarm_perf_tests` on the `tickle-perf` machine; tracks rmw_tickle against itself, not against the vendors |

After every push, check **every** workflow, not just one job.

## 3. The HIL rig

- **Hardware:** two Raspberry Pi 5s. rpi#1 runs the client (publisher side), rpi#2 the server (subscriber side).
- **Data link:** a direct point-to-point Ethernet link, `192.168.10.0/24` on `eth0`, with no third host on it.
  SSH and orchestration use the separate 10.1.1.x management LAN.
- **Loss and delay:** `tc netem` on the client's `eth0` egress, read back before each cell.
- **Runner:** the self-hosted `tickle-hil` runner on the lab PC SSHes into both Pis as `ci`.
  Setup: `.github/scripts/README.md`.
- **What runs there:**
  - `performance.yml` (manual);
  - the cross-vendor campaign (`examples/perf_hil/{tickle,cyclonedds,fastdds,zenohpico}/`, one native client/server pair per framework, run through `run_scenario.sh`);
  - A-B-B-A comparisons between two TickLE builds;
  - same-host shared-memory cells on one Pi.
- **The rig lock:** `examples/perf_hil/rig_lock.sh`. Every sweep takes it **inside** the detached process, not around it.
  - `RIG_LOCK_SCOPE=hil` (the default) claims the Pis.
  - `RIG_LOCK_SCOPE=box` claims the lab PC (used by `compare_rmw_perf.sh` and the rmw-perf job).
  - `RIG_LOCK_WAIT` sets the wait in seconds; `0` fails at once if the rig is busy.
  - Test the lock by taking it. The lock file only records the last holder.
- **The PC next to the rig:**
  - Never run TickLE processes in the PC's default network namespace. They reach the rig over 10.1.1.x and contaminate its runs.
  - Run them in a private netns instead. A netns with only `lo` needs a default route, or `sendto()` fails with "Network is unreachable".
  - Do not run the netns acceptance suite during a rig campaign. PC load fails its control.
  - `sudo -n` allows only `ip` and `tc`. Anything else fails silently.

### Long runs

- Detach the run: `setsid nohup ./script.sh > /path/outside/the/session.log 2>&1 < /dev/null &`.
  - Keep the script in the repository.
  - Write the results outside any session-scoped directory.
  - `$!` is the PID of `setsid`, which exits at once. Watch the output file, not the PID.
- Restore `tc` in the cleanup trap. Use one `EXIT` trap per script, because a second one replaces the first.
- State an estimate, schedule a check at 1.5x, and read the first minute of output.
- Check whether a job is alive from its log or output. A `pgrep -f` pattern can match the checking command itself.

## 4. Fair comparisons

TickLE is compared with FastDDS and CycloneDDS at two levels:
- **Core against core:** each framework through its own native API, no ROS 2.
- **rmw against rmw:** `rmw_tickle` against `rmw_fastrtps_cpp` and `rmw_cyclonedds_cpp`, each reached through `rclcpp` and `RMW_IMPLEMENTATION`.

zenoh-pico is measured to the same standard but kept as reference only; it is never scored.

**Same configuration**
- **The same QoS on every framework.** Every value is passed explicitly: reliability, history, durability, the
  KEEP_ALL bound (`-N`), `max_blocking_time` (`-B 100`) and KEEP_LAST depth (`-K 64`). A row whose `RESULT:` line
  does not echo those values is VOID.
- **Everything that is not a QoS policy stays at each product's shipped default.** TickLE runs `rmw_tickle`'s shipped
  defaults. A tuned arm (for example FastDDS with `maxMessageSize` 1472) is reported next to the shipped one, never
  instead of it.
- **The same environment:**
  - the same Pis and session, with frameworks interleaved in every repetition;
  - the same CPU governor, and no pinning: the OS schedules every process (since 2026-10-05; section 5). The
    earlier runs pinned to cores 1-3 (cross host) or one core per process (same host); `PIN`, `PIN_SERVER` and
    `PIN_CLIENT` reproduce them for a labelled note only;
  - data restricted to `eth0`, and release builds on all sides (TickLE rows assert `core_build=release`);
  - the same server lifetime and 3 s drain cap.
- **The same payloads**, P1-P4, gated by `check_bench_shapes.sh`. The size rule is `sample + base framing <= 1514`.
- **The same instruments in every harness.** Counters live in a shared header with the same field names:
  - CPU from `getrusage`, normalised per sample and per MB, never as a percentage or as absolute seconds;
  - peak RSS from `VmHWM`;
  - wire bytes and packets from `/proc/net/dev` on both hosts.

**What is carried is proven, not configured**
- **Transport witness:** an independent counter shows which path carried the data.
  - For same-host cells it is the loopback packet counter (`s6_witness_check.sh`, `s6_transport_cells.sh`). It reads about 0 for a shared-memory path and about 2 per sample for the kernel path.
  - TickLE's own `tx_shm` counter must agree with it.
  - A framework's introspection only reports its configuration, so it is not accepted.
- **Payload-boundary gate:** at N0 each payload must produce its designed packets per sample (1.0 at P1/P2; at P3 1.0 for TickLE and 2.0 for the vendors; at least 2.0 at P4). If it does not, the cell is VOID.
- **Library identity:** before an rmw number is believed, check which `librmw_tickle.so` the benchmark actually has mapped (`assert_loaded_rmw_tickle.sh`, `/proc/PID/maps`). Stale workspaces can shadow it.

**Goodput, delivery and voids** (the user, 2026-10-17)
- **Throughput is goodput:** the payload of the samples the receiving application got complete and unique, inside
  the same measured window (section 5 item 9). A partly received large sample counts 0; duplicates and
  retransmissions do not count. Every bench's receiver already counts this way: it counts a sample once, when its
  application takes it whole. `campaign_summary.py` scores it as `server.goodput_mbps`, its header's rules G1-G6
  (`--selftest` checks them).
- **The send rate is diagnostic only.** It is printed beside goodput (`diagnostic` rows `client.send_mbps` and
  `server.recv_mbps` in `campaign_summary.py`; `diag_*`, `send Mbps` elsewhere) and never decides a cell.
- **CPU and wire bytes are per delivered sample** (or MB delivered, or round trip answered), saturating cells
  included. For CPU this supersedes the offered-ticks denominator of 2026-10-12; delivered % of offered still ranks
  a saturating cell, and ranks it as goodput does.
- **Incomplete delivery in a RELIABLE cell:** compare received with sent, in every repetition (`campaign_summary.py`
  read only the last one until 2026-10-17). `loss_pct` cannot see samples that never arrived.
  - If a vendor delivers less than it sent, it is excluded from that cell and listed as DELIVERY FAILED, with its
    counts.
  - If TickLE delivers less than it sent, every metric in that cell counts as a LOSE.
  - **BEST_EFFORT cells are scored on goodput alone:** a sample not delivered is a sample not counted.
- **No drop-free rule.** Until 2026-10-17 throughput came only from repetitions with no drops, because a rep that
  dropped read a higher send rate. Goodput prices a drop at zero, so every repetition counts at its goodput.
- **A cell is VOID** if:
  - a leftover guard fired;
  - there is no `RESULT:` line;
  - an instrument read zero;
  - the QoS echo is wrong;
  - an expected `drained=acked` is missing.

  Report VOID as VOID, never as zero.

**Latency from creation time** (the user, 2026-10-17)
- **Latency is the arrival time minus the sample's creation time.** In a fixed-rate cell that is the scheduled
  publish time, start + k / rate; for a ping it is the ping's due time. The schedule does not slip when the send loop
  falls behind, so a publisher that blocks in `publish()` or queues behind a sample in flight pays for the wait.
- **The send-stamp latency is diagnostic only.** It is measured from the stamp the sender writes just before it sends.
  It is printed beside the creation figure (`rtt_*` beside `crt_*` in the core clients; "stamp" in the rmw reader) and
  never decides a cell.
- **Percentiles are over offered ticks.** A tick that was never published or never delivered counts as +inf, so a
  percentile that lands on one reads UNBOUNDED. Two UNBOUNDED values are equal, which makes a draw.
- **A saturating cell is compared at equal windows only.** There, a publisher that falls behind has a creation
  latency that grows with the run's length. The reader prints that growth, and marks arms whose windows differ by
  more than 1 s NOT COMPARABLE.
- **Every reading has a control:** a ping-pong where nothing waits at the sender must read the same on both
  definitions, up to the sender's timer wake. A failed control leaves every creation figure it gates NOT READ.
- Reader and rules R0-R5: `experiments/creation_latency_summary.py` (its header); the rmw cells take
  `rmw_keepall_rig.sh CREATION_TRACE=1`.

**Controls and reading**
- **Every campaign has a control:** an arm the change provably cannot touch, such as a vendor arm, the `rmw_cyclonedds_cpp` acceptance run, or the shared-memory-OFF build. Compare our delta with the control's delta.
- **Compare a figure with its own previous value**, not with a vendor or with a bare historical point.
- **Mirrored blocks:** builds are run interleaved (A P B B P A since gate v2, section 6.1; A B B A before), so drift over the session falls on every arm. Tools: `campaign_ab_chain.sh`, read with `ab_compare.py`.
- **Pre-registered metrics decide an A/B (2026-10-06):** the verdict (`PASS` / `WORSE`) is taken on the metrics named before the run alone, at a Bonferroni threshold for their count. Every other metric is printed as secondary and never decides. Since 2026-10-11 a TickLE-vs-TickLE A/B on the rig decides by gate v2 (section 6.1).
- **2xSE reading of the secondaries:** a difference counts only if it lies beyond twice the combined standard error.
  - With about 40 comparisons per chain, about 2 WORSE rows are expected by chance.
  - So a secondary WORSE row is only a candidate. It is confirmed if the same metric is WORSE in at least half of its cells, or if a targeted A-B-B-A re-run with it as primary finds it again.
  - An unconfirmed candidate is listed with its t value, not hidden.
- **Repetitions from data:** `reps_needed.py` turns earlier campaigns' rep-to-rep spread into the repetitions per arm a 1% or 3% change needs. Choose `REPS` from it, not from habit.
- **Preflight before the rig:** `rig_preflight.sh` runs every cell shape a rig job is about to run for a few seconds in private network namespaces on the PC and checks each RESULT line. The campaign drivers call it before they take the rig lock (`PREFLIGHT=0` skips it).
- **Win rule:** TickLE wins a cell only when it beats every scored vendor outside the spread. Inside the spread the cell is a draw.
- **The layout floor:** between two builds whose code differs in size, a throughput or CPU-per-sample difference below about 1% (p1), or about 10 KB of peak RSS, says nothing.
  - Function placement alone moves results that much.
  - Adding repetitions does not help. Gate v2 (section 6.1) builds every arm layout-stable and measures this floor in every run (arm P).
  - The Pi bench's own A/A floor is ±2.5 ns for send and ±1.2 ns for receive (`core_cost_pi_drift.sh`).
- **Session-bound:** compare only within one session. Day-to-day offsets of about 13 µs in latency affect every implementation.

## 5. Principles: the setup must not favour TickLE

Section 4 says how the frameworks are made equal. This section says what decides it, and was written after the
same-host table turned out to rest on a setting that suited TickLE (2026-10-05, the user's review): every process was
pinned to one core. TickLE is single-threaded, so one core is all it uses; FastDDS and CycloneDDS run several threads,
and one core makes them take turns. Pinning had started for a sound reason - core 0 of a Pi takes the interrupts, and
a single thread that lands there runs slower - but nobody runs software pinned, and a vendor could dismiss the whole
table for it.

1. **A result is only worth publishing if a vendor cannot dismiss it.** A lead that comes from the test setup is not
   a lead. When a setting is in doubt, first take the DDS default (the products' shipped default outranks every
   other choice, the user, 2026-10-06), and only where there is none, the one that favours the other side. So the
   BEST_EFFORT cells give every framework KEEP_LAST 1 explicitly (`-K 1`), the DDS default, rather than a deeper
   history; TickLE's BEST_EFFORT subscriber keeps no history at all and hands each sample to its callback as it
   arrives, which is what KEEP_LAST 1 with a reader that takes at once does. The bench code we write is held to the
   same rule: a vendor's bench must use its API the way its documentation does (CycloneDDS's BEST_EFFORT bench took
   one sample per wake until 2026-10-06; it now takes everything waiting, as the Fast DDS bench does).
2. **Run software the way its users run it.** The OS scheduler places threads, kernel settings are the
   distribution's, every product keeps its shipped defaults for everything that is not a QoS policy. Nothing is
   pinned, tuned or prepared for the measurement outside the run (item 9's warm-up is part of every run, identical
   for all, and only decides which samples are counted).
3. **Any change to the environment is an arm, not the setup.** Pinning off the interrupt core, a larger socket buffer,
   a tuned vendor profile: each is measured on every framework and reported beside the untuned headline, labelled,
   never instead of it.
4. **Symmetric is not the same as neutral.** The same `taskset` on every process still favoured TickLE, because the
   frameworks are built differently. Ask what each setting does to each architecture - threads, buffers, batching,
   transports - not only whether it was applied to all.
5. **P1-P4 all stay, and every one is reported.** Each measures something real (the user, 2026-10-05). They follow
   the Ethernet frame (`sample + base framing <= 1514`); P3 is the size where TickLE's smaller framing still fits one
   datagram and the vendors need two, which is a real difference in framing - so no claim rests on P3 alone.
6. **Measure what is delivered.** A sender's rate says nothing about a reader that kept up with a tenth of it (the
   same-host BEST_EFFORT rows were send rates until 2026-10-17). Every throughput row is goodput (section 4).
7. **Report everything.** Every cell, every VOID, every row TickLE loses or draws, every tuned arm beside its shipped
   one; a correction is made in place with its cause, never by deleting the figure it corrects.
8. **Constants are not fitted to the rig.** A value tuned on two Raspberry Pi 5s may be wrong elsewhere. Prefer an
   algorithm that derives it from what the running system measures; where a fitted value stays, README.md gives the
   formula (ROADMAP "Testbed-independent constants").
9. **No figure is a start-up or teardown transient** (the user, 2026-10-06). Every cell excludes the same warm-up
   and cool-down for every framework before its statistics. Latency: 4096 round trips at each end, 1 ms apart
   (`-W`/`-C`/`-I`; 4096 is TickLE's largest ring, so every slot is written once), each edge ending early once 20 s
   have passed since its first ping (`-T`; 2026-10-07, after 8,192 round trips at 10 ms delay outran a cell's timeout),
   around `-d` seconds of measured pings; RTT figures are over the measured round trips only and the line says
   `warmup= cooldown= measured= edge_max_s= warmup_end= cooldown_end=` (`count` or `time`).
   Throughput: 2 s at each end of sender time (`--warmup-s`/`--cooldown-s`) around `-d` measured seconds, client and
   server windowed by the same rule on each sample's `send_ns`; score the server's `win_recv_mbps` (goodput, section
   4), with the client's `win_send_mbps` and the whole-run `send_mbps`/`recv_mbps` beside it as diagnostics. `window=fail:...` is VOID, never a figure. Defaults and reasons:
   `examples/perf_hil/tickle/common/BenchWindow.h`. Found when TickLE's same-host p4 latency turned out to be a
   first-lap figure (ROADMAP 0a).

Still to audit against these (ROADMAP "A fair testbed"): socket buffer sizes, the CPU governor, IRQ affinity, the
vendor profiles (FastDDS XML, CycloneDDS URI, iceoryx), build flags, and whether any rate or duration was picked
because TickLE does well at it.

## 6. Main experiment harnesses

All are in `examples/perf_hil/experiments/`. Each one's header holds its pre-registered reading.

| Harness | What it answers |
|---|---|
| `campaign_sweep.sh` | The cross-vendor campaign: 12 cells x 3 frameworks x 3 repetitions, interleaved, five metrics |
| `campaign_summary.py` | Computes the per-cell verdicts (WIN/DRAW/LOSE/VOID) from a sweep's output: throughput as goodput, CPU and wire bytes per delivered sample (`--selftest` checks its rules) |
| `campaign_ab_chain.sh` | Is build B better or worse than build A on the campaign cells? (A P B B P A, gate v2) |
| `ab_compare.py` | Reads A/B arms per cell and metric; with `--placement`, by gate v2 (`gate_v2.py`, section 6.1) |
| `layout_flags_cost.sh` | What a code-alignment flag changes in each function's instructions, and what it costs in size (static) |
| `layout_identity.sh` | Does a layout-only change (`layout_perturb.sh`) or a relink at 63 placements change any function's instructions? (static, aarch64 and x86) |
| `rig_dryrun.sh` | Any rig harness end to end on the PC, against two stand-in Pis in private namespaces (never the rig: `rig_dryrun_test.sh`) |
| `rmw_gate_v2_chain.sh` | Is rmw_tickle build B better or worse than A? (A P B B P A, gate v2; read by `rmw_gate_v2.py`) |
| `rig_preflight.sh` | Does every cell shape about to go to the rig run, and print a sound RESULT line? (PC, private netns, ~70 s) |
| `reps_needed.py` | How many repetitions per arm does a 1% or 3% change need, from earlier campaigns' spread? |
| `comparison_remeasure_chain.sh` | Re-measures the published comparison on one build, all three frameworks |
| `core_cost_ab.sh` | Core CPU per sample, send and receive, between commits on the PC (no network, no rig lock) |
| `core_cost_pi_drift.sh` | The Pi bench's own noise floor (one build against itself) |
| `layout_control.sh` / `p1_layout_check.sh` | Is a small cross-host difference function layout or real work? |
| `s6_witness_check.sh` | Does the loopback counter really show which transport carried the data? |
| `s6_transport_cells.sh` | Same-host shared-memory cells for all three frameworks, each with a witness |
| `shm_transport_identity.sh` | Which transport carried each payload shape (asserted, not assumed) |
| `local_segment_cell.sh` | Quick same-host segment check on the PC in a netns before going to the rig (not a published figure) |
| `mixed_delivery.sh` | One publisher with a local and a remote subscriber: correct, and at what cost? (TickLE, FastDDS, CycloneDDS with iox-roudi; `mixed_delivery_campaign.sh` runs all three at one commit) |
| `veth_c6_check.sh` | c6 (P4, RELIABLE, 5% loss) reproduced off-rig on a veth pair |
| `wire_inventory.sh` | What TickLE puts on the wire, by submessage kind |
| `netem_counter_check.sh` | Does the tx counter include packets netem dropped? |
| `rmw_crosshost_rtt.sh` | Cross-host rmw round trip against FastDDS, CycloneDDS and zenoh |
| `rmw_samehost.sh` | The rmw comparison on one Pi: RTT (block, poll) and perf_test delivered rate, each rmw on its shipped same-host transport, witnessed; read with `rmw_samehost_summary.py` |
| `rmw_memory_footprint.sh` | Memory of a default `rclcpp::Node` under each rmw |
| `idle_cpu_arm.sh` | What an idle node costs in CPU |
| `rmw_lease86_rig_chain.sh` | Template for an rmw-layer A-B-B-A on the rig |
| `zenoh_cells.sh` | zenoh-pico reference cells (outside the DDS QoS gate) |
| `assert_loaded_rmw_tickle.sh` | Which `librmw_tickle.so` a running benchmark has mapped |
| `flake_count.sh` | How often a test binary fails or hangs over N runs |

Also `.github/scripts/compare_rmw_perf.sh`: a hand-run comparison of rmw_tickle against both vendors through
`buildfarm_perf_tests`, with the vendors forced onto UDPv4. It takes the `box` lock.

### 6.1 Gate v2: how a TickLE-vs-TickLE rig A/B decides (2026-10-11)

Why: the old gate was more sensitive than the build is reproducible. 2e9e861c vs a5747a85 read client CPU +2% WORSE
three times on the Pi 5 with equal executed instructions (x86 perf, exact aarch64 counts, Pi `instructions:u`), and the
gap vanished with `-falign-functions=64` on both arms. Definitions: `gate_v2_lib.sh`; the reading: `gate_v2.py`.

- **A1, layout-stable builds.** Every TickLE build the rig A/Bs or publishes from is built with
  `-falign-functions=64 -falign-loops=1 -falign-jumps=1 -falign-labels=1`, and on aarch64
  `-mno-fix-cortex-a53-843419` (`TICKLE_LAYOUT_CFLAGS`, the default when
  `TICKLE_EXTRA_CFLAGS` is unset; set and empty is the plain build). Each build is traced and its log read back
  (`build_flags_check.sh`); a build that does not show every one of its flags is not measured.
  - Function alignment: on aarch64 it fixes every in-function offset modulo 64, so no function's code changes when
    code ahead of it moves (`layout_flags_cost.sh`).
  - No padding inside a function (gate v3, 2026-10-12). Padding before a function entry is never executed; a nop at a
    loop head is, on every iteration. The Pis' gcc 13 aligns loops to 8, so a change that moves a loop head by 4
    bytes changed the instruction count by a nop per iteration. Under qemu (`aarch64_insn/layout_qemu.sh`, cells c2
    c3 c11 and same-host p2-p4) the bench executes no nop with these flags in any run, where the old flags executed
    3-10 a sample; the positive control (function alignment 4 vs 8, loop alignment 8) moves that by 1-2.
  - Placement alone (64, 128, 256) never moved the count, with either flag set: every function body is the same
    instructions, and the executed nops were equal. So X2r's WORSE (2026-10-11) was not padding: it was the CPU
    witness (client `cpu_s_per_Msample` +1.65% at c2, +1.15% at c3), and the client's -2 instructions a sample at c2
    is timing (a slower client finds more work per wake). These flags cannot change that; cycles are not counted,
    CPU time is a witness and still decides net of P's floor.
  - No linker rewrite by page offset (2026-10-15), aarch64 builds only: `-mno-fix-cortex-a53-843419`
    (`TICKLE_A53FIX_OFF_CFLAGS`; `GATE_BUILD_ARCH`, default aarch64, the Pis; PC builders set `$(uname -m)`). The Pis'
    toolchain has GNU ld patch an adrp at page offset 0xff8/0xffc: into an adr (one instruction for one) or, for a
    target beyond 1 MiB, by moving the following load into a stub (+2 executed instructions a pass). That depends on
    address bits 6-11, which function alignment does not fix and P moves. The Pi 5's Cortex-A76 does not have the
    erratum.
  - Checked against layout-only changes P does not make (`layout_identity.sh`, `layout_perturb.sh`: text ahead of
    tickle.c, of hal_linux.c, of the bench; static data moved; an uncalled function added mid tickle.c), every linked
    function of the rig's bench (latency p1, throughput p2, client and server), commit ab722f4e. With these flags every
    function's instructions are unchanged, aarch64 and x86, and in 63 relinked placements of each binary. With gate
    v3's flags (no A53 option), 1 placement in 63 put a stub in `tt_log_internal` in 3 of the 4 aarch64 binaries (cold
    here; nothing keeps a hot path out). Controls: loop alignment 8 (the Pis' gcc 13 default) moves nops under T1-T3
    on both ISAs, and the plain build moves them on x86.
  - Executed, on the PC (`srv_gap_pc.sh` arms `label:commit:L::<pert>`, 3 blocks, cells L p1/p4, T p2 Q0/Q1): no
    perturbation moved user instructions in a latency cell (|d| <= 0.84 a round trip, SE 0.2-0.8, against 4,105
    server and 6,007 client at p1). In throughput cells the server's count moves by up to 25 a sample (SE 5-13) under
    P as under every perturbation: batching follows timing, not code. P's floor is what absorbs that, so a
    throughput-cell server WORSE needs the floor; a latency-cell WORSE of a few instructions is work.
  - Not the product default: it adds 4.6% to the core's .text and 6.7% to the client on aarch64, and the rule was
    "only if it costs nothing measurable". Published figures carry `TICKLE_EXTRA_CFLAGS` in their provenance.
- **A2, instructions decide.** Every process runs under `perf stat -e instructions:u,instructions:k`. The primary
  metric is instructions per sample (per round trip on latency cells), user and kernel, client and server. CPU, rate
  and latency are witnesses: they still decide, net of the floor below. A row without counts is VOID.
- **A3, the placement floor.** Arm P is A's commit rebuilt with `-falign-functions=128` (and the same no-padding
  flags): the same instructions, every
  function moved by a multiple of 64 bytes, as a change moves the code it does not touch. Blocks run A P B B P A.
  A WORSE must exceed the Bonferroni Student-t threshold (Welch df) and `|P - A|` of the same metric and cell.
  No counts, no P arm, or a named metric that was not measured gives NO VERDICT.
- **Tools** (`GATE=v1` repeats an old chain or same-host run, read by the old rule).
  - Cross-host: `campaign_ab_chain.sh`, read by `ab_compare.py campaign --placement`.
  - Same-host: `ab_samehost.sh` (`ab_samehost.py`); `WITNESS=` is required and names what `PRIMARY` named before.
  - rmw: `rmw_gate_v2_chain.sh HARNESS=rtt|keepall` runs `rmw_crosshost_rtt.sh` or `rmw_keepall_rig.sh` per block,
    builds checked by `rmw_build_flags_check.py`, read by `rmw_gate_v2.py`.
  - On the PC, `rig_dryrun.sh` runs any of them end to end against two stand-in Pis in private namespaces. ssh and scp
    are stood in, and a real remote client started by the command fails the dry run (`rig_dryrun_test.sh`).
  - Validating the gate itself: give B an alignment different from P's, with the same no-padding flags
    (A vs A at `-falign-functions=256` and `TICKLE_NOPAD_CFLAGS`). A B with P's flags reads
    WORSE about half the time, because the floor is a single draw of placement.
  - Expected on the rig after A1'': X1 (A vs P) and X2 (A vs A at 256) PASS with no PRIMARY WORSE, every
    latency cell's `insn_u_per_sample` within its threshold, and no NOTE of a PRIMARY moving between A and P beyond
    it. A throughput-cell instruction or CPU move may still exceed the threshold and must sit inside P's floor
    (timing). An instruction WORSE in a latency cell would mean a layout path this check did not cover.
    `rig_queue_gate_xval.sh` runs both under one lock on the core cells, read by `gate_xval_read.py`.
  - Result (2026-10-17, build 5263213e, gate v2 A P B B P A, 3 reps, 12 core cells): X1 PASS and X2 PASS, 0 of 88
    deciding rows WORSE in each, every latency cell's `insn_u_per_sample` within its threshold. X1 had no A-vs-P NOTE;
    X2 had three, all in throughput cells (c1 and c3 `insn_u_per_sample`, c13 `insn_k_per_sample`), so the
    instruction floor of a throughput cell is that wide run to run. The gate is validated on the rig.

## 7. Measurement checklist

Before the run
- [ ] Write the reading rules into the harness, including what would falsify the hypothesis, and enforce them in code, not in a comment.
- [ ] Include a control arm the change cannot touch, and at least one arm that must fail.
- [ ] Work out the sample size first. If an event happens with probability p, n runs miss it with probability (1-p)^n. Let the noisier arm set the repetition count.
- [ ] Use the same QoS on every framework and shipped defaults otherwise. Check each arm's build type.
- [ ] Take the rig lock inside the detached job. Make the trap restore `tc`.
- [ ] Never pin a perf_test process to one CPU. Its runner holds a bare test-and-set spinlock across
  `spin_once(100 ms)` and its main thread spins for it every second; on one CPU the spinner runs only while the
  holder sleeps holding the lock, so the stats stall (one 1 s loop took 8.49 s) and the process never exits once
  traffic stops - with any rmw. `rmw_wait_hang_repro.sh` (2026-10-05, rmw_tickle 7b760c5d): on one CPU rmw_tickle hung in
  10 of 10 valid runs (RELIABLE KEEP_ALL and BEST_EFFORT) and an idle CycloneDDS subscriber in 3 of 3, with
  rmw_wait returning every ~100 ms in the hung process; unpinned 0 of 4, two CPUs 0 of 3, one CPU with a fair
  lock in perf_test 0 of 6. Two or more cores, as the rig's `taskset -c 1-3`.

During the run
- [ ] Read the first minute of output.
- [ ] Each arm prints the treatment it actually applied, next to its verdict.

Reading
- [ ] Check the sample count before the value.
- [ ] Check delivery (received equals sent) and the transport witness before any rate.
- [ ] Treat "no answer" (could not look, empty output, a zero instrument) as VOID, never as "no".
- [ ] Decide a TickLE-vs-TickLE rig A/B by gate v2: instructions first, every WORSE beyond the threshold and the placement floor measured in the same run (section 6.1). Decide anything else from the 2xSE result and the control's delta, never from a range against a single point.
- [ ] Decide from measured outcomes, not from configuration or build labels. A compile-time field cannot fail.
- [ ] Before claiming a behaviour, test it against a control. Reading the code is not enough.

Publishing
- [ ] Put results in the documents, not in a message.
- [ ] Every figure carries its provenance: build SHA (after push), rate, arm and repetitions.
- [ ] Correct a wrong published figure in place and state the cause. Do not delete it.
- [ ] Write everything in the repository in English. Never copy CycloneDDS or Fast DDS source.
