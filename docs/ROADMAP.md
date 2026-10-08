# TickLE roadmap

One page for what is being worked on, what comes after it, what waits on the user, and what just landed.
The details live in the source documents named in parentheses. Last consolidated 2026-10-05.

## Now

The user's active list (2026-10-05), in order.

0a. **Warm-up and cool-down for every framework** (the user, 2026-10-06): every cell discards an equal warm-up at
   the start and cool-down at the end for all frameworks before its statistics, so no figure is a start-up or
   teardown transient. Found by item 6: TickLE's same-host p4 latency (S10) was a first-lap figure (first-touch page
   faults on the 11.6 MB reorder ring, ~16 us of its +20 us; `p4_reorder_firsttouch.sh`). **Implemented in the
   benches, re-measurement pending:** every latency, best-effort and reliable throughput client and server of all
   three frameworks takes the same window (`tickle/common/BenchWindow.h`, TESTING section 5 item 9);
   `campaign_sweep.sh` and the S6 harnesses pass it to every framework and void a row whose `window=` is not ok.
   Every published figure before it is whole-run. Next: re-measure S7-S10, the cross-host latency rows and the
   campaign. Separately, in core: deliver in-order fragments from one hot buffer instead of the reorder ring (the
   ring stays for out-of-order).
0. **A fair testbed** (the user, 2026-10-05; ahead of everything below once rmw KEEP_ALL is closed): the published
   comparison must not rest on a setup tuned for TickLE. Found: the same-host cells (S1-S16, `s6_transport_cells.sh`,
   `s6_witness_check.sh`) pin each process to ONE core, which squeezes the multi-threaded DDS processes and suits
   single-threaded TickLE; the cross-host cells pin every framework to cores 1-3 (symmetric, but not how software is
   run). Pinning began because Pi core 0 takes the interrupts. Re-measure the headline tables with no pinning, the OS
   scheduling every framework, and publish the pinned figures only as a labelled note. Then audit the rest of the rig
   the same way: socket buffer sizes, CPU governor, IRQ affinity, vendor profiles (FastDDS XML, CycloneDDS URI,
   iceoryx), build flags. P1-P4 stay as they are: each measures something real (the user, 2026-10-05). Audited
   2026-10-05: socket buffers (all three capped at the rig's 212,992 B; if `rmem_max` is ever raised, FastDDS's XML must
   raise its own too), governor (`ondemand` for all, the distribution's default; no `performance` arm, the user's
   decision 2026-10-06), IRQs (all on CPU0 for all), vendor profiles (neutral; CycloneDDS's shared memory is opt-in and
   enabled for it) are even. Fixed: the latency clients now pace alike (TickLE's sent on a fixed period and idled one
   round trip less) and print median and p99 beside the mean; the campaign rotates the frameworks' order; rmw_tickle is
   built at the vendors' `-O2` instead of `-O3` (`rmw_crosshost_rtt.sh`; `rmw_keepall_rig.sh` next); versions are in
   RESULTS section 3. Settled (the user, 2026-10-06): KEEP_ALL is bounded in samples for DDS and in bytes for TickLE, at
   equal conditions - the native campaign gives DDS `N` samples and TickLE (N + 1) x its record bytes, and voids a row
   whose realized bound differs (`campaign_sweep.sh` common_args, qos_identity_void). Open: (a) the rmw rows (RESULTS
   72-75) still run each rmw at its own default (rmw_tickle 512 KiB, Fast DDS 5,000 samples, CycloneDDS unlimited); give
   them the same rule - Fast DDS `max_samples` = N through its XML, CycloneDDS's equivalent if rmw_cyclonedds passes
   one, rmw_tickle `RMW_TICKLE_KEEP_ALL_BYTES` = N x sample bytes - and re-measure. Done 2026-10-08 at `29dff630`
   (RESULTS rows 73 and 75, source `Ke`; 10 of 10 lossy runs clean): `rmw_keepall_rig.sh` `EQUAL_BOUND=auto` (N = 492 for Array1k, 125 for Array4k; rmw_tickle (N + 1) x its 1,064 /
   4,174-byte footprint, Fast DDS `max_samples` N; CycloneDDS cannot be bounded in samples without code - rmw_cyclonedds
   never sets resource_limits and its config has only byte watermarks - so its row stays default, labelled); (b)
   `s6_transport_cells.sh` runs each framework's repetitions back to back, not interleaved.
