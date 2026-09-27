# rmw_tickle performance: where the ~0.08 ms deficit to CycloneDDS lives

> **Names (2026-09-27):** `tt_Node` is now `tt_Context`, `tt_NODE_*` is `tt_CONTEXT_*` and a public `node_id` field is `context_id`
> (rmw_tickle/CONTEXT_NODE_PLAN.md stage 1, 25ac7fe0). This document keeps the names of its time.

2026-09-26. The user's instruction: make TickLE core win p1-p4 with DATA_FRAG (done, COMPARISON.md
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

TickLE's *core* wins the same round trip natively: 0.207 ms against CycloneDDS's 0.259 (COMPARISON.md row 1).
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

## 7. The missing time was the ping's wait loop, and block mode reverses the ranking (2026-09-26)

Dev found on veth (`087c52d6`, `experiments/rmw_ping_wait_mode.sh`) that the ping's default wait accounts
for the missing time. The default wait, `--wait poll`, is `spin_some()` plus a 100 us sleep, with the
RTT read after the loop. So every row included the sleep that followed the spin that took the reply,
quantised to the loop's period. `--wait block` waits in `spin_once()` and reads the RTT in the callback,
as the native client waits in `tt_Node_poll()`. The rig session (`rmw_crosshost_rtt.sh`
`WAITS="poll block"`, build `92a27ffa`, Bench, 3 repetitions, 36 rows, 0 void) confirms it. Median
mean RTT:

| Bench | rmw_tickle | rmw_fastrtps_cpp | rmw_cyclonedds_cpp |
|---|---:|---:|---:|
| block, BEST_EFFORT | **0.253 ms** | 0.321 | 0.267 |
| block, RELIABLE | **0.255** | 0.328 | 0.269 |
| poll, BEST_EFFORT | 0.504 | 0.477 | **0.427** |
| poll, RELIABLE | 0.514 | 0.483 | **0.431** |

- **In block mode rmw_tickle has the lowest RTT of the three.** Its minimum is also the lowest: 0.232
  against 0.252 and 0.297 ms.
- **The poll loop costs rmw_tickle ~250 us and the vendors ~160 us.** So the old deficit came from how
  each rmw interacts with a polling loop, not from the blocking receive path. Real `rclcpp`
  applications spin, which is block's behaviour.
- The packet split that would localise the poll-mode difference is queued. The first capture session
  was void: a stale file had been copied into every row, which `7e15fc43` fixed. `f386f2e7` keeps
  per-row logs.

### 7.1 Source comparison with the vendors' rmw (user's direction, 2026-09-26)

The versions compared are the ones installed on the rig: rmw_cyclonedds 2.2.3 over Cyclone DDS 0.10.5,
rmw_fastrtps 8.4.4 over Fast DDS 2.14.6, and rclcpp 28.1.21. The question was what the vendors' rmw
does that rmw_tickle does not. **Structurally, the three have the same shape**:

| step | rmw_cyclonedds | rmw_fastrtps | rmw_tickle |
|---|---|---|---|
| publish | `dds_write_ts` inline in the caller (`rmw_node.cpp:1951`) | synchronous by default (`participant.cpp:278`) | encode and send inline in the caller (`rmw_publish`) |
| receive thread | `recvUC` blocks in `recvmsg` on the data socket; discovery is on a separate `recv` thread (`q_init.c` `setup_and_start_recv_threads`, `q_receive.c` `recv_thread`) | transport listening thread | poll thread in `ppoll`, then `recvfrom` |
| delivery | synchronous on the receive thread by default (`ddsi_proxy_endpoint.c:223`: latency_budget 0 <= bound inf, priority 0 >= 0) | on the receive thread into the history | synchronous on the poll thread (`subscriber_callback`) |
| conversion | one deserialisation, in `dds_take` on the executor thread | one deserialisation at take | two on the poll thread (CDR → TickLE struct → ROS message), then a C++ move at take |
| `rmw_wait(0)` | cached waitset; re-attaches only when the set changes (`rmw_node.cpp:4361`) | attach/detach every call unless something has already triggered (`rmw_wait.cpp:120`) | mutex plus flag checks, no syscall (`rmw_wait_set.c:245`) |

There is one handoff between threads in every case: receive thread to executor. The differences found
are small and are listed as measurable candidates, not as explanations:

1. **Receive syscalls per wake: 2 against 1.** Cyclone's data thread is woken by `recvmsg` returning,
   while rmw_tickle's is woken by `ppoll` and then reads. rx_wake → rx_datagram was 2.7 us in the pong
   trace, so this bounds at a few microseconds per receive.
2. **Two conversions before the wake, against one after it.** The serial path has the same total length,
   and for Bench (76 B) it is about 1 us.
3. **Graph guard condition on every announce** (`rmw_node.c` `discovery_callback`, via core's upsert at
   `tickle.c:1195`). Core calls the discovery callback for every announced entity on every announce, 1 s
   apart, changed or not, and each call broadcasts `wait_cond`. `rclcpp` only starts its graph listener
   when a graph event is requested (`node_graph.cpp:607`), which ping/pong never do. So the cost here is a
   spurious wake of a blocked `rmw_wait` about once a second per peer entity, not latency. It is still
   worth fixing: fire only on a real change, as Cyclone does, where lease renewals produce no sample.
4. **FastDDS's `rmw_wait` attach/detach churn** is the one clear inefficiency among the three. It is
   consistent with FastDDS being slowest in block mode.

**What the source does not explain is the poll-mode difference** (~250 against ~160 us), because
`rmw_wait(0)` is cheap in both rmw_tickle and Cyclone. It is measured, not argued: the queued captures
give ping_send, pong_turn, wire and ping_recv per rmw and mode, with wire as the cross-rmw control.

## 8. Both wait modes, and each path split at the kernel boundary (pre-registered 2026-09-26, before the run)

**User decisions (2026-09-26):**
- Both wait modes are test cases. An application can wait either way (a `spin_some()` loop is poll;
  `spin()` or `spin_once(timeout)` is block), so both are scored, as separate rows.
- For all three rmw implementations, measure precisely the path from the data's creation out to the
  kernel, and the path from the kernel in to the application. That locates rmw_tickle's latency.

**Findings since section 7:**
- **Poll mode is the loop's grid, not a cost in rmw_tickle** (Dev, `274c7b8e`, veth). rmw_tickle's
  `spin_some()` takes 5.5 us against the vendors' 41-46 us. Its cycle (~165 us) is shorter, so on the
  rig its reply is caught one iteration later. The rig rows now carry the `LOOP:` line (`f5e99582`), and
  the prediction is iterations_per_rtt ≈ 3 for rmw_tickle and ≈ 2 for the vendors.
- **rmw_tickle's ping broadcast every sample in 4 of 7 runs.** It was a peer-registration race, not the
  wait mode:
  - in those runs the ping's publisher never registered the pong's subscriber (`rx_self_sent_data=100`,
    no "Publisher peer registered" line);
  - in the other 3 it registered at once;
  - no leftover processes: `pong_procs=1` and `ping_procs_before=0` on every row.

  Broadcasting costs the ping ~9 us of send time (20.1 against 11.5 us, tap split). Handed to Dev.
- **Tap split, block mode, BEST_EFFORT rep 1 (mean us):**

  | rmw | ping_send | pong_turn | ping_recv | wire (control) |
  |---|---:|---:|---:|---:|
  | rmw_tickle | 11.5 | 43.2 | 31.2 | 168.1 |
  | rmw_cyclonedds_cpp | 20.5 | 55.1 | 43.1 | 169.3 |
  | rmw_fastrtps_cpp | 30.1 | 73.0 | 51.5 | 171.5 |

  rmw_tickle is lowest in every segment. The wire control agrees within 3.4 us, except CycloneDDS
  RELIABLE at ~152 us, 16 us below the others. That misses the 10 us pre-registration and is reported as
  it is. It is a candidate for interrupt coalescing (Cyclone's RELIABLE sends a second packet close
  behind the data), not yet tested. The wire is ~2/3 of every round trip on this rig, the same for all
  three.

**The session (`f4e8e35f`):**
- Bench, CAPTURE=1, `WAITS="poll block"`, `SYSSTAMP_ARMS="off on"`, 2 repetitions, all three rmws,
  interleaved.
- `experiments/sysstamp` (LD_PRELOAD) timestamps every socket and wait call on the capture's clock.
- Per sample, `rmw_pcap_split.py` gives:
  - ping: app→send entry (user tx) and send entry→tap (kernel tx);
  - pong: tap→wake, tap→recv return (kernel rx + wake), recv→send (user: rmw rx, executor, callback,
    rmw tx) and send→tap;
  - ping: tap→wake, tap→recv return, and recv→app as a mean.

**How to read it, written before running:**
- **CONTROL 1, the layer's own cost.** In block mode, the on - off difference in mean RTT must be
  <= 10 us for every rmw. The grid makes poll mode unusable for this check. If the difference is
  larger, the kernel-boundary segments are reported with that cost stated, and differences between
  rmws smaller than it are not read.
