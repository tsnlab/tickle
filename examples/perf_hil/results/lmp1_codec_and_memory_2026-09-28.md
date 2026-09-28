# Stage 1, pass 5: the codec alone, and the per-endpoint memory

Dev, 2026-09-28, the PC half of LARGE_MESSAGE_PLAN.md pass 5 (Plan has the rig half). Both
measurements compare `fe26a97b` - the C++ codec, before rmw used either - against `feda8093`, the
wiring. Release builds, CPU 15 pinned for the codec run.

## How these were read, fixed before running

- **The codec.** The prediction is that the ROS-to-struct step disappears, about 1.6 us per
  direction at 64 KB on this PC (a719dfaa). The saving is real when (struct - direct) is positive
  and larger than the rounds' spread of both medians, on both directions. It is *about the copy*
  when the saving at 64 KB is close to the measured `to_tickle` time and the smaller control saves
  proportionally less; if the control saves as much, the explanation is wrong whatever the number
  says. A negative saving on either direction fails this half outright, and `roundtrip_ok` must
  stay 1 - a faster codec that does not reproduce the message is not a result.
- **The memory.** Allocator bytes (`mallinfo2().uordblks`), not VmHWM, which is whole-process and
  misses untouched calloc pages. A slope over many endpoint pairs, taken after a warm pair, so a
  fixed per-node cost is not read as a per-endpoint one. `std_msgs/Empty` is the control: what a
  pair costs that has nothing to do with the message costs the same for it.

## The codec alone (conv_cost, 15 rounds of 200 calls)

| Type | struct encode | direct encode | struct decode | direct decode | `to_tickle` |
|---|---|---|---|---|---|
| Image, 64,000 B | 3.213 us | **1.591 us** | 3.226 us | **1.617 us** | 1.589 us |
| ByteMultiArray, 16,384 B | 0.324 us | **0.106 us** | 0.278 us | **0.111 us** | 0.162 us |

Image saves **1.622 us encoding and 1.609 us decoding**, against spreads of 0.074 to 0.177 us - an
order of magnitude clear of the noise. The saving is the `to_tickle` step to three figures
(1.589 us), which is what the prediction said it would be. The 16 KB control saves 0.218 us, a
quarter the size for a quarter the bytes, so the saving scales with the copy rather than being a
fixed overhead. `roundtrip_ok=1` for both.

## The per-endpoint memory (endpoint_memory, one publisher and one subscription per topic)

| Build | Image, bytes per pair | Empty, bytes per pair |
|---|---|---|
| `fe26a97b`, before the wiring | 133,705 | 5,396 |
| `feda8093`, after | **5,577** | 5,331 |

**The slope falls by 128,128 bytes per pair**, against a predicted 2 x 64,044 - the publisher's
`publish_scratch_buf` and the subscription's `decode_scratch`, one TickLE struct each. The two
agree to within 40 bytes. The control does not move.

After the fall, Image's slope is within about 250 bytes of Empty's, and that difference is a fixed
24.7 KB across the whole run rather than a per-endpoint cost: at 50, 100 and 200 pairs the
difference per pair halves as the count doubles (493.5, 246.7, 123.4 bytes), so the type-specific
*slope* is indistinguishable from zero.

Residue after destroying every endpoint was checked for the same reason and is not a leak: it does
not scale with the count either (27,408 at 50 pairs, 29,488 at 100, 31,536 at 200 - about 2 KB per
doubling, not 100 pairs' worth).

## Verdict

Both halves of pass 5 that are PC-side pass, and both match their predictions rather than merely
pointing the right way.
