# rmw_tickle performance: where the ~0.08 ms deficit to CycloneDDS lives

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
