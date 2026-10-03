# Working in this repository

Operational rules specific to TickLE. General engineering discipline is not here; this is what this
codebase, this rig and this CI will do to you if you assume otherwise.

## Roles

**Plan** designs, measures and documents. **Dev** writes all production code — `src/`, `include/`,
`platform/`, `rmw_tickle/` sources, `tests/`, `.github/scripts/` and the gates. Plan owns
`examples/perf_hil/experiments/` and the planning documents, and reads production code to answer design
questions. When the next step is "write the code", Plan writes the task for Dev — design, pre-registered
criteria, the control, and what would falsify it — rather than the diff.

## Before a change leaves your machine

```
clang-format (lintenv 19) → git add → make check-gates → read the table → push only on GATES_EXIT 0
```

- **Format with `/tmp/lintenv/bin/clang-format`, not the system one.** Three night-shift gate runs died on
  comment alignment alone. The same applies to clang-tidy: the gates use lintenv 19, a bare `make lint` uses
  the system clang-tidy 21, and they disagree on the same bytes.
- **Read the gate table by shape**: `grep -E '^[A-Z]{4}  '`, then count rows and count non-PASS. Never check
  it against a list of verdicts you expected — that is how two `ADVS` rows were once read as "all PASS".
- **Never chain a push after the gates with `&&`.** You have to read the table yourself.

### The gates build ONE configuration

A commit once passed 13 of 13 gates and broke `-Dtt_SEGMENT_ENABLED=0`; CI failed four jobs on it, two of
them FreeRTOS, which also compiles the segment out. The `build configs (syntax)` gate now sweeps eight
configurations, but the table still does not cover:

- `make test-linux` — two nodes over the Linux HAL in a netns. It ran red all day on 2026-09-29 while every
  gate reported PASS. Run it before trusting a push that touches the HAL or the transport.
- the interface set CI assembles — the gates build against this machine's `/opt/ros`, and a capacity row
  naming a type that exists here and not there passes locally and fails in CI.

## Git in this checkout

`/home/semih/tickle` is shared with another session.

- **Explicit paths only.** Never `git commit -a`.
- **Never `git stash` here.** A stash+pop with only untracked changes once applied an old unrelated stash.
- **Push over SSH**: `SSH_AUTH_SOCK=/run/user/1000/openssh_agent`, `git@github.com:tsnlab/tickle.git`. The
  `https` origin is read-only and returns 403.
- **Cite a SHA only after rebase and push**, read back from `origin/main`. `check-doc-shas` fails on a
  pre-rebase id with "exists but is not reachable from HEAD".
- **Check CI after every push, on every workflow.** A red gate once sat unseen for ten commits because the
  watcher was scoped to one job.

## Build configurations that differ from this machine

- **CI compiles our ROS `.cpp` at C++17; this machine defaults to C++20.** `lint_rmw.sh` compiles them at 17;
  `CXX20_TARGETS` lists the exceptions.
- **A new `find_package` needs the matching `<depend>` in `package.xml`**, or colcon builds in the wrong order.
- Flag-gated core code (`tt_CONTEXT_ID_CLAIM`, `tt_LOCAL_DELIVERY`) is compiled out of `check-gates` and
  linted by CI. Lint it with `clang-tidy -p build/rmw_tickle`.

## The rig and the network

- **Never run TickLE processes in this PC's default network namespace.** They reach the rig over the
  10.1.1.x management LAN and contaminate its runs. Tests go in a private netns.
- **A netns with only `lo` has no route** — `sendto()` fails with "Network is unreachable". Add a default
  route, and for the bench a link carrying the configured broadcast address.
- **Take the rig lock** (`examples/perf_hil/rig_lock.sh`) inside the detached process, not around it. Test it
  by taking it, not by reading the lock file: the file records the last holder, not a held lock.
- **Do not overlap the netns acceptance suite with a rig campaign.** The suite's control fails under the
  campaign's PC load and reads as a vendor finding.
- **`sudo -n` permits only `ip` and `tc`.** Everything else fails silently under `2>/dev/null`.

## Long-running work

- **Detach it**: `setsid nohup ./script.sh > /path/outside/the/session.log 2>&1 < /dev/null &`. The script
  belongs in the repository and the results outside `/tmp` — a capture was once lost because both lived in a
  session scratchpad that went when the session did.
- **Clean up in a trap**, and **one `EXIT` trap per script**: a second one silently replaces the first and
  once left the rig shaped at 10 ms delay after a clean exit.
- **State an estimate, then in the same turn launch the work and schedule a wakeup at 1.5x** that forces the
  overrun analysis. A `timeout` on a wait loop ends the wait and forces nothing.
- **Look at the job within its first minute.** Its first lines show a refusal or a missing variable before
  any estimate matters.

## Measurement

Harnesses live in `examples/perf_hil/experiments/`. Results belong in the documents, not in a message.

- **Write the reading rules into the harness before the run**, including what would falsify the hypothesis,
  and implement them in the code rather than the header comment. A criterion no code enforces is a comment;
  one of ours printed a confident verdict on a void run because the control lived only in the comment.
- **Every campaign needs a control the change provably cannot touch.** Compare our delta to its delta, not
  our range to a bare historical point.
- **Check the sample count before the value.** A ten-round-trip run once agreed with a 1,950-round-trip
  published figure to three decimals.
- **Identify a process by `/proc/<pid>/comm` or `/proc/<pid>/exe`, never by its command line.** Every
  pattern, including the `[c]ommand` bracket idiom, eventually matches the checking command itself.

## Documents

`rmw_tickle/COMPARISON.md` (vendor comparison), `SHM_PLAN.md`, `PLAN.md`, `DESIGN.md`. **Everything in the
repository is English**, including commit messages.

- A published figure carries its provenance — which build, which rate, which arm. A number without it gets
  re-measured against the wrong baseline, or a correct change gets reverted for measuring worse.
- When a published number turns out to be wrong, correct it in place with the cause rather than deleting it.
  The reading drawn from it was published too.
- **Never copy CycloneDDS or Fast DDS source.** Reimplementing their behaviour in our own words and code is
  fine and expected.
