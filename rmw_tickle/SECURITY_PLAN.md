# SECURITY_PLAN: SROS2-compatible security on TickLE, with MACsec for the link

**Status: PARKED.** On 2026-09-27 the user decided to put this plan on the roadmap, and to execute none of it until
they say so ("이건 내가 실행 하자고 할 때까지 실행하지 말자"). Nothing below is pre-registered, designed in detail or
coded. Only g7 (below) goes ahead now.

## What exists today (2026-09-27)

- `rmw_tickle` reads neither `rmw_init_options_t.security_options` nor the enclave's files (COMPARISON.md 2.7a).
- TickLE core has no authentication, key exchange or message encryption.
- **g7 (in progress, not parked):** refuse to start when `ROS_SECURITY_ENFORCEMENT=Enforce` is requested, and log once
  that security is not applied when security is enabled but permissive. A user who demands security must never get
  an unsecured process without a word.

## The idea: split SROS2's three parts between the link and TickLE

SROS2 (OMG DDS-Security, driven by `ros2 security` keystores and enclaves) protects three things:
- **authentication:** each process (enclave) proves an X.509 identity;
- **access control:** signed governance and permissions say who may publish or subscribe to what;
- **cryptography:** AES-GCM encryption or signing per topic, keys agreed during authentication.

MACsec (IEEE 802.1AE, keys by MKA / 802.1X) encrypts and authenticates each Ethernet link, port to port, and admits
devices to the network. It fits a TSN network. On its own it does **not** cover:
- per-process identity (every process on an admitted host passes);
- per-topic access control;
- end-to-end protection (each switch decrypts and re-encrypts);
- traffic inside one host (loopback);
- routed paths (MACsec is layer 2 only).

The hybrid:
- **MACsec** carries confidentiality and integrity on every link.
- **TickLE** reads the same SROS2 keystore and enclave files and adds what MACsec lacks:
  - a per-process authentication handshake on the enclave certificate, against the identity CA;
  - access control from the signed permissions, checked when an endpoint is created and when a remote endpoint is
    matched;
  - optionally, signing or encryption for the topics the governance marks as such, for intra-host traffic and for
    paths that MACsec does not cover.
- Users keep the `ros2 security` tooling. The result is "SROS2 file-compatible, TickLE's own protocol": TickLE nodes
  secure each other, but it is not wire-compatible with DDS-Security.

## Open questions, for when the user starts it

1. Whether authentication alone must be mandatory whenever access control is on (Plan's view: yes, since without it
   an identity can be spoofed).
2. The crypto library on FreeRTOS (e.g. mbedTLS), and its flash, RAM and CPU budget.
3. The wire change for the handshake and for the signed or encrypted submessages, and its bytes under the wire rule.
4. What counts as "enforced" when MACsec is expected but not verifiable from user space.
5. How to test: a CycloneDDS SROS2 setup as the behavioural control (refused participants, denied topics), and the
   rig's links with MACsec on.
