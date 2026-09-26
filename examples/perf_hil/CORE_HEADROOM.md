# Is there more to optimise in TickLE core? Yes, and it is not in our code

2026-09-26. Asked directly by the user: check whether TickLE core has further performance
headroom, and if not, move to `rmw_tickle`. This answers it from measurement rather than from
reading the source.

Raw data: `results/syscall_headroom_2026-09-26.txt` (exact syscall counts, under strace, so the
*times* there are inflated but the *counts* are exact) and `results/single_packet_release_2026-09-26.txt`
(CPU split, no strace).

## Where TickLE's CPU actually goes

`reliable_throughput_p1`, release build, median of 3, no strace:

| role | framework | utime_s | stime_s | kernel share |
|---|---|---:|---:|---:|
| client | **TickLE** | **0.822** | 4.184 | **83.6%** |
| client | CycloneDDS | 1.542 | 3.554 | 69.7% |
| client | FastDDS | 2.271 | 3.671 | 61.8% |
| server | **TickLE** | **0.923** | 2.253 | **70.9%** |
| server | CycloneDDS | 3.624 | 4.419 | 54.9% |
| server | FastDDS | 2.954 | 2.745 | 48.2% |

**TickLE's own user-space code is already the smallest of the three, by 1.9x to 3.9x.** That is the
finding that decides where the remaining work is: on the server TickLE spends 0.923 s of user time
against CycloneDDS's 3.624 s while delivering 1.7x the samples. Deleting *every instruction* of
TickLE's own algorithm would cut client CPU by 16% and server CPU by 29%, and that is the entire
ceiling available from optimising our code. The other 71-84% is the kernel.

So the question is not "what is slow in our code" - nothing is, relative to the alternatives. It is
"how many times do we enter the kernel, and can it be fewer".

## Syscalls per delivered sample

From the strace arm, with CycloneDDS as the control:

| | TickLE | CycloneDDS |
|---|---:|---:|
| send syscalls / sample | 1.09 (`sendto`) | 1.03 (`sendmsg`) |
| receive syscalls / sample | **1.58** (`recvfrom`) | **0.74** (`recvmsg`) |
| wasted receives (EAGAIN) | **49,958 of 148,448 = 34%** | - |
| share of syscall time, client send | 89.2% | 9.3% |
| share of syscall time, server receive | 95.1% | 2.5% |

Two results, and the second is the one the control earned:

**1. The send side is at parity, so batching there is headroom and not catch-up.** The
pre-registered reading said that if CycloneDDS also issued ~1 send syscall per sample then syscall
count is not what separates the two and `sendmmsg` would not close the gap. It does (1.03), so that
is what the data says, and the batching hypothesis is *not* supported as an explanation of any
difference. It remains worth doing - one `sendto` per datagram is 89% of the client's syscall time,
and `node_flush` already gathers datagrams on a 1 ms grid, so the batch exists at the call site -
but it should be proposed as "spend less kernel time than we do now", not "catch up with
CycloneDDS", which is not a thing TickLE needs to do here.

**2. The receive side is a real, control-backed inefficiency.** TickLE issues **2.1x** CycloneDDS's
receive syscalls per sample, and **a third of them return EAGAIN having read nothing**. The cause
is visible in `src/hal_linux.c:563-567`: `tt_try_receive()` probes two sockets with
`MSG_DONTWAIT`, so an empty first socket costs a whole syscall to discover. CycloneDDS is below one
receive per sample, meaning it drains several datagrams per wakeup. This is 95.1% of the TickLE
server's syscall time.

## What this does not say

CycloneDDS's client spends **73.8%** of its syscall time in `futex` and its server **95.5%** -
thread synchronisation. TickLE's is 1 futex call, total. That is why TickLE's user time is lower
and why it wins CPU today, and it is a property of the single-threaded design, not something to
trade away while chasing the syscall count. Any batching change that introduces a worker thread
would buy back syscalls and pay for them in futex.

## Proposed order

1. **`recvmmsg` on the receive path, plus removing the two-socket blind probe.** Control-supported,
   attacks 95% of the server's syscall time, and the 34% EAGAIN figure is a floor on the waste that
   is removable without changing any protocol behaviour.
2. **`sendmmsg` in `node_flush`.** Honest framing: headroom on 89% of the client's syscall time, not
   a gap against anyone. The datagrams are already batched by the 1 ms grid.
3. **Nothing further in our own algorithms.** They are 16-29% of CPU and already the leanest of the
   three; the effort/return there is worse than in either item above.