- **CONTROL 2.** Wire agrees across rmws as before.
- **The reading.** Take a segment where rmw_tickle is slower than the best vendor by more than 5 us:
  - if it is a user segment (app→send, pong recv→send, recv→app), it is rmw_tickle or core code;
  - if it is a kernel segment (send→tap, tap→recv), it is the socket usage: broadcast against unicast,
    ppoll+recvfrom against a blocking recvmsg, and socket options.
- If no segment exceeds the best vendor by more than 5 us in block mode, the block-mode comparison has
  no path left to fix, and the work goes to poll mode's grid and to the broadcast race.

### 8.1 Result, before the broadcast fix (`9a48b2be`, 2026-09-26)

Bench, 2 repetitions × BEST_EFFORT/RELIABLE × poll/block × sysstamp off/on × 3 rmws: 48 rows, 0 void.
The rows and split are in `results/rmw_sysstamp_2026-09-26/`; the pcaps and records stay in `/tmp`.
Figures are means over the 4 rows of each cell, in us.

**CONTROL 1 passes.** In block mode, the RTT on - off is 0.0 for rmw_tickle, +5.5 for CycloneDDS and +8.3
for FastDDS, all within 10 us. pong_turn on - off is +0.8, +1.9 and +7.5. The first row's apparent
+16 us for rmw_tickle was a single poll-mode row, and poll mode cannot judge this.

**CONTROL 2 as before.** The wire is 168 us for rmw_tickle and 172 for FastDDS. For CycloneDDS it is 160,
and lower in RELIABLE (see section 8).

**The poll-mode grid, confirmed on the rig** (`LOOP:`):

| rmw | iterations per RTT | spin_some | real sleep |
|---|---:|---:|---:|
| rmw_tickle | 3.01 | 6.4 us | 156.8 us |
| rmw_cyclonedds_cpp | 2.03 | 46.5 | 156.4 |
| rmw_fastrtps_cpp | 2.03 | 64.8 | 158.2 |

This is exactly the pre-registered 3 against 2. rmw_tickle's `spin_some()` is 7-10 times cheaper, so its
cycle (~163 us) is shorter than a true round trip (~250 us) by less than the vendors' (~203 us). As a
result its reply is caught a whole cycle later. Poll-mode RTT therefore depends on where each rmw's true
RTT falls against a 100 us sleep chosen by the test program. A single sleep value is an arbitrary grid,
not a property of the rmw.

**The kernel-boundary split, block mode, sysstamp on:**

| segment | rmw_tickle | CycloneDDS | FastDDS | kind |
|---|---:|---:|---:|---|
| ping app → send entry | **3.5** | 11.3 | 22.3 | user |
| ping send entry → tap | **8.4** | 10.0 | 10.1 | kernel |
| pong tap → ppoll return | 13.8 | - | - | kernel |
| pong tap → recv return | 18.2 | **12.6** | 14.8 | kernel |
| pong recv → send entry | **22.9** | 36.7 | 62.4 | user |
| pong send entry → tap | **9.3** | 10.9 | 10.4 | kernel |
| ping tap → ppoll return | 13.3 | - | - | kernel |
| ping tap → recv return | 17.9 | 17.5 | **13.4** | kernel |
| ping recv → app (mean) | **18.4** | 26.8 | 40.9 | user |

Read against the pre-registration:
- **rmw_tickle is fastest in every user segment**, ahead of the best vendor by 7.8-13.8 us.
- **The only segment where it is more than 5 us behind the best vendor is the pong's kernel receive:**
  18.2 against CycloneDDS's 12.6, so 5.6 us behind. That is a kernel segment, so it is socket usage.
  - The vendors' data threads block in `recvmsg` and return with the datagram. rmw_tickle's poll thread
    returns from `ppoll` (13.8 us after the tap, already slower than CycloneDDS's whole receive) and
    then calls `recvfrom` (+4.4 us).
  - On the ping side rmw_tickle equals CycloneDDS (17.9 against 17.5) and is 4.5 us behind FastDDS, which
    is under the threshold.
- The pong's receive thread and the thread that sends the reply are different threads in every sample,
  for all three rmws. So the handoff to the executor is not a difference between them.

**What this says to do next:**
1. For rmw_tickle, the receive wake-up: `ppoll` over the well-known and data sockets, then a read.
   CycloneDDS gives its data socket a thread of its own that blocks in `recvmsg`. That is a design
   question for Dev, and bpftrace (installed under the rig lock) will show where inside the kernel the
   extra time goes: IRQ, softirq, socket enqueue, or wake-up of a `ppoll` waiter against a `recvmsg` waiter.
2. For the poll-mode test case, a sleep sweep instead of one arbitrary value: busy (0), 50, 100 and
   200 us. That makes the result about the rmw rather than about one grid.
3. The NIC's interrupt coalescing (`macb` rx-usecs/tx-usecs 49) is a candidate for much of the 168 us
   wire. It affects all three alike and is a rig setting, so it is the user's call and is not changed here.

### 8.2 After the broadcast fix, with application stamps (`9d89f78f`, 2026-09-26)

The same session shape as 8.1, with `--stamps` on every row: 48 rows, 0 void. The rows and split are in
`results/rmw_sysstamp_fix_2026-09-26/`.

**CONTROL 1 fails for CycloneDDS in this session.** Block-mode RTT on - off is +7.6 us for rmw_tickle,
+10.2 for FastDDS and **+29.8 for CycloneDDS**. CycloneDDS's pong_turn went from 58.9 to 87.5 us with
the layer on. So, per the pre-registration, this session's sysstamp-on segments are not read for
CycloneDDS; FastDDS is at the threshold. The cause is not known. In 8.1, without `--stamps`, the same
check was +5.5.

**The rows without the layer are valid for all three**, and they are framework-neutral: pcap taps plus
each process's own stamps. Block mode, mean of 4 rows, us:

| segment | rmw_tickle | CycloneDDS | FastDDS |
|---|---:|---:|---:|
| ping: app → tap (send) | **12.1** | 21.8 | 31.5 |
| pong: tap → callback (kernel rx, rmw rx, executor) | **36.5** | 40.1 | 53.7 |
| pong: callback → tap (app, rmw tx, kernel tx) | **10.8** | 18.8 | 27.1 |
| ping: tap → reply callback (kernel rx, rmw rx) | **33.9** | 43.6 | 51.4 |
| RTT | **260.2** | 282.2 | 333.8 |

**With the broadcast race fixed, rmw_tickle is fastest on every segment of the round trip.** The slower
kernel wake that 8.1 found is still there (tap → recv return 18.9 us, sysstamp on, against CycloneDDS's
12.6 in 8.1). It is more than paid back in user space, so the pong's whole receive side is still the
shortest.

rmw_tickle's own user-space pieces (sysstamp on, which passes control 1 for rmw_tickle):
- pong recv return → callback: 21.1 us. This is the poll thread's delivery, the handoff to the
  executor and the take: the largest remaining piece.
- callback → send entry: 1.9 us.
- send return → publish return: 2.7 us.

**What is left to gain**, in order of size:
1. The poll thread → executor handoff: ~21 us, inside recv → callback.
2. The ppoll wake: ~6 us against a blocking recvmsg. bpftrace is now installed on both Pis, and its
   split (IRQ → NAPI → UDP enqueue → socket wake → sched_waking → switch-in → syscall return) is the
   next measurement.

### 8.3 The poll-mode sleep sweep (`099374d2`, 2026-09-26)

`POLL_SLEEPS="0 50 100 200"`, Bench and Array1k, BEST_EFFORT and RELIABLE, 3 repetitions: 144 rows,
0 void. Every row's `LOOP:` line confirmed its sleep. `results/rmw_pollsweep_2026-09-26.txt`. Median
RTT in ms, rmw_tickle / FastDDS / CycloneDDS:

| sleep | Bench BE | Bench REL | Array1k BE | Array1k REL |
|---|---|---|---|---|
| 0 (busy) | **0.242** / 0.317 / 0.270 | **0.245** / 0.328 / 0.270 | **0.269** / 0.339 / 0.292 | **0.268** / 0.355 / 0.286 |
| 50 us | **0.367** / 0.445 / 0.399 | **0.382** / 0.507 / 0.390 | **0.462** / 0.535 / 0.486 | 0.463 / 0.538 / **0.416** |
| 100 us | 0.505 / 0.471 / **0.431** | 0.507 / 0.490 / **0.436** | 0.516 / 0.503 / **0.450** | 0.510 / 0.528 / **0.442** |
| 200 us | **0.546** / 0.651 / 0.617 | **0.548** / 0.668 / 0.633 | **0.552** / 0.657 / 0.621 | **0.550** / 0.668 / 0.634 |

- **rmw_tickle has the lowest RTT in 11 of 16 cells**, CycloneDDS in 5: all of 100 us, and Array1k
  RELIABLE at 50 us.
- At busy polling (0) the cheapest `spin_some()` wins outright. At 200 us every rmw needs two iterations
  (2.00-2.03), and rmw_tickle's shorter cycle then wins.
- 100 us is the one sleep where rmw_tickle's reply is caught an iteration later (3.02 against 2.01). At
  50 us the counts are closer: 3.15-4.00 against 2.14-3.01.
- Check on a disturbance: at 21:24 a one-second `bpftrace -l` ran on the pong Pi mid-sweep (rep 1, Bench
  RELIABLE, sleeps 0-50). Those rows agree with reps 2 and 3 within their spread.

### 8.4 Executor-driven receive on the rig (pre-registered 2026-09-26, before the run)

Dev's v3 (`d2c1e36f`, default off, `RMW_TICKLE_EXECUTOR_POLL=1`). While an executor waits in `rmw_wait()` it
takes the poller role and calls `tt_Node_poll()` itself. The receive → executor handoff (~21 us of the pong's
path, 8.2) then disappears in the steady state. The poll thread parks on a timerfd lease (10 ms), which is
armed on release and disarmed on re-claim, so there are no per-message thread switches and no extra idle wakes.

PC veth A/B, 5 reps, in Dev's `rmw_executor_poll_ab.sh`:
- At a 100 ms ping gap, RTT 216 → 169 us (-3.6 SE) and pong whole-run CPU 28.7 → 27.4 ms.
- At a 5 ms gap, RTT 100 → 66 us and pong CPU -19%.
- The poll-mode control did not move.
- v1 had failed its own CPU criterion and was not merged.

**Rig A/B:** `rmw_crosshost_rtt.sh` with `EXEC_POLL_ARMS="off on" RMW_LIST=rmw_tickle WAITS="poll block"`, Bench and
Array1k, BEST_EFFORT and RELIABLE, 3 repetitions, the same binaries in both arms. Each row asserts the ping's own
`rmw_tickle: executor_poll=<0|1>` line.

**How to read it:**
- **Pass (default on):** in block mode, the RTT median falls by at least 5 us beyond 2 × SE in every
  message/QoS cell.
- **CPU:** the pong's whole-run CPU (COMPARISON rows 70-71's metric) must not rise beyond 2 × SE in any cell.
- **Control:** the poll-mode rows must not move beyond 2 × SE, since executor polling is not on the
  `spin_some()` path.
