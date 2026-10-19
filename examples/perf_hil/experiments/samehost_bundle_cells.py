#!/usr/bin/env python3
"""Per-cell treatment and sample count of one s6_transport_cells.sh output, for samehost_bundle_rig.sh.

Usage: samehost_bundle_cells.py <cell .txt> <reps asked> <frameworks asked, space-separated>
       samehost_bundle_cells.py --selftest

It decides no row: the row readers (fair_samehost_summary.py, s11_publisher_cpu.py) do, by their own rules. It prints,
before any value is read, what the cell actually ran (an arm must report its treatment):
  ORDER   the framework order each rep realized, from s6's own "--- rep r/R order k/n framework=fw" lines, and whether
          it is the rotation s6 promises (rep r = the asked order rotated by r-1). A file without those lines comes from
          an s6 that did not interleave: NOT INTERLEAVED.
  SAMPLES per framework, the ON-arm reps recorded (s6_reps.read_reps, the readers' own parser) against the reps asked,
          and per rep what the samples were: throughput - client sent and server recv; latency - the client's measured
          round trips and crt_kept (the round trips with a due time, creation_latency_summary.py's C1 count).
The last line is "CELL <file> order=<rotated|NOT ROTATED|NOT INTERLEAVED> reps=<ok|SHORT>"; exit 0 when both are the
first value, 1 otherwise (the driver prints it and goes on: a short cell is the readers' to rule on, not this script's).
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import s6_reps  # noqa: E402 - beside this file

TURN = re.compile(r'^--- rep (\d+)/(\d+) order (\d+)/(\d+) framework=(\S+)')


def realized_orders(path):
    """{rep: [fw in the order run]} from s6's turn headers."""
    orders = {}
    for line in open(path, errors='replace'):
        m = TURN.match(line)
        if m:
            orders.setdefault(int(m.group(1)), []).append(m.group(5))
    return orders


def expected_order(asked, r):
    k = (r - 1) % len(asked)
    return asked[k:] + asked[:k]


def fmt(x):
    return 'n/a' if x is None else f'{x:.0f}'


def report(path, reps_asked, asked):
    orders = realized_orders(path)
    if not orders:
        order_state = 'NOT INTERLEAVED'
    elif sorted(orders) != list(range(1, reps_asked + 1)) or any(
            orders[r] != expected_order(asked, r) for r in orders):
        order_state = 'NOT ROTATED'
    else:
        order_state = 'rotated'
    print(f'  order ({order_state}): ' + ' | '.join(f'rep {r}: {" ".join(orders[r])}' for r in sorted(orders)))
    reps, orphans = s6_reps.read_reps(path)
    short = False
    for fw in asked:
        on = [r for r in reps if r['fw'] == fw and r['arm'] == 'ON']
        per = []
        for r in on:
            c, s = r['client'], r['server']
            if c.get('scenario') == 'reliable_latency':
                per.append(f"measured={fmt(s6_reps.num(c, 'measured'))} crt_kept={fmt(s6_reps.num(c, 'crt_kept'))}")
            else:
                per.append(f"sent={fmt(s6_reps.num(c, 'sent'))} recv={fmt(s6_reps.num(s, 'recv'))}")
        if len(on) < reps_asked:
            short = True
        print(f'  samples {fw}: {len(on)}/{reps_asked} ON reps recorded' + (': ' + '; '.join(per) if per else ''))
    print(f'  orphan lines: {orphans}')
    print(f'CELL {os.path.basename(path)} order={order_state} reps={"SHORT" if short else "ok"}')
    return 0 if order_state == 'rotated' and not short else 1


def selftest():
    import tempfile
    ok = True
    lines = []
    for r, order in ((1, 'a b'), (2, 'b a')):
        for k, fw in enumerate(order.split(), 1):
            lines.append(f'--- rep {r}/2 order {k}/2 framework={fw} x')
            lines.append(f'arm=ON RESULT: framework={fw} scenario=reliable_throughput sent=10 role=client')
            lines.append(f'arm=ON RESULT: framework={fw} scenario=reliable_throughput recv=10 role=server')
    for name, text, reps, want in (
            ('rotated', lines, 2, 0),
            ('short', lines, 3, 1),
            ('not rotated', [x.replace('order 1/2 framework=b', 'order 1/2 framework=a') for x in lines], 2, 1),
            ('not interleaved', [x for x in lines if not x.startswith('---')], 2, 1)):
        with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as f:
            f.write('\n'.join(text) + '\n')
        got = report(f.name, reps, ['a', 'b'])
        os.unlink(f.name)
        print(f'selftest {name}: exit {got}, want {want}')
        ok = ok and got == want
    print(f'selftest: {"PASS" if ok else "FAIL"}')
    return 0 if ok else 1


if __name__ == '__main__':
    if sys.argv[1:] == ['--selftest']:
        sys.exit(selftest())
    if len(sys.argv) != 4:
        print(__doc__.splitlines()[2], file=sys.stderr)
        sys.exit(2)
    sys.exit(report(sys.argv[1], int(sys.argv[2]), sys.argv[3].split()))
