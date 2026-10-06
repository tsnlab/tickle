"""One same-host repetition per record, from s6_transport_cells.sh's output: what each process cost and what arrived.

Shared by s6_transport_cells.sh's verdict block and fair_samehost_summary.py, so the two cannot read the same file two
ways. Lines read (leading spaces allowed: the tickle cell's lines are copied into the output indented):

  arm=<A> RESULT: framework=<fw> ... role=client ...    the client's own line (a latency client may carry no role=)
  arm=<A> RESULT: framework=<fw> ... role=server ...    the server's own line, printed when it is stopped with SIGINT
  arm=<A> PROC: framework=<fw> role=server state=...    the server's CPU and memory read from /proc (proc_snap.sh)
  arm=<A> ROUDI: framework=cyclonedds role=roudi ...    iox-roudi's, over the repetition (CycloneDDS's ON arm only)

A client line opens a repetition; the other three attach to the latest repetition of the same framework and arm that
does not have one yet, and a line with nowhere to go is counted as an orphan rather than attached to the wrong one.

WHAT EACH FIGURE IS, decided before any run that carries them (2026-10-06):
  client_us          the client's cpu_s_per_Msample: its whole-process CPU over the samples it SENT (latency: pings)
  server_us          the server's CPU over the samples it RECEIVED (latency: the client's pings): its own
                     utime_s+stime_s where its RESULT line has them (every throughput server), else the /proc reading
                     (the latency servers): the larger of its tick count and its schedstat sum, both lower bounds
  roudi_us           iox-roudi's CPU over the repetition (its schedstat difference), over the samples DELIVERED
  total_us           (client + server + roudi CPU) over the samples DELIVERED - the cost of a sample that arrived
  *_rss_mb           client peak_rss_kb, server VmHWM, roudi VmHWM, in MB; sum_rss_mb adds the three and is an upper
                     bound, because roudi's shared memory is counted again by each client that maps it (roudi_shm_mb)
  send_mbps          the client's win_send_mbps (send_mbps on a line from before the window)
  recv_mbps          the server's win_recv_mbps - the DELIVERED rate; delivered_ratio = recv_mbps / send_mbps
A figure whose input is missing is None, and prints as n/a: never as 0.
"""
import re
import statistics as st

LINE = re.compile(r'^\s*arm=(\S+) (RESULT|PROC|ROUDI): ?(.*)$')


def fields(text):
    return dict(kv.split('=', 1) for kv in text.split() if '=' in kv)


def num(f, key):
    if f is None or key not in f:
        return None
    try:
        return float(f[key])
    except ValueError:
        return None


def read_reps(path):
    """Returns (reps, orphans): reps a list of dicts with keys fw, arm, client, server, proc, roudi."""
    reps, latest, orphans = [], {}, 0
    for line in open(path, errors='replace'):
        m = LINE.match(line)
        if not m:
            continue
        arm, kind, rest = m.groups()
        f = fields(rest)
        fw = f.get('framework')
        if fw is None:
            continue
        if kind == 'RESULT' and f.get('role', 'client') == 'client':
            rep = {'fw': fw, 'arm': arm, 'client': f, 'server': None, 'proc': None, 'roudi': None}
            reps.append(rep)
            latest[(fw, arm)] = rep
            continue
        slot = {'RESULT': 'server', 'PROC': 'proc', 'ROUDI': 'roudi'}[kind]
        rep = latest.get((fw, arm))
        if rep is None or rep[slot] is not None:
            orphans += 1
            continue
        rep[slot] = f
    return reps, orphans


def window_ok(f):
    return f is not None and f.get('window', 'ok') == 'ok'


