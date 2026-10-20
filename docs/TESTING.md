# Testing and measurement

Older text: `git show 829a7d82:docs/TESTING.md`

**TL;DR**
- Before a push: `make check-gates`, read the table, push only if it exits 0. HAL or transport change: also `make test-linux`.
- Throughput is goodput; latency runs from the sample's creation time. TickLE WINs a cell only outside every vendor's spread.
- On the rig: take `rig_lock.sh` inside a detached job; never run TickLE in the PC's default netns.
- A TickLE-vs-TickLE rig A/B decides by gate v2 (section 6.1).

Run from the repository root. Figures: [RESULTS.md](RESULTS.md).

## 1. Tests

| Command | What it runs | Needs |
|---|---|---|
| `make test` | `tests/test_*.c`, mostly on a mock HAL | C compiler |
| `make coverage` | line/branch report in `platform/linux/obj/coverage/report/` (not enforced) | gcovr |
| `make sanitize` / `make tsan` | unit tests under ASan+UBSan / TSan | |
| `make fuzz FUZZ_ARGS='-max_total_time=60'` | packet-parser fuzzing; `make fuzz-corpus` after a wire change | clang |
| `make headers-cpp` | public headers compile as C and C++ | |
| `tests/version_mix.sh [PARENT]` | a `tt_VERSION` 13 core and this tree's refuse each other's announce, each applying its own (control) | C compiler, git |
| `make test-samehost` | two nodes on one host, plus an io_uring-refused pass | |
| `make test-linux` | two nodes in two netns over a veth pair | `sudo ip` |
| `make test-freertos` | HAL selftest, two contexts in one image over the segment (4 arms), then round trips under QEMU | RISC-V toolchain, QEMU |
| `make test-all` | `test headers-cpp check-doc-shas check-rig-lock check-bench-shapes test-samehost test-linux test-freertos` | all of the above |
| `make test-typesupport` / `make regen` | generator pytest / regenerate codecs (CI fails on a diff) | Python |
| `make test-rmw` / `make test-rmw-behaviour` | rmw ctest suite in a netns / plus CI's interface checks | ROS 2 workspace (`RMW_TEST_WS`, `~/rmw_accept_dev`, `~/rmw_accept_ws`) |
| `rmw_tickle/scripts/rmw_gap_acceptance.sh -w ~/rmw_accept_ws` | acceptance; `rmw_cyclonedds_cpp` first as control | ROS 2 |
| `make createns` ... `make deletens` | manual netns runs (`platform/linux/netns.mk`) | `sudo ip` |

FreeRTOS or `include/tickle/hal.h` change: also `make -C platform/freertos lint`. Upstream conformance: CI only.

## 2. Gates and CI

`make check-gates` runs these rows in parallel: `lint (clang-format + clang-tidy)`, `lint-shell`, `check-doc-shas`,
`check-rig-lock`, `check-bench-shapes`, `check-unsupported-list`, `check-context-reset`, `check-results-provenance`,
`test (unit)`, `tsan (thread safety)`, `sanitize (ASan + UBSan, as CI)`, `test-typesupport (pytest)`,
`lint-freertos-hal (as CI)`, `lint-rmw`, `rmw suite (as CI)`, `build configs (syntax)`, `build freertos (link, as CI)`.
`DOCS_ONLY=1` runs only the document rows when only documents changed.

```
/tmp/lintenv/bin/clang-format -i <files> → stage <paths> → make check-gates → read the table → push only on exit 0
```

- clang 19 only (the system clang disagrees): `python3 -m venv /tmp/lintenv && /tmp/lintenv/bin/pip install clang-format==19.1.0 clang-tidy==19.1.0`.
- Read by shape: `grep -E '^[A-Z]{4}  '`, count rows and non-PASS. `SKIP` and `ADVS` (non-19 clang) are not PASS.
- Never chain the push after the gates with `&&`.

Not covered: `make test-linux`; CI's interface set; C++17 for ROS `.cpp` (here 20); `tt_CONTEXT_ID_CLAIM` and
`tt_LOCAL_DELIVERY` code (`clang-tidy -p build/rmw_tickle`).

