# Adaptive send pacing - design and acceptance criteria (draft, 2026-11-19)

Status: DRAFT, 2026-11-19. Branch `ab/adaptive-pacing`, off `ab/release-2.2.0`. The estimator and its model test exist
(section 10); the library integration is being written on this branch (section 11). Nothing is measured on a real link.
The option is opt-in and **off by default everywhere**; making it the WIFI profile's default is a separate decision for
the user, taken after the validation below.

User decisions of 2026-11-19, applied below: the option is `tt_LinkOptions.rate_control` (not `pacing_mode`); the
adaptive code is always compiled in and behaves exactly as before when `rate_control` is off or fixed (the wired gates
and the core gate prove that; no byte-identical-objects requirement); rate control is per link, with link-level
information pooled over all readers on the link (section 4.2 item 5).

Numbers written `[X]` or `[N]` are placeholders to be filled from the tc runs (Stage C oracle rates, rate-step test);
constants written `(initial)` are starting choices to be fixed by the userspace model and the tc runs, not results.

## 1. Problem

A large reliable sample (1 MiB = 723 units) leaves as one back-to-back burst, then its repairs leave as further bursts.
A link that carries less than the burst (a small queue behind a rate limit) drops most of it; the retransmissions drop
too. tc/netem, S1 (4 Mbit/s, queue 40), 1 MiB: 263,602 datagrams sent, 251,915 of them retransmissions, p50 5907 ms
against CycloneDDS 2514 ms. A fixed `pacing_rate` at 75 % of the carried rate fixes it (11,187 datagrams) - but only
when the user knows the rate, and on a real wireless link the rate moves. The aim is a pacer that finds the rate by
itself, keeps the queue short (TickLE is real-time: a standing queue is latency), and leaves everything that is not a
large unicast reliable sample alone.

## 2. What the code gives today (read 2026-11-19, `src/tickle.c`, `include/tickle/tickle.h`)

**The pacer** (`struct tt_Pacer`, "Send pacing"): one token bucket per link, kept as `until_ns`, the time its sends so
far finish at the pacing rate; `pace_charge()` adds every UDP datagram the context sends on the link (any kind, at the
seam); `pace_units()` lets a large sample's units go while backlog + cost stays within `burst_ns`, else the send cursor
stops and the scheduler resumes it at `pace_at_ns`; `pace_repair_allowed()` lets a retransmission go while the backlog
is within two bursts, else it is dropped and counted (`pacing_repairs_deferred`) - the reader asks again. Callers test
`node->pacing` first: a context with no paced link pays one branch. Small samples, discovery, summary, HEARTBEAT are
charged but never wait. Rate and burst are fixed at init (`tt_LinkOptions.pacing_rate`, `pacing_burst`, validated in
`validate_link_pacing()`).