- If the pass fails, or the CPU criterion or the control fails, the feature stays off, and the numbers are
  recorded here.

### 8.5 The pong's kernel receive, split with bpftrace (`763125c9`, 2026-09-27)

`pong_rx_split.bt` v2 on the pong Pi. Block mode, Bench, 2 repetitions, bpftrace off/on, 24 rows, 0 void, 200
pings per cell. Rows and raw lines are in `results/rmw_bpf3_2026-09-26/`.

**Caveats first:**
- With bpftrace on, every rmw's RTT rises ~30 us (rmw_tickle 248 → 278, CycloneDDS 270 → 300, FastDDS
  316 → 358). The probes' cost is common to all three, so the segments compare across rmws, but their
  absolute sizes are inflated.
- The receiving thread's own wake was caught in only 4 of 200 bursts. Those fields are not read.

Median us after NAPI handed the frame to the stack, BEST_EFFORT / RELIABLE:

| point | rmw_tickle | CycloneDDS | FastDDS |
|---|---:|---:|---:|
| socket data_ready (`rd`) | 17.9 / 15.0 | 12.9 / 15.7 | 12.6 / 15.6 |
| ppoll returns | 30.0 / 28.6 | - | - |
| receive syscall returns | 35.5 / 34.4 | **24.9 / 26.4** | 25.6 / 28.4 |
| executor switched in | 47.4 / 48.9 | 46.2 / 44.1 | 51.2 / 50.6 |
| reply's send entered | **61.1 / 61.0** | 69.0 / 70.2 | 93.1 / 100.5 |

**Reading:**
- **The wake itself costs the same in all three.** From data_ready to the woken thread's syscall
  returning takes ~12 us: ppoll for rmw_tickle, the blocking receive for the vendors.
- **rmw_tickle's receive lag is the second syscall.** The recvfrom after ppoll adds ~5.5 us. In BEST_EFFORT
  its enqueue is also ~4 us later, which is not explained yet (socket lookup with two sockets on one port is
  a candidate). A persistent epoll set would keep two syscalls, so it is not expected to close the gap. The Pi
  run of `recv_wake_cost.c`'s epoll3 arm tests exactly that.
- **rmw_tickle still sends the reply first:** 61 us after NAPI, against CycloneDDS's 69-70 and FastDDS's
  93-101. Its executor-to-send path (take, callback, publish) is the shortest by ~10-30 us. That is where
  its overall lead comes from.
- **The two remaining levers, with sizes:**
  - the handoff from receive to executor, recv return → executor switched in: ~12-14 us. Executor-driven
    receive removes it; the rig A/B is queued (8.4).
  - the extra receive syscall, ~5.5 us, which only a blocking receive on the data socket removes.

**8.4 result (2026-09-27): passes; the default goes on.** `results/rmw_execpoll2_2026-09-27.txt`: build `5b4fdc84`, 48
rows, 0 void, and no 10.1.1.x peer in any log. The first attempt was set aside: in one row the rig's ping had
registered a PC test node that reached the Pis over the management LAN (`DISCOVERY_PLAN.md` / memory rule), and
the whole session was re-run rather than cherry-picked. Mean difference on - off, 3 reps per arm:

| cell | block RTT (2 SE) | pong CPU, ms (2 SE) | poll RTT, the control (2 SE) |
|---|---|---|---|
| Bench BEST_EFFORT | **-8.3 us** (4.4) | -0.65 (0.81) | -1.0 (3.7) |
| Bench RELIABLE | **-11.0 us** (4.0) | +0.83 (1.20) | -0.3 (3.0) |
| Array1k BEST_EFFORT | **-13.0 us** (2.2) | -1.03 (1.37) | -1.7 (2.7) |
| Array1k RELIABLE | **-7.3 us** (2.5) | -0.08 (1.32) | +0.3 (3.9) |

- Every block cell drops by at least 5 us beyond 2 SE.
- No pong CPU rises beyond 2 SE.
- The poll control does not move.
- **Pass, so the default goes on.**

The prediction ("at least as large in us on the Pi as the PC's -47") **did not hold**. The rig gains -7 to -13 us.
The likely reason is the ping side: at a 100 ms gap it sleeps outside `rmw_wait()` far longer than the 10 ms
lease, so it takes the role back every round trip and only the pong gains fully. That was a prediction, not
a criterion, and it is recorded as missed.

**8.5 addendum: epoll does not close the gap on the Pi (2026-09-27).** `recv_wake_cost.c` with Dev's epoll3 arm, on the
server Pi, 2 passes interleaved (`results/recv_wake_cost_rpi_2026-09-27.txt`). Medians:
- recvfrom 11.22 us, recvmsg 11.26-11.30 (the control pair agrees);
- **epoll3 13.54-13.59 us, +2.35**;
- ppoll3 14.00-14.04, +2.8.

The pre-registered bar was epoll3 within ~0.5 us of recvfrom, and it misses by a wide margin. So a persistent epoll set
is not worth doing. This agrees with the bpftrace split: the cost is the second syscall, not the kind of wait. Only a
blocking receive on the data socket would recover the ~2.8 us, and that needs the well-known socket and the wake
path handled another way. Deferred: ~1% of the round trip for a large change.

Next for the rmw rows, in order:
1. the ping-side executor lease (keep the role across a regular caller gap), guarded on idle CPU and timer latency;
2. further wire versions (W5, W1) wait for the user's ruling on WIRE_PLAN's rule reading.

### 8.6 A lease that follows the caller's cadence (pre-registered 2026-09-27, before code)

**Why.** 8.4's rig gain was -7 to -13 us against the PC's -47 at the same 100 ms gap. The ping sleeps outside
`rmw_wait()` for 100 ms, longer than the fixed 10 ms lease, so its poll thread takes the node back every
round trip and the ping re-claims it after each publish: an interrupt, a wake and a park. Hypothesis: on the
Pi that handover is slow enough that the reply is sometimes already there and is delivered by the poll thread,
through the handoff executor-driven receive exists to remove. The pong never leaves `rmw_wait()` but for its
callback, so it gains in full.

**Change (Dev).**
- The lease a release arms is 1.5 x how long the executor was last away (claim time - previous release),
  clamped to 10-250 ms. A caller with a regular cadence up to ~160 ms keeps the role across its sleep; an
  irregular one falls back towards 10 ms.
- While the executor is away within its lease, the parked poll thread wakes only at core's next scheduler
  deadline (new `tt_Node_next_due()`) and runs one non-blocking `tt_Node_poll(0)`: due entries on time, and
  whatever arrived meanwhile drained. It never waits in `ppoll`, so the executor's re-claim finds the role
  free and costs no thread switch.
- Any `rmw_wait()` may still claim the role from an executor that is away (it is not polling), so a
  MultiThreadedExecutor is never held up by another thread's lease.
- Diagnostic: the shutdown line adds `delivered_by_executor=` and `delivered_by_poll_thread=`, which tests the
  hypothesis above directly on the rig, in both arms.
  `park_wakes=` counts the parked thread's returns from `ppoll()`, for the idle-CPU criterion below.