CI: `test-all.yml` (tests) and `check-all.yml` (lint, codecs, rmw) on every push and PR; `performance.yml` (the Pis)
and `rmw-perf.yml` (`tickle-perf`) by hand. After every push, check **every** workflow.

## 3. The rig

- Two Raspberry Pi 5s (rpi#1 client, rpi#2 server), data on `eth0` `192.168.10.0/24`, SSH over 10.1.1.x. Loss and
  delay: `tc netem` on the client's egress. Runner setup: `git show 3c0c505b:.github/scripts/README.md`.
- **Lock:** `examples/perf_hil/rig_lock.sh`, taken **inside** the detached job. `RIG_LOCK_SCOPE=hil` (default, the
  Pis) or `box` (the lab PC); `RIG_LOCK_WAIT=0` fails at once. Test it by taking it; the file only names the last holder.
- **PC:** never run TickLE in the default netns (it reaches the rig over 10.1.1.x). A private netns needs a default
  route. No netns acceptance suite during a campaign. `sudo -n` allows only `ip` and `tc`.
- **Detach:** `setsid nohup ./script.sh > /path/outside/the/session.log 2>&1 < /dev/null &`, script in the repo.
  Watch the output file, not `$!` (setsid's PID). One `EXIT` trap per script, restoring `tc`. Estimate, check at
  1.5x, read the first minute.

## 4. Scoring

Core against core (native APIs) and rmw against rmw, against FastDDS and CycloneDDS. zenoh-pico: reference, never scored.

| Rule | What it says |
|---|---|
| Same QoS | Every QoS value explicit (`-N`, `-B 100`, `-K 64`); a `RESULT:` line not echoing them is VOID. Everything else at shipped defaults; a tuned arm sits beside, never instead. |
| Same setup | Same Pis and session, frameworks interleaved, no pinning, data on `eth0`, release builds, 3 s drain cap. P1-P4 = 76, 1292, 1424, 2800 B (`sample + base framing <= 1514`, `check_bench_shapes.sh`). |
| Instruments | CPU `getrusage` per sample/MB, RSS `VmHWM`, wire `/proc/net/dev`. |
| Proven transport | Loopback packets (~0/sample shared memory, ~2 kernel) agree with `tx_shm`; packets/sample match the design; `assert_loaded_rmw_tickle.sh` checks the mapped library. |
| Goodput | Throughput = payload of samples the application got complete and unique in the window; partial = 0; send rate is diagnostic; CPU and wire per delivered sample (`campaign_summary.py` G1-G6). |
| Incomplete delivery | RELIABLE: received vs sent, every repetition. Vendor short = DELIVERY FAILED, excluded. TickLE short = every metric in the cell LOSE. BEST_EFFORT: goodput alone. |
| Creation latency | Arrival minus creation time (scheduled publish time, or a ping's due time); send-stamp latency is diagnostic. Percentiles over offered ticks; a lost tick is +inf (UNBOUNDED; two UNBOUNDED draw). Saturating arms with windows >1 s apart: NOT COMPARABLE. |
| R4 control | A ping-pong must read alike on both definitions: median gap g <= 0.2 ms, no gap < -0.02 ms, d = g within noise + print resolution (1 us; 0.1 us when every rep prints `rtt_p50_us`/`crt_p50_us`); noise = range of reps' stamp p50s plus range of creation p50s. Failed = NOT READ (`creation_latency_summary.py`, R0-R5). |
| WIN / DRAW / LOSE | WIN only when TickLE beats every scored vendor outside the reps' ranges; inside = DRAW; worse outside = LOSE. |
| VOID | Leftover guard fired, no `RESULT:`, an instrument read zero, wrong QoS echo, missing `drained=acked`, `window=fail`. VOID is never zero. |
| Warm-up | Same edges for all. Latency: 4096 round trips per edge, 1 ms apart, cut at 20 s. Throughput: 2 s per edge. rmw ping waits for one answered warm-up ping (`tickle/common/BenchWindow.h`). |

**Reading.** Every campaign has a control the change cannot touch; compare deltas, and a figure with its own previous
value, within one session. Pre-registered metrics decide (Bonferroni); a secondary beyond 2xSE is a candidate until
repeated. `REPS` from `reps_needed.py`. Under ~1% throughput/CPU or ~10 KB RSS between builds is layout noise.

## 5. Principles: the setup must not favour TickLE

1. A lead a vendor can dismiss is not a lead. A doubtful setting takes the DDS default first (BEST_EFFORT: `-K 1`); vendor benches use their APIs as documented.
2. Run software as users do: shipped defaults, nothing tuned or pinned (`PIN`, `PIN_SERVER`, `PIN_CLIENT`: labelled notes only).
3. A change to the environment is a labelled arm on every framework, beside the headline.
4. Symmetric is not neutral: ask what a setting does to each architecture (threads, buffers, batching).
5. P1-P4 all stay and are all reported; no claim rests on P3 alone.
6. Measure what is delivered (goodput).
7. Report everything, losses and VOIDs included; correct in place with the cause.
8. Constants are not fitted to the rig; where one stays, its formula is in [RMW.md](RMW.md#options-you-may-tune).
9. No figure is a start-up or teardown transient (warm-up, section 4).

## 6. Harnesses

In `examples/perf_hil/experiments/`; each header holds its pre-registered reading.

| Harness | Answers |
|---|---|
| `campaign_sweep.sh` / `campaign_summary.py` | cross-vendor campaign / its verdicts (`--selftest`) |
| `campaign_ab_chain.sh`, `ab_samehost.sh` (`WITNESS=`), `rmw_gate_v2_chain.sh` | gate v2 A/B: cross-host, same-host, rmw |
| `rig_preflight.sh` / `rig_dryrun.sh` | every cell shape runs (PC, ~70 s) / a harness end to end against stand-in Pis |
| `s6_transport_cells.sh`, `rmw_samehost.sh`, `rmw_crosshost_rtt.sh`, `rmw_keepall_rig.sh` | same-host; rmw same-host; rmw RTT; rmw KEEP_ALL (`CREATION_TRACE=1`) |
| `core_cost_ab.sh` / `core_cost_pi_drift.sh` | core CPU per sample on the PC / the Pi bench's A/A floor |
| `l4_rtt_rig.sh`, `stepb_rtt_rig.sh` | rmw large messages by round trip on one clock: cross-host L4 / same-host step B, with each process's own CPU over the schedule (no perf_test) |

Also `.github/scripts/compare_rmw_perf.sh` (rmw vs both vendors, `box` lock). Never pin perf_test to one CPU: it hangs
with any rmw (`rmw_wait_hang_repro.sh`). perf_test's subscriber CPU includes its AnalyzeRunner busy-waiting about one
period a second (`bcf8624d`): at 30 Hz it swamps a small cell's rmw cost, so a CPU control is not read from it.

### 6.1 Gate v2: TickLE-vs-TickLE rig A/B

Definitions `gate_v2_lib.sh`, reading `gate_v2.py`; `GATE=v1` repeats an old run by the old rule.

- **A1, layout-stable builds:** `-falign-functions=64 -falign-loops=1 -falign-jumps=1 -falign-labels=1`, plus
  `-mno-fix-cortex-a53-843419` on aarch64 (`TICKLE_LAYOUT_CFLAGS`, default when `TICKLE_EXTRA_CFLAGS` is unset).
  `build_flags_check.sh` reads each build log; a build missing a flag is not measured. Not the product default.
- **A2, instructions decide:** `perf stat -e instructions:u,instructions:k` per sample or round trip, client and
  server. CPU, rate and latency are witnesses, net of the floor. No counts = VOID.
- **A3, placement floor:** arm P is A rebuilt with `-falign-functions=128`. A WORSE must exceed the Bonferroni Welch-t
  threshold **and** `|P - A|` for that metric and cell. No P arm, or a named metric unmeasured = NO VERDICT.
- Throughput-cell counts move with timing (P's floor absorbs it); a latency-cell instruction WORSE is real work.
- Validated on the rig at `5263213e` (A vs P, A vs A at 256: 0 of 88 rows WORSE each; `rig_queue_gate_xval.sh`).