def figures(rep, scen):
    """The figures above for one repetition, as a dict; any of them may be None."""
    c, s, p, r = rep['client'], rep['server'], rep['proc'], rep['roudi']
    out = {}
    latency = scen == 'reliable_latency'
    sent = num(c, 'sent')
    delivered = num(c, 'recv') if latency else num(s, 'recv')
    served = sent if latency else delivered
    c_cpu = None
    if num(c, 'utime_s') is not None and num(c, 'stime_s') is not None:
        c_cpu = num(c, 'utime_s') + num(c, 'stime_s')
    # The server's own getrusage when it printed one (microseconds, every thread - the client's instrument too), else
    # the /proc reading (every thread, but whole clock ticks: 10 ms on the Pis, a few percent of a latency server).
    s_cpu = None
    if num(s, 'utime_s') is not None and num(s, 'stime_s') is not None:
        s_cpu = num(s, 'utime_s') + num(s, 'stime_s')
    elif p is not None and p.get('state') == 'ok':
        # Two readings, each of which can only undercount: whole ticks are truncated, and sched_cpu_s misses threads
        # that exited. The larger is the tighter bound (smoke run 2026-10-06: a CycloneDDS latency server read 0.040 s
        # in ticks and 0.048 s by schedstat).
        s_cpu = max(x for x in (num(p, 'cpu_s'), num(p, 'sched_cpu_s'), 0.0) if x is not None)
    # The daemon's threads live as long as it does, so its schedstat difference is complete and fine-grained; a tick
    # difference over a repetition is 0 or 10 ms for the ~5 ms it spends (same smoke run). threads= on its PROC
    # readings would show a thread coming or going.
    r_cpu = None
    if r is not None and r.get('state') == 'ok':
        r_cpu = num(r, 'sched_cpu_s') if num(r, 'sched_cpu_s') is not None else num(r, 'cpu_s')
    out['client_us'] = num(c, 'cpu_s_per_Msample')
    out['server_us'] = s_cpu / served * 1e6 if s_cpu is not None and served else None
    out['roudi_us'] = r_cpu / delivered * 1e6 if r_cpu is not None and delivered else None
    parts = [c_cpu, s_cpu] + ([r_cpu] if rep['fw'] == 'cyclonedds' and rep['arm'] == 'ON' else [])
    out['total_us'] = sum(parts) / delivered * 1e6 if all(x is not None for x in parts) and delivered else None
    kb = 1024.0
    out['client_rss_mb'] = num(c, 'peak_rss_kb') / kb if num(c, 'peak_rss_kb') is not None else None
    s_rss = num(p, 'vmhwm_kb') if p is not None and p.get('state') == 'ok' else num(s, 'peak_rss_kb')
    out['server_rss_mb'] = s_rss / kb if s_rss is not None else None
    r_rss = num(r, 'vmhwm_kb') if r is not None and r.get('state') == 'ok' else None
    out['roudi_rss_mb'] = r_rss / kb if r_rss is not None else None
    r_shm = num(r, 'rssshmem_kb') if r is not None and r.get('state') == 'ok' else None
    out['roudi_shm_mb'] = r_shm / kb if r_shm is not None else None
    rss = [out['client_rss_mb'], out['server_rss_mb']] + (
        [out['roudi_rss_mb']] if rep['fw'] == 'cyclonedds' and rep['arm'] == 'ON' else [])
    out['sum_rss_mb'] = sum(rss) if all(x is not None for x in rss) else None
    if not latency:
        send = num(c, 'win_send_mbps') if 'win_send_mbps' in c else num(c, 'send_mbps')
        out['send_mbps'] = send if window_ok(c) else None
        out['recv_mbps'] = num(s, 'win_recv_mbps') if window_ok(s) and s is not None else None
        out['delivered_ratio'] = (out['recv_mbps'] / out['send_mbps']
                                  if out['recv_mbps'] is not None and out['send_mbps'] else None)
        if 'history_arg' in c:  # BEST_EFFORT lines since BenchHistory.h; nothing else prints one
            out['history'] = (c['history_arg'] + ' ' + c.get('history', '-') + '/' +
                              (s.get('history', '-') if s is not None else '-'))
    return out


def median(values):
    v = [x for x in values if x is not None]
    return (st.median(v), len(v)) if v else (None, 0)


def fmt(value, digits=3):
    return 'n/a' if value is None else f'{value:.{digits}f}'


COST_KEYS = ('client_us', 'server_us', 'roudi_us', 'total_us', 'client_rss_mb', 'server_rss_mb', 'roudi_rss_mb',
             'roudi_shm_mb', 'sum_rss_mb')
RATE_KEYS = ('send_mbps', 'recv_mbps', 'delivered_ratio')


def summarise(rep_figures, scen):
    """Median and count of every figure over a list of figures() dicts, as printable 'key=value(n)' strings."""
    keys = COST_KEYS + (() if scen == 'reliable_latency' else RATE_KEYS)
    shown = []
    for k in keys:
        value, n = median([f.get(k) for f in rep_figures])
        if k.startswith('roudi') and n == 0:
            continue
        digits = 0 if k.endswith('mbps') else 3 if k == 'delivered_ratio' or k.endswith('_us') else 1
        shown.append(f'{k}={fmt(value, digits)}' + ('' if n == len(rep_figures) else f'(n={n})'))
    return shown
