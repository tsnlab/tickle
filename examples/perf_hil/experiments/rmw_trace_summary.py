#!/usr/bin/env python3
"""Summarise rmw_crosshost_rtt.sh TRACE=1 output: what each rmw's pong process does per round trip.

strace slows every syscall, so no time from these traces is a performance figure. What they are good for
is structure: how many syscalls of which kind, on how many threads, one ping-pong costs each rmw. The
active window is taken from the pong's own trace: from 5 s after its first line (the pong is started 4 s
before the ping, so discovery and node creation are over) for 8 s (the ping runs 10 s at 10 Hz). Counts are
divided by the pings that fall in that window, 10 per second.

usage: rmw_trace_summary.py <traces-dir>
"""
import collections
import os
import re
import sys

LINE = re.compile(r'^(\d+)\s+(\d\d):(\d\d):(\d\d\.\d+)\s+(?:<\.\.\.\s+)?([a-z_0-9]+)')
GROUPS = {
    'receive': ('recvfrom', 'recvmsg', 'recvmmsg', 'read'),
    'send': ('sendto', 'sendmsg', 'sendmmsg', 'write'),
    'wait': ('ppoll', 'poll', 'epoll_wait', 'epoll_pwait', 'select', 'pselect6', 'clock_nanosleep', 'nanosleep'),
    'futex': ('futex',),
}


def group_of(name):
    for g, names in GROUPS.items():
        if name in names:
            return g
    return 'other'


def summarise(path):
    events = []
    for ln in open(path, errors='replace'):
        if 'resumed>' in ln:
            continue                       # count each syscall once, at its start
        m = LINE.match(ln)
        if not m:
            continue
        tid, hh, mm, ss, name = m.groups()
        t = int(hh) * 3600 + int(mm) * 60 + float(ss)
        events.append((t, int(tid), name))
    if not events:
        return None
    t0 = events[0][0]
    lo, hi = t0 + 5.0, t0 + 13.0
    win = [e for e in events if lo <= e[0] < hi]
    pings = (hi - lo) * 10.0
    by_group = collections.Counter(group_of(n) for _, _, n in win)
    by_name = collections.Counter(n for _, _, n in win)
    threads = collections.Counter(tid for _, tid, _ in win)
    return pings, by_group, by_name, threads


def main(d):
    rows = []
    for f in sorted(os.listdir(d)):
        r = summarise(os.path.join(d, f))
        if r is None:
            print(f'{f}: no syscall lines')
            continue
        pings, g, n, th = r
        rmw = f.rsplit('.', 1)[0]
        print(f'== {rmw}: {pings:.0f} pings in the window, {len(th)} threads making syscalls')
        print('   per ping: ' + '  '.join(f'{k} {g[k] / pings:.1f}' for k in ('receive', 'send', 'wait', 'futex', 'other')))
        print('   top syscalls per ping: ' + ', '.join(f'{k} {v / pings:.1f}' for k, v in n.most_common(8)))
        print('   syscalls per ping by thread: ' + ', '.join(f'{tid} {v / pings:.1f}' for tid, v in th.most_common(6)))
        rows.append((rmw, sum(g.values()) / pings))
    if rows:
        print('\n== total syscalls per ping: ' + ', '.join(f'{r} {v:.1f}' for r, v in rows))


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else '.')
