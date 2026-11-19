# Adaptive send pacing - design and acceptance criteria (draft, 2026-11-19)

Status: DESIGN ONLY. Nothing here is implemented, nothing is measured. Branch `ab/adaptive-pacing`, off
`ab/release-2.2.0`. The option is opt-in and **off by default everywhere**; making it the WIFI profile's default is a
separate decision for the user, taken after the validation below.

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
3. **Delivery-rate sample:** at each ACKNACK from a reader, `delivered = watermark advance x unit payload bytes`; the
   sample is `delivered / interval`, interval at least one RTprop (BBR's rule against ACK compression). A sample taken
   while the pacer had nothing to send (application-limited) only counts if it raises BtlBw.
4. **RTT sample:** a solicited HEARTBEAT's send time against the next ACKNACK from that reader. It can only be too
   long (the reader's poll delay, an unrelated gap ACKNACK arriving first), never too short, which the windowed
   *minimum* tolerates; the max filter tolerates the matching under-estimate of the delivery rate. Both errors are in
   the safe direction (rate a little low, BDP a little high); the probe recovers the former.
5. **Multi-reader:** the bucket is per link, feedback is per reader. v1 is specified and validated for **one reader
   per link**; with several, BtlBw is the sum of each reader's own windowed max (stated here, validated later).

### 4.3 State machine (per link)
- **START:** rate `min_rate`-seeded `(initial)`; RTprop seeded from `announce_rtt_ns` of the peer. Each RTprop the
  rate doubles (gain 2/ln2 as in BBR Startup, `(initial)`) while delivery-rate samples still grow by 25 % per round.
- **PLATEAU:** three rounds without 25 % growth: BtlBw is found; go to CRUISE. Repairs seen in START (lost units
  during doubling) are expected and count as budget, not as a signal.
- **CRUISE:** rate = BtlBw. Every 8 RTprop probe at 1.25 x BtlBw for one RTprop; if delivery rate rises, BtlBw follows
  (the max filter), else return. Window of the max filter `(initial)` 10 RTprop, floor 500 ms; of the min filter
  `(initial)` 10 s (RTprop is a property of the path, not of the load).
- **Queue guard:** if `RTT - RTprop > queue_target` on two consecutive samples, stop probing and hold at the last
  delivery rate; queue_target = max(2 ms, RTprop / 4) `(initial)`. This is the real-time term: bound the queueing
  delay, not just the loss.
- **Rate-drop guard (fast down):** repairs / sent units > 50 % over one RTprop, or delivery rate below half of BtlBw
  for two rounds: set BtlBw to the latest sample at once instead of waiting for the max filter to age out.
- **Floor and cap:** `pacing_min_rate` (default `tt_LINK_RATE_MIN`) and the cap `pacing_rate` (default `link_rate`),
  so an adaptive link can still be told "never above this".

### 4.4 Repairs and completion
- Repairs are charged to the same bucket and go **ahead of new units** within it (today's order); the deferral
  threshold scales with the adaptive burst. A sample's completion time is bounded below by `size / BtlBw`; sending
  faster only converts the excess into loss, so the target is that bound plus a few RTprop.

## 5. API (opt-in, zero = today)

```c
enum { tt_PACING_FIXED = 0, tt_PACING_ADAPTIVE = 1 };
struct tt_LinkOptions {
    ...                        // existing fields unchanged, in place
    uint8_t  pacing_mode;      // 0 = fixed (pacing_rate as today); tt_PACING_ADAPTIVE = find the rate
    uint64_t pacing_min_rate;  // adaptive only: the floor, bits/s; 0 = tt_LINK_RATE_MIN
    uint32_t pacing_queue_ns;  // adaptive only: queue-delay target; 0 = max(2 ms, RTprop / 4)
};
```
- `pacing_rate` under ADAPTIVE is the **cap**; 0 = `link_rate`. `tt_PACING_OFF` with ADAPTIVE is rejected.
- `validate_link_pacing()`: `pacing_mode` in {0, 1}; adaptive fields only with `pacing_mode == ADAPTIVE`
  (like `pacing_burst` on an unpaced link today); floor >= `tt_LINK_RATE_MIN`, floor <= cap; `pacing_queue_ns` at least
  `(initial)` 100 us; ADAPTIVE only in a build with `tt_LARGE_SAMPLES`, else `tt_RET_INVALID_ARGUMENT`.
- The WIFI profile's default stays `tt_PACING_FIXED`/off. The documentation says "on Wi-Fi turn adaptive pacing on"
  only if the acceptance results below support it.
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
| A1 | `pacing_mode` off (every profile by default): wire identical, bytes and times, to the parent; ETHERNET/T1S/TSN/WIFI unchanged | `tests/wire_identity_adaptive_pacing.sh`, the pattern of `wire_identity_send_pacing.sh` (control, identity, mutant) |
| A2 | Core gate v2 (158 deciding comparisons): 0 WORSE, adaptive off | rig core gate |
| A3 | Wired DDS comparison: no LOSE that the parent did not have | the 136/14/0 campaign, adaptive off (unchanged path) |
| A4 | Rate-step 30 -> 4 -> 30 Mbit/s (tc, 1 reader): BtlBw within 25 % of the carried rate within `[N]` s after each step; queue delay bounded; no run lost | tc time-varying schedule, Pis |
| A5 | Within `[X]` % of the Stage C **oracle** (best fixed pacing_rate per profile class) on 1 MiB / 128 KiB p50 and p99, same cells | tc cells, >= 10 reps each, A/A noise rule |
| A6 | No LOSE against Fast DDS or CycloneDDS in any tc cell (M1, M2, S1, S2; be_small, rel_small, rel_large 128 KiB, 1 MiB) | the Stage A/B/C scoring |
| A7 | Small samples and be_small: no regression (they bypass the pacer) | same cells, adaptive on vs off |
| A8 | Datagram count at 1 MiB / S1 within `[X]` x the oracle's (no retransmission storm) | counters |
| A9 | Unit tests: estimator converges on synthetic rate steps, queue bounded, floor/cap honoured; every `validate_link_pacing()` bound refused and accepted | `tests/test_adaptive_pacing.c`, mutants as for `test_send_pacing.c` |
| A10 | Object-size delta reported for the three builds; objects byte-identical with the code compiled out | `tt_PACING_ADAPTIVE_COMPILED` switch if the user wants byte-identical objects (decision) |

## 8. Validation the Pis need once Stage C is done

1. **Rate-step test** (A4): a tc schedule 30 -> 4 -> 30 Mbit/s inside the netns pair, 1 MiB at 0.25 Hz, 60 s, adaptive
   vs oracle vs default, >= 10 runs; the existing wifitc harness plus a netem-schedule driver.
2. **Cell sweep** (A5-A8): M1, M2, S1, S2 x {be_small, rel_small, rel_large 128 KiB, 1 MiB}, adaptive on, against the
   Stage A/B/C data (same link, same DDS arms), 10 pooled runs per deciding cell.
3. Core gate v2 and the wired DDS comparison with adaptive off (A2, A3) - the ordinary release checks.
4. Estimator-trace run: one 1 MiB per profile with the counters dumped, to compare the estimator's BtlBw/RTprop with
   the calibrated carried rate and ping RTT.

## 9. Open questions for the user
- Compile switch (A10): code compiled out by default for byte-identical objects, or always compiled in and
  identical in behaviour only?
- Multi-reader semantics (4.2 item 5): accept "one reader validated" for 2.2.0?
- Whether adaptive pacing becomes the WIFI default (only after A1-A8).
