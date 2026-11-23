# Adaptive rate control on the tc-emulated link: plan (2026-11-19, written before the first run)

Question: does `tt_LinkOptions.rate_control = tt_RATE_CONTROL_ADAPTIVE` (design: `../adaptive_pacing/DESIGN.md`) keep TickLE
no worse than - ideally better than - Fast DDS and CycloneDDS on the Wi-Fi-like links, without a rate set by the user, and
without costing the small cells anything? Nothing here is measured yet.

Arms (the runner's `FWS`): `tickle-ad` (ADAPTIVE, cap = the WIFI profile's link_rate, floor `tt_RATE_MIN_DEFAULT`),
`tickle` (the library default: unpaced), `tickle-lg` (small cells only: the small-sample node built with large samples, as `tickle-ad` needs, without the option - the control of A7), `tickle-p95` (FIXED at 95 % of the profile's carried rate: the Stage C oracle),
`cyclonedds`, `fastdds`. Settings stated in every result row: the profile's netem line, `rate_control`, the cap, the
floor, the queue target (defaults), the TickLE commit.

Stages (`wtc_chainD.sh`): `s1` (the first, as decided on 2026-11-19), `all`, `step` (30 -> 4 -> 30 Mbit/s), `readers` (2 and
3 readers). Cells and rules as the earlier stages (PLAN.md): be_small, rel_small, rel_large 128 KiB and 1 MiB at 0.25 Hz;
A and A2 copies and the A/A rule (VOID if |A2 - A| > 10 %); WIN/TIE/LOSE against each DDS stack; 5 rounds x 2 copies = 10
pooled runs in a deciding cell.

What decides (acceptance, DESIGN.md section 7): A4 the estimate follows the rate step (the `.steps` file gives the step
times; the RESULT line's `btlbw_bps` and the per-sample latency show it); A5 within the oracle's reach on p50 and p99;
A6 no LOSE against either DDS stack; A7 small cells unchanged (adaptive on against off); A8 datagram count near the
oracle's; A11 2 and 3 readers keep the link used, the queue bounded, no reader starved (`.far2`, `.far3` hold the other
readers' RESULT lines).

Known before the run: the model and a one-host loopback check only (DESIGN.md sections 10 and 11). The loopback run showed
a start-up ramp of 10-20 ms per sample; on these links the ramp is expected to be a small part of a sample's time, but
that is a prediction, not a result.

How to run: build and ship this branch (the t1pi scripts were removed from the tree; last at cb9c8606: `git show cb9c8606:examples/perf_hil/experiments/t1pi/t1pi_build.sh`), then
`OUT_BASE=$HOME/rig_results_safe/wtc_adaptive_<date> ./wtc_chainD.sh s1` (detached, under the rig lock; the runner takes it).
