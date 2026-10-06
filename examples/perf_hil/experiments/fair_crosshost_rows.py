#!/usr/bin/env python3
"""RESULTS section 2's A/T rows, read from fair_crosshost_remeasure.sh's campaigns (2026-10-06).

Usage: fair_crosshost_rows.py <A.txt> <T.txt> [<T6.txt>]

Each row is one (campaign, cell, metric) of campaign_summary.py's output, which already applies the campaign's reading
rules (instrument=ok, boundary gate, VOID, win only outside the reps' spread). This only picks the rows the table
shows, in the table's column order (TickLE, FastDDS, CycloneDDS), and prints the summary's own verdict beside each.
Rows 16-17 (retention) are the loss cell's send rate over the unshaped cell's, per framework; row 41 is row 35 less
the 76-byte payload.
"""
import re
import subprocess
import sys

HERE = __file__.rsplit('/', 1)[0]

ROWS = [
    ('1', 'A', 'c10', 'client.rtt_avg_ms'), ('2', 'A', 'c10', 'client.rtt_max_ms'),
    ('3', 'A', 'c11', 'client.rtt_avg_ms'), ('4', 'A', 'c11', 'client.rtt_max_ms'),
    ('5', 'A', 'c12', 'client.rtt_avg_ms'), ('6', 'A', 'c12', 'client.rtt_max_ms'),
    ('6a', 'A', 'c16', 'client.rtt_avg_ms'), ('6b', 'A', 'c17', 'client.rtt_avg_ms'),
    ('7', 'A', 'c1', 'client.send_mbps'), ('8', 'A', 'c2', 'client.send_mbps'),
    ('9', 'T', 'c3', 'client.send_mbps'), ('10', 'T', 'c4', 'client.send_mbps'),
    ('11', 'A', 'c8', 'client.send_mbps'), ('11a', 'A', 'c13', 'client.send_mbps'),
    ('11b', 'A', 'c14', 'client.send_mbps'), ('11c', 'A', 'c15', 'client.send_mbps'),
    ('12', 'A', 'c9', 'client.send_mbps'), ('13', 'A', 'c5', 'client.send_mbps'),
    ('14', 'T', 'c6', 'client.send_mbps'), ('15', 'A', 'c7', 'client.send_mbps'),
    ('18', 'A', 'c1', 'client.cpu_s_per_Msample'), ('19', 'A', 'c1', 'server.cpu_s_per_Msample'),
    ('20', 'A', 'c2', 'client.cpu_s_per_Msample'), ('21', 'A', 'c2', 'server.cpu_s_per_Msample'),
    ('22', 'T', 'c4', 'client.cpu_s_per_Msample'), ('23', 'T', 'c4', 'server.cpu_s_per_Msample'),
    ('24', 'A', 'c10', 'client.cpu_s_per_Msample'), ('25', 'A', 'c11', 'client.cpu_s_per_Msample'),
    ('26', 'A', 'c5', 'client.cpu_s_per_Msample'), ('27', 'T', 'c6', 'client.cpu_s_per_Msample'),
    ('28', 'A', 'c1', 'client.peak_rss_kb'), ('29', 'A', 'c2', 'client.peak_rss_kb'),
    ('30', 'T', 'c4', 'client.peak_rss_kb'), ('31', 'A', 'c10', 'client.peak_rss_kb'),
    ('32', 'A', 'c1', 'server.peak_rss_kb'), ('33', 'T', 'c4', 'server.peak_rss_kb'),
    ('34', 'T', 'c6', 'server.peak_rss_kb'),
    ('35', 'A', 'c1', 'client.wire_bytes_per_sample'), ('36', 'A', 'c2', 'client.wire_bytes_per_sample'),
    ('37', 'T', 'c3', 'client.wire_bytes_per_sample'), ('38', 'T', 'c4', 'client.wire_bytes_per_sample'),
    ('39', 'A', 'c5', 'client.wire_bytes_per_sample'), ('40', 'T', 'c6', 'client.wire_bytes_per_sample'),
]
LINE = re.compile(r'^\s+(\S+)\s+tickle (\S+?)(?:\[\S*\])?\s+cyclon (\S+?)(?:\[\S*\])?\s+fastdd (\S+?)(?:\[\S*\])?\s+(\S+)')


def summary(path):
    out = subprocess.run(['python3', f'{HERE}/campaign_summary.py', path], capture_output=True, text=True).stdout
    cells, cell = {}, None
    for line in out.splitlines():
        head = re.match(r'^== (c\d+) ', line)
        if head:
            cell = head.group(1)
            continue
        m = LINE.match(line)
        if m and cell:
            metric, t, c, f, verdict = m.groups()
            cells[(cell, metric)] = (float(t), float(f), float(c), verdict)
    return cells


def main():
    camps = {'A': summary(sys.argv[1]), 'T': summary(sys.argv[2])}
    if len(sys.argv) > 3:
        camps['T'].update({k: v for k, v in summary(sys.argv[3]).items() if k[0] == 'c6'})
    got = {}
    for row, camp, cell, metric in ROWS:
        v = camps[camp].get((cell, metric))
        got[row] = v
        print(f'{row:>4} {camp} {cell:>3} {metric:28} ' + ('MISSING' if v is None else
              f'tickle {v[0]:g}  fastdds {v[1]:g}  cyclonedds {v[2]:g}  {v[3]}'))
    for row, loss, base in (('16', '13', '7'), ('17', '14', '10')):
        if got.get(loss) and got.get(base):
            r = [100.0 * got[loss][i] / got[base][i] for i in range(3)]
            print(f'{row:>4} retention %  tickle {r[0]:.1f}  fastdds {r[1]:.1f}  cyclonedds {r[2]:.1f}')
    if got.get('35'):
        print(f'  41 framing    tickle {got["35"][0] - 76:g}  fastdds {got["35"][1] - 76:g}  cyclonedds {got["35"][2] - 76:g}')


main()
