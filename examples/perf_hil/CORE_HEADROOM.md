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
