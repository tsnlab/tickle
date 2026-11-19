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