**Found while implementing (2026-09-27, before any rig run).** The timer-latency test caught two defects in
the design above, each confirmed by a mutant:
- An entry scheduled from another thread while the executor is away was not seen until the lease ended: core
  wakes only a thread waiting inside `tt_Node_poll()`, and the parked thread waits outside it. Fix:
  `tt_Node_next_due()` also sets `tt_Node.idle_waiter`, and until the next `tt_Node_poll()` starts every
  `tt_Node_schedule()` signals the node's wake descriptor, which the parked thread now waits on too. The
  release arms the park timer for the earlier of the lease's end and core's next due entry, so the parked
  thread does not sleep through an entry the executor's own poll left behind.
- A non-blocking `tt_Node_poll(0)` does not read that wake descriptor, so after the first such signal the
  parked thread returned from `ppoll()` at once, over and over, until the lease ended - a spin, which the
  first mutant run hid by firing every timer on time. Fix: it drains the descriptor itself before its poll,
  and if the executor has taken the role meanwhile (`tt_RET_BUSY`) it signals again, since the drained wake may
  have been the executor's. New shutdown counter `park_wakes=`; the test bounds it at 10 per 100 ms absence.
- The test allows 1 ms of lateness as pre-registered; 20/20 runs on the PC passed. It schedules its second
  entry 10 ms after the release, because core's own entries due just after a release woke the parked thread
  and hid the first defect.

**How to read it** (rig, `rmw_crosshost_rtt.sh`, 100 ms gap, block and poll, Bench and Array1k, BE and REL,
3 repetitions, this commit against its parent, both with executor-driven receive on):
- **Pass:** block-mode RTT median falls beyond 2 x SE in every cell. Predicted: towards the PC's gain, i.e.
  several us more than 8.4 in each cell. If the parent's ping shows a large `delivered_by_poll_thread` share
  and this one shows ~0, the hypothesis is confirmed whatever the RTT does.
- **Idle CPU:** an idle node (no traffic, 10 s, schedstat) must not rise beyond 2 x SE. An executor waiting in
  `rmw_wait()` is unaffected by construction; the poll thread's scheduler wakes during an away period replace
  its `ppoll` wakes one for one.
- **Pong CPU and the poll control:** the pong's whole-run CPU and the poll-mode rows must not move beyond 2 x SE.
- **Timer latency:** a unit test holds the role away for 100 ms within the lease; an entry scheduled 20 ms in
  must fire within 1 ms of its time, on the poll thread. A test that a second thread's `rmw_wait()` claims
  from an away executor at once.
- **Known cost, stated in advance:** a datagram arriving while no `rmw_wait()` runs and no core entry falls
  due waits until the executor returns, the next core entry (<= 1 s), or the lease's end (<= 250 ms). The
  RELIABLE cells carry the ACK traffic that would show it; they are in the pass criterion.
- Any criterion failing: the change is not merged and these numbers are recorded here.

**Result on the rig (Plan, 2026-09-27): FAIL, reverted.** `rmw_lease86_rig_chain.sh`, `3e00eb97` / `22a8f2cb` /
`22a8f2cb` / `3e00eb97`, 3 repetitions per block, IDLE_S=10, 48 + 48 ok rows and 0 VOID. Raw rows are
`results/rmw86_{A_3e00eb97,B_22a8f2cb}_2026-09-27.txt`. Read by WIRE_PLAN 8.3: 0 better, 54 held, 2 WORSE.

- **The mechanism works as designed.** The 8.6 ping reports `delivered_by_executor=100 delivered_by_poll_thread=0
  executor_handovers=2 park_wakes=50-51` per 10 s block-mode run. The parent has no such counters, so the "confirmed
  whatever the RTT does" clause cannot be read from the parent's side.
- **The RTT does not move** in any block cell, so the pass criterion fails. rtt_avg in ms, parent -> 8.6:

  | cell | parent | 8.6 | t |
  |---|---:|---:|---:|
  | bench BEST_EFFORT | 0.2440 | 0.2415 | -1.6 |
  | bench RELIABLE | 0.2415 | 0.2420 | +0.4 |
  | array1k BEST_EFFORT | 0.2640 | 0.2633 | -0.7 |
  | array1k RELIABLE | 0.2643 | 0.2640 | -0.4 |

  On the Pi, the poll thread's handover to the executor costs nothing that a 100 ms-gap ping-pong can see. On PC
  veth the same change gained several us.
- **Everything else holds:**
  - idle CPU, `pong_idle_cpu_ns` ~2.0 ms over 9.4 s, in all 8 cells;
  - pong whole-run CPU in 7 of 8 cells;
  - the poll control in 3 of 4 cells.
- **Chance candidates under WIRE_PLAN 8.3,** one cell each: array1k BEST_EFFORT poll pong CPU +1.8% (t 2.6), and bench
  RELIABLE poll RTT +0.4% (t 2.5).
- **As pre-registered, the change is not kept.** Dev reverts its behaviour on `main`. R1 and R1d (sections 9.2-9.3)
  were built on its park timer and stop with it.

## 9. Where an rmw round trip goes, above and below core (2026-09-27, veth, PC; numbers only)

**Setup.** One ping-pong, BEST_EFFORT bench, `--wait block`, 5 ms gap, ~975 round trips. It runs on a veth pair
between two private netns, with executor-driven receive on (the default), and both processes dump their latency
stamps.
- Tooling: `experiments/rmw_trace_split.sh` and `rmw_trace_split.py`.
- Eight stamps are new in `tickle/trace.h`. They are compiled out unless `-DRMW_TICKLE_TRACE=ON`, and the dumps are
  the proof that the traced build was the one loaded.
- The table is the responder (pong) of the first run. RTT median was 62.8 us; a second run gave 58.6 us and the same
  shape, a few tenths higher throughout.

| segment | median us | p90 | whose |
|---|---:|---:|---|
| ppoll returns -> datagram read | 1.18 | 3.09 | kernel: recvmsg |
| -> rmw's callback | 0.54 | 1.69 | core: packet, lookup |
| -> CDR decoded | 0.10 | 0.36 | typesupport |
| -> ROS message filled (`from_tickle`) | 0.38 | 1.30 | rmw: shell pool, convert |
| -> queued, waiter signalled | 0.19 | 0.68 | rmw |
| -> poll role about to be released | 0.66 | 1.97 | core poll return, drain, rmw's wait-set check |
| -> park timer set | 0.65 | 1.77 | rmw: `tt_Node_next_due()` + `timerfd_settime()` (8.6) |
| -> `rmw_wait()` returns | 0.04 | 0.06 | rmw |
| -> `rmw_take()` entered | 1.44 | 5.39 | **rclcpp executor** |
| -> `rmw_take()` returns | 0.13 | 0.27 | rmw |
| -> `rmw_publish()` entered | 0.92 | 3.17 | **rclcpp**: callback dispatch, user code, publish |
| -> `to_tickle()` done | 0.36 | 0.51 | rmw: mutex, convert |
| -> CDR encoded | 0.23 | 0.61 | core + typesupport |
| -> send syscall made | 0.18 | 0.51 | core: framing, route |
| -> send syscall returned | 8.59 | 13.35 | kernel: sendmsg, **on veth including the peer's receive path** |
| whole responder | 16.97 | 32.85 | |

**Reading.**
- **The responder's own user-space work is ~6 us of its 17.**
  - rmw's part: ~2.4 us.
  - core's part: ~0.95 us.
  - typesupport: ~0.3 us.
  - rclcpp: ~2.5 us, the largest of these.
- **The syscalls are the rest.** The send is 8.6 us here because a veth send runs the peer's receive softirq on the
  sender's CPU; on the rig's NICs it is a different number.
- **Of the ~60 us RTT, ~17 is each process's responder-like path. The rest is two thread wakeups and the kernel's
  network path, which no stamp sees.**
- **The largest rmw-owned piece is the poll role's return and release**, 1.3 us together. It includes one
  `timerfd_settime()` per release, plus the one per claim that is not on this path.

