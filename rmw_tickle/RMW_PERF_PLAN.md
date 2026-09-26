# rmw_tickle performance: where the ~0.08 ms deficit to CycloneDDS lives

2026-09-26. The user's instruction: make TickLE core win p1-p4 with DATA_FRAG (done, COMPARISON.MD
section 1), look for further core optimisation, then optimise `rmw_tickle`. This file is the `rmw_tickle`
step. It is Plan's, as research and measurement; the code it points at is Dev's.

## 1. The deficit, measured

Cross-host through `rclcpp`, ping on one Pi and pong on the other, all three `rmw` implementations on the
eth0 link only, identity-checked per row (`experiments/rmw_crosshost_rtt.sh`, `results/rmw_crosshost_2026-09-26.txt`,
36 rows, 0 void). Round-trip mean, median of 3:

| message / QoS | rmw_tickle | rmw_fastrtps_cpp | rmw_cyclonedds_cpp |
|---|---:|---:|---:|
| Bench, best_effort | 0.508 | 0.472 | **0.425** |
| Bench, reliable | 0.504 | 0.490 | **0.432** |
| Array1k, best_effort | 0.515 | 0.511 | **0.448** |
| Array1k, reliable | 0.506 | 0.523 | **0.443** |

TickLE's *core* wins the same round trip natively: 0.207 ms against CycloneDDS's 0.259 (COMPARISON.MD row 1).
So `rmw_tickle`'s layer adds **~0.30 ms** where CycloneDDS's adds **~0.17**. The deficit is ~0.08 ms per round
trip, ~40 us per direction, and it sits inside `rmw_tickle`, not in the protocol.

## 2. What has been ruled out

**CPU frequency (DVFS). Ruled out by a pre-registered test.** Each repetition was run with and without a
nice-19 spinner on core 0 of both Pis, which holds the shared cpufreq policy at 2.4 GHz
(`SPIN_ARMS="off on"`, `results/rmw_spin_2026-09-26.txt`, 36 rows, 0 void, every "on" row at 2400000 kHz on
both Pis). The TickLE - CycloneDDS gap went from +79 to **+91 us** (best_effort) and from +68 to **+83 us**
(reliable). The pre-registered rule was that a change within 20 us means not DVFS. If anything, the clock
costs the vendors more (a pinned clock sped FastDDS by 59 us and CycloneDDS by 35 us, TickLE by 15-23 us),
which makes TickLE's deficit structural.

## 3. What the traces show (structure only)

Each pong run under `strace -f -tt -T` (`TRACE=1`, `results/rmw_trace_2026-09-26/`). strace slows every
syscall, so none of its times are figures. What it shows is the shape of the path.

| per ping, in the pong | rmw_tickle | rmw_fastrtps | rmw_cyclonedds |
|---|---:|---:|---:|
| syscalls | **10.7** | 8.0 | 7.6 |
| receives | **2.4** | 1.3 | 1.2 |
| waits (ppoll / sleeps) | **3.4** | 0.0 | 0.4 |
| futex | 3.8 | 4.3 | 4.9 |
| threads making syscalls | 4 | 9 | 8 |

- **All three hand a received ping to the executor thread exactly once** (one futex wake). The executor
  half, from wake to reply send, has the same shape in all three.
- **The receive half differs.** CycloneDDS's receive thread sits in a blocking `recvmsg` and wakes with the
  data. TickLE's poll thread wakes from `ppoll`, calls `recvfrom`, runs core and `rmw` delivery, and then
  wakes the executor. After the wake it drains once more (a `recvfrom` returning EAGAIN, which is the "n+1
  per drain" that `recvmmsg` removes) and re-enters `ppoll`.
- **In the traced ping, the executor's reply `sendto` goes out just after the poll thread re-enters
  `ppoll`.** That is one sample, under strace, and it proves nothing on its own. It is the observation
  behind hypothesis H1.
- A 50 ms `clock_nanosleep` loop on one thread (2 per ping at 10 Hz) is off the critical path, but it
  costs idle CPU.

## 4. Hypotheses, to be tested in this order

- **H1: the executor's publish waits for the node state lock while the poll thread finishes its
  post-delivery drain.** The core serialises `tt_Publisher_publish()` and the poll thread's processing on
  one owner-tracked lock per node. The drain after a delivery is exactly the window where the executor
  wants to publish the reply.
  - *Test:* the core's existing lock counters (acquisitions / contended / ns waited, the ones
    `test_thread_safety` prints), exported from the pong at exit, with and without traffic.
  - *Confirms:* contended acquisitions per round trip of about 1, with ns waited per round trip that
    accounts for a good part of 40 us.
  - *Refutes:* contention near zero.
- **H2: per-message work on the poll thread before the wake** (decode, `rmw` delivery, queueing) is
  heavier than CycloneDDS's.
  - *Test:* in-process `CLOCK_MONOTONIC` stamps in the pong at ppoll return, `recvfrom` return, core
    delivery, `rmw` enqueue and signal, executor wake, `rmw_take` return, `rmw_publish` entry and
    `sendto` return. Logged to a ring buffer and dumped at exit.
- **H3: the extra receive per drain.** It is on the path only if H1 holds, since the EAGAIN `recvfrom`
  is part of the drain the executor waits behind. `recvmmsg` (Dev, in progress) removes it either way.
  **Measure `recvmmsg`'s effect on these rows, not only on throughput.**

