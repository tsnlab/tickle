#!/bin/sh
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.

# Milestone 4 (see /home/semih/.claude/plans/lively-sauteeing-rainbow.md): builds a pong (server,
# node 2) and a ping (client, node 1) image and runs both at once, joined to the same QEMU
# `-netdev socket,mcast=` group - the same "two independent guests on one broadcast segment"
# setup the two real Raspberry Pis simulate for the actual HIL performance workflow
# (.github/workflows/performance.yml), just emulated instead of on real hardware.

set -u
cd "$(dirname "$0")"

MCAST_GROUP=230.0.0.1:5000
DURATION_S=${DURATION_S:-10}

make ROLE=pong NODE_ID=2 all
make ROLE=ping NODE_ID=1 all

rm -f pong.pcap ping.pcap pong.log ping.log

# pong runs in the background for the whole test; ping (foreground, time-bounded) is what
# actually drives it - once ping's `timeout` returns, there's nothing further to wait for.
# stdin is explicitly redirected from /dev/null on both: -nographic multiplexes the guest's
# serial console AND the QEMU monitor over stdin, and a *backgrounded* process left attached to
# the invoking terminal's stdin can get suspended by the shell's job control (SIGTTIN) the moment
# it tries to read - at which point it's stopped, not running, and later `kill` (SIGTERM) can't
# actually terminate a stopped process, so `wait` below would block forever. Redirecting both
# (not just pong's) keeps the two instances' behavior identical and avoids relying on foreground
# vs background job-control semantics at all.
qemu-system-riscv32 -machine virt -nographic -bios none -kernel RTOSDemo-pong-2.elf \
    -global virtio-mmio.force-legacy=off \
    -netdev socket,id=net0,mcast=$MCAST_GROUP -device virtio-net-device,netdev=net0 \
    -object filter-dump,id=dump0,netdev=net0,file=pong.pcap \
    </dev/null >pong.log 2>&1 &
pong_pid=$!

# Give pong a moment to finish booting/negotiating its virtio-net link before ping starts
# sending - not load-bearing (a request during that window would just time out and retry), but
# avoids a guaranteed-to-be-wasted first attempt.
sleep 1

timeout "$DURATION_S" qemu-system-riscv32 -machine virt -nographic -bios none -kernel RTOSDemo-ping-1.elf \
    -global virtio-mmio.force-legacy=off \
    -netdev socket,id=net1,mcast=$MCAST_GROUP -device virtio-net-device,netdev=net1 \
    -object filter-dump,id=dump1,netdev=net1,file=ping.pcap \
    </dev/null >ping.log 2>&1
ping_status=$?

kill "$pong_pid" 2>/dev/null
wait "$pong_pid" 2>/dev/null

echo "=== pong.log ==="
cat pong.log
echo "=== ping.log ==="
cat ping.log

# timeout's own exit code for "killed after the time limit" (124) is the expected, successful
# outcome here - ping has no other way to stop on its own (see main_ping.c: it's just an
# infinite send/poll loop, like the real ping/pong example's run-until-Ctrl+C default).
if [ "$ping_status" -ne 124 ] && [ "$ping_status" -ne 0 ]; then
    echo "run_pair: ping exited unexpectedly (status $ping_status)"
    exit 1
fi

# Neither QEMU process crashing nor ping's own process exiting cleanly actually proves a real
# round trip happened - pong could have silently failed to answer a single request (e.g. the
# link came up but requests never actually arrived) and this would still look like a clean run
# by exit status alone. Require a real minimum number of successful replies logged on each side
# instead of only checking that nothing crashed.
MIN_REPLIES=5
replies=$(grep -c 'ping: seq=.*rtt=' ping.log)
requests=$(grep -c 'pong: request seq=' pong.log)

echo "run_pair: pong answered $requests request(s), ping received $replies real repl(y/ies)"

if [ "$replies" -lt "$MIN_REPLIES" ] || [ "$requests" -lt "$MIN_REPLIES" ]; then
    echo "run_pair: fewer than $MIN_REPLIES real round trips observed - treating as a failure"
    exit 1
fi