How to read the outcome, pre-registered: item 1 should reduce the server's `recvfrom`/`recvmsg`
count per sample toward CycloneDDS's 0.74 and reduce server `stime_s` roughly in proportion. If
`stime_s` does **not** fall when the syscall count does, then per-syscall entry cost was not what
the kernel time was, and item 2 should be dropped rather than attempted on the same reasoning.


## Result of item 1, measured against the pre-registration above (2026-09-26)

Dev's `1af57e3e` stops `tt_try_receive()` probing a socket already known to be empty.
`results/recv_probe_verify_2026-09-26.txt`, release build, same session, 3 clean repetitions per
arm plus one straced repetition for exact counts.

| | before (`23465a68`) | after (`1af57e3e`) |
|---|---:|---:|
| receive syscalls / sample | 1.650 | **1.066** |
| EAGAIN receives | 45,948 | **736** |
| server `stime_s`, median of 3 | 2.278 | **1.972** |
| server `cpu_s_per_Msample` | 3.350 | **2.932** |

The three repetitions' `stime_s` ranges are [2.275, 2.380] and [1.970, 2.004] - no overlap.

**The pre-registration's fail case did not occur, and that is the load-bearing part.** It said: if
the syscall count falls and `stime_s` does not, the kernel time was per-byte rather than per-entry
and `sendmmsg` should be dropped on the same reasoning. `stime_s` fell 13.4% while the syscall
count fell 35%, so per-syscall entry cost is a real component of the kernel time and item 2 keeps
its justification. Had it gone the other way this measurement would have cancelled the next piece
of work rather than endorsing it, which is why it was worth running before that work started.

Server CPU per Msample is now 2.93 against CycloneDDS's 10.85.

Two caveats on the numbers, so they are not over-read. The straced repetitions delivered different
sample counts between arms (81,848 and 135,084), because strace perturbs throughput; only the
*ratio* per sample is comparable there, and the CPU figures come from the unstraced runs. And
1.066 receives per sample is close to the floor Dev names for this change - n+1 per drain cannot
go below one per sample - so reaching CycloneDDS's 0.74 needs `recvmmsg`, which is the next step
rather than a shortfall in this one.

## Result of item 3 (`recvmmsg`, Dev's `ef0c7ea0`), against the pre-registration (2026-09-26)

Campaign cells 1, 4, 10 and 11, all three frameworks, 3 repetitions, before (`559b91f6`, the parent) and
after (`ef0c7ea0`) in one session: `results/campaign_rxb_{before,after}_2026-09-26.txt`. Both score
**WIN 28, DRAW/TIE 4, LOSE 0**. TickLE, before against after, medians with the three repetitions' range:

| TickLE | before | after | pre-registered |
|---|---:|---:|---|
| c1 server receive calls per sample | (1.066 earlier) | **0.137** | <= 0.8, **met** |
| c1 server `stime_s` | 1.992 | **1.847 (-7.3%, no overlap)** | falls, **met** |
| c1 server `cpu_s_per_Msample` | 2.987 | **2.529 (-15.3%, no overlap)** | - |
| c1 server `utime_s` | 0.846 | 0.550 (-35.0%) | - |
| c4 server `cpu_s_per_Msample` | 8.849 | 8.011 (-9.5%, no overlap) | - |
| c1 / c4 throughput, wire bytes | 115.7 / 939.5 Mbps | 116.0 / 939.6 Mbps, identical bytes | unchanged, **met** |
| c1 server peak RSS | 1,780 KB | 1,836 KB (+56) | about +48 KB (32 x 1.5 KB), **met** |
| **c10 / c11 RTT mean (kill criterion)** | 0.207 / 0.233 ms | 0.213 / 0.236 ms | no regression: **not settled** |

The receive calls per sample come from the server's own `rx_batch_calls / recv`: 130-132 thousand
calls for about 950 thousand datagrams, an average of 7.3 datagrams per call. **Only 10-23 calls in
130 thousand filled all 32 slots**, so N=32 never binds on the rig. A smaller N would hold the same
result for less memory. That is a sizing choice, recorded, and not a defect.

**The latency criterion is not settled by this run, and it is not waved through.** c10's mean moved
+6 us (ranges [0.207..0.209] and [0.208..0.214]), which overlap at n=3 but only just. As for this
morning's poll change, the settling run is 10 repetitions per arm. **Pre-registered: the drift is real
if |mean_after - mean_before| exceeds twice the combined standard error, and not resolved otherwise.**
A real regression fires the kill criterion, whatever the CPU saving.

Strace cross-check (`results/rx_batch_verify_2026-09-26.txt`, same build pair): receive syscalls per sample
**1.069 → 0.056**, EAGAIN 728 → 1; unstraced server `cpu_s_per_Msample` 2.95 → 2.50 (-15%), matching the
campaign. The straced figure is lower than the counter's 0.137 because strace slows the receiver and
fuller batches result - both are far inside the <= 0.8 pre-registration. The latency item stays open until
the 10-repetition pair reports.