**Candidates, pre-registered as proposals only (no code; each needs its own A/B, and the rig's block RTT is the
judge):**
- **R1 - release without a timer syscall on the executor's path.**
  - Leave the park timer armed at claim (no disarm), and at release re-arm only when no earlier deadline is armed.
    The parked thread re-arms itself if it wakes early inside a lease.
  - Predicted: -0.4 to -0.6 us per wait on the executor, and one fewer syscall per claim.
  - Risk: extra parked-thread wakes (8.6's `park_wakes=` counts them).
  - PASS: veth RTT median better beyond 2 x SE, pong CPU not WORSE, `park_wakes` per second not WORSE by more
    than 10%.
- **R2 - the drain's read that finds nothing.** After a wakeup with one datagram, core's drain calls
  `tt_try_receive()`, which makes up to two `recvmmsg()` calls that return EAGAIN (two sockets).
  - First measure how many per wakeup.
  - If at least one, skip the drain when the wakeup's own read returned short of a batch: the socket was empty an
    instant before, and a later arrival is seen by the next poll.
  - Predicted: -0.3 to -0.6 us per wakeup.
- **Not rmw's to change:** rclcpp's executor, ~2.4 us (p90 5-8 us). It is the same for every rmw.

### 9.1 R2 measured (Dev, 2026-09-27, branch `r2-adaptive-drain`): the empty read goes, the latency does not move

- **The measurement first.**
  - strace -c on the pong, ~940 round trips at a 5 ms gap: 2087 `recvfrom()`, of which 1043 returned EAGAIN. That is
    1.1 empty reads per round trip. Here the rmw build reads one datagram at a time (`tt_RX_BATCH` 1, its buffer
    being larger than a control datagram).
  - The empty read is the drain after the blocking wakeup, and it sits on the latency path, before the poll returns.
- **The change.** The drain after a blocking wakeup is skipped once 2 in a row have found nothing, except every 16th
  wakeup, and any drain that finds something ends the skipping (`tt_RX_DRAIN_SKIP_AFTER`,
  `tt_RX_DRAIN_PROBE_EVERY`).
  - Tests: skipping after a run, still probing, and a probe that finds a burst ends it.
  - Mutants: never skip, never probe, never reset. Each fails.
- **Result.**
  - Empty reads: 1.1 -> 0.07 per round trip (strace, 956 round trips).
  - The stamped segment that holds the read, callback -> release, is 0.28-0.29 us in both arms: 3 alternating runs
    each of main and the branch, ~975 round trips each.
  - RTT medians: 45.9-51.2 us on main and 46.4-56.9 us on the branch, within run-to-run noise.
- **Verdict: FAIL on §9's rule.** The veth RTT is not better beyond 2 x SE, and the empty read costs no latency that
  the stamps can resolve. It saves one syscall of CPU per wakeup, which is not what the candidate was for. It does
  not go to the rig; the branch stays for the record.

### 9.2 R1 measured (Dev, 2026-09-27, branch `r1-release-no-timer-syscall`): RTT -4.6 us, but pong CPU +12% - FAIL

**The change.**
- A claim no longer disarms the park timer.
- A release re-arms it only when none is armed to fire after now and within the new lease; otherwise the parked
  thread, woken early inside a lease, re-arms it itself (`park_timer_at_ns`, `rmw_tickle_arm_park_timer()`).
- 8.6's timer-latency tests stay green. The mutant "never re-arm at release" fails them: a timer scheduled before
  the wait is late.

**Result.** veth, BEST_EFFORT bench, 5 ms gap, 5 s, 3 alternating runs each, non-traced builds; pong CPU is the
whole run's schedstat (4 s idle + 5 s traffic):

| | main | R1 |
|---|---:|---:|
| RTT median, us | 44.8 / 45.1 / 46.1 | 41.4 / 40.3 / 40.4 (**-4.6**) |
| pong CPU, ms | 39.6 / 41.6 / 40.3 | 45.1 / 45.7 / 45.4 (**+4.9, +12%**) |
| pong `park_wakes`, the run | 4 / 2 / 2 | 494 / 495 / 494 |
| pong `park_wakes`, 10 s idle alone | 2 / 2 / 2 | 3 / 3 / 3 |

**Reading.**
- The stamped release -> timer-set segment is 0.62-0.69 us in both arms. So the timer syscall there was not what it
  cost, and the RTT gain comes from elsewhere: plausibly from no longer disarming at every claim. Which kernel cost
  that avoids is not measured.
- The CPU rise is the timer left armed through each hold. It fires once a lease (10 ms) while the executor polls,
  and wakes the parked thread for nothing: ~55 wakes/s at ~10 us each.

**Verdict: FAIL on §9's rule** (pong CPU WORSE, `park_wakes` WORSE). Not for the rig as it stands.

**Worth knowing for a next variant.** The RTT gain is real and large, -4.6 us of ~45, beyond run-to-run noise. A
version that keeps it must stop the spurious wakes without a syscall per claim - for example, a claim that disarms
only when the armed deadline falls within the expected hold. That is not designed here.

### 9.3 R1d - a claim disarms only a timer due inside its expected hold (pre-registered 2026-09-27, before code; branch only)

**Why.** R1 moved RTT by -4.6 us, and the stamps put the gain outside the release (9.2), plausibly in the claim-time
disarm. R1 lost on CPU because a timer left armed fires during long holds.

**The change.**
- A claim disarms the park timer only if it is armed to fire before now + 1.5 x the last hold (claim -> release, kept
  in the context; the same 1.5 x rule 8.6 applies to the away time).
- A release re-arms it as today, which pushes the deadline on.
- So:
  - a short hold, the pong at a 5 ms gap and the ping at any gap, makes no claim syscall and never sees the timer
    fire;
  - a long hold, the pong at the 100 ms gap, disarms as today.

**Why this and not a lazy disarm.** Leaving the timer armed and letting it fire once per hold would cost the pong one
parked-thread wake per message at 100 ms, which Plan's wake bar forbids. The predictor exists already.

**Also measured.** The claim-time `timerfd_settime()` itself, stamped before and after, in main. If it is only a few
hundred ns, the RTT gain is an indirect kernel cost, and this says so.

**PASS, all of:**
- veth RTT median better beyond 2 x SE at gap 5 ms or at gap 100 ms, over 5 interleaved reps;
- pong whole-run CPU not WORSE beyond 2 x SE, at both gaps;
- pong `park_wakes` per run no more than +10% over main, at both gaps;
- 8.6's timer-latency tests green.

**R1d result (2026-09-27, branch `r1d-claim-disarm-predicted`): FAIL, and it answers where R1's gain came from.**

`experiments/rmw_lib_ab.sh`, 5 interleaved reps. Both libraries are copied in place run by run, and each run's md5 is
logged: main ae11b2cb, R1d edbc2037. Pong whole-run CPU; mean over reps, paired difference +- SE:

| | gap 5 ms | gap 100 ms |
|---|---|---|
| RTT median | 44.94 -> 45.24 us (+0.30 +- 0.35) | 121.8 -> 126.5 us (+4.7 +- 6.0) |
| pong CPU | 39.21 -> 38.86 ms (-0.35 +- 0.35) | 24.26 -> 24.46 ms (+0.20 +- 0.38) |
| pong `park_wakes` per run | 2.6 -> 4.0 | 2.0 -> 3.0 (every rep) |

- **No RTT gain at either gap, and one more parked-thread wake per run.** FAIL on the pre-registered bars.
- **The claim's disarm is not where R1's gain was.** Stamped before and after, in main, ~975 round trips:
  - the claim's `timerfd_settime()` costs 0.44 us on the pong and 0.48 us on the ping (median, p90 0.47 / 0.63);
  - the release's costs 0.51 / 0.52 us.
  - R1d removes the claim's syscall on both sides, and the RTT does not move.
- **So R1's -4.6 us most likely did not come from syscalls saved.** It came with ~55 extra parked-thread wakes a
  second (9.2), and a CPU kept busier leaves the deep idle states less often, so it wakes faster for the next datagram.
  That is a hypothesis, consistent with R1's +12% CPU and with R1d's null result, but not measured (the idle
  residency was not recorded). It is not a gain to ship: the same effect costs any rmw the same CPU.

## 10. Poll mode at 50 and 100 us: where rmw_tickle's reply waits on the ping (pre-registered 2026-09-27, before the run)

COMPARISON rows 59-63 are the only rmw RTT rows TickLE loses: poll wait with a 50 or 100 us sleep. There rmw_tickle's
ping catches the reply on its 3rd loop iteration (3.02), and CycloneDDS's on its 2nd (2.01), section 8.3. rmw_tickle's
cycle is ~163 us and its block-mode RTT ~0.25 ms. If the reply were visible to `spin_some()` as soon as it reaches the
ping host, the 2nd check at ~326 us would already see it. That it does not means ~80 us or more pass on the ping
between the reply's arrival and the moment `spin_some()` can take it. This run finds where.

**Run:** `rmw_crosshost_rtt.sh`, current `main`, all three rmws, WAITS=poll POLL_SLEEPS="100 0", bench, both QoS,
2 repetitions, CAPTURE=1 STAMPS=1 SYSSTAMP_ARMS="off on". Busy poll (0) is the control: there TickLE wins. Split
with `rmw_pcap_split.py` into
- the reply's arrival at the ping host (pcap),
- the ping's receive syscall on its receiving thread (sysstamp),
- the ping's application seeing it (stamps, `reply_ns`).

**How to read it:**
- **The gap is between the arrival and the receive syscall:** the ping's poll thread wakes late while the main thread
  sleeps. That points at the poll thread's wait: the executor-poll lease, or the park timer. Its fix is in rmw_tickle's
  poll-thread handover.
- **The gap is between the receive syscall and `reply_ns`:** delivered but not seen. The candidates are `spin_some()`
  → `rmw_wait(0)` not reporting a ready subscription the poll thread queued, or a guard condition not triggered.
  The fix is in `rmw_wait`'s readiness check.
- **No gap (the reply is seen at the first check after its arrival):** then 3 against 2 iterations is the cycle
  arithmetic alone. It means rmw_tickle's true poll-mode RTT exceeds 326 us for a reason upstream of the ping (the
  pong's turnaround in poll mode), which the same pcaps split.
- **Control:** at busy poll, rmw_tickle's arrival-to-seen gap must be a few us. If it is not, the instrument
  (sysstamp) is the cause, and the `on` / `off` RTT difference says by how much.

### 10.1 Result: no hidden delay; rmw_tickle's reply arrives first and is seen at the next check (2026-09-27)

48 + 48 rows ok (sysstamp off and on), 0 VOID, every ping and echo matched in the pcaps. Raw rows are
`results/rmw_poll10_2026-09-27.txt`; the per-row split is `results/rmw_poll10_split_2026-09-27.txt`. Means over
2 repetitions, bench, in us after the ping's publish:

| rmw, QoS | sleep | reply at the ping's NIC | NIC -> app sees it | seen at | reported RTT | RTT - seen |
|---|---:|---:|---:|---:|---:|---:|
| rmw_tickle BE / REL | 0 | **219 / 222** | **24 / 24** | **243 / 246** | 244 / 246 | 1 / 0 |
| CycloneDDS BE / REL | 0 | 246 / 234 | 36 / 44 | 283 / 278 | 284 / 278 | 1 / 1 |
| FastDDS BE / REL | 0 | 278 / 290 | 50 / 49 | 328 / 339 | 330 / 340 | 2 / 1 |
| rmw_tickle BE / REL | 100 | **221 / 224** | 126 / 130 | 347 / 354 | 504 / 511 | 157 / 157 |
| CycloneDDS BE / REL | 100 | 243 / 233 | 36 / 48 | **280 / 281** | 438 / 438 | 158 / 158 |
| FastDDS BE / REL | 100 | 277 / 290 | 49 / 53 | 325 / 343 | 486 / 504 | 160 / 161 |

- **The pre-registered outcome is the third one:** no gap. At 100 us, rmw_tickle's poll thread reads the reply
  17.7-18.0 us after it reaches the NIC (sysstamp, against 12.4-22.6 for CycloneDDS). The application then takes it
  at the next `spin_some()`.
- **Plan's claim to the user at 08:00 of "~80 us more on the ping in poll mode" was wrong,** and it is corrected here.
  - It assumed the reported poll-mode RTT ends when the reply is seen.
  - It does not. Every rmw's reported RTT includes one more loop sleep after the reply was seen (157-161 us, the same
    for all three), because the ping reads its RTT after the loop.