1. **The p4 same-host latency gap - found 2026-10-06**: not idling but first-touch page faults. A RELIABLE subscriber
   stores every fragment in reorder slot `seq_no % reorder_slots` before copying it out, even in order; the bench's ring
   is 4096 slots x 2840 B (11.6 MB, untouched .bss), so each p4 round trip writes 5.7 KB of new memory per process for
   the first lap (~2048 samples): ~16 us of the +20 us (~4.2 extra faults per round trip), the rest ~5 us of steady
   fragment-path cost. Prefaulting removed it (+22 -> +6 us); a long run at 5 ms fell to +6 us untreated
   (`experiments/p4_reorder_firsttouch.sh`, H-RING SUPPORTED,
   `~/rig_results_safe/p4_reorder_firsttouch_c1ebf43e_20261006-163432.txt`). In core since 2026-10-06: in-order fragments
   are put together in the node's `frag_scratch` (`frag_fast_take()`), the ring only for out-of-order - to be A/B'd
   on the rig (`experiments/p4_fastpath_pc.sh` is the PC check); and item 0a's warm-up for the figures (RESULTS S10).
   **Landed 2026-10-07** (`bc398ed2`, A/B `ab_frag_fastpath.sh` 9f919c8b vs c868483e,
   `~/rig_results_safe/ab_frag_fastpath_9f919c8b_c868483e_20261006-223659.*`): cross host PASS (4 primaries held: c17
   RTT, c6 send and wire bytes, c15 send); same host, warmed: p4 rtt p50 35 -> 33 us, p3 control 30 -> 30, PASS.
   The first-lap arm was NOT realized - s6's own window arguments overrode `-W 0 -C 0` (every row says warmup=4096),
   so the rig has not yet confirmed the first-lap gain the PC showed (+16 -> +2 us); and the driver's parser missed
   the indented RESULT lines and printed NO VERDICT (re-read with the same rules).
   **First lap confirmed 2026-10-08** (`ab_frag_fastpath.sh`, A = main `29dff630` with only the fast path switched off
   (branch `ab/frag-fastpath-off`), B = `29dff630`, A B B A x 5;
   `~/rig_results_safe/ab_frag_fastpath_6354ca54_29dff630_20261008-140933.*`): first lap p4 rtt p50 44 -> 26 us, p4 - p3
   gap +21 -> +3 us (IMPROVED; p3 control 23 -> 23); warmed p4 29 -> 26 us (PASS); cross host 1 better, 3 held (PASS).
   Done.
2. **Fill COMPARISON 1a's empty cells**: CPU and RSS for all three frameworks on every same-host cell (CycloneDDS
   including iox-roudi), and the vendors' DELIVERED rates on the same-host BEST_EFFORT rows. The rows compare send
   rates today, and FastDDS's KEEP_LAST 1 reader took 154 of 3.1M samples. (COMPARISON 1a notes)
   Harness ready (2026-10-06): `s6_transport_cells.sh` records each server's RESULT line (stopped with SIGINT now), its
   /proc CPU and VmHWM (`proc_snap.sh`) and iox-roudi's per repetition; `fair_samehost_summary.py` prints them with the
   delivered rate. Waiting on the user: the vendors' BEST_EFFORT history depth, `BE_HISTORY=` (`-K` on all three;
   unset = KEEP_LAST 1, as today; `tickle/common/BenchHistory.h`). A PC smoke run showed CycloneDDS's shared-memory
   reader taking 4-8% of what was sent at either depth, with its bench reader taking one sample per wake.
3. **Publisher user time** (0.637 us/sample at p3): broken down with perf on the rig (RESULTS, same-host notes):
   clock 25%, memcpy 28%, segment indices 12% (now on their own cache lines, `e0873623`). Next: let
   `tt_Publisher_publish()` called from a scheduled callback reuse the time the poll loop just read (one of the four
   clock reads), and copy once by encoding into the slot.
   Encode into the slot (branch `ab/encode-in-slot`, 1552a504): rig A/B 2026-10-07 (`ab_samehost.sh`,
   `~/rig_results_safe/ab_samehost_encode_in_slot_20261007-003950.*`) VOID by its own rule: the primary, same-host
   p3 BEST_EFFORT publisher CPU per sample, fell 13.7% (0.799 -> 0.689 us, t -62; the PC had shown +12%, from x86's
   `rep movsq`), but the registered control, p4, moved too (CPU -1.5%, delivered +1.5%) - the change splits
   segment_write() into claim and publish, which p4's fragment path also uses, so p4 was not a control it cannot
   touch. Re-run with a control it really cannot reach (or the A-to-A drift of the A B B A blocks), pre-registered
   anew; not landed until then. Re-run 2026-10-07 (rebased, `ab_samehost.sh` A B B A x 5, control = a SENTINEL: A's own
   build of p3 in every block; `~/rig_results_safe/ab_samehost_encode_in_slot2_20261007-221008.summary.txt`): IMPROVED -
   p3 publisher CPU per sample 0.722 -> 0.608 us (-15.8%, t -68), sentinel held (-0.08%); p1 / p2 / reliable p3
   publisher CPU -13 / -16 / -8%, receive rate +14 / +17 / +9%; p4 (fragments, never in the slot) and p2 latency
   held. Landed (`01ff88fe`) and reverted the same night: CI's unit tests (gcc 13) fail on it -
   `test_peer_discovery.c:1299`, one datagram more after `tt_Publisher_assert_liveliness()`. The cause was not item
   8: `tt_Context_create_publisher()` left `departed_next` unset, so a Publisher's first departure wrote past
   `departed_acks` (fixed underneath it, then item 8 landed again). Next: the poll loop's clock reuse.
