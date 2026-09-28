# The p1 client CPU regression is not in core's per-sample send path

Dev, 2026-09-28, at Plan's request (WIRE_PLAN.md section 10, b9f4ddff). Plan measured 6910d840
against 268dd20e on the rig and confirmed, at n=10 per arm, that client CPU per sample at p1 is
worse by 0.5-0.7% - about 30 ns - in c1, c5 and c9, every one of them RELIABLE. Plan's order of
suspicion was g10's depth check on every reliable publish, then g9's local-delivery branch, then
g8's id state on the send path.

## How this was read, fixed before running

`core_cost_ab.sh`, which builds `core_cost_bench.c` at each ref and runs them alternately on one
pinned CPU. No kernel and no network: TickLE core's own work per sample. `BENCH_ARGS=-R` is
RELIABLE KEEP_LAST 64, the c9 shape; `-R -c` adds the client's scheduler-driven send. 15 rounds,
200,000 samples, CPU 15.

Every experiment carries an A/A arm: the same commit built twice, under its full and its short
SHA, so its two medians differ only by placement and build noise. A difference counts as real when
it is positive, larger than that A/A difference, and larger than the rounds' spread of both arms.
The effect being looked for is about 30 ns.

## g10 is not the cause

| Arm | send median | spread |
|---|---|---|
| 04093971 (g10's parent) | 231.7 ns | 20.6 |
| 7119a779 (g10) | 232.0 ns | 23.0 |
| 04093971 again (A/A) | 232.7 ns | 18.0 |

g10 costs +0.3 ns, against an A/A difference of 1.0 ns. The medians resolve to about 1 ns, so a
30 ns effect would be plain. It is not there.

## Nor is anything else in core's send path, over the whole range

The bench does not compile before `25ac7fe0` (tt_Node became tt_Context), so the range was
measured from there to its head.

| Arm | `-R` send | `-R -c` send | `-R` recv |
|---|---|---|---|
| 25ac7fe0 | 233.1 ns | 249.7 ns | 77.3 ns |
| 268dd20e | 232.0 ns | 251.8 ns | 80.4 ns |
| 25ac7fe0 again (A/A) | 232.4 ns | 250.5 ns | 77.3 ns |

Send is flat both ways: -1.1 ns plain, +2.1 ns as a client, against A/A differences of 0.7 and
0.8 ns and spreads of 10 to 27 ns. Receive is +3.1 ns (about 4%), reproduced at +2.2 ns in the
client run, with the A/A arms agreeing exactly - small, real, and on the wrong side to explain a
client-CPU regression.

## Two of the three suspects cannot reach p1 at all

g8, g9 and g6 are compiled only where `rmw_tickle/rmw_tickle/CMakeLists.txt` defines
`tt_CONTEXT_ID_CLAIM`, `tt_LOCAL_DELIVERY` and `tt_DISCOVERY_OPTIONS`.
`examples/perf_hil/tickle/build.sh` never defines them, so every p1 arm is built without that
code. g10's hook is unconditional, which is why it was worth measuring - and it measures clean.

## What this leaves

The 30 ns is not in core's per-sample send path, and it is not in the three commits named. It is
therefore in what this bench excludes: the system calls, the client program's own loop, or work
that is not per sample - a periodic announce, for instance, which wire v11 made larger (Plan
confirmed c10's +1.4 B) and whose cost divides into every sample of a cell. A rig measurement that
separates those, rather than another core commit, is the next step.