- **rmw_tickle is ahead at every stage it controls:**
  - its reply reaches the ping's NIC first, 219-224 us against 233-290;
  - with busy polling the application sees it first, 24 us after arrival against 36-50.
- **Why it loses at 100 us: the grid, and nothing else.**
  - The ping publishes at the start of a loop cycle, so the reply lands at the same phase of the cycle in every round
    trip.
  - rmw_tickle's cycle is ~163 us, with a 6 us `spin_some()`. Its reply, delivered at ~240 us, falls between the
    checks at ~163 and ~326 us and waits ~100 us for the second one.
  - The vendors' 46-65 us `spin_some()` puts a long check window across their own arrival time. At this one sleep
    value, it catches the reply at once.
  - At a sleep of 0 or 200 us the phases fall the other way and rmw_tickle leads (COMPARISON rows 52-55, 64-67).
- **What this means for the test (proposal, for the user's decision):**
  - **(a) Random phase.** A phase-locked poll loop measures the grid, not the rmw. A real application's poll loop is
    not synchronised with its peer's replies. A poll-mode case that publishes from a timer independent of the loop
    gives each rmw its average catch delay, about half a cycle, and rewards a short cycle rather than one lucky
    sleep value.
  - **(b) Report "seen at".** It is `reply_ns`, as block mode does, rather than the after-loop time. That removes the
    ~158 us every rmw carries today and changes no ranking.

### 10.2 Poll mode with a random phase, the round trip ending at the callback (pre-registered 2026-09-27, before the run)

**The user's decision (2026-09-27, "너의 추천대로 진행하자"):** the poll-mode rows of COMPARISON.md are measured with the
ping's `--phase jitter --rtt-at callback`. The sleep sweep stays. The earlier phase-locked rows (P, section 8.3)
stay in the document as reference, labelled as such.
- **jitter:** a random pause of up to one measured poll cycle between each publish and its first check. The reply then
  lands at a random phase of the cycle, as in an application whose poll loop is not synchronised with its peer, and
  the ping stays one thread.
- **callback:** the round trip ends when the reply's callback runs, as in block mode, not after that cycle's sleep.

**Run:** `rmw_crosshost_rtt.sh` on `main` with the ping change merged. All three rmws, interleaved per repetition.
WAITS=poll, POLL_SLEEPS="0 50 100 200", bench and array1k, BEST_EFFORT and RELIABLE, 3 repetitions, PHASE=jitter,
RTT_AT=callback. That is 144 rows. Each row is VOID if:
- its PHASE line does not show `phase=jitter rtt_at=callback`;
- its placement chi2 is 21.67 or above (10 bins, 9 degrees of freedom, p = 0.01);
- either side's /proc maps shows the wrong or a second rmw implementation;
- any ping is lost.

**Expected, from section 10.1's stages (a prediction, not a criterion):**
- The time a reply is seen is roughly its arrival at the ping, plus the rmw's delivery, plus on average half a poll
  cycle.
- At 100 us that is roughly 320 us for rmw_tickle (arrival ~222, cycle ~163), ~360 for CycloneDDS (~240, ~212) and
  ~410 for FastDDS (~285, ~222).
- So rmw_tickle should be lowest in every cell. The margin grows with the sleep, which lengthens every cycle alike,
  and shrinks toward busy polling.

**How to read it:**
- A cell is rmw_tickle's win only if its mean is lowest and outside the next rmw's mean by more than 2 x SE combined;
  otherwise it is a draw. A vendor lower beyond 2 x SE is a loss, and is investigated as 10.1 was, before anything
  else.
- **Control:** at a sleep of 0 (busy polling) the cycle is only the `spin_some()`. The callback-ended round trip must
  then agree with the old P rows 52-55 within a few us for every rmw. If it does not, the ping change itself moved
  the measurement, and the other rows are not read.
- **Also checked:** callback_p10/p90 on the PHASE line. With a random phase, their spread should be about one cycle
  for every rmw. A narrow spread means the phase is still locked, whatever chi2 says.

### 10.3 Result: rmw_tickle lowest in 15 of 16 poll cells, 9 beyond 2 x SE, no loss (2026-09-27)

`results/rmw_polljitter_2026-09-27.txt`, build `499763db` (`ca48a906` is the same behaviour, lint only). 141 ok
rows, 3 VOID: 2 jitter placements at chi2 21.8 and 22.6, the expected chance rate at p = 0.01, and 1 loss. Means
in us, rmw_tickle / FastDDS / CycloneDDS:

| cell | 0 (busy) | 50 us | 100 us | 200 us |
|---|---|---|---|---|
| Bench BE | **240** / 320 / 273 | **293** / 357 / 308 | 316 / 372 / 322 (draw) | 365 / 419 / 367 (draw) |
| Bench REL | **242** / 330 / 270 | 297 / 359 / 340 (draw) | **320** / 387 / 329 | 366 / 424 / 375 (draw) |
| Array1k BE | **261** / 336 / 289 | **311** / 369 / 324 | **338** / 394 / 349 | 388 / 433 / 395 (draw) |
| Array1k REL | **263** / 350 / 281 | 315 / 377 / 354 (draw) | 338 / 400 / 346 (draw) | 392 / 442 / **384** (draw, CycloneDDS lower) |

- **The control passes.** At busy polling every rmw's callback-ended round trip agrees with the old rows 52-55 within
  3-8 us. The callback now closes the round trip a few us before the loop did, the same for all three.
- **The phase is random in every row:** the reply-callback spread p90-p10 is about one poll cycle for every rmw (18-37
  us busy, 84-106 at 50, 131-152 at 100, 205-241 at 200).
- **The prediction was right in direction and wrong in size.** rmw_tickle is lowest in 15 of 16 cells, but its margin
  over CycloneDDS at 100 us is 6-11 us, not the ~40 predicted. The prediction added half of each rmw's own cycle
  on top of its arrival. In fact a reply that arrives during CycloneDDS's long `spin_some()` is taken within that
  same call, so CycloneDDS's effective wait is shorter than half its cycle.
- **No vendor is lower beyond 2 x SE in any cell.** COMPARISON.md rows 52-67 now hold these figures (`J`), and the
  phase-locked ones are kept as reference.

## 11. Where the copies are, and what removing them could save (pre-registered 2026-09-27, before any code)

**Why.** The user approved Plan's copy-reduction design in steps ("설계한 대로 진행하자"). Step 1 is done
(`0e71efc0`): a deferred service response is encoded from the caller's struct, with no slot copy. Steps (2) and (3)
are measured first, and built only if the measurements leave room:
- (2) scatter-gather `sendmsg()` for large string and sequence fields;
- (3) ROS 2 loaned messages (`rmw_borrow_loaned_message` / `rmw_take_loaned_message`) for fixed-size types.

**M-a, the copy map (PC, private netns).**
- **Scope.** Every copy a sample's bytes go through, for `bench` (64 B), `array1k` and `struct16`, on four paths:
  - `rmw_publish`, and `rmw_take` on the other side;
  - a service's request and response.
- **Method, two independent measurements that must agree:**
  1. **Code.** Each copy is named with its code reference:
     - ROS message -> TickLE struct (`to_tickle`);
     - TickLE struct -> CDR in `tx_buffer` (encode);
     - `tx_buffer` -> kernel (`sendmsg`), kernel -> `rx_buffer` (`recvmmsg`);
     - `rx_buffer` -> TickLE struct (decode; strings alias);
     - TickLE struct -> ROS message (`from_tickle`);
     - the reliable cache and the reorder hold.
     Its bytes per sample are computed from the message layout.
  2. **A counting shim.** An `LD_PRELOAD` wrapper around `memcpy`/`memmove` records bytes per call site
     (`__builtin_return_address`, resolved to a symbol) over N samples of a ping-pong.
     - It cannot see copies the compiler inlined, which are only small fixed-size ones. So the check is on
       copies of 64 B and up: every such copy in the code map appears in the shim, and nothing of that size
       appears in the shim that is not in the map.
     - A disagreement is a finding, not a rounding.
- **Time per copy-bearing stage.** Taken from the existing trace split (`rmw_trace_split.sh`, section 9), run for
  each message type: `to_tickle`, encode, decode and `from_tickle`, as a median over ~1000 round trips.

**M-b, memcpy vs iovec on the Pi.** CPU 3 of the client Pi, under the rig lock, `hil` scope.
- **Sizes:** 64 B, 256 B, 1 KB, 4 KB, 16 KB and 60 KB field payloads.
- **Two arms, alternated, 20 rounds x 100000 ops:**
  - (i) the field copied into a contiguous buffer behind a 24-B header, sent as one iovec;
  - (ii) `sendmsg()` with two iovecs, header plus the field in place.
- **Transport:** UDP over `lo` to a socket of the same process, drained between batches. Both arms pay the same
  receive.
- **Reported:** user+sys per op (`getrusage(RUSAGE_THREAD)`), paired by round.

**How it reads, written before any number:**
- **(2)'s threshold:** the smallest field size at which (ii) is cheaper than (i) beyond 2 x SE. Below it, a copy is
  the cheaper send and (2) would stay out for such fields.
- **(2)'s ceiling:** for each message type, per sample, the saving (i) - (ii) at its large fields' sizes, set against
  the rmw per-sample send CPU on the Pi.
- **(3)'s ceiling:** the `to_tickle` + `from_tickle` time M-a measures for the fixed-size types, set against the
  responder's user-space time (section 9: ~6 us of 17 for `bench`) and the RTT.
- **Drop rule, the same for both:** a ceiling under 2% of the per-sample rmw CPU, or under 0.3 us, drops that step.
  The report says so and nothing is built. Above it, the step gets its own design and pre-registration.
- **Control:** the shim is checked on a program of known copies, a 4096-B memcpy loop: it must report exactly 4096 B
  per iteration at the right call site, or its other counts mean nothing.

**M-c (added at Plan's request, same day, before any number): the held-back datagram's extra copy in the HAL.**
- **The copy.** `hal_linux.c`'s `rx_take_pending()` `memcpy`s every datagram a `recvmmsg()` batch held back (slots 2..N)
  from `hal->rx_batch[slot]` into `node->rx_buffer` before it is processed. A throughput server with full batches of
  32 copies 31 of every 32 datagrams once more.
  This is the first step of Plan's receive-buffer lending design (OPTIMIZATION_PLAN 12), and a pure copy cut.
- **Measured on the Pi, with M-b's tool:** a copy-only arm, the per-datagram `memcpy` at the p1 datagram size (96 B in
  the bench capture) and at p4's full datagram (1472 B), 20 rounds x 1000000 copies, in ns per copy.
- **What processing in place would need, from the code, before anyone builds it:**
  - core passes `node->rx_buffer` only as the buffer handed to `tt_receive()`/`tt_try_receive()` and then to
    `process_packet()` (`drain_rx()` and the poll loop). No other reader or writer.
  - A HAL call that hands out a pointer to the held slot instead of copying it: a HAL API addition.
    `hal_freertos.c` holds nothing back (`tt_rx_buffered()` is 0), so it would take its copy path as today.
  - The same alignment as `rx_buffer`: 4-aligned, `_Static_assert`ed for the codec. `rx_batch` rows are 1472 B
    apart, a multiple of 8, so the rows need only the array's own start aligned. Add `tt_ALIGNAS` and an assert.
  - The same lifetime: a slot stays valid until the next `recvmmsg()`, as `rx_buffer` stays valid until the next
    receive. Decode aliases its input either way, and only the one poller touches either.
- **How it reads:**
  - The ceiling per sample is the copy's cost x 31/32, set against the campaign server's per-sample CPU on the Pi
    (~2.49 us at c1, WIRE_PLAN 8.9 follow-up).
  - The drop rule is 11's: under 2% or under 0.3 us, it is dropped.

### 11.1 Result: (2), (3) and M-c all dropped for the benchmark types (2026-09-27)

Raw files:
- `results/copy_cost_pi_2026-09-27.txt` (M-b and M-c, the client Pi, CPU 3, 20 rounds);
- `results/copy_map_shim_2026-09-27.txt`;
- `results/rmw_trace_split_{bench,array1k,struct16}_pong_2026-09-27.txt` (PC veth, ~970 round trips each).

**M-a, the copy map.**

*From the code.* Per sample and per direction, user space copies a payload twice, and the kernel twice.
- Send side:
  - `to_tickle`, ROS -> TickLE struct: generated element loops (`tickle->array[i] = ros.array[i]`);
  - encode, struct -> `tx_buffer`: a constant-size `memcpy` for `byte[1024]`, field stores for the rest;
  - `sendmsg`, `tx_buffer` -> kernel.
- Receive side:
  - kernel -> `rx_buffer` (`recvmmsg`);
  - decode, `rx_buffer` -> struct: a constant-size `memcpy`; strings alias instead;
  - `from_tickle`, struct -> ROS: element loops.
- In addition: the reliable cache on a RELIABLE publish, the reorder hold on an out-of-order sample, and M-c's copy
  for a datagram a batch held back.

*From the shim.* Its control read exactly 1000 calls and 4096000 B. It found **no per-sample `memcpy`/`memmove` of
64 B or more** for any of the three types.
- That is the disagreement 11 called a finding: every per-sample copy is inlined or a loop, which a libc shim
  cannot see.
- So the code map stands alone for the byte counts, and the trace split for the time.

*Time per copy-bearing stage.* Responder medians in us, from the trace split:

| type | `to_tickle` | encode | decode | `from_tickle` | whole responder |
|---|---:|---:|---:|---:|---:|
| bench | 0.10 | 0.33 | 0.12 | 0.19 | 20.4 |
| struct16 | 0.08 | 0.28 | 0.14 | 0.14 | 17.8 |
| array1k | 0.22 | 0.53 | 0.28 | 0.34 | 30.3 |

**M-b, a copy + 1 iovec against 2 iovecs, on the Pi.** The copy arm's cost minus the iovec arm's, in ns per send
(user+sys); wall time agrees within 3 ns.

| field size | copy - iovec | SE |
|---:|---:|---:|
| 64 B | **-9.5** (the copy is cheaper) | 2.0 |
| 256 B | -2.5 | 2.2 |
| 1 KB | +17.5 | 2.6 |
| 4 KB | +104 | 3 |
| 16 KB | +472 | 2 |
| 60 KB | +2288 | 14 |

**M-c, the held-back datagram's copy, on the Pi:** 6.4 ns at 96 B, 44.5 ns at 1472 B.

**How it reads, by the rules written in 11:**
- **(2) scatter-gather: dropped for the benchmark types.**
  - The threshold lies between 256 B and 1 KB: 1 KB is the first size at which two iovecs win beyond 2 x SE.
  - The ceiling per sample: `bench` and `struct16` have no field that large, so 0. `array1k`'s 1 KB field
    saves 17.5 ns, 0.4% of a ~4.3-us send and under 0.3 us.
  - It clears the bar only at fields of about 16 KB and up (0.47 us). Such messages (Image, PointCloud2) go as
    DATA_FRAG fragments, whose copies are a different path. That would be a proposal of its own if anyone wants
    it.
- **M-c, the held-back copy: dropped.** 6.4 ns x 31/32 per sample at p1 is 0.25% of the server's ~2.49 us. At
  p4's 1472-B datagram it is 43 ns, still under 2% and under 0.3 us. This removes the first step of Plan's lending
  design as a copy cut on its own merits. The design's other reasons are not measured here.
- **(3) loaned messages: dropped for all three benchmark types (Plan's reading, same day).**
  - The bar is the per-sample figure: `array1k`'s 0.56 us is 1.8% of its 30.3-us responder, under 2%. The
    ~8% below is of the user-space part alone, which is not the bar 11 set.
  - A loan would skip `to_tickle` + `from_tickle` and nothing else: encode and decode stay.
  - `bench` 0.29 us and `struct16` 0.22 us are both under 0.3 us.
  - `array1k` is 0.56 us per responder, ~8% of its ~7 us of user-space work (section 9's split) and ~1.8% of its
    responder latency.
- **Recorded as seen, not proposed.** For a fixed-size type whose TickLE struct has exactly the ROS C++ struct's
  layout, the conversion could be skipped without the loan API at all:
  - `array1k`'s `byte[1024]`, `int64` and `uint64` sit at the same offsets in both;
  - the encoder would then read the ROS message itself.
  - Whether that layout identity holds generally, and at what cost to the generator, is unmeasured. It would be
    (3)'s cheaper form, if (3) is pursued.

## 12. What a large primitive sequence costs to convert, and how the interface packages are built (pre-registered 2026-09-27, before any number)

**Why.** 11.1 found that the generated C++ converters copy primitive sequences element by element
(`tickle->data[i] = ros.data[i]`); the C ones use `memcpy`. A camera topic would pay that on every frame. Two
facts bound the question, both found before measuring:
- **The capacity.** rmw_tickle's shipped capacity for `sensor_msgs/Image.data` is 64,000 B
  (`capacities/profile_65507.tsv`). One sample is capped at 65,507 B. So 640x480 and 1080p frames cannot be
  published at all; Plan is taking that to the user as a design question. Only the sizes that fit are measured
  here.
- **The build type.** `scripts/build_ros2_interfaces.sh` ran colcon without `CMAKE_BUILD_TYPE`, so users' interface
  packages, converters and codecs included, were compiled with no `-O`. CI builds them Release.
  - It is fixed to Release, with an override, whatever this measures (Plan, same day).
  - The measurement stays as the record of what users paid.

**The measurement (PC first, then the client Pi under the rig lock).**
- **Types, at the most rmw_tickle carries:**
  - `sensor_msgs/Image` with 64,000 B of `data`;
  - `std_msgs/ByteMultiArray` with 16,384 B.
- **Arms:**
  1. The generated C++ `to_tickle` + `from_tickle`, as a call pair, built -O0 (the script as it was) and -O2
     (Release). Each is timed on its own, median over 20 rounds x 2000 calls.
  2. A plain `memcpy` of the same number of bytes, twice (one per direction). This is the floor.
  3. The rmw-level reference, where cheap: `rmw_serialize()` + `rmw_deserialize()` of the same Image through
     `rmw_tickle` (-O0 and -O2 interface builds), `rmw_fastrtps_cpp` and `rmw_cyclonedds_cpp`. This is the vendors'
     serialisation next to ours, on the same message.
- **The code:** `objdump` of the -O2 converter, to see whether the element loop is vectorised or turned into a
  `memcpy` call.

**How it reads, written before any number:**
- **The generator** switches primitive-element arrays and sequences (bool, char, fixed-width ints and floats, where
  the element layout is identical) to a single `memcpy` if the -O2 converter pair is at least 2 x arm 2 at 64,000 B.
  - Its tests: every primitive type round-tripped in its array, bounded-sequence and sequence forms.
  - Endianness is not the converter's job: it copies host to host, and the codec swaps.
- **The script** goes to Release regardless. The -O0 / -O2 ratio is recorded as what users were paying.
- **The vendor numbers** are a reference, not a pass or fail.

### 12.1 Result on the PC: the converters are 40x a memcpy even at -O2, and users' -O0 build was 24x slower again (2026-09-27)

**How it was run.**
- `experiments/conv_cost/conv_cost.cpp` calls `to_tickle`/`from_tickle` through the `rosidl_typesupport_tickle_cpp`
  handle, as rmw_tickle does, next to a `memcpy` of the same bytes and `rclcpp::Serialization` under each rmw.
- Medians over 20 rounds x 2000 calls, pinned to one CPU.
- Raw file: `results/conv_cost_pc_2026-09-27.txt`.
- **Identity:**
  - each row names the converter library it loaded (`dladdr`);
  - the -O0 overlay was built by `build_ros2_interfaces.sh -t ''` (the old behaviour) and the -O2 one by
    `-t Release`.
  - The first run was void: its -O0 rows loaded the -O2 library, because the program's `setup.bash` re-sourced
    the overlay it was built against. It was re-run with `local_setup.bash`, which chains nothing.

| per call, us | -O0 (users' build) | -O2 (Release) | memcpy of the same bytes |
|---|---:|---:|---:|
| Image, 64,000 B: `to_tickle` | 2,157 | 86 | 1.6 |
| Image: `from_tickle` | 936 | 43 | 1.6 |
| ByteMultiArray, 16,384 B: `to_tickle` | 151 | 8.2 | 0.11 |
| ByteMultiArray: `from_tickle` | 117 | 8.1 | 0.11 |

The same Image, through `rmw_serialize` + `rmw_deserialize` (-O2 interfaces), for reference:

| rmw | serialize | deserialize |
|---|---:|---:|
| rmw_tickle | 88.0 | 45.7 |
| rmw_fastrtps_cpp | 2.7 | 2.7 |
| rmw_cyclonedds_cpp | 19.4 | 16.0 |

**How it reads, by 12's rules.**
- **The script:**
  - -O0 costs about 24x -O2 on these converters: 3.1 ms per 64-KB frame each way, against 0.13 ms.
  - It now builds Release by default (`-t` overrides it), which was decided before the number.
- **The generator:**
  - At -O2 the converter pair is about 40x two `memcpy`s at 64,000 B, far past the 2x bar.
  - Primitive-element arrays and sequences go to a single `memcpy` where the element layout is identical.
  - `rmw_tickle`'s serialisation of this Image is almost all conversion: 88 of 88 us in `to_tickle`. It is 30x Fast
    DDS's.
- **The Pi run** (12's second machine) is not done yet. The PC's margin, 40x against a 2x bar, does not hang on it.
  It is kept as the record before and after the generator change.

### 12.2 The generator copies primitive arrays and sequences with std::copy / assign(): -98% on a 64-KB Image (2026-09-27)

**The change.** `ros2_cpp_adapter.py` now emits `std::copy(ros.x.begin(), ros.x.end(), tickle->x)` and
`ros.x.assign(tickle->x, tickle->x + tickle->x_count)` for primitive-element arrays and sequences, in place of
element loops.
- Over contiguous trivially copyable elements, libstdc++ makes those a single `memmove`.
- `std::vector<bool>` still gets a correct element-wise copy, and `std::array` and `BoundedVector` work alike.
- `assign()` also drops the zeroing that `resize()` did before the copy.
- Strings and nested elements keep their per-element code.
- **Why the loop was slow at -O2:** its `uint8_t` stores may alias anything, the vector's own data pointer
  included, so the compiler reloaded that pointer every element and could not vectorise.

**Tests.**
- `rosidl_typesupport_tickle_c_tests`' new `Primitives.msg` holds all 13 primitive types as a fixed array, a bounded
  sequence and an unbounded sequence. There is no `bool[<=3]`: this build's `rosidl_typesupport_fastrtps_cpp` fails
  to compile one.
- `test_primitives_cpp` round-trips them full and empty, compared with `operator==`. CI runs it beside the dispatch
  tests.
- **The mutant control,** `assign(..., count / 2)`, fails it ("round trip differs"); the change passes.
- **The first mutant run was void, for two reasons, both fixed:**
  - The generated converters are not regenerated when only the generator changes. CMake does not track the
    generator's Python, so the run tested stale code. It was re-run from a clean build, and the generated file was
    checked for the mutant.
  - The test's missing-handle check was an `assert()`, compiled out in Release. It is now an explicit check.
- The typesupport pytest: 33 passed. The adapter tests pin the `std::copy`/`assign()` output and that a bool
  sequence never `memcpy`s.

**Measured** (the same `conv_cost` run as 12.1, on a fresh Release overlay built with the new generator;
`results/conv_cost_pc_after_2026-09-27.txt`), per call, us:

| | before (12.1, -O2) | after | memcpy |
|---|---:|---:|---:|
| Image 64,000 B: `to_tickle` | 86 | **1.60** | 1.60 |
| Image: `from_tickle` | 43 | **1.60** | 1.60 |
| ByteMultiArray 16,384 B: `to_tickle` / `from_tickle` | 8.2 / 8.1 | **0.20 / 0.11** | 0.10 |
| Image through `rmw_serialize` / `rmw_deserialize`, rmw_tickle | 88.0 / 45.7 | **3.6 / 4.8** | |
| the same, Fast DDS | 2.7 / 2.7 | 2.6 / 2.7 | |
| the same, CycloneDDS | 19.4 / 16.0 | 21.2 / 17.8 | |

The converters now cost what a `memcpy` costs. rmw_tickle's serialisation of this Image went from 30x Fast DDS's to
1.4x, and is 6x faster than CycloneDDS's.