4. **Reduce the reader wake cost** (~3.1 us of system time per wake) without tuned spin values: each wake is paid
   per sample at low rates. (COMPARISON 2.2c; SHM_PLAN 7 q1)
   Rig A/B 2026-10-07 (`ab_samehost.sh`, three arms, `~/rig_results_safe/ab_samehost_reader_wake_20261007-021122.*`):
   the lost-wakeup fence (B) cost nothing - every primary held - and landed (`7f9f7c62`); x86 lost 3-4 wake-ups per
   100,000 round trips without it. epoll_pwait2 + the edge-triggered bell (C, `ab/wake-epoll`) cut same-host RTT p50
   20% (30 -> 24 us) and system time per sleep 17-21%, but raised max-rate BEST_EFFORT p3 system time per sample 59%
   and user time 33%; VOID by its control and not landed. Round 2 (2026-10-07, C' = C + a claim wait bounded by the
   reader's own measured sleep cost, `ab/wake-epoll2` 56ccb264; `~/rig_results_safe/ab_samehost_reader_wake2_*`):
   against the fence alone, latency still -20% (RTT p50 30 -> 24 us) and system time per sleep -17..-21%, but
   max-rate BEST_EFFORT p3 CPU per sample +13.4% (t 59) - WORSE, not landed; against C it is 20% better. Round 3
   (2026-10-07, C'', `ab/wake-epoll3` against `021e6314`, A B B A x 5 reps;
   `~/rig_results_safe/ab_samehost_reader_wake3_20261007-104246.summary.txt`): IMPROVED 7/7, controls held - RTT
   p50 29 -> 23 us (-20%), system time per sleep -22..-29%, max-rate BEST_EFFORT p3 CPU per sample 1.595 -> 1.316
   us (-17.5%). Not landed yet: on a PC placement with a ~260 ns cache-line round trip between the two cores, max-rate
   CPU per sample is +53%. Round 4 (`ab/wake-epoll4`: C'' rebased + the watch taken only where it measures cheaper, per epoch; `~/rig_results_safe/ab_samehost_reader_wake4_20261007-134608.summary.txt`, against `f25747ae`): IMPROVED 7/7, controls held - RTT p50 29 -> 23 us (-20%), system time per sleep -24..-28%, max-rate p3 CPU per sample 1.614 -> 1.420 us (-12%). Landed (`8c1e6431`). Left: on the PC's ~260 ns placement max-rate CPU per sample is still +3.6% against main (1.381 vs 1.333 us; the epoll sleep rings 0.038 vs 0.029 times a sample).
   Cause (2026-10-08, PC, diagnostic counters): a sleep called off by the drain after its announcement had usually
   been rung already, and with the edge-triggered bell - never read - that ring ended the next sleep at once, whose own
   ring then landed after the reader woke: a chain of waits that each returned at once and each cost a ring. At ~260 ns
   main announced 763 k sleeps a run against ebcb35b1's 290 k and rang 0.073 times a sample against 0.025; reading the
   bell on every wake (the old way) took it to 0.029, ppoll to 0.037. Fix: a called-off sleep's generation is announced
   again by the next sleep (`segment_sleep_called_off()`). PC, best_effort_throughput p3 max rate, CPU us per sample
   (rings): ~260 ns ebcb35b1 1.343 (0.033), 8c1e6431 1.463 (0.043) -> with the fix 1.367 (0.028), main 1.655 (0.066)
   -> 1.472 (0.033); ~100 ns main 1.204 -> 1.208, -O0 writer 1.548 -> 1.568 (n 4-10 an arm; each within 2 x SE);
   reliable_latency RTT p50 unchanged (p3 29 us, p2 28 us). Main's remaining +0.13 us at ~260 ns is user time on both
   sides from commits after 8c1e6431, not rings. Rig A/B still to do.
5. **rmw_tickle KEEP_ALL on the rig**: the first run lost ~5% of samples under 5% loss in every build. Six causes
   found and fixed on 2026-10-05: a busy socket starved the data socket (`b6de8a4d`); an endpoint was announced
   before its QoS was final (`155eecb7`); a publisher could not learn its reader except from an announce lost in
   its own looped-back flood (`25c2e1ce`, matched from the ACKNACK; `71289352` and `5028366d`, the reader
   acknowledges a KEEP_ALL writer once); the reader's queue overflowed when the reorder buffer released a burst
   (`7b760c5d`); KEEP_ALL evicted the unacknowledged rest of a fragmented sample (`7fb6fabf`). At `7b760c5d`, 0%
   loss evicts nothing (6/6). Then three more: the poll thread starved of the state lock (`40db021d`), a reader
   that had not heard the writer announce did not acknowledge it (`a4408d0a`), and an unmatched KEEP_ALL writer
   evicted by bytes what it broadcast before its first match (`a8c1cd83`). **Closed 2026-10-05:** at `a8c1cd83`
   all 12 runs lose nothing, at 0% and 5% loss (RESULTS rows 72-75). Also queued as its own task: a reader process that hangs after
   its writer leaves. (`rmw_keepall_rig.sh`, `rmw_keepall_evict_repro.sh`, `rxo_mismatch_repro.sh`)
5a. **Testbed-independent constants** (added by the user 2026-10-05, not urgent; ordered after the reader-wake item
   and before rmw same-host and FreeRTOS, because FreeRTOS is the first other platform and re-measurements should
   rest on settled constants): every tunable in TickLE (intervals, thresholds, ring and window sizes, retry and
   spin counts, timer periods) was measured on two Raspberry Pi 5s and may be fitted to them. For each, record
   whether it is (a) derived by an algorithm from what the running system measures (preferred - e.g. the dynamic
   retry interval from srtt/rttvar), (b) a protocol or memory bound that does not depend on the platform, or (c) a
   value fitted to the rig. Replace (c) with (a) where possible; where a fitted value stays, README.md gives the
   formula that produced it and how to recompute it on another platform. Start from the old constants audit
   (`git show 3c0c505b:examples/perf_hil/CONSTANTS_AUDIT.md`) and include config.h, hal_linux.h and rmw_tickle's
   RMW_TICKLE_* defaults.
   Inventory done 2026-10-05; the fitted values that stay (c) are in README.md "Tuning for your platform".
   Done 2026-10-08 on a branch, pending its rig A/B: the last fitted values, `tt_RELIABLE_RETRY_GRANULARITY` 100 us and
   `tt_CALL_RETRY_GRANULARITY` (2x it), are (a): G is the context's own timer lateness, measured on the retry timers'
   own waits that run to their deadline (DESIGN.md 6; `test_timer_lateness`,
   `mutants_timer_lateness.py`). On the dev PC G is ~64 us idle and ~71 us under full load; against the fixed 100 us,
   c5 (P1, 5% loss) wire bytes a sample +0.04% (t 2.4) and throughput held, c6 held (`granularity_pc.sh`). Rig A/B:
   `~/rig_queue_granularity.sh`.
   Done 2026-10-06: the list-request retry is the round trip measured to that peer (smoothed, timed from a
   request's first send to its list being applied) plus `tt_CONTEXT_TX_INTERVAL`; `tt_DISCOVERY_REQUEST_RETRY`
   (10 ms) is only the seed before a peer is timed, and a near peer's request now moves the one retry entry earlier
   (`test_discovery_retry_follows_the_round_trip`). `tt_SEGMENT_DEAD_READER_NS` is one summary interval of
   `tt_LIVELINESS_SILENCE_NS` (1 s at defaults), with a static_assert that it stays below that silence - the
   literal 1 s broke that order at an update interval under ~286 ms. `RMW_TICKLE_HEARTBEAT_PIGGYBACK_EVERY`'s
   default is the tracking window / `RMW_TICKLE_HEARTBEATS_PER_WINDOW` (16), the measured 64 at today's window
   (`test_heartbeat_piggyback_default`); k = 16 is still the sweep's choice and is in README.md. On the rig a
   same-host peer's retry drops from 10 ms to about 1 ms, so the four attempts span ~4 ms instead of 40 ms: the
   M5 discovery cell under loss should be re-run before its figures are re-quoted.
   **Reverted 2026-10-06** after a rig A/B (TickLE only, A B B A, `campaign_ab_chain.sh`, then a 10-against-10
   confirmation of the WORSE cells, both at 2 x SE): `20f89a7a` (`tt_SCHEDULER_IO_INTERLEAVE` -> a timed receive check,
   `tt_RX_CHECK_RATIO`) cost c6 (P4, 5% loss) +0.8% wire bytes per sample (t 5.1, more retransmissions) and +2.0%
   client CPU (t 2.3); `c73a22e7` (`TT_RX_IDLE_RECHECK`, `tt_SEGMENT_ATTACH_RETRY_SENDS`, `_REVALIDATE_SENDS` -> times)
   cost c10 (P1 latency) +3.1% RTT (t 5.8) and ~5 KB RSS. Both replicated, so both are reverted and the constants
   are back. Results: `/tmp/ab_rxcheck_compare.txt`, `/tmp/ab_counts2time_compare.txt`, `/tmp/abc_rxcheck_compare.txt`,
   `/tmp/abc_counts2time_compare.txt` (copied to `~/rig_results_safe/ab_hotpath_2026-10-06/`). To redo: one change
   per commit (the three counts separately), each with its own A/B on the cells it can touch, before it lands. The
   designs and their tests are in the reverted commits.
   Redone 2026-10-06 one change per commit on branch `ab/hotpath-split`, each A/B'd against its parent (cells 1 6 8
   10, 10 reps per arm; `~/rig_results_safe/ab_split_2026-10-06/`): the RX idle recheck by time landed (`12036197`)
   after a re-judgement with pre-registered primaries (PASS: c1 send +6.1% t 1.5, c10 RTT -0.6%, c6 wire -0.1%, c1
   RSS +0.5%; `~/rig_results_safe/ab_rxidle_2026-10-06/`). Kept off main: the segment attach backoff (c10 RTT +2.7%,
   t 5.0 - the source of the bundle's latency cost - and its 64-bit `tx_clock_ns` does not link on RV32 FreeRTOS),
   the revalidate by time (needs that clock; redesign without a 64-bit atomic), and the timed receive check (c6 CPU
   +2.0% t 4.3 and wire +0.6% t 3.8 again; hypothesis: with io_uring its budget falls below a microsecond, so ACKNACKs
   are handled one at a time and repairs are duplicated).
   Being rebuilt one change per commit, each A/B'd alone before it lands: (1) `TT_RX_IDLE_RECHECK` (64 datagrams) ->
   `TT_RX_IDLE_RECHECK_NS` = `tt_RECEIVE_TIMEOUT` by the poll's own clock (`test_poll_signal`, `rx_idle_check`).
   Done 2026-10-05: the RPC retry bounds are srtt-relative (`tt_CALL_RETRY_MAX_SRTT_MULTIPLE`, per-retry doubling;
   `tt_CALL_RETRY_INTERVAL` is now only the seed and `_MAX` is gone), and the server's response cache lives as long
   as the client's schedule it can know plus the retry gaps it has measured (`tt_SERVER_CACHE_GAP_MULTIPLE`;
   `tt_SERVER_CACHE_TIMEOUT` is gone). `tt_CALL_DEADLINE_PER_SEND` (250 ms) stays absolute on purpose: an auto call
   still reports failure within `(count + 1) x` it (1 s), the old worst case - a policy bound on how long an
   application waits, not a link estimate. `RMW_TICKLE_CLIENT_RETRY_INTERVAL_NS` (100 ms) stays: an rmw call must
   live until the server's `tt_SERVER_DEFERRED_RESPONSE_TIMEOUT` (ROS 2 has no rmw-level service timeout), and
   core's auto path ends a call within 1 s. It can move to the auto interval once a service can set the call's
   deadline separately from its retry interval.
   Open: a restarted client can still get its predecessor's cached answer in one case. A server tells client
   incarnations apart by discovery's per-launch entity_id (recorded with each response) and drops a source's
   responses on its farewell. Not covered: a client that crashed (no farewell), restarted under the same context
   id within the old response's lifetime, and whose first request reaches the server before its new announce
   does - the server still knows the old entity_id then. The same holds without a discovery table attached,
   for any restart without a farewell. Nothing on the wire identifies the incarnation in a CallRequest (the
   segment header's `incarnation` exists only between same-host peers); a per-launch tag in the CallRequest's
   `reserved` byte would close it, as a wire change.
6. **Re-measure rmw same-host performance**: the rmw same-host rows predate the segment, FIFO and wake fixes.
   (COMPARISON 2.7; RMW_PERF_PLAN)
   Done 2026-10-08 at `03585237` (`experiments/rmw_samehost.sh`, RESULTS.md rows R1-R21): WIN 50, DRAW 1, LOSE 1.
7. **FreeRTOS: implement tt_rx_maybe_ready() with an lwIP netconn receive callback**: today only the Linux HAL has
   the receive hint. lwIP's socket layer hard-wires its own netconn callback (`DEFAULT_SOCKET_EVENTCB` in
   `sockets.c` is not configurable), so the callback needs the HAL on the netconn API: `netconn_new_with_callback()`
   per socket, a callback that counts arrivals and gives one FreeRTOS semaphore, `tt_receive()` waiting on that
   semaphore (which also replaces the loopback wake socket), reads with `NETCONN_DONTBLOCK`. About 250 lines of
   `src/hal_freertos.c`; `make test-freertos` is the check. (include/tickle/hal.h; SHM_PLAN 7 q3)
   Done 2026-10-06, as described, default `tt_HAL_FREERTOS_NETCONN=1`; the socket HAL stays behind `=0`. The QEMU
   selftest counts empty reads per datagram: 1.0 (waiting poll) and ~1,200 (busy poll) on sockets, 0 and 0 on
   netconn, which also answers `tt_try_receive()`'s "nothing" from the count. Found on the way: closing an lwIP
   socket faults, because lwIP sets errno, picolibc keeps errno thread-local, and the board never sets `tp`.
8. **CycloneDDS arm of the mixed shared-memory + network test**: the mixed-delivery test has no Cyclone arm yet.
   It needs iceoryx plus network config. (COMPARISON 1a, mixed delivery)

## Next

Open work that is not parked, deduplicated across all sources. Rough priority order within each group.

### Fast DDS under KEEP_ALL, from its side (the user, 2026-10-06)

**Run 2026-10-06** (`fastdds_keepall_arms.sh`, RESULTS rows 72-75 notes): F0 reproduced the timeout; F1 and F2
falsified the 3 s heartbeat explanation (writes stalled over 5 s even with 50 ms heartbeats); F3 survived only because
its 50,000-sample history never filled (760 / 109 msg/s at 5% loss). **Closed** (the user, 2026-10-06): with a normal
configuration it does not run, and RESULTS says so; Fast DDS's internal cause is not pursued.

The rmw KEEP_ALL rows (RESULTS 72-75) run every rmw at its defaults. Fast DDS's write times out at 5% loss: 100 ms
`max_blocking_time`, a 5,000-sample history and a 3 s heartbeat period (RESULTS, rmw layer notes). To show it fairly:
(1) fix `rmw_keepall_rig.sh`'s summary to count loss only after the first received id and report the pre-match gap
separately (perf_test's `--expected_num_subs` is compiled out in the rig's build); (2) run F0 (defaults; must
reproduce), F1 (+ `max_blocking_time` 5 s: predicts ~3 s stalls, no timeout), F2 (F1 + `heartbeatPeriod` 50 ms:
predicts short stalls and a much higher rate), F3 (F0 + `max_samples` 50,000: tells the subscriber bottleneck at
Array1k 0% from a writer-history one), with CycloneDDS as the control; (3) publish a "vendor-tuned" Fast DDS arm
beside the defaults, labelled as such. (1) is done and (2) is ready to run: `experiments/fastdds_keepall_arms.sh`
(profiles `fastdds/fastdds_keepall_F*.xml`, reading rules in `fastdds_keepall_arms_summary.py`), ~30 min of rig.

### QoS reshaped like rmw (agreed with the user 2026-10-08; supersedes the two 2026-10-05 items)

Replaces "RESOURCE_LIMITS shaped like DDS" and "A core QoS API shaped like rmw's" (2026-10-05). Discussion material:
`~/claude_reports/QOS_DDS_SCOPE_2026-10-08.ko.md`. Decided:

- **rmw's model, not DDS's entity scopes.** QoS is per endpoint (publisher, subscriber, client, server), as
  `rmw_qos_profile_t`; context-wide settings are init options, as `rmw_init_options_t`. No Topic or group-level QoS.
- **rmw's defaults** (`rmw_qos_profile_default`: RELIABLE, KEEP_LAST 10, VOLATILE, ...), as `tt_QOS_PROFILE_DEFAULT`
  and the other rmw profile constants.
- **`tt_Topic` becomes `tt_TypeSupport`**: the type name and its encode/decode/size functions only (its three unread
  QoS fields go). The topic name is its own create argument, as in rmw: `create_publisher(ctx, pub, &type_support,
  topic_name, &qos, ...)`; `endpoint_id` stays hash(type name, topic name). Services likewise; the retry settings move
  off `tt_Service` (a type) onto the client and server.
- **QoS is given at creation, as in rmw; the field-by-field API goes.**
- **One flat `tt_QosProfile`**: the rmw fields first, same names and order (history, depth, reliability, durability,
  deadline, lifespan, liveliness, liveliness_lease_duration), then TickLE's extensions as plain fields under a one-line
  comment per rmw field they extend - no nested `_ext` structs. 0 in an extension field means "TickLE's computed
  default", so a profile filled from rmw alone works. Ambiguous names get a short prefix (e.g. `call_retry_*`).
  - reliability extensions: `max_blocking_ns` (today `RMW_TICKLE_MAX_BLOCKING_MS`), heartbeat period / piggyback,
    ack solicitation period and watermark, the reader's tracking window, `call_retry_interval_ns` / `call_retry_count`.
  - history extensions: `max_bytes` (the KEEP_ALL / KEEP_LAST byte budget; today `RMW_TICKLE_KEEP_ALL_BYTES` and the
    cache arena), `reorder_slots`. How KEEP_ALL's sample-count bound (the old RESOURCE_LIMITS item) sits beside
    `depth` and `max_bytes` is settled when the work starts.
- **Not QoS goes in options structs**, as rmw's publisher / subscription options: `batch`, `accept_callback`
  (closest: `content_filter_options`), local delivery (`ignore_local_publications`), the writable callback.
- **Context settings in `tt_InitOptions`**, as `rmw_init_options_t`: domain id, discovery options (range, static
  peers), later security; TickLE extensions: broadcast address, announce interval / lease, shared memory, io_uring -
  today compile-time constants and `_tt_CONFIG`.
- **Memory stays the caller's**: the reliable cache, reorder storage and service caches are passed in; a helper
  computes the bytes a profile needs (no malloc in core, FreeRTOS included).
- **Wire, API and struct changes are allowed** (`tt_VERSION` 12 if the announce must carry more for rmw's QoS
  compatibility check, `rmw_qos_profile_check_compatible`'s rules). The wire rule still applies: no test may get worse.
- Order: after the wired work, before Security; rmw_tickle then passes rmw's profile through nearly as is.

### Wired work (the user's order of 2026-09-29: finish wired, then Security, then wireless)

- **Receive-buffer lending** (`tt_Sample_retain`/`release`, SHM stage 2 / S7): most of the same-host win is
  here, and it is the machinery loans need. User-approved 2026-09-28. (PLAN "wired work" #2; SHM_PLAN 5; RMW_GAPS S7)
- **Loaned messages** (`rmw_borrow_loaned_message` and five more): the last ❌ in the API table, 0 of 6 today.
  (PLAN #3; COMPARISON 2.7a)
- **Large-message stage 2** (samples above 64 KB): user's staged-support decision. It reuses lending's machinery
  and is not yet pre-registered. (PLAN #4; LARGE_MESSAGE_PLAN)
- **Introspection round-trip rows** (names, type names, GIDs, event counts, serialization format): only the QoS row
  is done. (PLAN #6; RMW_GAPS g14 generalisation)
- **Housekeeping**: delete the merged ablation branches `d4-state-lock-chunk-on-main` and `d5-inline-skip-gate`.
  Nothing blocks this since WIRE 10.5. (PLAN #8; WIRE_PLAN 10.5)
- **Same-host discovery**: keep UDP for discovery, or move it into the segment? Moving it means no network stack
  on embedded. (SHM_PLAN 7 q2)
- **FreeRTOS form of the segment** (HAL-provided rather than `shm_open`): it shapes the seam. (SHM_PLAN 7 q3)
- **Stage 1 / S1 against the WIRE 10.4 floors** (CPU, RSS and size, p1-p4): the module's "no cost when off" is still
  owed. g15's uncounted zero-copy sends close with it. (SHM_PLAN 6b; RMW_GAPS S1/S3/g15; MODULE_PLAN 3)
- **Refuse the shm-only submessage when it arrives on the socket, with a test**: stops injection from the network.
  (SHM_PLAN 6e(a))
- **S9 window A**: a service datagram too large for one path sends one leg over UDP and one in shm, and may
  reorder. It can be measured now. (RMW_GAPS S9)
- **Received samples carry an all-zero publisher_gid**: tools cannot match a sample to its writer.
  (RMW_GAPS g14 generalisation)
- **Run the rmw behaviour tests locally in the netns**: today they run only in CI. (RMW_GAPS "Known gap in the local
  gates")

### rmw latency and correctness

- **rmw kernel wake (~6 us) and poll-to-executor handoff (~21 us)**: the one segment where the vendors are faster.
  (COMPARISON "Where TickLE does not come first"; COMPARISON to-do 3; RMW_PERF_PLAN 8.4/8.5; WIRE_PLAN 5)
- **C++ subscriber segfault, once in ~370 runs**: cause unknown and never reproduced. (PLAN "Open", rmw tests)
- **Discovery peer-registration race**: a first DATA can still be broadcast before its peer is registered.
  (DISCOVERY_PLAN 9)
- **Reorder buffer smaller than the window causes a re-request storm**: integrators who size the buffer small are
  exposed. Add a size check or stop re-requesting what cannot be held. (COMPARISON to-do 17)

### Measurement debt

- **Re-run the c6 cell with the pre-registered `retransmitted` figure (~46x)**: it is not yet comparable.
  (COMPARISON 2.8; OPTIMIZATION_PLAN 9a)
- **Re-measure the reliable_throughput cells marked ◊ under `taskset -c 1-3`**: the CPU0 IRQ coin flip.
  (COMPARISON to-do 14; PLAN "still outstanding")
- **Re-measure COMPARISON's A/T/V rows after large-message stage 1**: those rows read slightly optimistic on client
  CPU and RSS. (WIRE_PLAN 10)
- **Confirm DATA_FRAG's under-loss throughput (527-536 / 830-857 Mbps) in the full cross-vendor campaign**: the gain
  was not predicted. (DATAFRAG_PLAN 14)
- **DATA_FRAG follow-ups**: arm C with a 30 s FastDDS drain cap, a raw-line re-run of the 4-fragment arm, the
  unexplained 0.57 duplicates per loss on the rig. (DATAFRAG_PLAN 12, 16)
- **Discovery join time (M3) is not measured**: `discovery_join.sh` must be adapted first.
  (CONTEXT_NODE_PLAN stage 3 result)
- **rmw_zenoh QoS rows 42-44 and 47 are not measured**: the QoS probe now exists to measure them.
  (COMPARISON 2.6)
- **Harness residuals**: scenario 6's late-join loss varies from run to run (to-do 2), scenario 9 pauses for
  different times per arm (to-do 6), the LIFESPAN pause is bimodal (to-do 8). (COMPARISON to-do 2/6/8)
- **FastDDS liveliness server's unsynchronised globals**: a formal data race. Low priority; fix it when the file is
  next touched. (COMPARISON to-do 16)

### Throughput and CPU

- **Poll-loop I/O interleave branch** (`experiment/poll-loop-io-interleave-v2`): ~15-25% more throughput, never
  merged. (COMPARISON to-do 11)
- **recvmmsg rows processed in place, then scatter-gather for large fields**: copies remain on the receive path.
  (OPTIMIZATION_PLAN 12.1/12.2)
- **Decide what `rmw-perf.yml` and the two dashboard charts become**: recorded but never decided.
  (PLAN "standing methodology")

## Parked - only on the user's explicit go

- **SECURITY_PLAN: SROS2-compatible security with MACsec**: five open questions. Nothing is to be designed until
  the user starts it, and it comes before wireless. (SECURITY_PLAN; MODULE_PLAN 0/4; SHM_PLAN 7 q4)
- **WIRELESS_PLAN** (W1 correlated loss + delay, W2 Wi-Fi, W3 internet, relay, congestion control): queued behind
  Security. (WIRELESS_PLAN; PLAN "Priority")
- **OS IP fragmentation under loss**: deferred by the user. The sources record DATA_FRAG as the answer since
  2026-09-26. (COMPARISON to-do 15; DATAFRAG_PLAN header)
- **zenoh-pico p2-p4 reliable cells stay not measurable**: zenoh-pico does not repair loss. Never quote the
  rate-limited figure. (ZENOH_PICO_PLAN 1; COMPARISON 4.4a)
- **O(1) `find_resendable_cache_entry()`**: a deep cache makes loss worse. Pending the user's go-ahead.
  (COMPARISON to-do 9)
- **KEEP_LAST cache default of 1 MiB**: it retains fewer than depth for max-size samples. The user's call.
  (PLAN, cache budgets)
- **Full segment ring: back-pressure the writer or drop?** This defines the module's promise. Awaits the user's
  decision 6. (SHM_PLAN 6c)
- **Rig interrupt coalescing** (macb rx/tx-usecs 49): part of the 168 us wire time. It is a rig setting.
  (RMW_PERF_PLAN 8)
- **Perf access on the rig or PC**: blocks Now item 3. (COMPARISON 2.2c)
- **Anything on 10BASE-T1S**: cross-host rmw over the real medium, and revisiting duplicate suppression where
  bandwidth binds. Waits on T1S hardware on the rig. (COMPARISON to-do 5; DATAFRAG_PLAN 16.1)

### The parked security plan, in short

Parked by the user on 2026-09-27 ("이건 내가 실행 하자고 할 때까지 실행하지 말자"); nothing is designed or coded. Only g7
went ahead: refuse to start under `ROS_SECURITY_ENFORCEMENT=Enforce`, and say once that security is not applied.

- **Idea:** MACsec (802.1AE) gives every link confidentiality and integrity. TickLE reads the same SROS2 keystore
  and enclave files and adds what MACsec lacks: per-process authentication on the enclave certificate, access control
  from the signed permissions at endpoint creation and matching, and optional signing or encryption for intra-host
  traffic and paths MACsec does not cover. SROS2 file-compatible, not DDS-Security wire-compatible.
- **Open questions:** (1) must authentication be mandatory whenever access control is on (Plan: yes); (2) crypto
  library and budget on FreeRTOS (mbedTLS?); (3) the wire change and its bytes under the wire rule; (4) what
  "enforced" means when MACsec cannot be verified from user space; (5) testing against a CycloneDDS SROS2 control
  and the rig's links with MACsec on.

### rmw_tickle at REP-2004 Quality Level 2 (the user, 2026-10-05; after the security plan, the user, 2026-10-06)

Deferred until after the parked security plan (SECURITY_PLAN), including the switch to pull requests and the
disclosure policy.

The goal: from Quality Level 4 (`rmw_tickle/rmw_tickle/QUALITY_DECLARATION.md`) to Level 2. What Level 2 asks and
what is missing, in order:

1. **Every change through a pull request**, with DCO sign-off checked and CI required before merge (REP-2004 2.i,
   2.ii, 2.iv). Today changes are pushed to `main` after the local gates, unsigned. Needs the user's decision,
   since it changes how every session works. (Peer review is optional at Level 2 and required at Level 1.)
2. **Coverage tracking** in CI for core and rmw_tickle; Level 2 tracks it, Level 1 enforces a bound (4.iii).
   Core: done - `make coverage` (gcovr, unit tests, HALs excluded), run by `test-all.yml` with the table in the job
   summary and the reports as an artifact; 90.6% of lines and 72.6% of branches on 2026-10-05. rmw_tickle: open -
   CI's colcon build in `check-all.yml` is not built with `--coverage`; an untested recipe is in the declaration's
   4.iii.
3. **A vulnerability disclosure policy** (`SECURITY.md`, REP-2006 response schedule) (7.i).
4. **Dependencies at Level 2 or better** (5): quality declarations for `rosidl_typesupport_tickle_c` and
   `rosidl_typesupport_tickle_cpp`, and a written quality justification for TickLE core, rmw_tickle's one non-ROS
   dependency.
5. **A stable version** (`>= 1.0.0`) with declared API and ABI stability policies (1.ii, 1.iv, 1.v).
6. **All REP-2000 Tier 1 platforms** (6): Linux only today. Check jazzy's Tier 1 list; a platform not supported
   (Windows, if listed) needs a HAL port or a documented exception.


## Done recently (2026-10-04/05)

- io_uring receive hint: a busy context learns of arrivals without an empty read. (`e19c349f`)
- FIFO segment doorbell: 1.42x the UDP doorbell's rate at p3, and publisher CPU per sample down 24%. (`e17b4e6f`)
- Same-host p2 latency: the segment now saves a third of a round trip. (`674f0dcb`)
- ACK solicitation clocked by the ACKNACK that answers it, plus solicit-by-data. (`d7e02846`, `8d1c3712`)
- Per-sleep doorbell fixes the lost wakeup: same-host RELIABLE p4 is 2.35x CycloneDDS. (`fe45f276`, `313cda37`)
- COMPARISON gets its own same-host table (1a), ours on every latency cell. (`c746572c`, `63f00280`)
- Cross-host check of the last two core changes: 20 better, 121 held, 9 within the layout floor. (`c29630ff`)
- QoS probe: rmw_zenoh cannot report deadline or liveliness events. (`7f127d56`)
- Layout control: p1's +0.2% is not function alignment. Recorded and left. (`7f127d56`)
- Mixed delivery: one publisher, one subscriber on its host and one across the link. (`3c0c505b`)
