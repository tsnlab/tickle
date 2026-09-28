# Optional modules: where they plug in, and shared memory as the first one

The user asked on 2026-09-28 for two frameworks kept out of core and pluggable by a user who wants them: (1) same-host
communication over shared memory, (2) a security module that satisfies SROS2. This file is Plan's answer to *where*
such a module attaches and *what would have to be true* for the first one to be worth having. The security design stays
parked in `SECURITY_PLAN.md` until the user starts it; only the seam it would use is described here, because a seam
designed for one consumer and retrofitted for the other is how the same mistake gets made twice.

## 0. The user's decisions (2026-09-28)

1. **The shared-memory module and the security module are separate.** Not one "optional modules" layer serving both.
2. **The security module splits across the ROS line:** a TickLE Core half an embedded system can use, and an rmw half
   that meets SROS2's requirements.
3. **The core half is called TickLE Security.**
4. **Decided 2026-09-28, after Plan's recommendation in section 3a: the shared-memory module does not split - it is a
   core-only module.** The user's words: a module used only by core. There is no shared-memory half in rmw at all;
   what rmw contributes is what it already does for other transports - read an environment variable, and report which
   transport a match used - which is a few lines in existing code rather than a part of the module.
5. **The detailed module design plan is written once 4 is decided.** Written as `SHM_PLAN.md`; this file stays the
   seam-and-criteria document that both modules answer to.
6. **Using a module must cost no performance.** Read as: a build with the module present but not in use is
   indistinguishable from a build without it, which is criterion 3 below, and the module's own path is measured
   separately.
7. **The shared-memory module must be faster than the competing products under the same conditions.** What "the same
   conditions" means needs saying, because both vendors have a shared-memory transport of their own and their defaults
   differ - FastDDS ships its SHM transport on, CycloneDDS's shared-memory path is off unless configured. Plan's
   reading, unless the user says otherwise: score **both** comparisons and report both - each framework's shared-memory
   path against ours (the engineering comparison), and each framework's default configuration against ours (what a user
   actually meets). A win claimed on only one of those has to say which.
8. **TickLE Security must provide security equivalent to DDS Security.** Equivalence is measured against DDS
   Security's own five plugin functions and its threat model, which includes a malicious participant inside the domain,
   not only a wiretap on the link - see SECURITY_PLAN.md. Two things follow: the equivalence claim needs external
   review rather than our own reading, and the places we deliberately differ (no wire interoperability with a DDS
   vendor; our own handshake rather than DDS-Security's) are documented as differences, not as gaps.
9. **Shared memory first, security second** - which is also what section 3 recommends, for the reason given there.

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

- **core side - TickLE Security** (the user's name for it, decision 3): the handshake, the per-datagram transform, and
  an *opaque* identity and permission it is handed. No certificate parsing, no XML, no OpenSSL in core.
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

### 3a. Question 4, answered: the shared-memory module should not split - and the user decided so

**No core/rmw split. The whole transport belongs to the core-side module; the rmw layer only selects it and reports
it.** Four reasons, and one dependency that matters more than any of them.

1. **There is nothing for an rmw half to do.** "How do these bytes reach that peer" is the same question for a native
   TickLE application and for a ROS 2 node: no typesupport, no QoS translation, no keystore, no XML. The security
   module splits because SROS2's artefacts - X.509, S/MIME, the keystore layout - are ROS-side by nature and need
   libraries that must not enter core. Shared memory has no equivalent; it needs POSIX `shm_open` on Linux and a
   HAL-provided region on a target, both of which are where core already lives.
2. **Core already knows what "same host" means.** g8 built the host registry in /dev/shm that lets a process claim a
   context id and tells our processes on one host apart from each other. The prerequisite for choosing this transport
   is therefore already in core, and putting the choice above core would mean asking the question twice.
3. **Two places deciding which transport carries a sample is how criterion 5 gets violated.** If rmw could route a
   sample to shared memory, then every rule core enforces on the datagram path would have to be re-enforced above it,
   which is precisely the bypass that criterion exists to prevent.
4. **An embedded system benefits too.** Two TickLE tasks in separate protection domains on one FreeRTOS target are the
   same case as two processes on one Linux host. Logic placed in rmw would be unavailable to them, against the reason
   decision 2 splits the security module in the first place.

**What the rmw layer does get: selection and reporting, no policy.** An environment variable to force or disable the
transport, as the vendors offer, and the transport a match actually uses made visible in introspection - so a user can
tell why a same-host row is faster instead of guessing.

**The dependency: shared memory is worth much more with receive-buffer lending than without it.** Its real prize is not
a cheaper datagram, it is handing the reader a pointer into the segment. Copying out of the segment on arrival buys
loopback UDP minus a syscall; lending approaches g9's in-process cost, which is the tier above. The user already
approved lending on 2026-09-28 (`tt_Sample_retain`/`release`, LARGE_MESSAGE_PLAN's stage 2 neighbour). So the honest
recommendation is that lending lands first or alongside, and that the pre-registration below states which of the two
the measured win is attributed to - otherwise a good number cannot be told apart from a good number for another reason.

**Order: shared memory first, security second.** Shared memory has a measurable win criterion and no open user
decisions, while the security design has five open questions and is parked. And doing the transport first means the
security module's per-datagram seam is designed against two transports from the start rather than retrofitted onto a
second one later - which is the sequence that produces the bypass in criterion 5.

## 4. What this file does not decide

The packaging above is a recommendation and the user has not chosen it. Nothing here is implemented, and the security
half is not to be designed further until the user starts `SECURITY_PLAN.md`. The open cost question is honest: the
per-datagram seam is on the hot path, so it must be compile-time, and even then the claim "nothing when off" is a
measurement, not an argument - criterion 3 is how it gets made.