The instrumentation for H1/H2 is in `rmw_tickle` and core, so it is Dev's to build. The measurement is
Plan's, with the rows above as its baseline and with rows re-run on each change.

## 5. The H1-H3 session, pre-registered before it runs

Dev's instrumentation (`67829a5a`): `-DRMW_TICKLE_TRACE=ON` builds eight latency stamps into the receive-to-reply
path, and the node lock counters are split into poll-thread and other-thread waits in every build. The pong
writes both at shutdown when `RMW_TICKLE_TRACE_FILE` is set. `rmw_crosshost_rtt.sh TICKLE_VARIANTS="default trace
trace_rxb8"` measures three rmw_tickle builds in one session, interleaved with CycloneDDS and FastDDS: default,
trace, and trace with `tt_RX_BATCH=8`. Bench, both QoS, 3 repetitions. Each variant pong's `/proc/PID/maps` must
show that variant's `librmw_tickle.so`, or the row is VOID.

How each reading will be taken:
- **The stamps' own cost:** trace against default RTT. Under 10 us is expected. More than that means every
  H2 segment is inflated by it, and has to be read with it subtracted.
- **H1 (the executor waits on the poll thread's lock):** `contended - poller_contended` and its `wait_ns`,
  per round trip. **Confirmed** at about one contended acquisition per round trip with a wait that is a
  real share of ~40 us. **Refuted** at about zero. On x86 veth Dev measured 0 of 1,673, so the rig has to
  show it or it is gone.
- **H2 (where the time goes):** segment medians from `rmw_trace_segments.py` on the rig. On x86 veth the
  largest was signaled → exec_wake, the executor's condvar wake, at 17 us of 50. The question is which
  segment carries the rig's extra ~40 us per direction against CycloneDDS. CycloneDDS cannot be stamped
  the same way, so the reading is by elimination: a segment that is large and has no counterpart in
  CycloneDDS's path (section 3) is the candidate.
- **H3 (the extra receive per drain):** trace_rxb8 against trace RTT. A drop of more than 10 us means it
  matters. Within 10 us means it does not.

## 6. The H1-H3 session: all three refuted, and the time is somewhere else (2026-09-26)

`results/rmw_h123_2026-09-26/` (rows plus the rep-2 dumps; the full set was 18 dumps), build `9e989307`,
30 rows and 0 void. Each variant's pong mapped its own library.

| Bench, median of 3 | RTT best_effort | RTT reliable | pong CPU (ns-summed, whole run) | pong peak RSS |
|---|---:|---:|---:|---:|
| rmw_tickle default | 0.504 ms | 0.506 ms | **34.4 ms** | **11,760 KB** |
| rmw_tickle trace | 0.510 | 0.514 | 34.4 | 11,788 |
| rmw_tickle trace + RX_BATCH 8 | 0.504 | 0.508 | 34.7 | 12,172 |
| rmw_fastrtps_cpp | 0.476 | 0.486 | 56.0 | 23,620 |
| rmw_cyclonedds_cpp | **0.426** | **0.429** | 42.8 | 14,536 |

Read against section 5's pre-registration:
- **The stamps' own cost is +6 / +8 us**, under the 10 us threshold, so the segments can be read as they
  are.
- **H1 is refuted.** The pong's node lock was contended 0-4 times in ~950 acquisitions per run, which is
  0.01-0.04 per round trip against a threshold of about 1. The executor does not wait on the poll thread.
- **H3 is refuted.** RX_BATCH 8 against trace is -6 us, within the 10 us threshold.
- **H2, the pong's own path from poll wake to reply sent, is a median 32 us:**
  - publish → tx_done 12.3 us
  - signaled → exec_wake 7.8 us
  - exec_wake → taken 3.1 us
  - deliver → signaled 3.1 us
  - rx_wake → rx_datagram 2.7 us
  - rx_datagram → deliver 0.7 us
  - taken → publish 0.8 us

  No single segment is large.
- **New and unambiguous: CPU.** With the pong's thread run times summed from `schedstat` in
  nanoseconds, rmw_tickle uses the least CPU of the three: 34.4 ms against CycloneDDS's 42.8-47.4 and
  FastDDS's 56.0-59.5. The 10 ms tick could not show this.

**The accounting does not close, and that is the finding.**
- rmw_tickle adds about 300 us to TickLE's native round trip (0.207 → 0.505 ms). CycloneDDS's `rmw` adds
  about 167 us to its own (0.259 → 0.426).
- The pong's whole stamped path is 32 us. The ping's equivalent halves, publish and receive-to-take, are
  of the same order, so everything stamped comes to roughly 60 us per round trip.
- **About 240 us per round trip lies outside every stamped segment.** The only unstamped stretches are
  before `rx_wake` (packet arrival to the poll thread returning from `ppoll`), after `tx_done` (to the
  wire), and the network itself.

**Next (Plan, no code change needed): packet timestamps on both hosts.** `sudo tcpdump` is available on
the rig. On the pong host, the time from the ping's arrival to the reply's departure is the pong's
whole turnaround, kernel wake included, on one clock. It is framework-neutral, so the same number exists
for CycloneDDS and FastDDS, and for the native TickLE harness. On the ping host, application RTT
minus on-wire RTT is the ping side's overhead. Together they split the round trip into ping side, wire
and pong side for all three `rmw` implementations and the native harness. Aligning the pong's pcap
times with its `rx_wake` stamps then measures arrival → poll wake directly.
