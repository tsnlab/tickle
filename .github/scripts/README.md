# HIL (hardware-in-the-loop) runner setup

[`run_perf.sh`](run_perf.sh) is what [`performance.yml`](../workflows/performance.yml) runs on
every push to `main`: it builds and runs a real `ping`/`pong` (latency) and
`perf_client`/`perf_server` (throughput) round trip across two dedicated Raspberry Pi boards
connected by an Ethernet link, then hands the numbers to
[`github-action-benchmark`](https://github.com/benchmark-action/github-action-benchmark) and
[`publish_dashboard.sh`](publish_dashboard.sh) (see the top-level [README.md](../../README.md)'s
"Continuous performance testing"). This only needs to be set up once per runner; a normal
contributor never runs any of this by hand.

## What's needed

- **A self-hosted GitHub Actions runner**, registered with the `tickle-hil` label (that's what
  `performance.yml`'s `runs-on: [self-hosted, tickle-hil]` matches on) - see GitHub's own
  [Adding self-hosted runners](https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/adding-self-hosted-runners)
  guide for the actual registration steps. It just needs network access to both Pis and enough
  of a toolchain to run this script (`bash`, `ssh`, `git`) - the actual build/compile happens
  *on* the Pis, not here.
- **Two Raspberry Pis** on the same network, reachable by IP from the runner:
  - **rpi#1** - client role (`ping`, `perf_client`)
  - **rpi#2** - server role (`pong`, `perf_server`)

  Each one needs:
  - A `ci` user account.
  - A C toolchain (`gcc`, `make`) - the Pi builds `platform/linux` natively, the same way any
    Linux dev machine would (see the top-level README's "Build").
  - `~/tickle` already `git clone`d (as the `ci` user) - `run_perf.sh` only ever `fetch`es +
    `reset --hard`s an existing checkout, it never clones one from scratch.

  **rpi#1 only**, for `run_perf.sh`'s tc/netem loss-injection scenarios (QoS roadmap #5,
  RELIABILITY vs BEST_EFFORT under real packet loss): the `ci` user needs passwordless `sudo tc`.
  [`setup-rpi-tc-sudoers.sh`](setup-rpi-tc-sudoers.sh) does this - copy it to rpi#1 and run it
  there as root (`sudo bash setup-rpi-tc-sudoers.sh`), or run it directly over SSH without
  copying anything first:
  ```sh
  ssh <user>@<rpi#1> 'sudo bash -s' < .github/scripts/setup-rpi-tc-sudoers.sh
  ```
  It installs a `visudo`-validated `/etc/sudoers.d/tickle-ci-tc` granting `ci` passwordless sudo
  for the `tc` binary only (not a blanket `NOPASSWD:ALL`), then verifies the grant actually works
  before exiting. `run_perf.sh`'s own `probe_loss_testing()` checks this (`sudo -n tc ...`) before
  running any of these scenarios and just skips them - not the rest of the run - if it isn't set
  up, so this is optional to get everything else in this document working, only needed for that
  one section.

## SSH key setup

`run_perf.sh` SSHes into both Pis as `ci` using one key, shared between them:

```sh
# On the runner, as whatever user the tickle-hil runner service runs as:
$ ssh-keygen -t ed25519 -f ~/.ssh/tickle_ci_ed25519 -N ""
```

Then, on **each** Pi, append the resulting `tickle_ci_ed25519.pub` to the `ci` account's
`~/.ssh/authorized_keys`. `run_perf.sh` connects with `BatchMode=yes` (no password/interactive
prompt possible) and `StrictHostKeyChecking=accept-new` (trusts a Pi's host key the first time it
connects and pins it after - if a Pi is ever reimaged or its host key otherwise changes, delete
the stale entry from the runner's own `~/.ssh/known_hosts` before the next run, or that run will
fail with a host-key mismatch instead of silently trusting the new one).

## If a Pi's IP address changes

`RPI_CLIENT_HOST`/`RPI_SERVER_HOST` in `run_perf.sh` default to the two Pis' current addresses,
overridable via the environment (`RPI_CLIENT_HOST=10.1.1.x .github/scripts/run_perf.sh`) without
touching the script - but since `performance.yml` invokes it with no such overrides, a permanent
address change (a DHCP reassignment, a reimage, ...) needs the default itself updated in
`run_perf.sh`. See that file's own comment on `RPI_CLIENT_HOST` for the most recent one.

## Troubleshooting

- **Build or SSH step fails outright**: confirm the runner can reach both Pis at all
  (`ssh -i ~/.ssh/tickle_ci_ed25519 ci@<host> true`, run as the runner's own user) before
  suspecting anything about the workflow itself - a Pi being powered off, rebooted, or moved to
  a different subnet looks the same as a real CI failure otherwise.
- **A single flaky metric (e.g. `rtt mdev`) fails the whole run**: see `performance.yml`'s own
  comment on why jitter-flavored stats are tracked in a separate, non-failing benchmark group -
  a metric that's inherently noisy on real hardware shouldn't gate the build the same way a
  stable one (`rtt avg`, throughput) does.
