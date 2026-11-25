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
