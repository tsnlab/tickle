# Absolute constants: which can become relative, which must not

2026-09-26. Prompted by the question "can `tt_RELIABLE_RETRY_MAX` and numbers like it be
expressed relatively, the way RFC 6298 writes `srtt + 4*rttvar`?" This audits every numeric
constant in `include/tickle/config.h` (39 of them) and the six in `rmw_tickle.h`, and sorts
them by *why* each one is a number, because that is what decides whether it can be derived.

A constant is only a generality risk when **the right value depends on the deployment and the
code could have measured it instead**. That is a much smaller set than "all the hard-coded
numbers", and naming the other four categories is what makes this audit complete rather than
selective.

## A. Should become relative (5)

These have a measurable referent already in the process, so the number is a stand-in for
something the code knows.

### A1. `tt_RELIABLE_RETRY_MIN` (250 us) -> `1 * srtt`

The floor exists so the estimate cannot collapse to zero and spray retries. But there is a
*physical* floor, not merely a prudent one: **a retransmission cannot be answered in less than
one round trip**, so any retry scheduled sooner than `srtt` is guaranteed waste - the answer to
the previous one is still in flight. That is true on every link, at every speed, with no
calibration.

The present 250 us came from "86% of healthy rig recoveries completed under 256 us". The rig's
own `srtt` is ~200 us, so the derived floor lands within 20% of the measured one - the formula
reproduces the hand-tuned value on the link it was tuned for, which is the test a derivation
should pass before replacing a measurement.

### A2. `tt_RELIABLE_RETRY_MAX` (10 ms) -> `k * srtt`, k = 64

This is the one I had already flagged as a real generality defect before the question was
asked. The ceiling is not a physical bound, it is "stop believing an absurd estimate". Absolute
10 ms encodes the rig's latency scale:

| link | srtt | ceiling should be | fixed 10 ms says |
|---|---|---|---|
| rig, 100 Mbit/s | 0.2 ms | 12.8 ms | 10 ms - about right, by construction |
| 10BASE-T1S, 10 Mbit/s | ~2 ms | 128 ms | 10 ms - **clamps genuine recoveries** |
| high-latency / satellite | 200 ms | 12.8 s | 10 ms - **catastrophic**, retries 20x per RTT |

On TickLE's own target link the fixed ceiling is already in the wrong place, and it gets worse
monotonically with latency. A ceiling that clamps below the true RTT does not merely mistune -
it *inverts* the estimator, turning a congestion-avoiding backoff into a retry storm exactly
when the link is worst.

`k = 64` is chosen so the ceiling sits above what the estimator can legitimately produce: with
`srtt + 4*rttvar`, rttvar would have to reach 16x srtt to hit it, which is beyond any healthy
link and is the pathology the clamp is for. RFC 6298 makes the same choice with its 60 s
maximum; the difference is that TCP can afford one absolute number because it only ever runs on
links within a few orders of magnitude of each other, and TickLE spans 10 Mbit/s embedded to
gigabit.

Both A1 and A2 are nearly free to implement: `src/tickle.c:3981-3990` already holds
`proxy->recovery_srtt_ns` at the clamp site. The pre-first-sample path (`srtt == 0`) still needs
`tt_RELIABLE_RETRY_INITIAL`, which is category C below - see there.

### A3. `tt_SERVER_CACHE_TIMEOUT` (100 ms) -> measured

Its own comment is already the formula: `(Client server latency) * (CALL_RETRY_COUNT + 1)`. The
derivation was written down and then the arithmetic was done by hand once, on one link, and the
answer pasted in. Everything the formula needs is available at runtime; only the multiplication
is missing.

### A4. `tt_CALL_RETRY_INTERVAL` (5 ms) -> the same estimator as A1/A2

RPC retry has the identical structure to reliable retransmission - "how long before I assume my
message was lost" - and it used to literally share the reliable path's constant. It deserves the
same RFC 6298 treatment rather than a second hand-picked number. Lower value than A1/A2 (the RPC
path is less hot), but it is the same defect and it should not be left as the last hard-coded
retry interval in the file.

### A5. `tt_SOCKET_BUFFER_SIZE` (1 MiB) -> bandwidth-delay product

Weakest of the five, and worth saying so: the comment already argues the number is harmless
because the kernel clamps it down, so the cost of being wrong is one-sided. A BDP derivation
would be more honest but would change almost nothing. Low priority.

## B. Must stay absolute: they size static storage (14)

`tt_MAX_ENDPOINT_COUNT` 256, `tt_ENDPOINT_INDEX_SIZE` 512, `tt_MAX_SCHEDULER_LENGTH` 128,
`tt_MAX_SERVER_CACHE_COUNT` 64, `tt_MAX_PEER_COUNT` 8, `tt_MAX_ACK_ENTRIES` 16,
`tt_MAX_DISCOVERED_ENTITIES` 16, `tt_MAX_LINK_COUNT` 4, `tt_SCHED_INBOX_LENGTH` 32,
`tt_RELIABLE_BITMAP_BITS` 256, `tt_RELIABLE_BITMAP_MAX_BITS` 4096, `tt_MAX_RELIABLE_HISTORY` 64,
`tt_MAX_NAME_LENGTH` 255, `tt_MAX_STRING_LENGTH` 65535.

These are array dimensions. Deriving them at runtime means allocating at runtime, and core's
central invariant is zero `malloc`/`free` - the property that makes it usable on an MCU and that
produced the 69.5% RSS advantage. **Making these relative would trade the project's main
structural win for a cosmetic one.** They are already `#ifndef`-guarded, which is the correct
generality mechanism for a compile-time size: the integrator picks them for their target.

