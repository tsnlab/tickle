# Optional modules: where they plug in, and shared memory as the first one

**Status: a proposal, not agreed work.** The user asked on 2026-09-28 for two frameworks kept out of core and
pluggable by a user who wants them: (1) same-host communication over shared memory, (2) a TickLE Security that
satisfies SROS2. This file is Plan's answer to *where* such a module attaches and *what would have to be true* for the
first one to be worth having. The second one's design stays parked in `SECURITY_PLAN.md` until the user starts it; only
the seam it would use is described here, because a seam designed for one consumer and retrofitted for the other is how
the same mistake gets made twice.

## 1. The two are not the same kind of module

Calling both "a pluggable module" hides that they attach in different places, and an API built for one of them fits the
other badly.

**Shared memory is a transport.** It answers "how do these bytes reach that peer", which is the question the HAL and
the platform layer already answer for UDP. One seam: a transport a peer is reached through, chosen per peer.

**Security is not a transport.** It needs three separate attachments:

| What | Where it attaches | Why it cannot be the transport seam |
|---|---|---|
| Authentication | peer association, once per peer | It must complete *before* the first data datagram is accepted, and its result (a session key, a subject name) is per peer, not per datagram |
| Authorization | endpoint creation, and again on match | A publisher that may not publish a topic must fail at `create`, on the local process's own policy, with no peer involved |
| Protection | each datagram, both directions | The hot path. Whatever this costs is paid per sample, so it is the one that must disappear when the module is out |

So: one seam for a transport, three for security. A module system that offers only "wrap the datagram" would push
authentication into the data path, which is where protocols get broken.

## 2. What the plug should be, and what it should not

**Compile-time seams, runtime selection.** Core builds for FreeRTOS on RISC-V with static allocation, so `dlopen` is
not available there, and two different plug mechanisms - dynamic on Linux, static on the target - is worse than one.
The recommendation is the pattern core already uses for `tt_LOCAL_DELIVERY` and `tt_CONTEXT_ID_CLAIM`: the hook points
live in core, the implementations live in their own directories, and a build without a module contains none of its
code and none of its branches. Which of the modules *present in a build* is used can then be chosen at startup - an
env var under rmw, as a ROS 2 user expects - because that choice happens once, not per sample.

**Not DDS Security's own plugin model.** Its five dlopen'd plugins are a large part of why it is hard to audit, and we
would gain nothing from copying the shape of an interface we are not wire-compatible with anyway.

**The security module splits across the ROS line, and that split is the point.** Reading an SROS2 keystore needs X.509,
S/MIME and an XML parser; none of that belongs in core, and the user's standing principle is that core does not depend
on ROS 2. So:

- **core side:** the handshake, the per-datagram transform, and an *opaque* identity and permission it is handed. No
  certificate parsing, no XML, no OpenSSL in core.
- **Linux/rmw side:** read the keystore, verify the CA signatures, translate governance and permissions into that
  opaque form.

This is what lets an embedded target use TickLE Security with keys provisioned at build time and no parser at all, and
it keeps the SROS2-shaped work on the side of the line where SROS2 exists.

## 3. Shared memory must win, not merely work

Three tiers already exist, and shared memory has to sit between two of them or it has no reason to be written:

| Tier | Path | Measured cost |
|---|---|---|
| Nodes in one process | g9's local delivery, no datagram | +19 ns a 64 B sample, +1.6 us a 60 KB one |
| Processes on one host | today: a UDP datagram over loopback | the tier this module would replace |
| Hosts | UDP over the link | unchanged |

**Pass, pre-registered.** A shared-memory transport is worth keeping only if, for two processes on one host:

1. **It beats loopback UDP** at p1 through p4 on latency and on CPU per sample, by more than 2xSE, with no metric
   worse by the same rule (WIRE_PLAN 8.3's reading rule applies: a single WORSE cell is a candidate, not a finding).
2. **It does not beat g9**, which is the lower bound - a result faster than in-process delivery would mean the
   measurement is wrong, not that the module is fast.
3. **A build without it is unchanged**, shown the way the wire-identity checks are: the same bytes on the wire, and
   CPU and binary size indistinguishable from the parent build against a placement control (8.3's amendment).
4. **A reader that dies does not stall a writer**, and a segment left by a killed process is reclaimed - checked by
   killing the reader mid-run with the writer's own progress as the evidence, not by reading the code.
5. **Every rule the wire enforces is enforced here too.** Same-host delivery must not become a way past what the
   network path checks - QoS compatibility, the context-id rules g8 introduced, and, if the security module is ever
   built, its authorization and protection. A vendor's shared-memory path bypassing its own security checks is a real
   class of defect; the way not to have it is for this criterion to exist before the code does.
6. **The capacity is bounded and stated,** as every other core structure is, and exhaustion is counted and warned
   about rather than silent.

**Order: shared memory first, security second.** Shared memory has a measurable win criterion and no open user
decisions, while the security design has five open questions and is parked. And doing the transport first means the
security module's per-datagram seam is designed against two transports from the start rather than retrofitted onto a
second one later - which is the sequence that produces the bypass in criterion 5.

## 4. What this file does not decide

The packaging above is a recommendation and the user has not chosen it. Nothing here is implemented, and the security
half is not to be designed further until the user starts `SECURITY_PLAN.md`. The open cost question is honest: the
per-datagram seam is on the hot path, so it must be compile-time, and even then the claim "nothing when off" is a
measurement, not an argument - criterion 3 is how it gets made.