**What comes back from the reader** (no field is missing for the signals below):
- `tt_AckNackHeader`: `seq_no` = cumulative ack (every unit below it received) and `bitmap` of the units still missing
  above it. For a large sample each *unit* (fragment) has its own `seq_no` (the repair code speaks of "every unit below
  its seq_no, the watermark"), so the watermark advance between two ACKNACKs of one reader is a count of delivered
  units, computed by `process_acknack()` / `large_order_begin()` already (`prev_ack` .. `seq_no`).
- Writer-side use of ACKNACKs already has a precedent: `record_peer_ack()`, `hybrid_note_nacks()` (loss windows),
  `large_order_begin()` (what the reader shows received).
- A reader sends an ACKNACK on a gap (immediately on a new gap, then on the retry timer) and **not at all on a clean
  stream** ("a healthy stream needs no ACKNACK at all"). The writer can ask for one: `tt_Publisher_request_ack()` /
  `solicit_ack_throttled()` send a HEARTBEAT with `tt_HEARTBEAT_FLAG_FINAL` clear, and the reader must answer with an
  ACKNACK whatever its gap state. No wire change.
- RTT: the *reader* times recoveries (`recovery_srtt_ns`, RFC 6298 fold, Karn); a Publisher has no estimate of its own
  (comment at `reliable_retry_interval_publisher()`). `tt_Context.announce_rtt_ns[peer]` is a first round trip to a
  context (announce to answer), usable as the RTprop seed.

**What does NOT exist, and is local state only (no wire change):**
- a writer-side send timestamp per unit (`struct tt_LargeRepair` keeps `resent_at`/`serial` for repairs only);
- a delivery-rate sample and its filters;
- a writer-side RTT sample (a solicited HEARTBEAT's send time against the ACKNACK that answers it).

**Verdict for step 1: delivery rate and RTprop can both be derived at the writer with NO wire change**, from (a) the
ACKNACK watermark, (b) writer-local send stamps, (c) a solicited HEARTBEAT where the stream is clean. The same facts
mean the design needs a positive-feedback source on a clean stream (section 4.2), which BBR gets for free from TCP's
per-packet ACKs and TickLE does not.

## 3. Idea (BBR-inspired, not BBR)

Do not read loss as congestion (on Wi-Fi most loss is radio, not queue), and do not fill the queue to find the rate.
Estimate two things and operate at the knee between them:

- `BtlBw`: the bottleneck's delivery rate, the **windowed maximum** of delivery-rate samples.
- `RTprop`: the propagation round trip, the **windowed minimum** of RTT samples (the RTT with an empty queue).

Pacing rate = `gain x BtlBw`, bounded by `[pacing_min_rate, cap]`; data in flight (units sent, not yet under the
watermark) bounded by `inflight_gain x BtlBw x RTprop` (the BDP). That holds the link at its delivery rate with an
almost empty queue. The difference from TCP: TCP buys throughput with a full queue; TickLE buys latency with an empty
one, and gives up the last few percent of throughput to keep it.

## 4. Mechanism

### 4.1 Where it applies
- Only on a link with `pacing_mode == tt_PACING_ADAPTIVE`, only for **unicast reliable large-sample units and their
  repairs** (the traffic `pace_units()` and `pace_repair_allowed()` gate today). `tt_LARGE_SAMPLES` builds only.
- **Bypass (never waits, still charged):** small samples, discovery announce/summary, HEARTBEAT, ACKNACK, calls,
  BEST_EFFORT data, broadcast/multicast. Charging them keeps the bucket honest; making them wait would put a large
  sample's queue in front of a small sample's latency, which is exactly what this design exists to prevent.
- Broadcast and BEST_EFFORT get no feedback, so they are not adapted.

### 4.2 Feedback (writer-local, no wire change)
1. **Send stamps:** every `K`-th unit of a paced send (K `(initial)` 8) records `{seq_no, send_ns, delivered_at_send}`
   in a small ring per link (`(initial)` 64 entries; sparse so that RAM stays small on embedded builds).
2. **Positive feedback:** while a paced large send has data in flight and no ACKNACK has arrived for one `RTprop` (or
   `(initial)` 10 ms before the first sample), the pacer sends one throttled solicited HEARTBEAT (`FINAL` clear) via
   `solicit_ack_throttled()`. At most one outstanding per reader; one small datagram per RTprop at most.
3. **Delivery-rate sample:** one per *round*. A round ends at the first feedback at which at least one RTprop has
   passed **and** at least `AP_MIN_SAMPLE_BYTES` (24 kB, `(initial)`) were delivered, or 8 RTprop have passed. The
   byte floor matters: on a few-frame BDP (4 Mbit/s x 20 ms = 7 frames) a sample over one RTprop is quantised by a
   frame to 15 % of the rate, and a max filter over such samples overshoots the link by that much for good (found in
   the model, section 10). The sample is `delivered / round time`; `delivered` is the watermark advance (plus what the
   bitmap shows received) x unit bytes. A round in which the pacer sent less than 80 % of its allowance
   (application-limited) only counts if it raises BtlBw.
4. **RTT sample:** a solicited HEARTBEAT's send time against the next ACKNACK from that reader. It can only be too
   long (the reader's poll delay, an unrelated gap ACKNACK arriving first), never too short, which the windowed
   *minimum* tolerates; the max filter tolerates the matching under-estimate of the delivery rate. Both errors are in
   the safe direction (rate a little low, BDP a little high); the probe recovers the former.
5. **Several readers on one link (link-level state, user decision 2026-11-19):** the bucket, the estimator and the
   rate are per **link**, not per reader. Every ACKNACK from any reader on the link feeds the one estimator:
   - *delivered* is pooled: each reader's ACKNACK advances that reader's own delivered count (watermark plus the
     received units its bitmap shows), the increment is converted to bytes and added to one link-wide cumulative
     delivered counter; a round's delivery-rate sample is the pooled bytes over the round, i.e. the **sum of the
     readers' delivery rates**. Unicast reliable sends of one sample to N readers cross the link N times, and the pooled
     sample counts all N copies, which is what the bottleneck carries;
   - *RTprop* is the **minimum over all readers' RTT samples** (the filter already is a min filter; a reader whose
     poll loop is slow only adds larger samples that it ignores);
   - the solicit: one solicited HEARTBEAT per RTprop goes to all matched readers (as `tt_Publisher_request_ack()` does),
     so every reader answers and every answer is a sample; the first answer gives the RTT sample;
   - the queue guard, drain and drop guards are link-level for the same reason: a queue is shared by all readers.
   Remaining limit (stated, not hidden): readers' feedback arrives at different instants, so a round can end between
   two readers' answers and its sample covers a partial set. The byte floor per round (24 kB) and the max filter keep
   that bounded; the model test (section 10) runs 1, 2 and 3 readers to check it.

### 4.3 State machine (per link)
- **START** (no rate remembered): rate = floor `(initial)`; RTprop seeded from `announce_rtt_ns` of the peer. Each
  round the rate doubles while delivery-rate samples still grow by 25 %.
- **PLATEAU:** three rounds without 25 % growth: BtlBw is found; go to CRUISE. Repairs seen in START (lost units
  during doubling) are expected and count as budget, not as a signal.
- **After idle** (the pacer sent nothing for 1 s, as between samples at 0.25 Hz): the send starts in CRUISE at the
  remembered BtlBw and probes after its first round, instead of starting over in START. The BtlBw filter ages in
  *active rounds*, so idle time does not empty it; a link that got slower meanwhile is caught by the queue guard.
- **CRUISE:** rate = BtlBw (the max over the last 8 active rounds). Every 8 rounds a probe round at 1.25 x BtlBw;
  a probe that raised the delivery rate by 10 % is followed at once by another at 2 x (the link got faster), else by
  a drain round at 0.75 x for the queue it made. RTprop is the min over 10 s of wall time (a property of the path).
- **Queue guard:** if `RTT - RTprop > queue_target` on two consecutive samples: stop probing, take the delivery
  rate of the round so far as BtlBw if it is lower, and **drain** at 0.75 x until RTT is back under half the target;
  queue_target = max(2 ms, RTprop / 4) `(initial)`. This is the real-time term: bound the queueing delay, not just
  the loss.
- **Rate-drop guard (fast down):** repairs > 50 % of the bytes sent in a round, or delivery rate below half of BtlBw
  for two rounds: set BtlBw to the latest sample at once instead of waiting for the max filter to age out.
- **In-flight cap (`ap_inflight_limit()`, 2 x BDP): specified, not used by the model.** Lost units would have to leave
  the in-flight count when the reader names them missing, which needs a per-unit state the model does not have. The
  model holds the queue with rate pacing and the guards alone; whether the cap is needed is decided on the Pis.
- **Floor and cap:** `pacing_min_rate` (default `tt_LINK_RATE_MIN`) and the cap `pacing_rate` (default `link_rate`),
  so an adaptive link can still be told "never above this".

### 4.4 Repairs and completion
- Repairs are charged to the same bucket and go **ahead of new units** within it (today's order); the deferral
  threshold scales with the adaptive burst. A sample's completion time is bounded below by `size / BtlBw`; sending
  faster only converts the excess into loss, so the target is that bound plus a few RTprop.

## 5. API (opt-in, zero = today)

```c
#define tt_RATE_CONTROL_OFF      0U  // the default: see "compatibility" below
#define tt_RATE_CONTROL_FIXED    1U  // send pacing at pacing_rate, as today
#define tt_RATE_CONTROL_ADAPTIVE 2U  // find the rate: this document
struct tt_LinkOptions {
    ...                          // existing fields unchanged, in place
    uint8_t  rate_control;       // tt_RATE_CONTROL_*; 0 = see below
    uint64_t rate_min_rate;      // ADAPTIVE only: the floor, bits/s; 0 = tt_RATE_MIN_DEFAULT (100 kbit/s) or the cap
    uint32_t rate_queue_ns;      // ADAPTIVE only: queue-delay target; 0 = max(2 ms, RTprop / 4)
};
```
Names: the user's `rate_control`; the two tunables are named `rate_min_rate` and `rate_queue_ns` (not `pacing_*`), so
that every adaptive-only field reads as belonging to `rate_control`; `pacing_rate` and `pacing_burst` keep their names
and meaning.
- **Compatibility (the zero value).** `rate_control == tt_RATE_CONTROL_OFF` (0) does not mean "no pacing"; it means
  "what the existing fields say", so every existing program is unchanged: `pacing_rate` set (neither 0 nor
  `tt_PACING_OFF`) is FIXED at that rate; `pacing_rate == 0` is the profile's (T1S: FIXED at 75 % of `link_rate`; the
  other profiles: not paced, as now); `pacing_rate == tt_PACING_OFF` is not paced. An explicit `FIXED` requires a
  `pacing_rate` (0 = the profile's, which must then exist, else rejected). After resolution `rate_control` holds the
  concrete mode (OFF, FIXED or ADAPTIVE), as `pacing_rate` already holds the concrete rate.
- Under `ADAPTIVE`, `pacing_rate` is the **cap**; 0 = `link_rate`. `tt_PACING_OFF` with ADAPTIVE is rejected.
  `pacing_burst` keeps its meaning (the bucket depth in bytes on the wire, 0 = 4 frames); the bucket's rate follows the
  estimator.
- `validate_link_pacing()`: `rate_control` is in {0, 1, 2}; `rate_min_rate` and `rate_queue_ns` only with ADAPTIVE;
  floor >= `tt_LINK_RATE_MIN` and <= the cap; `rate_queue_ns` at least 100 us; ADAPTIVE only in a build with
  `tt_LARGE_SAMPLES` (the feedback it reads is the large-sample send's), else `tt_RET_INVALID_ARGUMENT`.
- The WIFI profile's default stays off. The user guide says "on Wi-Fi set `rate_control = tt_RATE_CONTROL_ADAPTIVE`"
  only as a plan until the Pi results exist (section 8); no result is claimed before then.
- The code is always compiled in (user decision); with OFF or FIXED the send path takes the branches it takes today.
- Counters (`tt_Context`): rate samples taken, solicitations sent, queue-guard trips, rate-drop trips, current BtlBw
  and RTprop (read-only), so a run can be diagnosed.

## 6. Failure modes (what can go wrong, and what covers it)

| Mode | Effect | Cover |
|---|---|---|
| Rate drops (Wi-Fi rate step down) | the max filter holds the old BtlBw for its window; queue builds, loss | queue guard and rate-drop guard (section 4.3); acceptance step test |
| ACK compression / reader poll jitter | delivery samples spike high | interval >= RTprop; solicited replies only; max filter fed only by samples over a full interval |
| Reader slow (CPU), not the link | looks like a low rate | accepted: sending faster would not help; the rate recovers by probing |
| Clean stream, no ACKNACK | no feedback, rate cannot rise | solicited HEARTBEAT (4.2 item 2); cost one small datagram per RTprop |
| Several readers on one link | per-reader samples mix | v1 one reader; sum rule stated, to be validated |
| Several TickLE writers on one link | each probes; bandwidth shared roughly | not addressed in v1; documented |
| RTT inflated by reader | BDP too big | min filter; error is in the safe direction |
| First sample late (RTprop unknown) | START too slow or too fast | seed from `announce_rtt_ns`; START cap |
| Probing causes a drop | one extra loss burst per probe | probe is 1.25 x for one RTprop only; queue guard |

## 7. Acceptance criteria

Nothing is claimed until each row is measured. `[X]`, `[N]` come from the Stage C runs and the rate-step run.

| # | Criterion | How |
|---|---|---|
| A1 | `rate_control` off or fixed (every profile by default): wire identical, bytes and times, to the parent; ETHERNET/T1S/TSN/WIFI unchanged | `tests/wire_identity_send_pacing.sh` against the parent, plus the wired gates |
| A2 | Core gate v2 (158 deciding comparisons): 0 WORSE, adaptive off | rig core gate |
| A3 | Wired DDS comparison: no LOSE that the parent did not have | the 136/14/0 campaign, adaptive off (unchanged path) |
| A4 | Rate-step 30 -> 4 -> 30 Mbit/s (tc, 1 reader): BtlBw within 25 % of the carried rate within `[N]` s after each step; queue delay bounded; no run lost | tc time-varying schedule, Pis |
| A5 | Within `[X]` % of the Stage C **oracle** (best fixed pacing_rate per profile class) on 1 MiB / 128 KiB p50 and p99, same cells | tc cells, >= 10 reps each, A/A noise rule |
| A6 | No LOSE against Fast DDS or CycloneDDS in any tc cell (M1, M2, S1, S2; be_small, rel_small, rel_large 128 KiB, 1 MiB) | the Stage A/B/C scoring |
| A7 | Small samples and be_small: no regression (they bypass the pacer) | same cells, adaptive on vs off |
| A8 | Datagram count at 1 MiB / S1 within `[X]` x the oracle's (no retransmission storm) | counters |
| A9 | Unit tests: estimator converges on synthetic rate steps, queue bounded, floor/cap honoured; every `validate_link_pacing()` bound refused and accepted | `tests/test_adaptive_pacing.c`, mutants as for `test_send_pacing.c` |
| A10 | Context size delta and send-path size delta reported for the three builds (the adaptive code is always compiled in; no byte-identity requirement) | `size`, `sizeof(struct tt_Context)` |
| A11 | 2 and 3 readers on one link (link-level pooled estimate): the aggregate rate follows the link, the queue stays bounded, no reader starves | model test (section 10), then Pis with 2 and 3 reader processes in one namespace |

## 8. Validation the Pis need once Stage C is done

1. **Rate-step test** (A4): a tc schedule 30 -> 4 -> 30 Mbit/s inside the netns pair, 1 MiB at 0.25 Hz, 60 s, adaptive
   vs oracle vs default, >= 10 runs; the existing wifitc harness plus a netem-schedule driver.
2. **Cell sweep** (A5-A8): M1, M2, S1, S2 x {be_small, rel_small, rel_large 128 KiB, 1 MiB}, adaptive on, against the
   Stage A/B/C data (same link, same DDS arms), 10 pooled runs per deciding cell.
3. Core gate v2 and the wired DDS comparison with adaptive off (A2, A3) - the ordinary release checks.
4. Estimator-trace run: one 1 MiB per profile with the counters dumped, to compare the estimator's BtlBw/RTprop with
   the calibrated carried rate and ping RTT.

## 9. Open for the user
- Whether adaptive rate control becomes the WIFI default (only after A1-A8 and A11).
- Decided 2026-11-19: option name `rate_control`; always compiled in; per-link pooled multi-reader state.

## 10. Userspace model (step 3, 2026-11-19)

`src/pacing_adaptive.h` is the estimator as plain C (nothing in the library includes it yet);
`tests/test_adaptive_pacing.c` drives it with a synthetic link (FIFO queue with a byte limit behind a rate schedule,
10 ms each way, a reader that reports what arrived and, through the solicit's last-sent unit, what is missing, a writer
that repairs what is named missing). It is a MODEL: the thresholds in it are what the design needs of the estimator on
an idealised link, not a performance claim; real numbers come from the Pis (section 8). What it showed, and what it
changed in the design:

- First version: BtlBw overshot the link by the frame quantisation of its samples and kept a full queue (mean 102 ms at
  4 Mbit/s with 186 repairs). Fixed by the byte floor per round and the drain rounds (section 4.2 item 3, 4.3).
- A step 30 -> 4 Mbit/s collapsed BtlBw to a tenth of the link and never came back: probing 1.25 x every 8 rounds could
  not climb out. Fixed by chaining probes while they succeed (2 x) and a success threshold of 10 %.
- Restarting in START after every idle period overshot on every sample (11 % repairs at 30 Mbit/s); remembered-rate
  restart removed it.
- The model also lost a sample whose tail was lost, until the solicit carried the writer's last sent unit: in TickLE
  that is the HEARTBEAT's `last_seq_no`, so the reader names the tail missing. The solicited HEARTBEAT is therefore
  not only a feedback source but how a lost tail is found.

Result on the final model (4 Mbit/s queue 40: 1 MiB every 4 s; 30 -> 4 -> 30 Mbit/s bulk): mean queue delay 2.4 ms and
no repair on the 4 Mbit/s link; BtlBw within 25 % of the link after the start in 1.0 s, after the drop in 0.24 s, after
the rise in 0.24 s; the unpaced control, same link, resends 87 % of what it sends. The model's unpaced control
completes a sample faster than the oracle fixed rate (2445 ms against 3042 ms at 4 Mbit/s) and no slower than the
adaptive pacer's 2367 ms: unlike the real tc runs (5907 ms), the model's writer has no retry-interval slowness, so the
model says nothing about the size of the real gain. Mutants: removing the queue guard or the probing fails a check;
removing the drain rounds (gain 100 %) does **not**, so the drain is not shown to matter in this model.

## 11. Implementation (2026-11-19, branch `ab/adaptive-pacing`)

What is in the library, all behind `tt_LinkOptions.rate_control == tt_RATE_CONTROL_ADAPTIVE` and always compiled in:

- `include/tickle/pacing_adaptive.h`: the estimator (`struct ap_pacer`), plain C.
- `struct tt_RateControl rate[tt_MAX_LINK_COUNT]` at the end of `tt_Context`: the estimator, the link's pooled delivered
  bytes, the unit size, and the solicitation bookkeeping. `struct tt_PeerAck` gains `rate_units` (what a reader's last
  ACKNACK showed it holds).
- `pace_charge()` (the seam: every datagram on the link) counts into the estimator; `pace_repair_allowed()` counts a
  repair that goes; `rate_acknack()` (from `process_acknack()`) turns a reader's ACKNACK into delivered bytes and an RTT
  sample; `large_paced_count()` keeps one solicitation in flight while a paced large send runs;
  `solicit_ack_throttled()` records where the next answer falls; `rate_sync()` gives the bucket the estimator's rate.
- Options and validation: `resolve_link_rate_control()`, `validate_link_pacing()`; the zero value is what `pacing_rate`
  says, so existing programs are unchanged (`tests/test_rate_control.c`).

Two defects the real stack showed that the synthetic model had hidden, both fixed and kept as tests: a loopback run
(2 readers, `TT_RATE_CONTROL=adaptive`, 1 MiB) delivered nothing because the rate collapsed to a few kbit/s - (1) a
unit was counted as the average datagram on the link, which the solicitations and ACKNACKs (90 bytes each) pulled
down to a fraction of a frame, so every delivery read as a fraction of what it was (now only datagrams of at least half
a frame count: `test_control_datagrams_are_not_units`); (2) a round's length followed the round trip alone, 7 us on a
loopback, so a round held a frame or two (now at least `AP_MIN_ROUND_NS`, 1 ms). After the fix the same loopback run
delivers 5 of 5 samples to both readers; the ramp from the 1 Mbit/s start costs about 10-20 ms of latency per sample
there (baseline about 1 ms), which a real link at 4-30 Mbit/s does not show this way. This is a functional check on one
host, not a measurement.

Known limits: the first ACKNACK read after a solicitation is taken as its answer even when it was sent for another
reason, a round trip that can only be too short (a low RTprop; the queue guard then holds the rate down, not up); the
delivered count reads the watermark plus the received units the bitmap names, so units received above the reader's
highest missing unit show up one ACKNACK later; `btlbw_bps` can read above the cap on a loopback (the rate itself is
clamped to the cap).

## 12. Validating on the Pis (commands; nothing here has been run on a Pi)

Harness: `examples/perf_hil/experiments/wifitc/` (this branch has `ab/wifi-tc` merged). New in it: the arm `tickle-ad`
(`tickle-ad-c<Mbit/s>` capped) which sets `TT_RATE_CONTROL=adaptive` for the `large` and `node_d100` programs,
`RATE_STEP`, `READERS`. See `wifitc/ADAPTIVE_PLAN.md` for the plan and `wifitc/wtc_chainD.sh` for the chains. Build and
ship the binaries of this branch first: `examples/perf_hil/experiments/t1pi/t1pi_build.sh`.

1. **S1 stage (ADAPTIVE vs the DDS stacks, one reader):**
   `OUT_BASE=$HOME/rig_results_safe/wtc_adaptive_$(date +%Y%m%d) examples/perf_hil/experiments/wifitc/wtc_chainD.sh s1`
2. **All four profiles, 1 MiB and 128 KiB, and the small cells (no regression):** `... wtc_chainD.sh all`
3. **Rate step 30 -> 4 -> 30 Mbit/s:** `... wtc_chainD.sh step`
4. **2 and 3 readers on the link:** `... wtc_chainD.sh readers`

Each run writes `<run>.near`, `.far` (and `.far2`, `.far3`), `.steps` for the rate step; the RESULT lines carry
`rate_control`, `rate_bps`, `btlbw_bps`, `rtprop_ns`, `rate_rounds`, `rate_queue_trips`, `rate_drop_trips` for the
estimator. Score as the earlier tc stages did (WIN/TIE/LOSE against each DDS stack, A/A noise rule, state the exact link
settings in every row).

## 13. The S1 failure of b9587eaa on the Pis, and what was found (2026-11-20)

Symptom (`wifitc/results/ADAPTIVE_S1.md`): at 1 MiB the estimate ended at the 100 kbit/s floor with `btlbw_bps` 0 in every run,
0-2 of 15 samples delivered; `rtprop_ns` read 0.01-4.5 ms on 10 and 40 ms paths.

Reproduced in the model before the fix (`tests/test_adaptive_pacing.c`, scenarios "S1-like, 1 % loss" and "M2-like, 3 % loss":
1 MiB every 6 s for 60 s, the reader reporting a watermark and a bitmap and by itself at its retry timer, the first report after
a solicitation read as its answer): old estimator, M2-like: **1 of 10 samples, the rate at the 100 kbit/s floor, 1407 queue
trips**; S1-like: 10 samples but a queue of 90 ms on average and 135 drop trips. The same scenarios with the fix: 10 of 10 and
10 of 10, mean queue 1.0 ms and 1.3 ms, no run at the floor.

Causes, in the order the evidence showed them, each fixed and tested:

1. **Round trip from the solicitation.** A report sent for another reason before the answer read as the answer, a round trip
   shorter than the path; the minimum filter kept it, and the queue guard (round trip over that minimum) then saw a queue on every
   sample. Now the round trip is the time from a unit's send to a report that shows that unit (or a later one) received -
   never shorter than the path (`ap_note_delivered_units()`, `test_the_round_trip_is_from_unit_send_times`), the guard reads the
   least of the last two samples against a target that is at least two units of send time, and `top_units` counts the units
   missing in front of a held one so that losses do not make every sample longer.
2. **A solicitation storm** (found on a loopback, not in the model): a large send asked for a report as soon as the last was
   answered, thousands a second, each 156 bytes on the wire, which the bucket charged. At a slow start (1 Mbit/s) or at the floor
   that is more than the rate: the data starved (a loopback run delivered 0 of 5 with 29,072 datagrams sent, 907 after the
   fix and 4 of 4 delivered in three runs), and on the Pis a rate that fell to the floor stayed there. Solicitations are now
   at least `max(round trip, 1 ms, two units of send time)` apart (`ap_solicit_gap_ns()`), about 5 % of the rate.
3. **Rounds with nothing delivered were samples of 0.** A round now needs delivery to end; sending a second with none restarts
   the search from the start rate; a guard trip lowers BtlBw to at most a quarter in one step; BtlBw 0 in CRUISE restarts the search
   (`test_adaptive_pacing`: "floor" and "outage" cases).
4. **A counter of "repairs the bucket let go" leaked** (a repair allowed and not sent was never consumed) and swallowed the
   stamps of the units that followed; it is now a flag consumed by the next data datagram.

Not verified: the Pis. The model, the unit tests and a one-host loopback pass; `wifitc/wtc_chainD.sh s1` on the tc links is
the check, the quick test being 15 of 15 delivered at 1 MiB in every cell.

## 14. The quick test of 7f9aff89 on the Pis, and the second round (2026-11-20)

Quick test (1 MiB, 1 rep, tc links): delivered of 15 - M1 15 and 6, M2 8 and 5, S1 6 and 11, S2 9 and 7; `rtprop` 0.02-0.07 ms on 10 and
40 ms paths in half the runs; `btlbw` 0 with the rate at the cap or at the 1 Mbit/s start; 33-50 thousand retransmissions in four runs.
The model of section 13 did not show it. A trace on a loopback did (`tt_RATE_TRACE`, one line a round: per-round delivered, round trip,
rate, counts), and the model now has what it lacked: a tail-dropping netem queue at 10 and 40 ms, independent and Gilbert-Elliott
bursty loss at the M1/M2/S1/S2 settings, readers that report by their retry timer and **leave out of their bitmaps what they named
within a round trip and a half**, and a **watermark that jumps over units nobody held**. What was found:

1. **A watermark jump read as delivery.** When a sample is evicted or given up, the reader's watermark passes its units at once; the
   count advanced by hundreds of units, a megabyte "delivered" in one ACKNACK, the estimate ran to its cap, the cap lost most of it, and the
   guards then took BtlBw to 0 - the pattern of the Pi runs (rate at the cap, BtlBw 0, retransmissions in the tens of thousands). The
   model with a jump every 5 s: the old estimator runs to the 100 Mbit/s cap on a 4 Mbit/s link with 75 % repairs. A count past what was sent
   cannot be delivery: the excess is a jump and is left out for good (`ap_on_feedback()`, `test_a_watermark_jump_is_not_delivery`).
   A jump smaller than the data in flight cannot be told apart; one such costs a short spike, not a collapse.
2. **Reports were as late as the send cursor.** The solicitations were sent from the send cursor, which wakes once per datagram; with a
   run of 44 units (68 kB) at a few Mbit/s that is once every few hundred ms. A report that late reads as a round trip of the interval,
   the queue guard drained, the send slowed, the next report was later: a spiral to the floor, seen in the trace (the round trip sample equal
   to the round's length, the rate falling by a quarter a round). Solicitations now go out on a timer while a large send runs
   (`rate_solicit_timer()`), and the queue target and the solicitation spacing are set by what a report can resolve - four units or one
   data datagram of send time at the pacing rate (`ap_report_resolution_ns()`), whichever is more.
3. **Units per datagram.** A run of n units is one datagram of n frames: the units are now counted from the wire size
   (`wire / unit_wire`), not one per datagram; delivered bytes are held units x the unit size (an average of datagram sizes was ten to
   forty times too large with runs).
4. A try without the queue guard (the round trip no longer steering the rate) failed in the model - queue 55-110 ms, 20-26 % repairs, the rate
   at the floor on S1/S2 - and was dropped; the guard stays, with the larger target.

Model checks that are looser than before, as a consequence of the larger target: the mean queue delay of a single reader on 4 Mbit/s is
below 20 ms (was 10), the 2 and 3 reader runs below 20 ms (was 15), the rate on a 4 Mbit/s link with watermark jumps below 50 Mbit/s.

**Reading a Pi run from one run.** The `large` build of `t1_build_node.sh` now sets `tt_RATE_TRACE`: the near and far files hold one
`RATETRACE` line per round (`t_ms`, round length, delivered bytes and rate, bytes sent and allowed, BtlBw, rate, state, phase, RTprop, the
last round trip sample, held/top/sent unit counts, jump units), and the RESULT line carries `rate_stall_restarts`, `rate_rtt_samples`,
`rate_units_sent`, `rate_held_units`, `rate_top_units`, `rate_jump_units`, `rate_delivered_bytes`. A healthy run: `held` follows
`units_sent` within the data in flight, `jump` stays small, `rtprop_us` is above the path's round trip.

**Is the estimator the right design?** See the answer given with the hand-back of this round: it has four estimates (delivery, round
trip, queue, loss) that all read one noisy feedback (an ACKNACK that counts samples, units and repairs in ways that change with loss,
eviction and runs), and each of the two failures found since the first Pi run was one of them misreading that feedback. A smaller design
- the delivery rate from the watermark alone, a loss-based AIMD on the share of repairs, no round trip steering - has fewer ways to be wrong
but, as the try in item 4 showed in the model, needs something to bound the queue on a link that tail-drops late.

## 15. Pi quick test 3 (a4db29f4) and the third round (2026-11-20)

Quick test 3 (1 MiB, traced): delivered 15, 14, 15, 15, 15, 15, 12, 14 of 15 (M1, M1, M2, M2, S1, S1, S2, S2); no run at the floor; S1 p50
2.3 s against the oracle's 2.37 s. Left: (1) `rtprop` microseconds in 5 of 8 runs; (2) the jump counter non-zero in nearly every trace line;
(3) the rate swinging from 250 kbit/s to the cap a round, 2000-5000 retransmissions a run.

1. **`rtprop` of microseconds.** The trace of S1 shows it: the sample was taken when `top == units_sent` - the newest sent unit - 562 us
   after its send on a 10 ms path. `top` was held plus the units the bitmap names missing; a solicitation's HEARTBEAT carries the
   writer's last sequence number, so the reader names every unit up to it that it has not received, **including units still in flight**, and the unit
   at `top` was one sent a moment ago. Now the round trip is read from the watermark alone (every unit below it is received, for
   certain; `test_the_round_trip_reads_the_watermark_only`), only where the watermark moved by at most four units or two data
   datagrams (a repair that closes a gap moves it over units sent long before: Karn's rule;
   `test_a_repair_is_no_round_trip_and_one_bad_sample_is_outvoted`), and the estimate is the median of the filter's bucket minima, so
   one early bad sample is outvoted by the others. Delivery still reads the count with the bitmap window (the watermark alone moves in jumps at
   each repair's arrival and the estimate followed them: in the model the M1 retransmissions went from 150 to 5600).
2. **The jump counter** is cumulative: it was non-zero in every line after the first. What it counts is real and small: the last short
   fragment of a sample is a sequence number and not a stamped unit, so the watermark runs about one unit per sample ahead of the
   stamped units (8-19 units in 15 samples, as seen). `jump_events` (new) counts the reports that did it. It is not a defect of the rule, which
   fires for a jump of hundreds of units and costs a unit or two here.
3. **Swings.** (a) START doubled three rounds past a link before it saw it: it now holds at 1.25 times its best round on the first round that
   does not grow (`test_start_does_not_run_past_the_link_and_a_spike_is_capped`: on a 4 Mbit/s link it peaks at 16 Mbit/s, 4 times the
   link). (b) A round's delivery is read at most 1.5 times the rate it was sent at, so a watermark that moves at a repair's arrival is not read
   as four times the link. (c) A drain lasts at most four rounds: the round trip that ends it is now rarer. The cap on the rate is still the user's
   `pacing_rate` (the WIFI profile's `link_rate`, 1 Gbit/s, if unset): **on a link with a small queue set the cap near the link**.

**Regressions, stated.** Reading the round trip from the watermark alone and only from small advances gives the queue guard fewer
samples, so a standing queue is longer-lived and the round trip estimate can follow it up: in the model the single-reader step test (4 to 30 Mbit/s)
now follows the rise in 3.0 s (0.25 s before), and **with three readers it does not follow the rise within 16 s** (1.7 s before; recorded as
`KNOWN LIMIT` in `tests/test_adaptive_pacing.c`, not as a check). The mean queue delay of the single-reader 4 Mbit/s case is 25.7 ms (12.6 before); the
checks are loosened to 30 ms. On the model's tc links (M1/M2/S1/S2: 1 MiB every 4 s for 68 s) all four deliver 14-17 of 17 samples with
repairs of 116-989 against the 95 % oracle's 113-727 and datagrams within 1.04-1.1 of it, a result the previous round also had (the model did not
reproduce the Pi's failures in either round).

**An honest read.** Three rounds on the Pis each removed a failure and the model has not predicted the next one; every one of them was one of the
estimator's signals misreading the ACKNACK. See the recommendation with the hand-back.

## 17. Root cause of the rate variation (Pi quick test 4 of 04cd1473) and the fixes (2026-11-20)

Quick test 4: 15 of 15 in six of eight runs, S2 12 and 13; per-round rate swings between 0.7-1 Mbit/s and the 1 Gbit/s cap (max/min rate: M1 1453 and
1000, M2 1000 and 1000, S1 20 and 18, S2 429 and 6536); 13.8-32.9 thousand datagrams against about 10.8 thousand needed; final `rtprop` 248-263 ms on 10 and 40 ms paths.
Analysis of the traces of the eight runs (`RATETRACE` lines of the `.near` files; the scripts are the `a2.py`-`a4.py` quoted in the hand-back):

| cause | count | runs |
|---|---|---|
| START doubles the rate in rounds where the sender had nothing to send (the pacer sent under 80 % of its allowance: control traffic between samples) | 96 doublings; 81 of the 84 rounds at more than 8 x the link were app-limited | 8 of 8 (the three profiles M1, M2, S2 reach the 1 Gbit/s cap; S1 does not) |
| repairs at the cap: at 1 Gbit/s the next gap's repairs go out at that rate (3.5 MB in 350 ms in M2r2, 8 Mbit/s of it delivered) | follows from the row above (retransmissions 490-21500) | 6 of 8 |
| stall restart: a second of "sent, nothing delivered" (the sent bytes included repairs and control) resets BtlBw to 0 and the rate to 1 Mbit/s | 35 | 8 of 8 |
| drop-guard resets to a noisy sample (down to a quarter of BtlBw) | 13 | 6 of 8 |
| queue-guard drains on a contaminated sample (2.1 s in M2r2: rtprop 42 -> 209 ms) | 6 | 3 of 8 |
| START doublings in delivery-limited rounds (the ramp, entered again after each restart) | 133 | 8 of 8 |

The first row is the root cause: a link that carries a 1 MiB sample every 4 s is idle most of the time, an idle round has nothing delivered and
little sent, and START read "no verdict" as "double". The rate ran to the cap during every idle stretch; the next sample and its repairs went out at 1 Gbit/s
into a 4-30 Mbit/s link, rounds delivered nothing, and the stall restart (which forgot BtlBw) started the ramp over: the 0.7-1 Mbit/s lows.

Fixes (`include/tickle/pacing_adaptive.h`): (1) a round that was application-limited holds the rate in START (no doubling, no stall count); START never doubles past twice the
best delivery rate it has seen (`AP_START_CEIL_PCT`). (2) A stall needs at least eight data units sent in the round (`AP_STALL_MIN_UNITS`): repairs and control
traffic are not data; and a restart keeps a quarter of what was known instead of forgetting it. (3) A guard trip lowers BtlBw to at most half of it in one step (was a
quarter). (4) A round trip sample above 1 s, or above 16 times the estimate once there are three, is dropped (`AP_RTT_MAX_NS`, `AP_RTT_OUTLIER_X`);
a factor of 4 was tried and blinded the queue guard (a queue of a few times the path is the signal: the multi-reader model went to 70-80 ms of queue).

Tests that fail on `04cd1473` and pass now (`tests/test_adaptive_pacing.c`): the replay of the Pi trace of M2r2 (round lengths, delivered bytes and app-limited flags of its first
64 rounds, fed to the estimator at the rate it chose: before, the rate runs to the 1 Gbit/s cap, 6.1 times twice the best delivery seen, and swings 16 times after START; now
peak 112 Mbit/s, never more than twice the best delivery, 6 times after START); idle rounds do not raise the rate (before: 1 Mbit/s to 1 Gbit/s in 40 idle rounds); control traffic for 2 s is no
stall; and the 3-reader rise is followed (before: never; now 2.0 s). The replay uses the recorded inputs, not a simulated link, and takes the rate it feeds the round from the estimator's
own choice, so it cannot reproduce what the real sender then did; the S2r2 replay of its first 120 rounds shows no violation before or after (its variation of 6536 x comes later in the run).

Variation metric (max/min rate after START): replay M2r2 16 -> 6, S2r2 5 -> 5; model tc links (M1/M2/S1/S2) deliver 17/17/17/15 of 17 with repairs 477/966/129/746. The target
of at most about 5 x after START is met on S2r2's first 120 rounds and not on M2r2 (6 x).

## 18. Pi S1 v5 (68d4174a): the fall-back to 1 Mbit/s was a false stall restart (2026-11-21)

Classified from the RATETRACE lines of `wtc_adaptive_20261120_v5`. M2 128 KiB fell to exactly 1000 kbit/s in 10/10 runs
(S2 10/10); `stall_restarts` was 2-9 per run on M1, M2 and S2 (5-10 at 1 MiB). State START, rate 1000, bw 1500 (150 % of
1000) is `ap_restart_search`, not a guard.

Cause: the stall test measured `now - round_start`, and the round starts at the previous round's end, so the idle gap
between two samples (3.4 s in the traces) counted as time sent with nothing delivered. The first feedback after the next
sample's first 8 units (nothing delivered yet) satisfied "1 s, 8 units, 0 bytes" and the restart forgot the rate.

Fixes: the stall clock runs from the last delivery news or the first data send into an empty pipe (`stall_start_ns`);
START begins at cap/16 (1..8 Mbit/s) when a cap or link_rate is set; a probe that raised delivery by 5 % (was 10 %) is
followed by another. Without the false restarts, the 3-reader rise after a link step no longer got rescued by a restart,
which the probe change fixes. Test: the idle-gap check in test_adaptive_pacing (fails on 68d4174a, passes now).
Cause table (v5 traces, 128 KiB unless noted):

| Observation | Evidence | Cause |
|---|---|---|
| M2 falls to 1 Mbit/s, 10/10 runs (S2 10/10, M1 and S1 none by the fall-back test) | rate exactly 1000, state START, bw 1500 = 150 % of 1000 | stall restart (`ap_restart_search`), not a guard |
| `stall_restarts` 2-9 per run (M1, M2, S2), 5-10 at 1 MiB | RESULT counters | the stall fired at about every sample |
| round 5 ends 3572 ms after round 4 | RATETRACE t_ms | an idle gap lies inside one round |
| why it fired | stall test used `now - round_start` | the gap counted as time with nothing delivered; the first feedback after the next sample's first 8 units (none delivered yet) met "1 s, 8 units, 0 bytes" |
| new run or sample did not use the remembered BtlBw | `ap_restart_search` keeps 1/4, START begins at 1 Mbit/s and doubles once per round of at least 24 kB (about 200 ms at 1 Mbit/s) | slow START from a low start rate |
| per-round changes of 3x or more after reaching half the link: 17-74 per cell, all of them in START after a restart, none in CRUISE (max 8x, p95 2-5x; a few 19x) | v5 traces, c3 analysis | the same restarts (8x down, doubling up) |
| rate 50-370 Mbit/s on the 29.7 Mbit/s links at 1 MiB (M1, M2), state START throughout | rounds of 12-14 ms read 52-76 Mbit/s; every restart re-ran START and overshot | repeated START overshoot after restarts; the delivery reading of short rounds on a link with a queue is above the link until the queue is full (not fixed, to be read from quick6) |

Damping: the model run now checks that the pacing rate never changes by more than 8x from one step to the next after 5 s
(`max_step_x100`); in the traces every change of 3x or more came from a restart. Not measured on the Pis.

## 19. Pi quick test 6 (5de6f40b): what was left (2026-11-21)

quick6 (stall fix of 5de6f40b): 128 KiB stall_restarts 0, no fall-back to the floor, M1 p50 42 / p99 98 ms (DDS 55-91 /
92-126), M2 p50 75-92 ms (DDS 100-106), 15/15 in all 16 runs. Three things left, read from `wtc_adaptive_quick6` (RATETRACE
in the .near files, RSTATS in the .far files):

| # | Observation | Evidence | Cause |
|---|---|---|---|
| 1 | 1 MiB M2 r2: 180.8 Mbit/s on a 30 Mbit/s netem link (limit 100), 47,623 datagrams, 36,353 retransmissions, p99 2.4 s; `drop_trips` 0, `queue_trips` 0 | reader: `rx_loss` 7321/10842, `repair_duplicate` 7234; rounds 12-17 read 41-90 Mbit/s (520 kB in 46 ms) while the sample as a whole went at 19 Mbit/s | (a) the delivery reading of a round is bursty (the watermark passes the units a reader held in jumps) and the max filter takes the burst for the link: START doubled to 91, then 180 Mbit/s; (b) it then stayed there for 48 s: the 1 MiB sample leaves in 46 ms, so every later round is application-limited, START holds in those, and the drop guard ran in CRUISE only |
| 2 | stall restarts at 1 MiB: S2 r2 round 27 (1); none found in S1 r1 | rate 7.8 -> 4.9 -> 2.9 -> 1.5 -> 0.7 Mbit/s in four rounds, RTT 389 ms against rtprop 128 ms | not a false stall: a real collapse after a START overshoot (queue of 390 ms, loss storm), the drop guard halving BtlBw until 700 kbit/s; the same causes as 1 and 3 |
| 3 | 128 KiB S1 r1: final BtlBw 0, rtprop 152 ms against a 10 ms path | rtprop and `rtt_last` unchanged for 10 rounds; BtlBw 0 after the quiet rounds | (a) the first round trip sample was taken in the queue that the START rate (8 Mbit/s since 5de6f40b, on a 4 Mbit/s link) built, and Karn's rule (watermark advance of more than 4 units) rejects every later one, so RTprop never recovered; (b) the quiet round between two samples aged the BtlBw filter (8 rounds), emptying it |

Fixes: START ends on a loss storm (repairs above 50 % of a round's bytes while the rate is more than twice what the round
delivered: BtlBw comes down and CRUISE starts - in application-limited rounds too); the watermark reaching the last unit
sent is a round trip sample whatever its advance (the path is empty then); the quiet round between samples (more than 8
RTprop, application-limited, no higher reading) does not age the BtlBw filter. Tests: `check_quick6_causes()` in
test_adaptive_pacing.c (each of the three fails on 7bd44e2b).

Tried and dropped: a delivery sample capped by the average over a window of 8 RTprop. In the model it read the START ramp
as BtlBw (M1 p50 312 -> 1008 ms, M2 413 -> 2675 ms, S2 12 of 15 samples) - removed.

Not measured on the Pis. Open: the first round after a burst still reads above the link before the loss exit fires (one
or two samples at the first 1 MiB), and the START rate of cap/16 (8 Mbit/s) is above a 4 Mbit/s link.

## 20. Pi quick test 7 (74df5385): what the loss exit broke, and what stayed (2026-11-21)

quick7 (`wtc_adaptive_quick7`, 1 run per cell x2): fixed 1 MiB M2 overshoot (peak 61/53 Mbit/s, retx 945/340, p99 0.64/0.99 s),
stall restarts (1 in 8 runs), 128 KiB S1 BtlBw (4.0/4.1). Regressed: 128 KiB M2 p50 188/183 ms, p99 303/453 (v6 86/161; DDS
100/106). Still weak: 1 MiB S2 (46 Mbit/s on a 4 Mbit/s link, swing 31x/11x, p99 6-9.5 s), 128 KiB S1 rtprop 153 ms.

| # | Observation | Evidence | Cause |
|---|---|---|---|
| 1 | 128 KiB M2: rate 5-10 Mbit/s of 30, never 80 % of the link; `drop_trips` 2 | r1 round 8: the tail of a sample, 1.7 kB sent, all of it repairs, reading 2.9 Mbit/s against a rate of 9.7 -> BtlBw 11.5 -> 5.7, state CRUISE from round 9; CRUISE then climbs by one 1.25x probe every 8 rounds (a sample is 3-4 rounds) | the START loss exit of 74df5385 fired on any round of mostly repairs when rate > 2 x reading: true of every tail round, whatever the volume. The recovery in CRUISE is slow, so one false exit costs the run |
| 2 | 1 MiB S2: START to 46 Mbit/s on a 4 Mbit/s link, 21 drop trips, 1 stall restart, swing 31x | r1 rounds 19-26: readings 9-23 Mbit/s (412 kB in 142 ms) on the 3.97 Mbit/s link; held total at the end is right (10813 of 10845), the readings are not | the readings are lumpy: the watermark waits at a hole and passes all units held above it when the repair arrives, and the bitmap cannot show them before (the ACKNACK names only missing units). START doubled on them: 8 -> 16 -> 25 -> 46 Mbit/s. The loss exit then fired a round late each time |
| 3 | 128 KiB S1/S2: rtprop 153 / 48-268 ms against 10 / 40 ms | S2 r1: rtprop 142175 us from round 2 on, unchanged for the run; `rtt_last` follows only at the tail | the first samples are taken in the queue that the START rate (8 Mbit/s, above a 4 Mbit/s link) builds; later reports advance the watermark by more than 4 units, so no clean sample comes. The tail sample of 74df5385 is no help: the last unit sent waits behind the queue |

Fixes: the START loss exit needs at least a sample's worth of repairs in the round (24 kB) - a tail is a few kB; START does not
grow in a round whose repairs are above 25 % of its bytes (at least 24 kB), it counts as a round that did not grow (three of
them end START) and holds the rate at no more than it is; RTprop is never above the announce round trip to the reader
(tt_Context.announce_rtt_ns, an idle measurement, fed at every ACKNACK). Tests: `check_quick7_causes()`
(each of three checks fails on 74df5385).

Not done: the START rate of cap/16 = 8 Mbit/s is above a 4 Mbit/s link and below a 30 Mbit/s one, and the link cannot be known
before the first round; a smaller start slows M, a larger one queues on S. Not measured on the Pis. The announce round trip
errs long (it includes a flush wait) but is taken as a minimum, so it can only lower RTprop; it carries no serialisation time
of a data frame (about 3 ms at 4 Mbit/s), so RTprop on S can read a few ms under the path of a data unit.

## 21. Decision (c) of 2026-11-21: one more bounded iteration on v7 (7c6e4fe3)

v7 full S1 (`0b608977`): 50/37/1/24 against the fixed 95 % arm 65/31/0/16; the only LOSE is 128 KiB M1 p99 against Fast DDS (98.4 vs
91.9 ms); 128 KiB M2 p50 +31 % and 1 MiB M2 p50 +39 % / p99 2.1x against the oracle; 1 MiB S2: 19 stall restarts in 7 runs, swing
up to 84x, one run delivered 9/15; 1 MiB S1: 7 restarts, datagrams 1.32x the oracle. No wire change.

| Change | Cause | Test (each fails on 7c6e4fe3 unless noted) |
|---|---|---|
| START from the user's prior: a cap below the 1 Gbit/s default (pacing_rate or link_rate the user set) starts at half of it; the first full round decides: delivering 60 % of what was sent - believed, CRUISE with a probe at the run gain (no doubling START); less - START goes on from the measured rate (x1.25). No prior (cap at the default): unchanged, 8 Mbit/s | START at cap/16 = 1-8 Mbit/s made every 128 KiB first sample slow on a 30 Mbit/s link (M1 p99 98 vs 92 ms: the 6 ms of START) although the user had said what the link was | `check_next_causes()` (iv): 15 Mbit/s under a 30 Mbit/s prior, right prior -> CRUISE above 25 Mbit/s (fails on 7c6e4fe3), wrong prior (4.3 Mbit/s delivered) -> START at under 7 Mbit/s (passes on both: a guard), no prior -> 8 Mbit/s (guard); (v) the model, 4 Mbit/s link under a 30 Mbit/s prior against none: same samples, median and queue (guard; the numbers are equal: 2248 ms, 23 ms) |
| A stall is no report at all: the clock restarts at every ACKNACK (any report is a live path) and at a send into an empty pipe; checked at every send (`ap_check_stall`): 8 units in flight and silence for max(1 s, 8 RTprop) | the old test ran in `ap_on_feedback`: a report that showed nothing new after 1 s was a stall, though the reports were coming - 19 restarts in 7 runs of 1 MiB S2, each followed by a doubling START | (i) five seconds of reports showing nothing: no stall (fails: 1); the 4b idle-gap check now sends into the silence for the real stall; 4b's live-report check (fails: 1) |
| Repairs sent in the last 2 RTT are waiting for their answer: no stall while they are (until 4 windows) | a hole being repaired is not a dead path | (ii) a repair 10 ms ago excuses a silent window (a guard: 7c6e4fe3 has no send-side stall; it fails if the rule is removed) |
| After a stall the search resumes at half the last CRUISE rate, in CRUISE, with probes at 110 % (no run gain) for 16 rounds, instead of START from the bottom | each restart re-ran the doubling START and overshot; the next restart followed | (iii) a stall at 20 Mbit/s -> CRUISE at 10 Mbit/s with damped probes (fails: no stall at all there) |
| `rate_units_held` counts the units up to the start of the last word the ACKNACK carries | (3) below | `test_rate_control`: three words, one hole at position 5: held 100 + 128, not 105 (fails: 105) |

(3) What the ACKNACK carries above the watermark, read in `send_acknack_skipping()` and `rate_units_held()`: the watermark `seq_no` and
`bitmap_words` words of bits for the units *missing*, cut at the reader's highest relevant bit (the highest it holds, or the last
sequence number of the latest HEARTBEAT); every position up to it is told, a bit set for missing and none for held. The writer
used only the highest *set* bit, so the units a reader held above its last hole counted all at once when a repair moved the
watermark over them (the lumps of 9-23 Mbit/s on S2's 4 Mbit/s link). The number of words also tells the highest relevant bit to
64 units, so the units up to the start of the last word are known held, a lower bound that never over-counts. That needs no wire
change and is in. Not in: the units between that and the reader's highest (up to 63): not on the wire. Narrow ACKNACKs (a new
gap's range, low_bit > 0) leave positions below low_bit as zero bits that mean not asked and not held; the formula cannot tell
them from full ones, an over-count it already had.

Not measured on the Pis.

## 22. The prior-anchored experiment (2026-11-22, base 6b05d93c)

quick9/v8 (arm B: the cap is the right prior; arm C: a 30 Mbit/s cap on the 4 Mbit/s links): B is about the fixed 95 % arm on M1
and on the slow links, but worse than A on M2 (128 KiB p50 165 ms against an oracle of 79; 1 MiB p50 523 / p99 1.1 s against
367 / 488; 26 stall restarts in 2 runs); C is bad at 1 MiB (delivery down to 4/15, p99 25-55 s, swing up to 181x). The adaptation
hurts when the prior is right, and backs off too slowly when it is wrong. The decision: the cap the user sets is the expected
bandwidth (the guideline: set it to what you expect), and the library takes 95 % of it. No wire change.

| Change | Cause | Test (fails on 6b05d93c unless noted) |
|---|---|---|
| With a prior (a cap below the 1 Gbit/s default) the state is ANCHOR: the rate is 95 % of the prior from the first datagram - no half-prior START, no doubling, no CRUISE probes; the ceiling is 95 % of the prior (the cap of the estimator is set to it), nothing above | the half-prior START and the CRUISE after a verified prior kept searching and probing a link the user had described; a right prior was only as good as the search | `check_anchor_causes` (a): START at 28.5 Mbit/s under 30, and no raise above it with delivery at twice the rate |
| Decrease on congestion only: the queue guard (RTT above RTprop by its target twice) takes the rate to 80 %, and no recovery while the queue is there; low delivery - under 60 % of what the previous round sent - for 3 rounds takes it to 80 %; neither is loss | a loss exit (repairs above half the bytes) and the CRUISE drop guard read random and bursty Wi-Fi loss as congestion: the M2/S2 misreading | (b) a fifth of every round repairs and a tail of repairs: the rate stays at 28.5, state ANCHOR; (c) the queue takes it to 22.8, it stays there while the queue does, comes back after (the last part passes on 6b05d93c); (d) two low rounds: no change, three: 80 % |
| Recovery: a tenth a round, up to the ceiling, while the queue is clean (`draining` off) | a decrease that is not undone keeps the rate under the link | (c) |
| Floor: 25 % of the prior (`AP_PRIOR_FLOOR_PCT`) while the prior is believed; the floor reached with congestion still there is a wrong prior | a rate that falls to the minimum floor starves the repairs and the samples | (g) the clamp of the estimator goes to 7.5 Mbit/s, not 100 kbit/s |
| A wrong prior is found within the first rounds and the rate steps down to the measured delivery x 1.05, in CRUISE with gentle probes under the ceiling: two low rounds in a row among the first 4, a loss storm (repairs above half the bytes, at least 24 kB, at more than twice the delivery), or the floor with congestion | arm C: the 30 Mbit/s cap on a 4 Mbit/s link never left its cap | (e) 4 Mbit/s delivered under 30: CRUISE at 4.2 Mbit/s after four rounds; (f) a storm: CRUISE at the round's delivery (both pass on 6b05d93c, which went to the measured rate too: guards); (h) the model: 30 Mbit/s on a 4 Mbit/s link finds the link within 2 s, completes the same samples with the same median and queue as no prior |
| The stall clock restarts at a send after a quiet spell (200 ms): a stall is silence while sending | the model of a right prior fell to the floor at its second sample: units of the last sample were still unanswered, the first send after 6 s of quiet found the last report 6 s old - a stall (the same false stall as v5, by another door) | (i2) fails on 6b05d93c; (h) right prior on M2-like 3 % loss: rate never below 28.5 Mbit/s (fails: 5.4), p50 395 ms against 579 without a prior |

The ceiling and the floor are fractions of the prior; the user's own `rate_min_rate` still applies (the clamp takes the larger).
Without a prior (the cap at the 1 Gbit/s default) nothing changes: arm A. Not measured on the Pis. Known: ANCHOR has no
estimate of the link above the prior, so a link faster than the user said is never found; that is the guideline (set it to the
expected bandwidth), a slow probe to 100 % was left out.

## 23. Pi quick test 10 (32e2a6c2): the ratchet of the right prior, and the wrong prior that was not seen (2026-11-22)

quick10: arm B (right prior) fixed M2, equal to the fixed 95 % arm on M1 and S1, 0 stall restarts; one collapse (1 MiB S2 r2: rate
3.8 Mbit/s -> 100 kbit/s in about ten rounds, 2 of 15 delivered); arm C (30 Mbit/s prior on 4 Mbit/s links) at 1 MiB on S1/S2:
delivery 8-15 of 15, p99 5-35 s, swing 285x.

| # | Observation | Evidence | Cause | Fix / test (fails on 32e2a6c2) |
|---|---|---|---|---|
| 1 | B S2 r2: rate 3800 -> 1000 -> 674 -> 100 kbit/s, 18 queue trips and 2 stall restarts with no queue | rounds 7-33: rtt 147, 147, 94, 144, 107 ms against an rtprop of 42; every trip took the rate to 80 %; round 33 is the floor (1000 = 25 % of 4 Mbit/s); round 34 is CRUISE at 1284 | (a) the round trips that tripped the guard were tail samples (the watermark reaching the last unit sent, accepted whatever its advance since 74df5385): after a recovery they are the time of the repair, not a queue; (b) at the floor with `draining` still set the wrong-prior rule fired (`at_floor && (low || draining)`) and stepped to the measured delivery x 1.05 - under the floor - into CRUISE, which has no floor and whose guards go on cutting: this is the path that bypassed AP_PRIOR_FLOOR_PCT | (a) a tail sample feeds the queue guard only when no repair went out in the last two round trips (RTprop still takes it); test (1) - fails: the guard got the samples. (b) the floor needs two low-delivery rounds to be a wrong prior, a queue alone never is; test (2) - fails: state CRUISE at 950 kbit/s |
| 2 | The decrease on `drain` with no real fall in the held units; BtlBw follows the falling paced rate | rounds 9-33: samples 2.8 -> 1.0 Mbit/s equal to the paced rate; `bw` follows it | in ANCHOR the queue guard cut at its first trip (two samples in a row) and the wrong-prior step took its measure from deliveries made at a rate the library had lowered itself: delivery at the paced rate says nothing of the link | a queue is congestion on a believed prior only when it lasts into a second round (`AP_ANCHOR_QUEUE_ROUNDS`; the guard only flags it); recovery waits for a round without one; the wrong-prior step needs low delivery against what was sent (the existing 60 % of the round before) - the measure is never delivery at a paced-down rate; test (c): one round of a queue does not decrease, a lasting one does (fails: 22.8 Mbit/s after one) |
| 3 | C at 1 MiB on S1/S2: 8-15 of 15 delivered, p99 5-35 s, swing 285x; the prior never seen to be wrong | S2 c30 r2: samples 24-30 Mbit/s on a 4 Mbit/s link in 42 ms rounds (150 kB per round), 721 of 722 units "held" in 0.5 s; `drop_trips` 42 | the reader leaves out of the bitmap of its answer the units it named within the last round trip and a half (their repairs are on the way, `answer_ack_request`); the count of units held took them as received, so at 70 % loss the delivery read was the rate of the requests and equal to the paced rate: no low round, no wrong prior | on a believed prior the units of the repairs sent in the last round trip (at the rate of the last two rounds, when repairs are a quarter of the bytes and at least 24 kB) are taken off the held count before it is used; `ap_note_repair` now also stamps `last_repair_ns` (the send path says "repair" to `ap_note_send` never, so the 2 RTT excuse of the stall rule was dead). Test (3): 400 units shown held, 300 of them repairs in the last round trip: 100 (fails: 400) |

Tried and left out: the same subtraction without a prior. In the model's multi-reader runs, whose reader leaves nothing out,
it under-read the delivery (a drop of 2.1 s and a rise of 7.6 s against 0.9 and 2.7), so it is in ANCHOR only; the search
without a prior (arm A) reads as before.

Test (4) of `check_quick10_causes` - the model with 30 Mbit/s on 4 Mbit/s, 1 % loss and a reader that omits - passes on 32e2a6c2
as well and is a guard. Not measured on the Pis.

## 24. The exact rate-step run (2026-11-25): a probe lost at the quiet round, and a cut made good one probe in eight

Report `WIFI_ADAPTIVE_RATESTEP_EXACT_2026-11-25.md`: after the step up (4 -> 30 Mbit/s) ADAPTIVE took 36 s to recover at 128 KiB (DDS 2 s, the
fixed rate 4 s; p50 102 ms against 43), and after the step down (30 -> 4) 17 s at 1 MiB (CycloneDDS 3 s; phase maximum 18 s).

| Change | Cause | Test (fails on ae5c192a) |
|---|---|---|
| A probe running when a round that was application-limited **and quiet** (more than 8 round trips: the gap before the next sample) ends is carried over, for up to 3 such rounds, instead of being scored a failed probe; the next full round judges it. A probe that raised the delivery rate still goes on at the run gain, application-limited round or not | at 128 KiB a sample is about 5 rounds: the probe at 1.25x succeeds, the run doubles, the sample ends, and the quiet round after it (3.8 s, sample 58 kbit) failed the probe - DRAIN at 0.75 BtlBw, so the run began again at each sample and BtlBw rose 4.05 -> 4.8 -> 9.0 -> 9.05 -> 10.2 -> 11.9 -> 13.4 Mbit/s (+1.3x a sample); at 1 MiB a sample is 28 rounds, several probe/run cycles fit in it | `check_ratestep_causes` (1): a running probe survives the quiet round (phase PROBE, rate 16 Mbit/s; was DRAIN at 6) and goes on to more than 20 Mbit/s at the next round (was 9); a probe nobody judges for five quiet rounds is still dropped (a guard) |
| Two drop trips within 4 rounds (a cascade) set `fast_probe`: the next probe comes at the next round (it was after 8), its first gain is 110 % (a cut that was right costs a tenth more, not a quarter), and the first probe that finds nothing clears it. One trip does not | the drop guard cut 3.8 -> 1.9 -> 0.95 Mbit/s in two rounds on the 4 Mbit/s link (a quarter of it) and the probe came every eighth round, 0.6-0.86 s each at that rate: 6 s of waiting, 8 s at a quarter of the link, a phase maximum of 18 s | (2): two rounds of mostly repairs set `fast_probe` (was not); a fluid 4 Mbit/s link, rounds of 0.7 s, from 0.95 Mbit/s: 3 rounds (2.1 s) to 3 Mbit/s (was 10 rounds, 7.0 s); one trip leaves it off (a guard) |

What the model did not do: reproduce the 36 s. The model (128 KiB every 4 s, 4 -> 30 Mbit/s at 20 s) recovers in 8 s with or without the change - it has
no feedback in the quiet gap, so its probe is not scored there - and is kept as a guard (`check_ratestep_causes` (3)); the two causes are tested on
the estimator with the rounds of the Pi trace. The limit of the 3-reader rise in the model's step run is now 5 s (it was 3): that time is chaotic in the probe
cadence (2.7 s on ae5c192a, 3.2 s with this change, 4.2 and 8.8 s with other settings of the cadence tried, 0.2-2.5 s for the other readers' counts) and
no setting is better in any other number; nothing else in the model moved (the right-prior, wrong-prior and queue-guard checks of the earlier
sections are unchanged and pass). The prior-anchored mode is untouched (no probes there); the carry and the fast probe act in CRUISE, so a wrong prior
that has stepped down gets them. Not measured on the Pis.

## 25. Re-validation of 1054f2ff (2026-11-25): the 1 MiB rate step that never recovered

`WIFI_ADAPTIVE_NEXT_REVAL_2026-11-25.md`: the 128 KiB fix works (recovery after the step up 36 s -> 8 s, p50 102 -> 49 ms). 1 MiB rate step, run 2 of 5: 5 of 12
samples delivered in the 4 Mbit/s phase and 1 of 11 after the step up, ended at 2.2 Mbit/s, 8,425 retransmissions.

Trace of the failed run (`RS/rl1m/rel_large/M1/r2_tickle-ad_A.near`, step down at about t = 44.2 s, step up at 88.2 s):

| When | What the rounds show | Cause |
|---|---|---|
| t = 48.5-49.2 s, rounds 256-270 | probe, drain, probe, drain every round at 5.8-9.4 Mbit/s on a 4 Mbit/s link; readings of 6.9-8.6 Mbit/s from 26 kB per 30 ms | `fast_probe` (probe at the next round, set by a cascade of two drop trips) taking lumpy readings of a standing queue for successes |
| t = 49.8-63 s, rounds 272-284 | loss, then drop trips halve BtlBw 7.7 -> 3.8 -> 1.8 -> 0.9 Mbit/s; rounds of 0.3-3.2 s | the overshoot above |
| t = 63-67 s | the probes climb back to 8.6 Mbit/s, a 1.28 MB round at 9.5 Mbit/s on the 4 Mbit/s link | probing from a BtlBw that a lump had set above the link |
| t = 70-117 s, rounds 304-315 | **one round a sample**: 3.1-4.0 s long, bytes 1,111,974 = 723 units every time, sent 53 % of the allowance (rate x 4 s) - every round "application-limited"; the rate stays at 4,302 kbit/s after the step up at 88 s, and the carried probe holds 4,732 for 3 rounds | the reports showed no progress until a sample was whole (the first hole's repair is deferred behind the paced send), so a round held the whole period, the idle half included; the rate of such a round is diluted and its app-limited flag always true, and a probe needs a round that is not application-limited: **no probe could begin** |

Changes (all on the CRUISE path; the prior-anchored mode is not touched):
- **`fast_probe` is removed.** It probed every round on lumps; the re-validation measured the 17 s -> 13 s it bought within noise and the cost above (arm A static: drop trips 26-69 against 7-19, retransmissions 987-2,189 against 262-781 on 1 MiB S1; queue trips on M1 about 3x). The carry of a running probe over the quiet round (the 128 KiB fix) stays.
- A round that delivered a whole sample at once (196,608 bytes or more) is read over the time from its first data unit to its end, not over the gap before it: its rate and its application-limited flag are the sample's. And a probe is due after 8 s, once two rounds have passed, when the round was such a one.

Not changed, though the trace shows it: the queue guard's counter stays at 1 from round 254 to the end of the run (no round trip sample renews it). Resetting it in a round without a new sample made the model's S2 run collapse to the floor (100 kbit/s), so it is left.

Tests (`check_ratestep_causes`): (2) the failed run's frozen state - one round a sample on a fluid link that rises 4 -> 30 Mbit/s after four samples: the rate reaches 20 Mbit/s 2 rounds after (1054f2ff: never, 4.3 Mbit/s after 20 rounds) - fails on 1054f2ff; (2c) twelve lumpy rounds after a cascade begin no more than two probes (a guard: passes on 1054f2ff, whose fast probes cleared themselves in this construction, not as on the Pi). The cut-recovery test of 1054f2ff is gone with `fast_probe`.
Limits moved: the replay of M2r2 may swing 11x after START (10x: the carry that makes the 128 KiB fix runs a probe on, 10.4x); the pooled readers' rise 5 s (it moves between 1 and 9 s with any change of cadence).

Static regressions of 1054f2ff (1 MiB, arm A, no prior): the extra drop trips, retransmissions and M1 queue trips are what `fast_probe` explains (more probes, each an overload); the S1 p99 of 4,024 ms is within the 2.8-5.8 s that ae5c192a showed in quick10, so it is not shown to be a regression. Not measured on the Pis.

## 26. DRAFT (2026-11-26, not committed): following a changing bandwidth faster - causes, candidates, model comparison

Analysis only: the library (448b685f) and the tests are untouched. Evidence: `WIFI_ADAPTIVE_VARYING_BW_2026-11-26.md`, RATETRACE of
`wtc_varybw_20261126` (3 runs a cell, step times from the `.steps` monotonic markers, which share the clock of the RATETRACE lines);
the offline model comparison is a scratch harness outside the repository (`~/tmp_build/exp`: the model of test_adaptive_pacing.c with a
rate schedule, and a copy of pacing_adaptive.h with the candidates behind `-DEXP_*`).

### 26.1 Why the rate is wrong, from the traces

Time-weighted by round length, all runs of the cell pooled, by what the rate and BtlBw were doing (`rate / capacity`, capacity = netem rate x 0.991):

| cell | within 0.7-1.3 | over 1.3x: BtlBw itself stale-high in CRUISE | over 1.3x: probe or run (rate = gain x BtlBw) | under 0.7x: BtlBw itself low (cut by a guard, climbing back) | under 0.7x: drain |
|---|---|---|---|---|---|
| P5a 128 KiB | 19.6 % | **33.4 %** | 3.8 % | **28.2 %** | 11.2 % |
| P5a 1 MiB | 48.8 % | 7.2 % | 0.9 % | **30.8 %** | 12.3 % |
| P7 128 KiB | 40.2 % | **22.7 %** | 6.7 % | 20.7 % | 6.7 % |
| P7 1 MiB | 44.9 % | **19.9 %** | 1.9 % | 24.3 % | 8.9 % |

(the rest is "other", 0-4 %). So over capacity is mostly **a BtlBw that was not brought down**, and under capacity mostly **a BtlBw that a guard cut too far
and that then climbs slowly** - not the probes themselves.

**Over capacity after a step down, 128 KiB** (P5a r1, step 30 -> 10 Mbit/s at mono 6,544,031,850; rounds 8-14): the first sample after the step is
read exactly (round 12: 9,990 kbit/s, round 14: 9,999 kbit/s - the capacity), yet BtlBw stays at 30,542 and the rate at 60,995 (the carried 2x run
of the last probe) for **8.0 s of the 10 s phase**; the first cut comes at round 14 (+8.05 s): the queue guard (it needs two round trip samples over
the target, and one sample a 4 s sample is all it gets) takes BtlBw to 15,271 and the rate to 11,453, still 1.15x. Second down step the same: BtlBw 29,881 held
at rounds 27-28, halved to 14,940 at round 29 (+7.93 s). The causes are in the estimator:
- *the filter window*: since quick test 6 a round that is application-limited and reads no more than BtlBw neither enters the filter nor ages it (a 128 KiB
  sample every 4 s emptied BtlBw otherwise). Round 12 is flagged application-limited (it sent 90 bytes: the sample's data left in the quiet round before it
  and sits in the bottleneck queue, delivering at the link rate) - so the correct reading of the new capacity is ignored by the one rule that would age it;
- *the evidence*: the round trip samples that feed the queue guard are accepted in 25 % of the rounds at 128 KiB and 13 % at 1 MiB on the Pi (the watermark advances by more than 4
  units a report), 85 % / 76 % in the model; the guard needs two, so one sample a sample period means two periods = 8 s;
- *the carried run* (1054f2ff) keeps 2x BtlBw in force across the idle gap: rate 61 Mbit/s on a 30 Mbit/s link before the step, and again in P6 128 KiB after the
  step up (rounds 71-77: rate 46-60 Mbit/s for 8 s, BtlBw 25 -> 30 Mbit/s).

**Slow to follow a step up, 128 KiB** (P6 r1, 4 -> 30 at mono 6,553,304,275; P5a r1 step 2, 10 -> 30): BtlBw rises by one probe a sample or less - P6 rounds 57-69: BtlBw
19.9-20.7 Mbit/s for 12 s (+0.96 s to +12.9 s), rate = BtlBw, every sample read at the rate it was sent at (delivery = rate says nothing of the headroom);
the next probe is due after 8 rounds, a 128 KiB sample is 3-4 rounds including the quiet one, so one probe every 2-3 samples (9-12 s): +12.98 s (round 71),
and the run gets to 30.4 at +16.97 s. P5a: 10 -> 30 reaches 0.7x in **6.0 s** (4 of 8 up steps), 10 s once, and **never within the 10 s phase** twice (11.9 s under 0.7x).
The limit is the probe cadence in rounds against a flow of 3-4 rounds a sample, and the 1.5x cap of a sample at the rate in force.

**Under capacity at 1 MiB after a step down** (P5a r1 step 3, 30 -> 10 at about 30 s, rounds 173-206): the first reading is right (round 178: 10,167) and BtlBw is halved
to 14,944, then a single 13 ms round reads 20,357 (a lump) and BtlBw goes back to 22,838; the drop guard then halves it each round on 6-9 Mbit/s readings: 8,108,
5,198, 3,898, 2,924 kbit/s by +1.05 s = **0.29x capacity**, and it stays there until the next sample's first round (+4.4 s), when the run gain takes it to 10.1 in 0.15 s.
14-21 such cuts below 0.7x per run (P5a 1 MiB 17, 17, 20; P7 1 MiB 14, 18, 21), clustered 0.1-1.2 s after each down step (30.7-31.1, 49.9-51.1, 70.6-71.2 s).
The down reaction itself is fast (0.0-0.5 s to the band); the cost is the over-cut and the wait for the next sample to probe. The model does not reproduce this
(no lumps): P5a 1 MiB under 0.7x is 3 % in the model, 43 % on the Pi.

### 26.2 Candidate designs

| | what | wire | cost | risk | in the model |
|---|---|---|---|---|---|
| (a) train | the first part of each sample sent as a train, capacity read from its dispersion | **not without a wire change**: the writer sees only when reports arrive; the reader's receive times are not on the wire. A variant with no wire change: dense solicitations (about 10 of 156 bytes at 2 ms spacing per sample) and the growth of the held count between two reports - unproven, the counts are lumpy (26.1) | a train of 30-50 kB is a burst into a 100-packet queue; dense control traffic at every sample | the same lumps that break the round readings break a 2 ms dispersion | oracle bound only (the model knows the link): 128 KiB step-up recovery 4.1 -> 0.2 s, nothing else moves - the only candidate that helps **up** |
| (b) shorter window / decay | BtlBw aged by stretch, not by round: a stretch (the rounds between quiet gaps) of at least two saturated rounds (the pipe held at least what the round delivered) that reads under 0.7 BtlBw replaces it; or BtlBw halved when delivery stays below the rate | yes | a counter and a max per stretch | a lump or a short stretch cuts BtlBw (the over-cut of 26.1) | B: P5a 128 KiB unchanged, P7 128 KiB median 1.12 -> 1.30; the 2-reader rise 9.8 s (that time is chaotic); no gain alone |
| (b') backlog-corrected app-limited flag | a round whose pipe held at least what it delivered is not application-limited, whatever it sent in the round | yes | one comparison; the in-flight count is already kept | lumps count as saturated readings: the M2r2 replay swing 19.6x (limit 11x) | E: **P5a 128 KiB median 1.41 -> 1.00, in +-30 % 35 -> 55 %, over 1.3x 56 -> 31 %**; P7 128 KiB 43 -> 34 % in band; 1 MiB neutral |
| (c) faster down on delay | the queue guard trips on one sample when the queue is at least 3x its target (it needs two) | yes | none | a round trip sample delayed by a repair trips it (quick test 10: 18 trips with no queue); the tail hygiene covers tail samples, not regular ones | C: P5a 128 KiB median 1.41 -> 1.06, in band 35 -> 44 %, over 1.3x 56 -> 41 %; P7 128 KiB 43 -> 53 % in band, over 1.3x 42 -> 32 %; **every existing model check passes** |
| (d) gated up | a probe only when the queue is empty and delivery is at least 90 % of the rate; run gain 1.5x when there is queue evidence | yes | two comparisons | slows every rise: the 128 KiB step-up of the existing model 8 -> 24 s (the Pi's 24 s), mean queue delay up in 3 existing checks | D: P5a 128 KiB over 1.3x 56 -> 36 %, but 4 existing checks fail; **rejected** |
| (e) more frequent probes | a probe every 3 or 4 rounds (it is 8) | yes | none | the queue delay of the pooled-reader checks 25-41 ms (limit 20), a watermark-jump run that collapses | F: no change in the 128 KiB step-up (4.1 s), 2-4 existing checks fail; **rejected** (the lesson of fast_probe) |

### 26.3 Model comparison

The model as it is does not reproduce the Pi: it accepts a round trip sample in 76-85 % of the rounds (the Pi 13-25 %) and reads no lumps. Calibration: with one round trip
sample in 12 accepted (`-DEXP_SPARSE=12`) the 128 KiB P5a run reads median 1.41, 35 % within +-30 %, 56 % over 1.3x (Pi: 1.15, 31 %, 44 %) and P7 128 KiB 43 % / 42 % (Pi 50 % / 27 %): the
128 KiB behaviour is reproduced, the 1 MiB over-cut and the 24-36 s step-up are not (the model's 128 KiB step-up takes 4 s, 4 -> 30 Mbit/s). Offline results in the sparse model, 3-4 patterns,
one run each (schedules of the real runs: P5a and P5b 30 <-> 10 Mbit/s with 20 s and 60 s periods, P7 the seed 20261126 walk, P6 the 5 s dip, a 4 -> 30 step; M1 link, 1 % loss, queue 100):

| design | P5a 128 KiB: median / in +-30 % / over 1.3x | P7 128 KiB | 128 KiB step 4 -> 30: recovery | existing model checks failing |
|---|---|---|---|---|
| current 448b685f | 1.41 / 35 % / 56 % | 1.12 / 43 % / 42 % | 4.1 s | 0 |
| (c) | 1.06 / 44 % / 41 % | 1.07 / 53 % / 32 % | 4.1 s | 0 |
| (b') | 1.00 / 55 % / 31 % | 1.09 / 34 % / 48 % | 4.1 s | 1 (M2r2 replay swing 19.6x) |
| (b) | 1.41 / 35 % / 56 % | 1.30 / 38 % / 49 % | 4.1 s | 1 (the 2-reader rise 9.8 s) |
| (b) + (b') + (c) | 1.00 / 66 % / 25 % | 1.07 / 48 % / 36 % | 4.1 s | 1 (the replay swing) |
| (c) + (b') | 1.00 / 66 % / 25 % | 1.28 / 41 % / 47 % | 4.1 s | 1 (the replay swing) |
| (d) | 1.10 / 51 % / 36 % | 1.10 / 37 % / 47 % | 4.2 s | 4 (128 KiB step up 24 s, queue delay) |
| (e) every 3 rounds | 1.41 / 35 % / 56 % | 1.12 / 43 % / 42 % | 4.1 s | 4 |
| (a) oracle train | 1.41 / 35 % / 56 % | 1.08 / 44 % / 43 % | **0.2 s** | 0 |

The model gives completion p50 within 5 ms of the current (83-87 ms for the candidates that help, fixed 95 % 76 ms) - the model's absolute latencies are not the Pi's (47 against 105 ms on P5a), so
no latency claim is made from it. What it **cannot** show: lumps in the delivery readings (the cause of the 1 MiB over-cut), the quiet-gap feedback of the Pi (the 24-36 s step-up), the Pi's
round trip sample sparsity beyond the one calibrated parameter, repairs delaying a round trip sample (the risk of (c)), and the interaction with a real link's loss and queue.

### 26.4 Recommendation

- **Rejected on the evidence**: (d) gated up (it reproduces the slow rise), (e) more probes (queue delay, a collapse), and (b) alone (no effect).
- **Candidate for a change**: (c) the one-sample queue cut, **with corroboration** - the same round's delivery under 90 % of the rate - since the one risk it adds is the repair-delayed round trip sample that
  made 18 false trips in quick test 10, which the model cannot test; it is wire-compatible, passes every existing model check, and moves the 128 KiB sawtooth from 56 % to 41 % of the time over 1.3x.
  Collapse to expect and test first: a ratchet of cuts on a link that is fine - the test is lumpy and repair-delayed round trip samples injected into a steady CRUISE (the rate must stay), like the
  frozen-state test of section 25; the cut is bounded (BtlBw not under half, a drain of 0.75), so a collapse needs repeated trips.
- **Not for 2.2.0**: (b') has the larger effect (median 1.41 -> 1.00 on the 128 KiB sawtooth) but fails the swing guard and counts lumps as saturated readings - it needs a smoothing companion
  and Pi evidence; (a) is the only design that makes the **rise** faster (the model's oracle: 4.1 -> 0.2 s) but needs the reader's receive times (a wire change) or an unproven dense-solicitation variant.
- **Release**: none of this fits 2.2.0 on the evidence available. The model reproduces half of the problem (128 KiB over capacity), none of the rise or of the 1 MiB over-cut, and every earlier change made
  in the face of that (fast_probe, the quiet-gap carry) needed a Pi round to find its failure. ADAPTIVE stays the opt-in experimental option, its limits stated (it follows slow changes, 72-77 % of the time
  within +-30 %; fast ones 31-49 %, and it is over capacity for about half the time). If a change is wanted in 2.2.0 anyway, (c) with corroboration only, validated by the same varying-bandwidth run
  (about 3-4 h on the Pis); (b'), the smoothing, and a measured answer for (a) belong in the next release.

### 26.5 Seed-averaged model in the Pi-like configuration (supersedes the single-run table of 26.3)

The single-run comparison of 26.3 was too noisy to rank anything: the same variant moves by 5-15 points with the loss seed. The model was therefore (a) calibrated further and (b) run with **6 seeds**
(`model/run_model.sh`, which rebuilds everything from `tests/test_adaptive_pacing.c` and a patch of `pacing_adaptive.h`; the library is not changed).
Pi-like configuration: one round trip sample in 12 accepted (the Pi: 13-25 % of the rounds), reader reports bunched at a 8 ms tick, **repairs deferred behind the paced send** (as `pace_repair_allowed()`
does; without it the model has no lumps in its delivery readings). Averaged over the six dynamic patterns (P5a, P5b, P7, P6, P3, P4) its numbers are those of the Pi's: 128 KiB in +-30 % 59 % (Pi 58 %), over 1.3x
24 % (Pi 22 %), under 0.7x 17 % (Pi 20 %); 1 MiB 61 % (Pi 62 %), 19 % (Pi 9 %), 20 % (Pi 29 %). The 1 MiB over-capacity is more than the Pi's and the under-capacity less; the model's static links
are 1-9 % slower than the fixed rate in the p50 except M2 (128 KiB 192 against 115 ms, +67 %; 1 MiB 457 against 404, +13 %): a weakness of the model's M2, not a claim about the Pi.

Mean over the six patterns and the 6 seeds [lowest..highest seed mean]; static p50 / p99 in ms, mean over the seeds:

| 128 KiB | in +-30 % | over 1.3x | under 0.7x | cuts below 0.7x | M1 | M2 | S1 | S2 |
|---|---|---|---|---|---|---|---|---|
| current (448b685f) | 59 [50..63] | 24 [20..31] | 17 [13..19] | 8.0 | 48 / 122 | 192 / 335 | 282 / 326 | 322 / 417 |
| (c) corroborated one-sample queue cut | 61 [58..62] | 23 [16..25] | 17 [13..22] | 8.3 | 47 / 122 | 199 / 339 | 282 / 320 | 322 / 419 |
| (b') backlog-corrected app-limited flag, cannot raise | 62 [54..66] | 21 [17..29] | 17 [16..20] | 8.1 | 45 / 122 | 200 / 346 | 282 / 316 | 316 / 440 |
| rise cap 25 % per 100 ms | 60 [56..63] | 23 [20..26] | 17 [13..22] | 8.3 | 54 / 122 | 217 / 408 | 282 / 310 | 316 / 415 |
| rise cap 100 % per 100 ms | 59 [54..62] | 24 [22..26] | 17 [12..21] | 8.3 | 48 / 122 | 196 / 366 | 283 / 310 | 315 / 426 |
| rise cap 25 % + (c) | 60 [56..63] | 23 [21..25] | 17 [14..20] | 8.5 | 54 / 125 | 208 / 401 | 283 / 302 | 314 / 424 |

| 1 MiB | in +-30 % | over 1.3x | under 0.7x | cuts below 0.7x | M1 | M2 | S1 | S2 | retransmissions |
|---|---|---|---|---|---|---|---|---|---|
| current | 61 [60..62] | 19 [17..20] | 20 [18..23] | 13.6 | 322 / 906 | 457 / 904 | 2323 / 4109 | 2422 / 3336 | 10.4 % |
| (c) | 61 [57..67] | 19 [16..22] | 20 [15..23] | 14.2 | 323 / 906 | 465 / 915 | 2318 / 4746 | 2418 / 3287 | 10.2 % |
| (b') | 61 [56..67] | 19 [18..21] | 20 [14..26] | 13.9 | 325 / 935 | 495 / 923 | 2319 / 5111 | 2410 / 2978 | 10.3 % |
| rise cap 25 % | 62 [59..68] | **12 [10..14]** | 26 [20..30] | 16.9 | 333 / 740 | **556 / 1244** | 2311 / 4422 | 2398 / 3077 | 7.0 % |
| rise cap 100 % | 61 [57..68] | 17 [16..18] | 23 [14..27] | 15.1 | 320 / 713 | 508 / 981 | 2331 / 4649 | 2407 / 2933 | 8.5 % |
| rise cap 25 % + (c) | 63 [59..66] | 12 [10..14] | 25 [22..28] | 15.7 | 332 / 734 | 558 / 1202 | 2335 / 4446 | 2404 / 3224 | 6.8 % |

The existing model checks (frozen state, probe storm, replay of the Pi traces, the static links, the multi-reader runs), compiled against each variant: (c) 0 failures; (c) + the 25 % cap 0; (b') 1 (the M2r2 replay swings
17.8x, limit 11x); the 25 % cap alone 1 (2 readers: mean queue delay 20.1 ms, limit 20); the 100 % cap 1 (the watermark-jump run collapses to 1 sample).

Reading, against the requirements set for this stage (no regression of the static links, no frozen-state or probe-storm failure, fewer 1 MiB over-cut cascades, less time over capacity):
- **(c) and (b') are not distinguishable from the loss seed** in any column: the 128 KiB means move by 1-3 points inside seed ranges of 5-15 points, and nothing at 1 MiB moves. (c) is safe (no check fails) but buys nothing the model can show.
- **The rise cap is the only change with an effect outside the seed range, and it is a trade:** at 1 MiB the time over 1.3x falls from 19 % to 12 % and the retransmissions from 10.4 % to 7.0 %, but the time under 0.7x rises from 20 % to 26 %,
  the cuts from 13.6 to 16.9, and M2 at 1 MiB gets slower by 22 % in the p50 and 38 % in the p99 (static regression). The 100 % cap halves the loss and the gain.
- **No variant reduces the over-cut cascades** (cuts below 0.7x per run): the cascade is a drop-guard action on lumpy readings, and none of the candidates touches the guard.
- Not shown, and not shown for the real link either: the 24-36 s rise of 128 KiB (the model's step run recovers in 4 s with or without any variant).

Conclusion: **none of the candidates meets the requirements in this model, so none is implemented in the library** on this branch (`ab/adaptive-next2` carries this section and the harness only). What the evidence supports trying next, each a separate
change with its own Pi run: (1) a guard that does not halve BtlBw on a reading that a later round contradicts (the cascade), tested against the lumpy model above; (2) the time-based rise cap only if its static cost can be removed
(the M2 slowdown shows the cap also delays a legitimate rise); (3) the dispersion estimate (a) once the reader's receive times are available. None changes the 2.2.0 position: ADAPTIVE stays opt-in experimental, fixed pacing is the choice for a known link.

## 27. Algorithm selection for Wi-Fi and 5G: an offline comparison (2026-11-27, phase A, model only)

The user's decision of 2026-11-27: find a rate-control algorithm suited to Wi-Fi and 5G and put it into 2.2.0. This section is the **selection**, done offline on the PC; nothing here is in the library, the wire is unchanged, and a wire change needs the user's approval of the design in 27.6. **Every number in this section is a model number**: see 27.7 for what the model cannot show. A model win is not a Pi win.

### 27.1 The question, and the likely root mismatch

BBR-class estimators (the current ADAPTIVE is one) are built for a dense stream of per-ACK delivery and RTT samples. TickLE gets one lumpy receiver report per round, a round trip sample in about 1 of 12 rounds, an 8 ms reader tick, and an application that publishes a 128 KiB or 1 MiB sample every 4 s. A sample is a train of 89 or 723 back-to-back datagrams, and what the sender never has is the receiver's own view of that train. The candidates below differ mostly in what they read from the receiver.

### 27.2 Method

`examples/perf_hil/experiments/adaptive_pacing/algosim/` (27.8): `algosim.c`, a packet-level model with a 100 us clock. It is the link of `tests/test_adaptive_pacing.c` (paced sender, drop-tail queue served at the link rate, a reader that answers solicitations at its 8 ms poll tick, watermark ACKNACKs, repairs deferred behind the paced send) generalised to a schedule of rate, one-way delay and loss, a packet-count queue limit, outages (100% loss), a Gilbert-Elliott loss state (N4: netem `gemodel 1% 15% 60% 0.5%`), a lossy reverse path, and optional stress (27.5). The control, ADAPTIVE (`448b685f`), runs **unmodified** (`include/tickle/pacing_adaptive.h` plus two knobs: the start-rate ceiling and the decimation of round trip samples to the Pi's 1 in 12). Where the model can be checked against the Pi it agrees (ADAPTIVE without a prior, 128 KiB, N3: p50 73 ms in the Pi's 5G run, 73 ms here; N2 55 ms there, 65 ms here; the rate under 0.7x of the capacity for 84-100% of the time on N2/N3 there, 83-100% here), but it is not tuned to them.

**Feedback models.** (a) *Current*: what the library has, with a round trip sample in 1 of 12 rounds (candidates read the held count, the watermark, the sparse round trip). (b) *Enhanced*: (a) plus a receiver report every 100 ms while data arrives, with the fields each candidate needs (arrival time of the first and last datagram and the bytes in the interval, a 15-sample minimum of the relative one-way delay, new gaps, 5 ms send-group arrival times for GCC, an exact round trip echo; for the train candidates, the first and last arrival of the sample's head). The report is delayed by the one-way delay and may be lost like any datagram. TRAIN1 reads one report per sample instead.

**Candidates** (`start` = 8 Mbit/s unless `_hi`: 62.5 Mbit/s):

| name | what it is | deviations from the reference, stated |
|---|---|---|
| `oracle` | 95% of the instantaneous link rate | a bound, not realisable |
| `fixed` | 95% of the schedule's time-weighted mean | as the Pi runs: told the mean, an advantage |
| `adapt` | current ADAPTIVE | unchanged; `adapt_hi` = start 62.5 Mbit/s |
| `bbr3` | BBRv3 (STARTUP 2.77, DRAIN, PROBE_BW DOWN/CRUISE/REFILL/UP, bw max filter over 10 samples, bw_lo from a >2% loss round) on delivery-rate samples | no ProbeRTT, no cwnd (a paced sender); a sample is the held count over the report spacing (a) or the receiver's interval (b) |
| `nada` | NADA, RFC 8698: accelerated ramp-up, gradual update (kappa 0.5, eta 2, tau 500 ms, XREF 10 ms), x_curr = queue delay + loss term | RMAX = twice the best receive rate of the last 10 s (the RFC's RMAX is the application maximum, which a sender that does not know the link does not have: with 1 Gbit/s the equilibrium queue would be XREF x RMAX / r = 333 ms on a 30 Mbit/s link); ramp-up loss threshold 2% (the links lose 1-4% at random); no non-linear warping |
| `gcc` | GCC: 5 ms send groups, trendline over 20 points, adaptive threshold, over-use detector, AIMD (beta 0.85, 8%/s, additive near the last decrease), loss cap | no ProbeController (WebRTC probes at 3x and 6x at start and after an idle period; without it a bursty sender ramps slowly). Under (a) the gradient comes from the sparse round trips |
| `train` | **the head of every sample (first 20 datagrams, 29 KB) is sent back to back; the receiver reports the first/last arrival time of the train; the capacity is the dispersion; the rest is paced at 95% of it.** `train` also reads the 100 ms reports (a paced stretch the path spreads, corroborated by queue delay) and raises 25% after two clean readings | Sprout's stochastic forecast is not implemented; the dispersion of a train is the capacity sample and inter-sample gaps are ignored |
| `train1` | the train alone: **one report per sample**, nothing else read | |
| `train1t` | `train1` with a train of about 10 ms of the estimated link (at least 20 datagrams, at most half the sample): long against the slots of an aggregating link | the receiver needs the train length (27.6) |
| `traint` | `train` with the 10 ms train | |

**Workloads.** 19 schedules, each at 128 KiB (89 datagrams) and 1 MiB (723) published every 4 s (the same bursty pattern for all), 6 seeds (the seeds change the loss draws; N1-N4 are the single schedule draw of the Pi's 5G run): P3, P4, P5a, P5b, P6, P7 (the varying-bandwidth patterns of RESULTS section 5); RS (30 -> 4 -> 30 Mbit/s, steps on the 4 s sample grid), RSm (the same, not aligned) and RSd (the step down lands 20 ms into a transfer); M1, M2, S1, S2 (the static links); N1-N4 (the 5G-like schedules of `ab/wifi-adaptive-s1`, `wifitc/results/5g_schedules/`); H100 and H200 (static 100 and 200 Mbit/s, for the start). 11,196 runs with the stress sweep below.

**Metrics.** In +-30% / over 1.3x / under 0.7x: the share of the run (after 2 s) the pacing rate is within 30% of / above 1.3x / below 0.7x of 0.991 x the link rate, as in section 26. Queue>100ms: the share of the time a sample is being sent that the queue ahead of a new datagram is more than 100 ms of the link. p50 x oracle, p99 x oracle: the sample latency percentile, pooled over seeds, divided by the oracle's on the same workload (the geometric mean over the family's workloads; an undelivered sample counts as infinite). Undeliv+late: samples not delivered by the end or later than 4 s. Retx: repairs over datagrams sent. Recovery up~down: the median over the family's capacity steps of at least 1.5x lasting at least 3 s, of the time until the rate is at least 0.9x of the new capacity (up) or at most 1.1x of it (down); it is measured on the pacing rate, which a candidate that learns once per sample can only change once per sample. Startup: the time until the rate is at least 0.9x of the initial capacity (180 s = never; meaningful on H100/H200, N2, N3). Goodput per capacity, feedback bytes/s and two guard proxies are in `results/cells.csv`: slow<10% (rate below a tenth of the capacity while a sample is sent: the frozen state) and over>3x (rate above three times the capacity: the probe storm).

### 27.3 Results, one table per workload family

Each cell is 128 KiB / 1 MiB. `-a` = the current feedback, `-b` = the enhanced feedback. Tables of every candidate including the `_hi` variants, the per-workload numbers and the raw cells are in `algosim/results/`.

**Varying bandwidth (P3 P4 P5a P5b P6 P7)**; each cell: 128 KiB / 1 MiB

| candidate | in +-30% | over 1.3x | under 0.7x | queue>100ms | p50 x oracle | p99 x oracle | undeliv+late | retx | recovery up~down s | startup s |
|---|---|---|---|---|---|---|---|---|---|---|
| oracle | 100% / 100% | 0% / 0% | 0% / 0% | 0.0% / 0.0% | 1.00 / 1.00 | 1.00 / 1.00 | 0.0% / 0.0% | 1.0% / 1.0% | 0.0~0.0 / 0.0~0.0 | 0.0 / 0.0 |
| fixed | 37% / 37% | 33% / 33% | 29% / 29% | 7.0% / 30.7% | 1.19 / 1.16 | 0.95 / 1.04 | 0.0% / 0.0% | 1.1% / 15.9% | 7.5~7.5 / 7.5~7.5 | 180.0 / 180.0 |
| adapt-a | 56% / 59% | 27% / 18% | 17% / 23% | 3.2% / 18.4% | 1.22 / 1.17 | 1.11 / 1.56 | 0.0% / 0.2% | 1.1% / 10.5% | 6.1~5.6 / 1.2~2.7 | 4.2 / 0.3 |
| adapt_hi-a | 58% / 60% | 25% / 18% | 17% / 22% | 3.4% / 17.4% | 1.17 / 1.14 | 0.98 / 1.74 | 0.0% / 0.4% | 1.1% / 11.3% | 3.1~5.2 / 2.3~2.7 | 0.0 / 0.0 |
| adapt-b | 58% / 62% | 15% / 18% | 27% / 20% | 1.2% / 9.8% | 1.26 / 1.15 | 1.22 / 1.56 | 0.0% / 0.4% | 1.1% / 7.2% | 3.9~2.1 / 1.2~2.1 | 6.1 / 0.3 |
| bbr3-a | 39% / 19% | 54% / 78% | 7% / 3% | 4.6% / 25.4% | 1.12 / 1.13 | 0.94 / 1.14 | 0.0% / 0.0% | 1.1% / 62.6% | 0.0~7.5 / 0.0~7.5 | 0.1 / 0.1 |
| bbr3-b | 56% / 67% | 29% / 6% | 14% / 27% | 2.9% / 12.5% | 1.28 / 1.39 | 1.01 / 1.05 | 0.0% / 0.0% | 1.1% / 5.7% | 1.1~7.5 / 13.2~2.7 | 0.3 / 0.3 |
| nada-b | 44% / 65% | 21% / 5% | 35% / 30% | 1.1% / 5.4% | 1.54 / 1.64 | 2.91 / 1.70 | 0.0% / 0.1% | 1.1% / 3.0% | 17.1~7.5 / 3.8~1.7 | 32.1 / 4.2 |
| gcc-b | 33% / 58% | 34% / 3% | 33% / 39% | 1.8% / 2.4% | 1.47 / 1.74 | 1.03 / 1.28 | 0.0% / 0.9% | 1.1% / 1.7% | 20.0~7.5 / 20.0~1.8 | 101.1 / 130.1 |
| train-b | 84% / 81% | 5% / 17% | 11% / 2% | 3.8% / 27.6% | 1.20 / 1.03 | 0.96 / 1.01 | 0.0% / 0.0% | 1.0% / 3.4% | 1.2~2.1 / 1.2~2.1 | 0.2 / 0.2 |
| train1-b | 93% / 93% | 5% / 5% | 3% / 3% | 3.0% / 7.7% | 1.05 / 1.00 | 0.94 / 0.99 | 0.0% / 0.0% | 1.0% / 1.3% | 2.6~2.1 / 3.3~2.1 | 0.1 / 0.1 |
| train1t-b | 93% / 93% | 5% / 5% | 3% / 3% | 3.0% / 9.7% | 1.04 / 1.01 | 0.94 / 0.98 | 0.0% / 0.0% | 1.0% / 1.4% | 1.6~2.1 / 1.6~2.1 | 0.1 / 0.1 |
| traint-b | 83% / 78% | 5% / 20% | 11% / 2% | 3.8% / 27.6% | 1.22 / 1.03 | 0.96 / 1.00 | 0.0% / 0.0% | 1.0% / 3.8% | 2.2~2.1 / 1.2~2.1 | 0.2 / 0.2 |

**Rate step 30 -> 4 -> 30 Mbit/s (RS aligned with the 4 s sample grid, RSm not, RSd steps down inside a transfer)**; each cell: 128 KiB / 1 MiB

| candidate | in +-30% | over 1.3x | under 0.7x | queue>100ms | p50 x oracle | p99 x oracle | undeliv+late | retx | recovery up~down s | startup s |
|---|---|---|---|---|---|---|---|---|---|---|
| oracle | 100% / 100% | 0% / 0% | 0% / 0% | 0.0% / 0.0% | 1.00 / 1.00 | 1.00 / 1.00 | 0.0% / 0.0% | 1.0% / 1.0% | 0.0~0.0 / 0.0~0.0 | 0.0 / 0.0 |
| fixed | 0% / 0% | 34% / 34% | 66% / 66% | 36.1% / 58.6% | 1.32 / 1.39 | 1.03 / 1.11 | 0.0% / 0.0% | 1.1% / 34.6% | 39.4~40.4 / 39.4~40.4 | 120.0 / 120.0 |
| adapt-a | 60% / 62% | 21% / 16% | 19% / 23% | 8.6% / 29.7% | 1.25 / 1.09 | 0.98 / 1.93 | 0.0% / 1.9% | 1.1% / 9.4% | 6.5~6.2 / 3.7~1.5 | 4.2 / 0.3 |
| adapt_hi-a | 57% / 60% | 19% / 15% | 24% / 25% | 10.2% / 31.2% | 1.16 / 1.07 | 1.03 / 2.02 | 0.0% / 2.2% | 1.1% / 10.7% | 4.1~6.9 / 3.5~1.8 | 0.0 / 0.0 |
| adapt-b | 62% / 64% | 13% / 17% | 25% / 19% | 4.2% / 28.0% | 1.34 / 1.07 | 1.07 / 1.46 | 0.0% / 0.6% | 1.1% / 9.6% | 6.4~2.8 / 2.3~1.6 | 6.1 / 0.3 |
| bbr3-a | 25% / 22% | 62% / 76% | 14% / 3% | 37.0% / 57.7% | 1.36 / 1.11 | 1.05 / 1.20 | 0.0% / 0.0% | 1.1% / 64.6% | 5.2~40.4 / 0.0~38.3 | 0.1 / 0.1 |
| bbr3-b | 41% / 66% | 19% / 3% | 40% / 31% | 9.8% / 25.1% | 2.63 / 1.75 | 1.01 / 1.10 | 0.0% / 0.0% | 1.1% / 5.6% | 39.4~12.2 / 27.7~1.1 | 0.3 / 0.3 |
| nada-b | 45% / 60% | 8% / 3% | 48% / 37% | 2.1% / 8.2% | 2.69 / 2.85 | 2.26 / 1.50 | 0.0% / 0.0% | 1.1% / 2.2% | 28.4~8.2 / 4.5~0.4 | 32.1 / 4.2 |
| gcc-b | 29% / 30% | 5% / 1% | 66% / 69% | 4.6% / 1.4% | 3.09 / 2.99 | 1.13 / 2.44 | 0.0% / 8.4% | 1.0% / 3.2% | 39.4~9.1 / 39.4~0.5 | 120.0 / 40.1 |
| train-b | 78% / 76% | 11% / 22% | 11% / 1% | 15.4% / 63.8% | 1.35 / 0.98 | 0.96 / 1.04 | 0.0% / 0.0% | 1.1% / 4.6% | 2.4~0.2 / 2.4~0.2 | 0.2 / 0.2 |
| train1-b | 96% / 96% | 2% / 2% | 2% / 2% | 5.2% / 9.3% | 1.00 / 1.00 | 1.00 / 1.00 | 0.0% / 0.0% | 1.0% / 3.7% | 5.5~2.8 / 3.5~2.8 | 0.1 / 0.1 |
| train1t-b | 96% / 96% | 2% / 2% | 2% / 2% | 5.2% / 9.0% | 1.00 / 1.00 | 1.00 / 1.01 | 0.0% / 0.0% | 1.0% / 3.4% | 3.5~2.8 / 2.4~2.8 | 0.1 / 0.1 |
| traint-b | 79% / 73% | 9% / 26% | 12% / 1% | 15.4% / 63.3% | 1.36 / 1.00 | 0.99 / 1.07 | 0.0% / 0.0% | 1.1% / 5.3% | 2.4~0.2 / 2.4~0.2 | 0.2 / 0.2 |

**Static links (M1 M2 S1 S2)**; each cell: 128 KiB / 1 MiB

| candidate | in +-30% | over 1.3x | under 0.7x | queue>100ms | p50 x oracle | p99 x oracle | undeliv+late | retx | recovery up~down s | startup s |
|---|---|---|---|---|---|---|---|---|---|---|
| oracle | 100% / 100% | 0% / 0% | 0% / 0% | 0.0% / 0.0% | 1.00 / 1.00 | 1.00 / 1.00 | 0.0% / 0.0% | 2.0% / 2.1% | -~- / -~- | 0.0 / 0.0 |
| fixed | 100% / 100% | 0% / 0% | 0% / 0% | 0.0% / 0.0% | 1.00 / 1.00 | 1.00 / 1.00 | 0.0% / 0.0% | 2.0% / 2.1% | -~- / -~- | 0.0 / 0.0 |
| adapt-a | 54% / 56% | 15% / 16% | 31% / 28% | 1.6% / 22.2% | 1.12 / 1.04 | 1.56 / 2.11 | 0.0% / 1.3% | 2.2% / 8.1% | -~- / -~- | 2.1 / 0.1 |
| adapt_hi-a | 57% / 57% | 13% / 15% | 30% / 28% | 2.3% / 23.5% | 1.14 / 1.03 | 1.21 / 1.70 | 0.0% / 0.7% | 2.1% / 10.0% | -~- / -~- | 0.0 / 0.0 |
| adapt-b | 55% / 59% | 10% / 14% | 34% / 27% | 0.8% / 16.2% | 1.13 / 1.03 | 1.62 / 2.72 | 0.0% / 2.2% | 2.2% / 7.0% | -~- / -~- | 3.0 / 0.1 |
| bbr3-a | 58% / 30% | 31% / 65% | 11% / 5% | 3.8% / 32.4% | 1.03 / 1.07 | 1.08 / 1.29 | 0.0% / 0.0% | 2.1% / 54.8% | -~- / -~- | 0.1 / 0.1 |
| bbr3-b | 67% / 79% | 11% / 2% | 22% / 19% | 0.8% / 13.2% | 1.11 / 1.00 | 1.33 / 1.09 | 0.0% / 0.0% | 2.1% / 3.1% | -~- / -~- | 0.1 / 0.1 |
| nada-b | 35% / 38% | 5% / 1% | 60% / 61% | 0.5% / 2.2% | 1.88 / 2.37 | 3.93 / 4.01 | 0.0% / 38.8% | 2.1% / 2.3% | -~- / -~- | 16.1 / 2.1 |
| gcc-b | 59% / 84% | 4% / 0% | 36% / 16% | 0.6% / 0.4% | 1.20 / 1.09 | 1.32 / 1.77 | 0.0% / 0.0% | 2.0% / 2.1% | -~- / -~- | 30.1 / 19.1 |
| train-b | 81% / 79% | 12% / 21% | 7% / 0% | 9.0% / 39.8% | 1.01 / 0.96 | 1.20 / 1.10 | 0.0% / 0.0% | 2.1% / 5.2% | -~- / -~- | 0.1 / 0.1 |
| train1-b | 100% / 100% | 0% / 0% | 0% / 0% | 1.3% / 1.1% | 0.95 / 0.98 | 1.02 / 1.03 | 0.0% / 0.0% | 2.0% / 2.1% | -~- / -~- | 0.1 / 0.1 |
| train1t-b | 100% / 100% | 0% / 0% | 0% / 0% | 1.3% / 1.1% | 0.95 / 0.99 | 0.99 / 1.05 | 0.0% / 0.0% | 2.0% / 2.1% | -~- / -~- | 0.1 / 0.1 |
| traint-b | 82% / 78% | 12% / 22% | 6% / 0% | 9.0% / 39.8% | 1.00 / 0.97 | 1.17 / 1.10 | 0.0% / 0.0% | 2.1% / 5.4% | -~- / -~- | 0.1 / 0.1 |

**5G-like (N1 N2 N3 N4)**; each cell: 128 KiB / 1 MiB

| candidate | in +-30% | over 1.3x | under 0.7x | queue>100ms | p50 x oracle | p99 x oracle | undeliv+late | retx | recovery up~down s | startup s |
|---|---|---|---|---|---|---|---|---|---|---|
| oracle | 100% / 100% | 0% / 0% | 0% / 0% | 0.0% / 0.0% | 1.00 / 1.00 | 1.00 / 1.00 | 0.0% / 0.0% | 1.1% / 1.3% | 0.1~- / 0.1~- | 0.0 / 0.0 |
| fixed | 65% / 65% | 23% / 23% | 12% / 12% | 5.0% / 19.1% | 1.04 / 1.26 | 0.95 / 1.15 | 0.0% / 0.0% | 1.2% / 3.1% | 4.5~- / 4.5~- | 90.0 / 90.0 |
| adapt-a | 12% / 37% | 6% / 17% | 82% / 45% | 0.8% / 12.7% | 2.37 / 1.54 | 1.09 / 1.35 | 0.0% / 0.1% | 1.1% / 2.2% | 4.5~- / 4.2~- | 180.0 / 2.2 |
| adapt_hi-a | 12% / 40% | 7% / 19% | 81% / 41% | 0.9% / 14.3% | 2.34 / 1.52 | 1.12 / 1.25 | 0.0% / 0.0% | 1.2% / 2.3% | 4.5~- / 4.2~- | 90.0 / 0.1 |
| adapt-b | 9% / 33% | 4% / 16% | 86% / 51% | 0.0% / 9.7% | 2.69 / 1.64 | 1.46 / 2.43 | 0.0% / 0.2% | 1.3% / 1.9% | 4.5~- / 4.0~- | 180.0 / 7.3 |
| bbr3-a | 28% / 34% | 35% / 57% | 37% / 9% | 3.2% / 22.6% | 1.32 / 1.40 | 1.03 / 1.15 | 0.0% / 0.0% | 1.2% / 12.9% | 4.5~- / 0.7~- | 2.1 / 0.1 |
| bbr3-b | 52% / 43% | 24% / 12% | 25% / 45% | 4.1% / 11.0% | 1.18 / 1.77 | 1.13 / 1.28 | 0.0% / 0.0% | 1.3% / 2.5% | 0.0~- / 4.5~- | 8.1 / 0.4 |
| nada-b | 33% / 42% | 10% / 13% | 57% / 45% | 0.8% / 8.4% | 2.22 / 2.28 | 1.51 / 2.07 | 0.0% / 4.5% | 1.2% / 1.8% | 4.5~- / 4.2~- | 94.1 / 68.1 |
| gcc-b | 21% / 19% | 18% / 11% | 61% / 70% | 1.3% / 3.1% | 2.10 / 2.98 | 1.09 / 2.77 | 0.0% / 2.7% | 1.1% / 1.3% | 4.5~- / 4.5~- | 146.1 / 137.1 |
| train-b | 74% / 74% | 11% / 15% | 15% / 11% | 3.0% / 12.4% | 1.09 / 1.33 | 1.00 / 1.13 | 0.0% / 0.0% | 1.2% / 2.2% | 0.0~- / 0.0~- | 0.2 / 0.2 |
| train1-b | 76% / 76% | 12% / 12% | 12% / 12% | 3.4% / 8.8% | 1.05 / 1.25 | 0.99 / 1.14 | 0.0% / 0.0% | 1.3% / 2.1% | 0.0~- / 0.0~- | 0.1 / 0.1 |
| train1t-b | 76% / 76% | 12% / 12% | 12% / 11% | 3.5% / 9.9% | 1.05 / 1.26 | 1.09 / 1.12 | 0.0% / 0.0% | 1.2% / 2.2% | 0.0~- / 0.0~- | 0.1 / 0.1 |
| traint-b | 73% / 72% | 11% / 15% | 16% / 13% | 3.1% / 12.1% | 1.10 / 1.30 | 0.99 / 1.14 | 0.0% / 0.0% | 1.3% / 2.2% | 0.0~- / 0.0~- | 0.2 / 0.2 |

**High-rate static, startup (H100 H200)**; each cell: 128 KiB / 1 MiB

| candidate | in +-30% | over 1.3x | under 0.7x | queue>100ms | p50 x oracle | p99 x oracle | undeliv+late | retx | recovery up~down s | startup s |
|---|---|---|---|---|---|---|---|---|---|---|
| oracle | 100% / 100% | 0% / 0% | 0% / 0% | 0.0% / 0.0% | 1.00 / 1.00 | 1.00 / 1.00 | 0.0% / 0.0% | 0.1% / 0.1% | -~- / -~- | 0.0 / 0.0 |
| fixed | 100% / 100% | 0% / 0% | 0% / 0% | 0.0% / 0.0% | 1.00 / 1.00 | 1.00 / 1.00 | 0.0% / 0.0% | 0.1% / 0.1% | -~- / -~- | 0.0 / 0.0 |
| adapt-a | 0% / 47% | 0% / 10% | 100% / 43% | 0.0% / 0.0% | 3.07 / 1.40 | 2.25 / 7.15 | 0.0% / 0.0% | 0.1% / 1.1% | -~- / -~- | 60.0 / 14.1 |
| adapt_hi-a | 0% / 49% | 0% / 9% | 100% / 42% | 0.0% / 0.0% | 3.28 / 1.35 | 1.77 / 2.12 | 0.0% / 0.0% | 0.1% / 0.6% | -~- / -~- | 60.0 / 8.2 |
| adapt-b | 0% / 43% | 0% / 6% | 100% / 52% | 0.0% / 0.0% | 3.04 / 1.64 | 2.25 / 7.16 | 0.0% / 0.0% | 0.1% / 0.9% | -~- / -~- | 60.0 / 10.1 |
| bbr3-a | 12% / 48% | 28% / 38% | 60% / 14% | 0.0% / 0.0% | 1.49 / 1.48 | 1.60 / 1.68 | 0.0% / 0.0% | 0.1% / 16.0% | -~- / -~- | 32.0 / 0.2 |
| bbr3-b | 49% / 76% | 23% / 9% | 28% / 14% | 0.0% / 0.0% | 1.02 / 1.24 | 2.59 / 2.99 | 0.0% / 0.0% | 0.1% / 3.6% | -~- / -~- | 6.1 / 2.2 |
| nada-b | 33% / 52% | 0% / 0% | 67% / 48% | 0.0% / 0.0% | 2.22 / 1.51 | 3.42 / 14.16 | 0.0% / 0.0% | 0.1% / 0.5% | -~- / -~- | 46.1 / 20.1 |
| gcc-b | 0% / 0% | 0% / 0% | 100% / 100% | 0.0% / 0.0% | 5.29 / 5.93 | 2.71 / 9.89 | 0.0% / 0.0% | 0.1% / 0.1% | -~- / -~- | 60.0 / 60.0 |
| train-b | 99% / 100% | 0% / 0% | 1% / 0% | 0.0% / 0.0% | 0.98 / 1.13 | 1.99 / 1.71 | 0.0% / 0.0% | 0.1% / 1.4% | -~- / -~- | 0.2 / 0.2 |
| train1-b | 100% / 100% | 0% / 0% | 0% / 0% | 0.0% / 0.0% | 0.98 / 0.98 | 1.30 / 1.05 | 0.0% / 0.0% | 0.1% / 0.1% | -~- / -~- | 0.1 / 0.1 |
| train1t-b | 100% / 100% | 0% / 0% | 0% / 0% | 0.0% / 0.0% | 0.98 / 1.01 | 1.30 / 1.05 | 0.0% / 0.0% | 0.1% / 2.0% | -~- / -~- | 0.1 / 0.1 |
| traint-b | 98% / 79% | 0% / 0% | 2% / 21% | 0.0% / 0.0% | 0.98 / 1.33 | 1.99 / 1.71 | 0.0% / 0.0% | 0.1% / 3.6% | -~- / -~- | 0.2 / 0.2 |

### 27.4 Ranking

Verdicts over all 38 (workload, size) cells, on the per-seed p50 latency (W or L when at least 5 of 6 seeds are better or worse by 5%):

| candidate | vs current ADAPTIVE (adapt-a) W/T/L | vs fixed 95% of the mean | vs oracle |
|---|---|---|---|
| **train1t-b** (train, 10 ms, one report per sample) | **25 / 13 / 0** | 13 / 23 / 2 | 0 / 33 / 5 |
| **train1-b** (train of 20, one report per sample) | **25 / 13 / 0** | 13 / 24 / 1 | 0 / 33 / 5 |
| train-b (train + 100 ms reports) | 17 / 20 / 1 | 7 / 29 / 2 | 0 / 27 / 11 |
| traint-b | 14 / 22 / 2 | 8 / 26 / 4 | 1 / 24 / 13 |
| bbr3-a (BBRv3 on the current feedback) | 12 / 23 / 3 | 7 / 23 / 8 | 0 / 14 / 24 |
| bbr3-b | 9 / 20 / 9 | 0 / 19 / 19 | 0 / 17 / 21 |
| adapt_hi-a (start 62.5 Mbit/s) | 0 / 38 / 0 | 6 / 19 / 13 | 0 / 15 / 23 |
| adapt-b (dense RTT) | 0 / 36 / 2 | 3 / 19 / 16 | 0 / 12 / 26 |
| nada-b | 6 / 8 / 24 | 0 / 5 / 33 | 0 / 3 / 35 |
| gcc-b | 2 / 12 / 24 | 1 / 6 / 31 | 0 / 6 / 32 |
| train1-a, train-a (the train read off the current feedback) | 0 / 18 / 20 | 0 / 14 / 24 | 0 / 9 / 29 |

1. **TRAIN1 / TRAIN1T on the enhanced feedback is the only candidate that is near the oracle in every family**: within 30% of the capacity 93% of the time on the varying patterns (ADAPTIVE 56-59%), 100% on the static links, 76% on the 5G-like links (ADAPTIVE 12% at 128 KiB, 37% at 1 MiB), within 5% of the oracle's p50 on the varying, static and high-rate families, no loss against ADAPTIVE in any of the 38 cells, startup 0.1 s on 100 and 200 Mbit/s where ADAPTIVE is below 90% of the capacity for the whole 60 s at 128 KiB. It needs a receiver timestamp the library does not report (27.6). Where it is worse than the oracle (5 cells) is after a change that lands inside a long transfer: it measures once per sample.
2. `train` with the 100 ms reports is **not better than the train alone** in this model (its paced-stretch probing costs queue: queue>100ms 28% on the varying patterns at 1 MiB against 8%); the extra feedback is not worth its wire change.
3. **BBRv3 on the current feedback is a trap**: it wins 12 cells against ADAPTIVE on latency but at 1 MiB it repairs 55-65% of what it sends on the static, step and varying families and sits above 3x the capacity 33-45% of the time (a lumpy held count read as a delivery rate). With the enhanced feedback it is stable (retx 3-6%) but slow to rise (recovery after a step up 13-39 s) and does not beat ADAPTIVE overall (9/20/9).
4. NADA and GCC, even with the enhanced feedback, are slower than ADAPTIVE in 24 of 38 cells. Their rise is the limit: NADA ramps 20% per report and a 128 KiB sample is one report, GCC 8% per second of an application that sends 100 ms in 4 s. They would need a start-up probe or a sender that is not application-limited; a bursty application is the case they were not made for.
5. **No variant on the current feedback is better than ADAPTIVE without a price.** The only no-wire change with a model benefit is the start rate (`adapt_hi`: 0 wins, 0 losses; 1 MiB on 100-200 Mbit/s startup 14 -> 8 s, p99 7.2x -> 2.1x the oracle's; the static links' 128 KiB p99 1.56x -> 1.21x) and it does nothing for the 5G case.

**The 5G report's inferred cause is not supported by the model.** The 5G run (RESULTS section 5) inferred, and said it had not tested, that ADAPTIVE's 16-19% of the capacity at 128 KiB on N2/N3 came from its 8 Mbit/s start rate. In the model a start rate of 62.5 Mbit/s leaves N3 at 75 ms (73 ms with 8 Mbit/s) and N2 at 58 ms (65 ms): the estimator does not climb during a 128 KiB sample (it ends before a probe round completes) and falls to the measured rate after every blockage or outage. **A higher start rate is not the fix** (this is a model result; the Pi has not been run with one).

### 27.5 Stress: links that serve in slots, and a receiver clock with jitter

The model's link serves continuously; a Wi-Fi TXOP or a 5G TTI delivers a queue in lumps, and a receiver stamps arrivals with a clock of finite resolution and interrupt coalescing. The stress sweep serves the queue only at slot boundaries (the datagrams of a slot arrive together) and adds uniform jitter to the receiver's arrival stamps, on 11 workloads. Cell: in +-30% / p50 x oracle / p99 x oracle / over 3x.


#### Stress, 128 KiB (workloads P7 N1 N2 N3 N4 M1 S1 RS RSm RSd H100; 6 seeds): in +-30% of the capacity / p50 x oracle / p99 x oracle / over 3x

| candidate | clean | slot 1 ms, jitter 0.2 ms | slot 2 ms, jitter 0.5 ms | slot 4 ms, jitter 1 ms |
|---|---|---|---|---|
| oracle | 100% / 1.00 / 1.00 / 0.0% | 100% / 1.00 / 1.00 / 0.0% | 100% / 1.00 / 1.00 / 0.0% | 100% / 1.00 / 1.00 / 0.0% |
| fixed | 56% / 1.11 / 0.98 / 11.6% | 56% / 1.11 / 0.98 / 11.6% | 56% / 1.11 / 0.99 / 11.6% | 56% / 1.13 / 1.01 / 11.6% |
| adapt-a | 37% / 1.62 / 1.23 / 2.1% | 36% / 1.72 / 1.30 / 1.6% | 38% / 1.66 / 1.46 / 2.3% | 32% / 1.76 / 2.00 / 2.4% |
| adapt_hi-a | 37% / 1.59 / 1.12 / 2.2% | 38% / 1.64 / 1.17 / 1.7% | 36% / 1.63 / 1.24 / 2.3% | 33% / 1.77 / 1.25 / 2.2% |
| bbr3-b | 53% / 1.43 / 1.22 / 5.6% | 50% / 1.38 / 1.21 / 8.2% | 47% / 1.38 / 1.28 / 6.6% | 43% / 1.41 / 1.27 / 10.3% |
| nada-b | 42% / 1.95 / 2.31 / 1.3% | 39% / 1.89 / 2.49 / 1.6% | 42% / 1.82 / 2.36 / 1.6% | 34% / 1.93 / 2.38 / 1.7% |
| gcc-b | 31% / 2.18 / 1.26 / 1.7% | 33% / 2.21 / 1.33 / 1.6% | 32% / 2.20 / 1.36 / 1.7% | 32% / 2.20 / 1.34 / 1.6% |
| train-b | 78% / 1.14 / 1.09 / 1.8% | 74% / 1.11 / 1.14 / 1.7% | 60% / 1.14 / 1.18 / 1.6% | 58% / 1.23 / 1.19 / 1.8% |
| train1-b | 88% / 1.02 / 1.02 / 2.1% | 81% / 1.02 / 1.03 / 1.9% | 64% / 1.07 / 1.06 / 1.9% | 62% / 1.13 / 1.07 / 2.0% |
| train1t-b | 88% / 1.02 / 1.03 / 2.1% | 86% / 1.01 / 1.01 / 2.0% | 81% / 1.01 / 1.09 / 1.9% | 63% / 1.10 / 1.07 / 2.1% |
| traint-b | 78% / 1.14 / 1.09 / 1.7% | 78% / 1.15 / 1.13 / 1.6% | 75% / 1.11 / 1.18 / 1.6% | 58% / 1.18 / 1.15 / 1.7% |

#### Stress, 1 MiB (workloads P7 N1 N2 N3 N4 M1 S1 RS RSm RSd H100; 6 seeds): in +-30% of the capacity / p50 x oracle / p99 x oracle / over 3x

| candidate | clean | slot 1 ms, jitter 0.2 ms | slot 2 ms, jitter 0.5 ms | slot 4 ms, jitter 1 ms |
|---|---|---|---|---|
| oracle | 100% / 1.00 / 1.00 / 0.0% | 100% / 1.00 / 1.00 / 0.0% | 100% / 1.00 / 1.00 / 0.0% | 100% / 1.00 / 1.00 / 0.0% |
| fixed | 56% / 1.19 / 1.08 / 11.6% | 56% / 1.20 / 1.08 / 11.6% | 56% / 1.19 / 1.06 / 11.6% | 56% / 1.21 / 1.08 / 11.6% |
| adapt-a | 52% / 1.23 / 1.79 / 2.4% | 47% / 1.33 / 1.91 / 2.1% | 49% / 1.27 / 1.89 / 2.4% | 47% / 1.29 / 2.31 / 2.1% |
| adapt_hi-a | 52% / 1.22 / 1.63 / 2.4% | 52% / 1.26 / 1.88 / 2.2% | 53% / 1.18 / 1.92 / 2.2% | 49% / 1.25 / 1.91 / 2.6% |
| bbr3-b | 60% / 1.49 / 1.24 / 1.7% | 60% / 1.45 / 1.24 / 1.8% | 61% / 1.40 / 1.27 / 2.1% | 62% / 1.31 / 1.27 / 2.4% |
| nada-b | 54% / 1.97 / 2.47 / 1.3% | 51% / 1.88 / 2.47 / 1.5% | 51% / 1.97 / 2.49 / 1.8% | 50% / 1.98 / 2.33 / 2.2% |
| gcc-b | 34% / 2.44 / 2.48 / 0.4% | 35% / 2.43 / 2.49 / 0.4% | 34% / 2.38 / 3.42 / 0.4% | 34% / 2.34 / 2.48 / 0.4% |
| train-b | 77% / 1.12 / 1.11 / 2.1% | 74% / 1.12 / 1.12 / 2.0% | 68% / 1.15 / 1.13 / 2.1% | 64% / 1.19 / 1.39 / 2.0% |
| train1-b | 88% / 1.08 / 1.05 / 2.2% | 82% / 1.15 / 1.10 / 1.9% | 64% / 1.22 / 1.13 / 1.9% | 62% / 1.28 / 1.53 / 1.9% |
| train1t-b | 88% / 1.08 / 1.05 / 2.2% | 86% / 1.10 / 1.08 / 2.0% | 85% / 1.15 / 1.10 / 2.0% | 79% / 1.23 / 1.54 / 3.5% |
| traint-b | 75% / 1.12 / 1.12 / 2.1% | 75% / 1.12 / 1.13 / 2.1% | 74% / 1.11 / 1.13 / 2.0% | 71% / 1.13 / 1.38 / 2.0% |

TRAIN1's fixed 20-datagram train loses its accuracy once the train spans fewer than a few slots (at 100 Mbit/s 20 datagrams are 2.4 ms: one or two slots of 2-4 ms): in +-30% falls from 88% to 62-64% at 128 KiB, p50 1.13x at 4 ms. The 10 ms train (TRAIN1T) holds 81-86% up to 2 ms slots with 0.5 ms jitter and 63-79% at 4 ms / 1 ms; its p99 at 1 MiB at 4 ms is 1.54x, against ADAPTIVE's 2.31x on the same link. So the length of the train must be long against the link's service interval (about 10 ms and at least a few slots), and a heavily aggregating link degrades it to the level of the current ADAPTIVE, no worse than it in this model. Packet-train dispersion is known to be fragile against cross traffic, token-bucket bursts, multi-hop paths and radio rate adaptation; **none of these is in the model.**

### 27.6 The exact wire and feedback change for the top two (a proposal, not implemented; needs the user's approval)

**(1) TRAIN1 / TRAIN1T: no new submessage, no `tt_VERSION` change, interoperable with v2.1/v2.2 readers and writers.**

* **Sender.** The first K datagrams of a large sample go out back to back (K = 20, or about 10 ms of the estimated link, between 20 and half the sample), the rest at 95% of the capacity estimate. Immediately after datagram K a HEARTBEAT is sent with a new flag bit **`tt_HEARTBEAT_FLAG_TRAIN` (1U << 2)**, `last_seq_no` = the train's last datagram. Flag bits 0 (FINAL) and 1 (LIVELINESS) are the only ones a v14 receiver tests (`process_heartbeat()` reads them as `flags & BIT`), so an older reader sees an ordinary HEARTBEAT; K is carried by the range, so a variable K needs no field. Cost: one HEARTBEAT (about 70 B on the wire) per large sample.
* **Reader.** It stamps the arrival of the first datagram of the sample and of `last_seq_no` (two clock reads per sample) and answers with its ordinary ACKNACK, whose **`tt_AckNackHeader.reserved` (u16, written zero and read by no one: `tickle.c` writes `acknack_header->reserved = 0` and no receive path reads it)** now carries the span between the two arrivals in 8 us units (0 = no measurement, the old meaning; 0xFFFF = 524 ms or more). The datagrams of the train that arrived follow from the ACKNACK's watermark and bitmap, so no count field is needed. Cost: **0 bytes**: the ACKNACK is sent today.
* **Sender, on the answer.** capacity = (datagrams received - 1) x wire bytes x 8 / span; a missing or zero span means an older reader and the sender keeps the current ADAPTIVE (feedback model a).
* **Compat.** Same rule as `marks` and `summary_interval` (section 3, the user's decision of 2026-10-22: a reserved field given a meaning whose zero is the old one). A v2.1/2.2 writer ignores the field; a v2.1/2.2 reader ignores the flag and answers with zero. A context with the train off (the default) sends today's bytes; with it on, the flag bit and the ACKNACK field are the only new patterns, and the wire identity tests would mask them as they mask `marks`. The one asymmetry: a **new writer sends 20 datagrams unpaced to an old reader** (the old reader can't use it); the queue ahead of the link must hold them.
* **Rate and size.** One report per large sample; extra bytes: the one HEARTBEAT per sample; the model's feedback total for train1-b is below ADAPTIVE's in every family (6.6 against 7.9 kB/s on the varying patterns at 1 MiB), since the ACKNACK/solicit stream is unchanged.
* **Implementation risk.** The reader's arrival stamps must be per datagram: the Linux receive path drains in batches (`recvmmsg`, the GRO merge, `rx_drain`), which hides the spacing a train is measured by; kernel receive time stamps (`SO_TIMESTAMPING`) or reading the clock per datagram in the drain would be needed, and the FreeRTOS path stamps at its own receive. This is the first thing to measure on the Pi.

**(2) TRAIN with 100 ms reports** (second in the ranking; **not recommended**: it adds little to (1) in the model): a **new submessage `RX_REPORT` (type 16)** from the reader every 100 ms while a large sample arrives: `endpoint_id` u32, `entity_id` u32, `rx_bytes` u32, `t_first` u32 and `t_last` u32 (reader microseconds), `n_pkts` u16, `gaps_new` u16, `owd_min15` i32 (a relative one-way delay in microseconds), and the train block of (1) (`span` u16): 32 B + 4 B submessage header; 82 B on the wire, **820 B/s while a transfer runs**. Compat: by the rule of section 3 a new submessage type bumps `tt_VERSION` (14 -> 15) and v14 and v15 nodes refuse each other's every datagram, which breaks 2.2.0 against 2.1.x; the alternative (no bump, relying on the receive path's "unknown submessage type, skipping" for type 16) leaves old readers logging a warning per report and is a departure from the rule that is the user's to decide.

Everything else the candidates read (the NADA/GCC/BBRv3-b fields) needs this submessage too, which is a reason to prefer (1).

### 27.7 What the model cannot show

* It does not reproduce the Pi's **lumpy readings** (executor and `recvmmsg` batching, ACKNACKs bunched behind the reader's work), **repair-delayed RTT samples**, or **absolute latency**; its RTT samples are cleaner, its reports are never merged, and its reader ticks at 8 ms exactly. The model's ADAPTIVE is therefore likely better than the Pi's, and the train's arrival stamps likely noisier on the Pi than here.
* **Single flow, single reader, no cross traffic**, a symmetric link, a reverse path without queueing; the N3 delay steps are made order-preserving (a delay decrease compresses the flight).
* The links serve **continuously** unless the stress is on; real Wi-Fi aggregates and rate-adapts, real 5G schedules per slot and per user. The stress covers two crude cases; cross traffic and token buckets are not covered.
* The train is **ideal** in the clean runs: the receiver stamps exactly. Whatever the Linux receive path does to arrival times is the largest unknown (27.6).
* The oracle and the fixed arm know the capacity or its mean; TRAIN1 knows neither. The comparison with `fixed` is the one that matters for the recommendation "fixed pacing at 95% of the known bandwidth".
* NADA, GCC and BBRv3 are re-implementations of the algorithms from their documents, with the deviations of the table in 27.2, not the vendors' code, and tuned by nobody; GCC runs without its probe controller. Their poor result is a statement about a bursty application and these feedback models, not about those algorithms in their home settings.
* Seeds change the loss draws and the BBR probe timers; the schedules (N1-N4 included) are single draws. 6 seeds per cell.
* **Guards.** The existing `tests/test_adaptive_pacing.c` checks (frozen state, probe storm, the M2r2/S2r2 replays) pass with the unmodified header, and with a start ceiling of 62.5 Mbit/s (they use caps of 30 Mbit/s and below, so that variant is not stressed by them). They test the ADAPTIVE header, so they do not apply to the other candidates; their model proxies are slow<10% and over>3x in `results/cells.csv`: BBRv3-a is over 3x the capacity 33-45% of the time at 1 MiB (a probe storm), the train candidates 0-3.6% (the outages of N2/N3 and the steps), ADAPTIVE 0-4%.

### 27.8 Reproduce

```
examples/perf_hil/experiments/adaptive_pacing/algosim/build.sh <repo root> <out dir>     # algosim from the unmodified header
python3 .../algosim/mksched.py .../algosim/sched <dir with N1-N4.json>                      # the schedules (committed)
python3 .../algosim/run_all.py <out>/algosim .../algosim/sched <out>/res 6 6                 # 6 jobs, 6 seeds
python3 .../algosim/aggregate.py <out>/res tables.md cells.csv; python3 .../algosim/condense.py tables.md
CANDS=... WLS=... SLOT_US=2000 JITTER_US=500 python3 .../algosim/run_all.py ...             # the stress sweep
```
`algosim <sched> <datagrams per sample> <candidate> <a|b> <seed>` prints one line per run. A run takes 0.01-0.3 s; the whole sweep a few minutes on 6 cores.

### 27.9 Recommendation (phase A)

1. If the user wants a Wi-Fi/5G-capable ADAPTIVE in 2.2.0, the candidate is **the packet train (TRAIN1T) with a one-field feedback in `tt_AckNackHeader.reserved`**: no version bump, no new submessage, v2.1/2.2 interoperable, no bytes added. It is the only candidate that follows the capacity in every family of the model, and it removes the 128 KiB startup problem on 100-200 Mbit/s links (in the model).
2. **Before any library work, measure on the Pi whether a reader can stamp per-datagram arrival times well enough** (27.6, implementation risk) and what the Pi's receive path does to a 29 KB train: a first, small Pi run with the train and the existing ACKNACK, no pacing change. That is phase B.
3. Without the wire change: nothing in the model beats the current ADAPTIVE; `adapt_hi` (the start rate) is a safe small gain and does nothing for 5G. The 2.2.0 position stays: fixed pacing at about 95% of the known bandwidth, ADAPTIVE opt-in experimental for Wi-Fi-class links.
4. The sentence in the 2.2.0 docs that gives the 8 Mbit/s start rate as the (untested) cause of ADAPTIVE's low rate on the 5G-like links should be reworded: the model does not support it (27.4).

### 27.10 Compatibility audit of the approved wire design, in code (2026-11-27)

The user approved 27.6(1) (a TRAIN bit in `tt_HeartbeatHeader.flags`, the arrival span in `tt_AckNackHeader.reserved`, no new submessage, no `tt_VERSION` change) on the condition that "reserved is read by nothing" holds in code on every path. Read from the source, not argued:

* **(a) HEARTBEAT flags.** `flags` is read at two places. `process_heartbeat()` tests `flags & tt_HEARTBEAT_FLAG_LIVELINESS`; the flags are then copied into `struct heartbeat_ctx`, whose only reader, `inform_subscriber_of_heartbeat()`, tests `ctx->flags & tt_HEARTBEAT_FLAG_FINAL`. The discovery summary sets `FINAL` and nothing reads the byte as a whole. So a reader that predates bit 2 ignores it: on a HEARTBEAT with FINAL clear it answers (an ACKNACK, with or without a gap) exactly as before. The rmw layer and the FreeRTOS build go through the same functions. **Confirmed.**
* **(b) `tt_AckNackHeader.reserved`.** Written at exactly two places, both zero: the data ACKNACK encoder (`send_acknack_*()`, the only ACKNACK a reader sends for a data endpoint) and `send_discovery_request()`. Read at none: `process_acknack()` decodes the header and uses `endpoint_id`, `seq_no`, `entity_id`, `sender_entity_id`, `bitmap_words` and the bitmap, nothing else. Nothing in `rmw_tickle/`, `platform/`, `examples/` or `tools` reads or validates it, and the repository has no Wireshark dissector (`find`: none). `include/tickle/tickle.h` documents the field as `reserved` with no meaning. An old writer therefore puts 0 there, and ignores what it receives. **Confirmed: the claim "reserved is read by nothing" holds on every path of this tree.** Two cautions that are not failures of the claim: (1) tests that build an ACKNACK by hand (`test_seq_wrap.c`, `test_tx_class.c`, `test_init_options_reliable.c`, `test_peer_discovery.c`, `test_data_frag.c`) leave `reserved` as it came (some declare the struct uninitialised), so a new writer must read the field **only while a TRAIN heartbeat is outstanding to that reader and only from that reader's next ACKNACK**, never from any ACKNACK; (2) section 3's ACKNACK row lists the body without the field, to be corrected when it gets a meaning.
* **(c) The contract of the new code** (not implemented; the tests of (d) get a new-build row when it is): a new reader writes a nonzero `reserved` only in the ACKNACK that answers a TRAIN heartbeat whose range it has arrival stamps for, and 0 in every other ACKNACK; a new writer reads `reserved == 0` as "no measurement" and falls back to the current estimator; a TRAIN bit from a build that does not send it cannot occur (no old build sets it).
* **(d) Interop, both directions, against released builds.** `tests/compat_train.sh [TREE-ISH ...]` (default: the `v2.1.0` tag and this tree) builds `tests/compat_train_driver.c` against each tree's own core and mock HAL, the way `tests/version_mix.sh` does, and drives it with the two new bit patterns: **R** a Subscriber is handed a HEARTBEAT with bit 2 set, with every high bit set, with and without a gap open, and must send exactly the datagrams the same HEARTBEAT without the bit draws (controls: FINAL set draws none, so the driver can tell a reply from none); **W** a reliable Publisher is handed an ACKNACK with `reserved` 0xBEEF and 0xFFFF, naming a gap and a pure acknowledgement, and must send exactly the repairs, and hold the same peer state, as with 0 (control: another bitmap draws other repairs). Run on `v2.1.0`, `a0dd559a` (the 2.2.0 head at the time) and this tree: **24 of 24 checks pass.** It is a whitebox test of the cores, not a two-process run: what it proves is the byte-for-byte equality of the old core's response, which is what interoperation rests on.

### 27.11 The receive-path probe (phase B step 1)

`examples/perf_hil/experiments/train_probe.c` and `train_probe_rig.sh`. No library change: it uses the real Linux HAL (`tt_receive()` / `tt_try_receive()` after `tt_rx_drain_start()`, receive offload as the HAL turns it on at a FRAG_CONT_L fragment, `tt_send_batch()` as core sends a sample's fragments) and, beside it, plain sockets with the kernel's software receive time stamps (`SO_TIMESTAMPING`), read one datagram at a time, in batches of 32 (`recvmmsg()`), and with `UDP_GRO`. A train is N datagrams of 1,468 B (a fragment on the wire) sent back to back every PERIOD ms; per train the receiver measures the span between the first and last datagram by every reader and says it against the ideal (N - 1) x (size + overhead) x 8 / rate, the number of distinct stamps (stamps more than 10 us apart are distinct: N = every datagram has its own stamp, 1 = the whole train under one stamp, the batching hiding the spacing), and how far the user stamp lags the kernel's. Output per cell: p5/p25/p50/p75/p95; `--csv` writes every train.

On the PC's loopback (no shaper, so the spans are those of an unshaped link and mean only that the readers run): the HAL without offload stamps a 20-datagram train over 22-51 us with 1-2 distinct stamps (on loopback the datagrams are less than 10 us apart, so this says nothing yet), with offload 12-44 us with 2, the HAL holding 8.9 datagrams per hand-out. The result that matters is the Pi's, on a shaped link.

```
# on each Pi (a checkout of ab/algo-select):
examples/perf_hil/experiments/train_probe_rig.sh build /tmp/train_probe
# from a host that reaches both (the tc rig: the two Pis in their namespaces, the link shaped by RATE_CMD):
SEND_EXEC="ssh <pi-a> sudo ip netns exec <ns-a>" RECV_EXEC="ssh <pi-b> sudo ip netns exec <ns-b>" \
RECV_IP=<b's address as a reaches it> SEND_BCAST=<a's broadcast> RECV_BCAST=<b's broadcast> \
RATE_CMD="<command that sets the link to $1 kbit/s>" RATES="4000 30000 100000 200000" TRAIN_SIZES="10 20 40" \
OUTDIR=/tmp/train_probe_out  examples/perf_hil/experiments/train_probe_rig.sh run | tee /tmp/train_probe_all.txt
```
84 cells of 200 trains (about 30 minutes at the default period); `TRAINS=50` for a first look. What to send back: `/tmp/train_probe_all.txt` and the `OUTDIR`.

## 28. The packet train in the library (2026-11-27, branch `ab/algo-impl`; default OFF; not validated on the Pis)

The user approved the wire design of 27.6 / 27.10 on 2026-11-27 and the receive-path probe came back GO (spans at 0.99-1.01 x ideal at 4-100 Mbit/s, one stamp per datagram on the HAL path; at 200 Mbit/s every mode read low, 0.94-0.98 at N = 20-40; a 4 ms-slot link's burst arrives within ~40 us). This section is what was built. Everything is behind `rate_control = tt_RATE_CONTROL_ADAPTIVE` (opt-in, experimental, needs `tt_LARGE_SAMPLES`); a context that is not ADAPTIVE sends today's bytes and runs today's code, bar the reader's stamps below.

### 28.1 What was built

* **Writer** (`tickle.c`, "The packet train, sending side"; `large_send_some()`, `train_begin()`, `train_sent()`). On an ADAPTIVE link the first K datagrams of a large sample go out in one batch past the link's bucket (`large_paced_count()` returns them unpaced; `pace_charge_batch()` charges them, so the bucket carries their debt and the rest of the sample waits for it at the pacing rate: the average rate stands), then a HEARTBEAT with `tt_HEARTBEAT_FLAG_TRAIN` (FINAL clear) naming the K-th datagram's seq_no. Per reader, a train is outstanding until its span comes back (`struct tt_PeerAck.train_*`). In the hybrid transport's run mode (AUTO fragmentation, a reader whose loss is measured) the head still goes as K MTU units, then the sample continues in runs.
* **Reader** (`train_rx_note()` in `process_frag_large()`, `train_rx_close()` in `process_heartbeat()`, `train_rx_take()` in the ACKNACK encoder). Each fragment of a sample's first `tt_TRAIN_MAX_K` (40) is stamped with `tt_get_ns()` as core handles it (per remote writer, `struct tt_TrainRx`, 4 writers, about 0.9 KB a context). A TRAIN HEARTBEAT computes the span between fragment 0 and the named last one; the next ACKNACK to that writer carries it in `reserved` in 8 us units (saturating at 0xFFFF = 524 ms), once. **0 unless all K fragments arrived, in order, and the span is for this sample**; 0 in every other ACKNACK; a span not sent within 1 s is dropped.
* **Estimator** (`pacing_adaptive.h`, `ap_train_sample()`; `rate_acknack()` reads `reserved`). The capacity sample is the wire bytes of the K - 1 datagrams after the first over the span.
* **Wire**: as 27.6: bit 2 of the HEARTBEAT flags, `tt_AckNackHeader.reserved`; no version change, no new submessage; docs/DESIGN.md section 3 updated.

### 28.2 Bounds, and why

* **K**: 20 datagrams (30 KB) until the link is known, then about 10 ms of the estimate, between 20 and 40 (61 KB): 24 at 30 Mbit/s, 40 from about 50 Mbit/s up (the tc agent's finding that short trains read low at 200 Mbit/s: 40 datagrams are 2.4 ms there; the probe read N = 40 at 0.97-0.98). **The burst bound**: 40 datagrams, because the smallest queue the rig's links have is 100 packets (M and S: `limit` 100) and a train must leave room for what is queued; a standing queue that the train overflows loses fragments of it, and the reader then reports no span (no measurement). A sample shorter than 2 K datagrams has no train.
* **What the clock resolves**: the span is in 8 us units and read to +-1-2 us (probe); **spans under 16 us (two units) are not read**, and with a line rate of 1 Gbit/s a 20-datagram train spans 234 us (3.4% quantisation), a 40-datagram one 480 us.
* **Plausibility** (`ap_train_sample()`): no span, under 16 us, over 2 s, no rate, **above 1.25 x the cap (1 Gbit/s by default)**, or above 16 x the last estimate (a 4 -> 30 Mbit/s rise is 7.5 x): not read.
* **The filter** (`ap_train_median()`, `ap_train_apply()`): the samples of one large sample's readers are the least over them (one round); the last three rounds are kept. A round's value **below the last estimate is taken at once** (an over-estimate is the dangerous one); **above it, up to 1.5 x the last estimate at once**, and further only as far as the **median of the last three rounds** agrees (two of three). So one outlier high moves the estimate 1.5 x at most and is gone at the next train; one outlier low is taken and undone at the next (median of 30, 4, 30 is 30); a real step up of 7.5 x takes a second train. The estimate becomes BtlBw at 95% of it, the state CRUISE (a train ends the START search), and the **probes wait 12 s** (`train_fresh_until_ns`: the train has just said what they would find).
* **When trains read too high** (a link that releases a burst at its line rate: the tc agent's 4 ms slot): in the sample after one was applied, **two guard trips (queue or drop)** count it bad; two such samples in a row block rises from trains for 16 samples, in which a train is read only to go *below* the estimator's own bandwidth (which it finds without trains) and the estimate it gave is forgotten for that. The guards (queue, drop, stall) run as before and can lower the rate at any time.
* **Fallback**: a reader that answers with 0 (an older build, or no whole train) leaves the estimator as today's; **three trains in a row unanswered with a span stop trains to anyone on the link for 16 samples** (the cost of an older reader is a 30-60 KB burst a sample, three times). Without trains `ap_train_*` never runs. A believed prior (ANCHOR) is only corrected by a train 40% below it.
* **Multi-reader**: each reader's span is a sample of the same round; the link's estimate is the least.
* **Not trained**: a destination that is not UDP (a same-host segment), not RELIABLE, a broadcast destination, a reader the hybrid transport has not yet measured under AUTO fragmentation (`hybrid_peer_unmeasured()`: its first samples go one datagram a call so that its loss is measured, never as a train) - so under AUTO the first samples have none; force MTU fragmentation (`-F mtu`, `net.large_fragmentation = tt_LARGE_FRAGMENTATION_MTU`) for trains from the first sample.
* **Cost**: the reader reads the clock once for each of a large sample's first 40 fragments (not only for an ADAPTIVE writer's: it cannot know) - about 1 us a sample.
* **The bucket's debt** is charged at the pacing rate of the moment: a first train at the 8 Mbit/s start rate charges 30 ms, so the first sample's rest waits that long even if the train then says 200 Mbit/s. Later trains charge at the known rate (2-3 ms).

### 28.3 Tests

`tests/test_train.c` (new; 19 tests, all pass): the estimator on synthetic spans (first sample, an immediate drop, a bounded rise and one the median confirms, the dip's recovery, an outlier high bounded, implausible samples, the least over a round's readers, trains blocked when they read too high and the block's end, a believed prior, no trains = no change); the reader (a complete train's span, none for a missing / reordered / too long / single-datagram / stale one, saturation, a sequence wrap, a new sample's fragment 0 starting over, one slot a writer); the writer on a mock ADAPTIVE link (20 fragments at one send time, the TRAIN HEARTBEAT names the 20th with FINAL clear, the 21st waits for the debt, the reader's train is outstanding with its length and wire size; an answer of 30 Mbit/s sets BtlBw to 28.5 and the bucket; an answer of 0 changes nothing; a stray nonzero `reserved` with no train outstanding is not read; three unanswered trains stop them for 16 samples; none off an ADAPTIVE link, for a short sample, or under AUTO while the reader is unmeasured; K by link speed). `tests/compat_train.sh v2.1.0 a0dd559a WORKTREE`: 32 of 32 (the reader answers a TRAIN HEARTBEAT with the span, with 0 for a short train and for an ordinary HEARTBEAT; builds without the train answer every HEARTBEAT as before; a writer ignores `reserved`). The guards of `tests/test_adaptive_pacing.c` (frozen state, probe storm, replays) and `tests/test_rate_control.c` pass unchanged; the wire identity tests (parent 0d9b337f) pass for `rate_control` OFF.

A loopback run with the real HAL (a writer and a reader context in one process, `tt_SEGMENT_ENABLED=0`, UDP over `lo`, 256 KiB samples, MTU fragmentation) shows the plumbing end to end: 6 of 6 samples delivered, a train per sample (K = 20, then 40 once the estimate is above 50 Mbit/s), every train answered with a span, the writer reading it (the first: 214 Mbit/s; the later ones above the line rate or 16 x the estimate and rejected, as loopback reads). It measures nothing about a link.

### 28.4 What is NOT tested

* **Any Pi, any real link.** All numbers of 27 are a model's; the library has run only on the PC's loopback and the mock HAL.
* **That the send path keeps the train back to back on the wire.** The train goes out through `tt_send_batch_nonblocking()` (`sendmmsg()`, and `UDP_SEGMENT` when the HAL has send offload on); the probe's sender was `sendto()`. `train_probe_rig.sh` with `SEND_HOW=hal` (its default) measures the HAL sender at `TT_UDP_OFFLOAD=0` and 1; **that run is needed before the Pi benchmark means anything.** If the sender coalesces or spaces the head, the train reads the sender's spacing, not the link's.
* **A burst-forming link** (slots, aggregation): the defence (`AP_TRAIN_BAD_*`, the plausibility bounds) is unit-tested on synthetic samples, never on a slotted link.
* **The estimator over a real rate step, a real loss pattern, or several readers on a network** (the least over a round is tested at the estimator level only).
* **A reader's stamp is taken when core handles the fragment**, not when the HAL read it: a reader slower than the link measures its own speed (about 5 us a datagram at the HAL's rate, above 1 Gbit/s).
* **FreeRTOS** builds compile the same code; not run. **Hybrid run mode** with a train head is exercised by no test.

### 28.5 The Pi benchmark (the tc agent)

Same harness as the 27/5G runs (`wtc_run.sh`, branch `ab/wifi-adaptive-s1`): the TickLE nodes of the ADAPTIVE arm built from `ab/algo-impl`; **arms `tickle-ad` (AUTO fragmentation) and `tickle-ad-fmtu` (forced MTU, trains from the first sample)**, against `tickle-ad` built from `448b685f`, `tickle-p95` / the fixed arm, CycloneDDS and Fast DDS; patterns P3-P7, the rate step (`RATE_STEP`), M1 M2 S1 S2, N1-N4 (the chains `wtc_chainVB.sh`, `wtc_chainRS2.sh`, `wtc_chain5g.sh`); 128 KiB and 1 MiB. **First**, the probe with the HAL sender: `SEND_HOW=hal train_probe_rig.sh run` (27.11), at 100 and 200 Mbit/s, `TT_UDP_OFFLOAD` 0 and 1.