`tt_ETHERNET_UDP_PAYLOAD` (1472) belongs here despite looking like a link property, because it
feeds `tt_MAX_BUFFER_LENGTH` and therefore sizes buffers. It should stay a compile-time size -
but the *assumption* it encodes (MTU == 1500) is checkable at runtime and currently is not. The
worthwhile change is not to derive it, it is to **read the interface MTU at node creation and
fail loudly if it is smaller than assumed**, instead of silently emitting frames that will be
fragmented or dropped. Same class of fix as the identity fields on the harness: not a new value,
an assertion that the assumed one holds.

## C. Policy choices, not measurements (6)

`tt_RELIABLE_RETRY` 3, `tt_CALL_RETRY_COUNT` 3, `tt_LIVELINESS_MISS_THRESHOLD` 3,
`tt_UNICAST_PEER_THRESHOLD` 2, `tt_RELIABLE_RETRY_INITIAL` 1 ms, `tt_RELIABLE_STUCK_WARN_INTERVAL` 5 s.

"How many attempts before giving up" and "how many misses before declaring a peer dead" are
statements about how much loss the application wants tolerated. There is no measurement that
yields them - a faster link does not imply you should try more or fewer times. Deriving these
would be *inventing* a dependency, not discovering one.

`tt_RELIABLE_RETRY_INITIAL` is the subtle one and worth stating explicitly: it is the value used
*before the first RTT sample exists*, so by definition there is nothing to derive it from. It is
the seed, not an estimate, and it must remain absolute for the same reason RFC 6298 specifies an
absolute 1 s initial RTO. Notably, RFC 6298's own seed is 1 s and TickLE's is 1 ms - a gap worth
a second look on slow links, but a gap between two chosen numbers, not a missing derivation.

`tt_RELIABLE_STUCK_WARN_INTERVAL` is a human-readability cadence for a log line. Its comment
already says it is rate-limited by time rather than retry count precisely so it stays readable
whatever the retry interval does. Correctly absolute.

## D. Must be absolute because both peers must agree (2)

`tt_NODE_UPDATE_INTERVAL` 1 s, `tt_NODE_TX_INTERVAL` 1 ms.

These set cadences that a *remote* node interprets - liveliness in particular, where the
receiver's miss threshold is counted against the sender's update period. If each node derived
its own value from its own local measurements, two nodes on the same link would disagree about
whether a peer is alive. Protocol parameters are shared constants or they are negotiated; they
are never independently derived. Making these "more general" is the change that would actually
break interoperability.

## E. Unit definitions and sentinels (12)

`tt_SECOND`/`tt_MILLISECOND`/`tt_MICROSECOND`, the bitmap word arithmetic,
`tt_NODE_ID_INVALID`/`BROADCAST`, `tt_LIMITED_BROADCAST`, `tt_SERVER_CACHE_ENTRY_LENGTH` and
`tt_CLIENT_CACHE_LENGTH` (both already expressed relative to `tt_MAX_BUFFER_LENGTH`),
`tt_RELIABLE_RETRY_INTERVAL` 0 and `tt_RELIABLE_DEADLINE` 0 (sentinels meaning "auto"),
`tt_THREAD_SAFE` 1 (a boolean). Not tunables.

One genuine loose end, flagged rather than proposed: `tt_SCHEDULER_IO_INTERLEAVE` 8 is described
in its own comment as "a first guess, not yet tuned", and that is still true. It is not a
generality problem - it is an untuned number, which is a different and smaller debt. It belongs
on the measurement list, not this one.

## rmw_tickle

`RMW_TICKLE_SUBSCRIPTION_QUEUE_DEFAULT_DEPTH` 10 is the ROS 2 default QoS depth - externally
anchored to the spec, which is the strongest possible justification for an absolute value.
`RMW_TICKLE_KEEP_ALL_BYTES_DEFAULT` 512 KiB is category A-adjacent: anchored to CycloneDDS's
WhcHigh watermark, and its comment already carries the link-rate re-derivation (~42 ms at
100 Mbit/s, ~420 ms at 10 Mbit/s). That re-derivation could be made executable rather than
documentary, but the anchor is defensible as it stands. `RMW_TICKLE_CLIENT_RETRY_INTERVAL_NS`
100 ms is the same shape as A4 and should follow whatever A4 does.
`RMW_TICKLE_MAX_BLOCKING_MS_DEFAULT`/`_LIMIT` and `RMW_TICKLE_TRACKING_WORDS` are category C and
B respectively.

## Priority

A2 first and alone if only one thing is done: it is the only entry here that is already wrong on
TickLE's own 10BASE-T1S target rather than merely inelegant, and it fails in the worst direction
(more retries when the link is already struggling). A1 rides along in the same three lines. A3
and A4 next. A5 is cosmetic.

The measurable claim for A1+A2, to be pre-registered before the change: on an unshaped rig the
derived bounds must reproduce present behaviour within noise (srtt ~200 us puts the floor at
200 us vs 250 us, the ceiling at 12.8 ms vs 10 ms - neither binds in healthy operation, so the
A/B should show *no difference*, and a difference would mean the clamps were binding when we
believed they were not). The generality gain shows up only under injected delay, so the arm that
matters is `netem delay 20ms`, where the fixed ceiling clamps and the derived one does not.
