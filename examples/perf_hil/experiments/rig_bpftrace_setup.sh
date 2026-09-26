#!/usr/bin/env bash
# Installs bpftrace on both rig Pis (2026-09-26, RMW_PERF_PLAN.md section 8). The user granted ci
# `sudo bpftrace` and `sudo ethtool` for the kernel-path split. apt-get install was already allowed.
# It runs under the rig lock, so the install cannot land in the middle of a measurement:
#   RIG_LOCK_SCOPE=hil examples/perf_hil/rig_lock.sh examples/perf_hil/experiments/rig_bpftrace_setup.sh
# It then checks that bpftrace can attach a probe under sudo, and lists the kernel probe points the split needs.
set -euo pipefail
K=$HOME/.ssh/tickle_ci_ed25519
for h in 10.1.1.214 10.1.1.213; do
    echo "== $h $(date -Is)"
    ssh -i "$K" -o BatchMode=yes "ci@$h" 'set -e
hostname; uname -r
command -v bpftrace >/dev/null || sudo -n apt-get install -y bpftrace > /tmp/rmwx_bpftrace_install.log 2>&1 \
    || { echo "install FAILED"; tail -5 /tmp/rmwx_bpftrace_install.log; exit 1; }
bpftrace --version
sudo -n bpftrace -e "BEGIN { printf(\"bpftrace attach ok\\n\"); exit(); }"
echo "-- probe points (tracepoints, then kprobes):"
for pat in "tracepoint:net:*" "tracepoint:irq:*irq*" "tracepoint:sched:sched_wak*" "tracepoint:syscalls:sys_exit_recv*" \
           "tracepoint:syscalls:sys_exit_ppoll" "kprobe:macb_*" "kprobe:gem_*" "kprobe:*udp*enqueue*" "kprobe:sock_def_readable"; do
    echo "   $pat: $(sudo -n bpftrace -l "$pat" 2>/dev/null | tr "\n" " " | cut -c1-400)"
done
/usr/sbin/ethtool -c eth0 | grep -E "^(rx|tx)-usecs:"'
done
echo "=== done ==="
