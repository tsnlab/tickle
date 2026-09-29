# Wireless and remote paths: what we would have to change, and what to measure first

**Status: PARKED.** The user asked on 2026-09-29 for this to be planned and **not executed until they say so**, and to
study an existing WAN-oriented system while planning it - referred to throughout as **Case A** rather than by name
(the user's decision, 2026-09-29): an open-source pub/sub middleware built for wide-area paths, whose published design we
read and whose behaviour we would verify by running it. Nothing here is started. Read it beside `MODULE_PLAN.md` (the seam work already under way,
which turns out to be the attachment point for most of this) and `SECURITY_PLAN.md` (also parked, and a prerequisite for
any public path).

## 1. The honest starting point: we have no wireless measurements at all

| measured | not measured |
|---|---|
| a direct wired link between two Pi 5s, base RTT ~0.24 ms | Wi-Fi or cellular, **nothing** |
| `tc netem` **uniform random** loss at 5% (cell c6), delay 10 ms with 2 ms jitter (c11, c12) | **correlated, bursty** loss; fades and outages of hundreds of ms to seconds |
| fixed, symmetric bandwidth | variable and asymmetric bandwidth, cellular bufferbloat |
| one L2 segment, broadcast discovery working | NAT, firewalls, an internet path |

**The loss we inject has the wrong shape.** Uniform 5% at a 0.24 ms base RTT is not 5% in bursts at a 30 ms base RTT, and
no amount of our current data extrapolates to the second case. Every claim below about wireless behaviour is therefore a
design argument, not a result, and is marked as such.

## 2. What changes when the link becomes wireless

| property of the link | helps TickLE | hurts TickLE |
|---|---|---|
| loss is common and bursty | per-datagram `seq_no` with selective repair; no head-of-line stall across topics; DATA_FRAG repairs a fragment rather than a message | — |
| path MTU shrinks (tunnels, VPN) | our own fragmentation exists | the **OS IP fragmentation** path measured **97.4% reassembly failure at 5% loss** - unusable here, and already a deferred item |
| RTT 30-100 ms | — | the 100 us poll cadence and the TSN-shaped scheduling stop being the point. **The value proposition shifts from determinism to efficiency** |
| shared medium, bufferbloat | — | **no congestion control of any kind.** Confirmed by reading core: no pacing, no cwnd, no rate limit. What exists is flow control toward a reader's ack window (`max_blocking_time`), which is a different thing |
| NAT and firewalls | `ROS_STATIC_PEERS` gives unicast peers (g6) | no NAT traversal: no STUN, TURN or ICE. Both ends must already be reachable |
| a public path | — | **TickLE Security is not implemented.** Today the wire is plaintext |
| bandwidth is metered and variable | **138.6 B per sample against CycloneDDS's 180 and FastDDS's 286** (measured 2026-09-29). This advantage **grows** when airtime is the scarce resource | — |
| the endpoint is small | 140 KiB of text, 207 KiB of bss, 1.9 MB RSS (measured) | — |

## 3. The three gaps that decide it, and what Case A does about each

Everything below about Case A is **our reading of its published design and has to be verified against its documentation
and behaviour before anything is built on it.** We do not copy its code - the same rule the user set for CycloneDDS and
FastDDS applies here - Case A is permissively licensed and that changes nothing; we study the behaviour and write our own.

### 3.1 Reaching the far end (NAT, firewalls)

- **What Case A appears to do:** router, peer and client modes. A client dials **out** to a router, and a router in the
  cloud relays between clients that could never reach each other directly. **That solves NAT with a relay rather than
  with hole-punching** - no STUN, no TURN, no ICE state machine.
- **What that suggests for us:** a relay is far smaller than ICE and fits what we already have. Our unicast peer
  mechanism (`ROS_STATIC_PEERS`, g6) is the dialling half; what is missing is something at the other end that forwards.
  The honest cost is that a relay is a new component with its own deployment, and it adds a hop to every sample.
- **What we would decide first:** whether a relay forwards *datagrams* (transparent, keeps our wire) or *samples*
  (smarter, can filter by topic, but becomes a second implementation of our matching rules). The first is much smaller
  and is the one to cost out first.

### 3.2 Not destroying the link (congestion)

- **What Case A appears to do:** a per-message congestion-control policy - block or drop - rather than a TCP-style
  window. The application says what matters; the middleware does not invent a rate.
- **What that suggests for us:** our RELIABLE/BEST_EFFORT split plus `max_blocking_time` is already close to that shape,
  so the policy layer may need little. **What we genuinely lack is pacing against the network**, which is what turns a
  burst into bufferbloat on a shared radio.
- **And a second thing worth more than pacing on a radio:** Case A batches small messages into one network frame. Over
  wireless, per-frame airtime and per-frame loss dominate per-byte cost, so **batching beats byte-efficiency** - and we
  already have the mechanism, `tt_send_batch`, with four call sites. This is the cheapest wireless win available and it
  is measurable on the wired rig first.

### 3.3 Being safe to put on a public path (encryption)

- Our answer is `SECURITY_PLAN.md`, which is parked. Nothing about wireless changes its design; it changes its
  **priority**, because a public path without it is not deployable at all.
- Case A's answer is link-level TLS/QUIC, which is a different choice from ours (end-to-end per-topic protection). Theirs
  is simpler and protects the link; ours protects against other participants too. Neither is wrong; they answer different
  threat models, and ours is the one DDS Security also answers.

## 3.4 A relay of our own: a separate process, or a module inside core?

Asked by the user on 2026-09-29, prompted by Case A having a router as a separate thing. **Plan's answer: a separate
process, built on core, and not a module merged into core.** Four reasons, and the fourth is the one that settles it.

**1. The capacity models conflict.** Every table in core is fixed at compile time, which is a decision made for the
embedded target (`tt_MAX_PEER_COUNT` 8, `tt_MAX_DISCOVERED_ENTITIES` 16 by default). A relay's client count is a
deployment variable. Putting the relay in core leaves two options: a large static table in every embedded image, or
dynamic allocation inside core. The second breaks "the library never allocates", which is not a preference here but the
thing the whole buffer-ownership design rests on.

**2. The acceptance rule is the opposite one.** g8's rule is that a packet is ours only when it arrives on our own
socket. A relay must accept packets **addressed to someone else** and forward them. Behind a flag inside core, that
becomes a switch that inverts a security-relevant acceptance rule in every build - the same class of hazard `SHM_PLAN.md`
criterion 5 exists to prevent for the same-host path.

**3. There is no application.** Everything in core serves an application's endpoints. A relay's publishers and
subscriptions are bookkeeping. Its lifecycle is a service's - restart, monitoring, deployment, possibly TLS termination -
which is not what a library does.

**4. The client side is already there, so there is no large piece to merge.** For an endpoint to use a relay it needs to
unicast to a configured address, and that is exactly what `ROS_STATIC_PEERS` already does (g6). **The endpoint does not
need to know that the peer it is configured with happens to forward.** So the core-side change is small and the work is
all in the separate process - which removes the premise of the question rather than answering it.

**The shape that follows:**

| where | what |
|---|---|
| core | the wire codec, discovery parsing, later TickLE Security's handshake, **plus a "forward" primitive**: explicitly opt-in, off by default, exposed as an API rather than as a behaviour |
| a separate binary | the relay: that API plus its own dynamic client table |
| an endpoint | almost unchanged - a static peer that happens to be a relay |

Case A is, as we read it, the same codebase in a different mode rather than a second protocol, which makes "module or
separate process" a false choice: the answer is both, a thin opt-in capability in core and a separate process as the
deployable thing. **That reading is unverified and is on section 5's list.**

### The hard part, which is discovery and has no answer yet

Our discovery is broadcast-based. Through a relay:

- if the relay **forwards announces verbatim**, endpoints match each other directly and **our matching rules stay
  untouched** - but the addresses in those announces are ones the far endpoint cannot reach;
- so either the relay **rewrites addresses** (it must then understand our wire, and becomes a participant rather than a
  pipe), or an endpoint **learns "reply to this peer via the relay"** (a per-peer return path in core, which is a small
  concept but a new one).

The second looks smaller and closer to our structure, but that is a **design judgement and is not asserted**: it is the
first thing to settle if this is ever started, and it is where a wrong choice costs the most.

### One thing this argues for that was not visible when we chose it

A relay sees everything that passes through it. **With end-to-end protection it cannot read the traffic**, so it can sit
outside the trust boundary. Our choice of end-to-end per-topic protection over link-level encryption - made for the DDS
Security threat model, where a malicious participant inside the domain is in scope - turns out to be what makes a relay
deployable at all. Case A's link-level TLS/QUIC answers a different threat model and would require trusting the router.

**And the costs, stated rather than implied:** a relay is a single point of failure, a bandwidth funnel, and one extra hop.
On a wide-area path the WAN RTT usually dominates that hop, but "usually" is a measurement nobody here has made.

## 4. The measurement path, and it comes before any of section 3

| stage | what it is | what it would settle | cost |
|---|---|---|---|
| **W1** | the existing campaign with **correlated** loss (`tc netem loss 5% 25%`) and delay 30 ms +/- 10 ms, still on the wired rig | how selective repair behaves against DDS when loss arrives in bursts rather than uniformly - **directly comparable to the cells we already have**, because only the netem parameters change | ~40 min, harness change plus a run |
| **W2** | the same cells over the Pis' **real Wi-Fi** | what a real medium does that netem does not model: contention, link-layer retransmission, rate adaptation | needs an AP and a quiet channel; a day |
| **W3** | one end on a different network, across the internet | whether static peers are enough, what the real MTU is, and how badly the absence of congestion control shows | needs a reachable endpoint; also the first place the missing encryption is a blocker rather than a gap |

**Pre-registered readings for W1, written before it can run:**

- TickLE's delivered fraction and its tail latency against both vendors, per cell, **with the reading rule from
  WIRE_PLAN 8.3** - and with the repetition count set by **the noisiest arm**, which under bursty loss will probably be a
  vendor rather than us (that is how the rosbag2 stall arm went: the control's spread was 33 against our 2).
- **A result that says "TickLE wins" and is inside the floors is not a result.** Below about 1% on throughput and 10 KB
  on RSS, two builds cannot be separated at all; against a *different implementation* the floors do not apply, but the
  2xSE rule and the confirmation rule do.
- **A cell where we lose is the point of the exercise**, not a failure of it: selective repair should win under bursty
  loss, and if it does not, the reason is more valuable than the win would have been.
- Any row whose RESULT line does not show the injected netem parameters is VOID, the same rule the campaign already
  applies to QoS.

## 5. What to verify about Case A before relying on any of section 3

Each of these is currently our reading and would be checked by running Case A rather than by reading about it - the same
standard we hold ourselves to:

1. that a router really does relay between two clients neither of which can reach the other, and what it costs in added
   latency;
2. whether its congestion-control policy is per-message or per-priority, and what it does when the link stalls;
3. how much its batching actually saves on a real radio, since that is the number that would justify doing the same;
4. what its discovery does on a network where multicast is filtered, which is most enterprise Wi-Fi.

## 6. Why this is parked rather than started

Our measured advantages (CPU, memory, bandwidth per sample, wired latency) survive or grow over wireless, but **none of
them is what blocks a wireless remote deployment.** The three gaps in section 3 are, and two of the three are large. So
the order is: measure W1 (cheap, comparable, and it tells us whether our repair strategy is actually the advantage we
think it is over a bursty link), then decide whether the relay is worth building, and keep the security work on its own
track since it gates any public path regardless.

Until then, the honest statement about TickLE's place is: **a controlled link** - TSN or 10Base-T1S wired, or a single
trusted L2 segment - is where the measured advantages are real today, and an internet-crossing remote deployment is
better served by WebRTC or by a Case A-shaped system at this moment.