### The 10-repetition latency settlement (2026-09-26)

Cell 10 (P1, RELIABLE latency), all three frameworks, 10 repetitions per arm, three builds:
`559b91f6` (before), `ef0c7ea0` (recvmmsg as first landed) and `5f70c3cc`, Dev's fix. In the fix, the read
that ends a wait is `recvfrom` again, and only the drain after it batches. Raw rows are in
`results/lat10_recvmmsg_2026-09-26/`. There are 0 void rows, and every TickLE row reports
`core_build=release`. The arms ran one after another in the order before, fix, ef0c7ea0, 17:21-17:53.
They were not interleaved, so the vendors, whose code is the same in every arm, serve as the drift control.

TickLE `rtt_avg`, mean ± standard error over 10:

| arm | TickLE | FastDDS (control) | CycloneDDS (control) |
|---|---:|---:|---:|
| `559b91f6` before | 209.3 ± 0.6 us | 290.5 ± 0.8 | 290.6 ± 15.4 |
| `ef0c7ea0` recvmmsg | 211.1 ± 0.5 | 289.5 ± 1.0 | 281.5 ± 13.3 |
| `5f70c3cc` fix | 210.1 ± 0.6 | 291.1 ± 0.8 | 310.3 ± 16.8 |

Read against the pre-registration (real if the difference exceeds twice the combined standard error):
- **`ef0c7ea0` − before = +1.8 us, against 2×SE = 1.6 us: real by the rule, and only just.** It is also
  far smaller than the +6 us the 3-repetition run suggested. With 18 such comparisons in this reading,
  one crossing by 0.2 us is weak evidence on its own. Dev's pre-registration for the fix is what makes it
  worth reading anyway.
- **`5f70c3cc` − before = +0.8 us, within 2×SE (1.7 us): no regression.**
- Dev pre-registered that if `ef0c7ea0` regressed and `5f70c3cc` did not, the extra probe before
  processing explains the regression. **That is what the rows show**, at the weak margin stated above.
- **The controls did not move.** FastDDS is within 2×SE in all three pairs. CycloneDDS's mean is noisy (its
  tail) and within 2×SE throughout, and its `rtt_min` is within 2×SE throughout. So the TickLE differences
  are not a drift of the rig across the 30 minutes.
- `rtt_min` agrees: `5f70c3cc` is 2.8 us below `ef0c7ea0` (2×SE 2.4), and neither differs from before.

**Verdict: the kill criterion does not fire for the code as it stands (`5f70c3cc`).** `ef0c7ea0`'s small
regression is gone in `5f70c3cc`. Cell 11 was not re-run at n=10. Its 3-repetition move (+3 us) was
inside its own ranges.

### c1 on `5f70c3cc`: the saving survives the fix, and item 3 is closed (2026-09-26)

The fix changed the receive path, so the CPU saving had to be shown to survive it. The rows are in
`results/lat10_recvmmsg_2026-09-26/c1_fix.txt`: 3 repetitions, 0 void. TickLE server:

| TickLE server, c1 | `559b91f6` before | `ef0c7ea0` | `5f70c3cc` (3 reps) | pre-registered |
|---|---:|---:|---:|---|
| receive calls per sample | 1.066 | 0.137 | **0.235** | <= 0.8, **met** |
| `stime_s` | 1.992 | 1.847 | **1.805-1.824** | falls, **met** |
| `cpu_s_per_Msample` | 2.987 | 2.529 | 2.551-2.569 | - |
| send Mbps (client) | 115.7 | 116.0 | 113.9-114.5 | - |

How the call count is read. `rx_batch_calls` counts `recvmmsg()` calls only (`hal_linux.c` `rx_fill`).
Since the fix, the read that ends a wait is a `recvfrom()` that the counter does not see. So the calls
per sample are `(rx_batch_calls + recv - rx_batch_datagrams) / recv`. For example, rep 1 gives
(107,311 + 936,321 - 823,932) / 936,321 = 0.235. That is more calls than `ef0c7ea0` made, which is the
fix's design: one plain read per wake, then batches. It is still 4.5 times fewer than before, and
`stime_s` is lower than on `ef0c7ea0`.

The client's send rate is about 1.5% below the earlier sessions. Throughput is not this item's
criterion, and c1's scoring against the vendors is unaffected. The difference is noted and was not
pursued.

**Item 3 is closed:** the CPU saving holds on `5f70c3cc` and latency does not regress. `rx_batch_full` was
5-16 per run, so N=32 still never binds, and the sizing choice for N is left to Dev as recorded above.
