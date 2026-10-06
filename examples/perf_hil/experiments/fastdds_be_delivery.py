#!/usr/bin/env python3
"""The reading rules of fastdds_be_delivery.sh, in code, written before any run of it (2026-10-07).

QUESTION. In the same-host re-measure of 2026-10-06 (fair_samehost_9f919c8b_20261006-204050, best_effort_throughput
p2-p4, -K 1 on both sides) Fast DDS's BEST_EFFORT reader received ~200 of ~5.5M samples while its writer sent at its
maximum rate - in BOTH arms. Is that Fast DDS or our bench?

HYPOTHESIS (to be tested, not assumed). Data-sharing is a DataWriter/DataReader QoS, not a transport, so the
eth0-only profile did not remove it: that run's "OFF" arm had a dds.dsha thread in its server and 0 packets per
sample on lo, exactly like ON. With data-sharing the reader takes the payload out of the WRITER's pool, whose size in
2.14.6 is history depth + extra_samples (KEEP_LAST 1: 1 + 1 = 2 payloads). A writer at ~600k samples/s reuses each
payload every few microseconds; ReaderPool discards a payload overwritten while it was read ("Dirty data"), skips
ahead when the writer laps it ("overtook reader"), and ReadTakeCommand drops at take() a change whose payload's
sequence number changed meanwhile ("is overidden"). So nearly nothing survives to take().

ARMS (each Fast DDS arm is one s6_transport_cells.sh call; the arm read is named, the other one is printed as extra):
  F1   KEEP_LAST 1,  data-sharing on (shm_and_eth0 profile)          - the 2026-10-06 configuration, replicated
  F2   KEEP_LAST 64, data-sharing on                                  - a 65-payload pool
  F3   KEEP_LAST 1,  data-sharing OFF (BENCH_FASTDDS_NO_DATASHARING), eth0-only profile: UDP over loopback
  F4   KEEP_LAST 1,  data-sharing on, publisher slowed by -i         - must send at <= 20% of F1's rate to count
  F1w  F1 with the server counting Fast DDS's own discard warnings (BENCH_FASTDDS_COUNT_WARNINGS=1) - the witness
  T    TickLE p3 same-host ON arm, unchanged                          - the control nothing here can touch

READING RULES (d = median delivered ratio = server win_recv_mbps / client win_send_mbps; R = median win_recv_mbps):
  validity, per arm: >= REPS recorded reps with both lines and window=ok; the identity below; else VOID ("could not
    look"), never read as a value.
    identity: transport_profile (F3 ends +nodsh, the others do not); history=keep_last:<depth> on client AND server;
    fdds_warn=on only on F1w; the server's own threads say which path ran - a dds.dsha.* thread on every
    data-sharing arm and none on F3 - and on F3 the client's loopback counter must show the data
    (wire_packets_per_sample >= 1.5), i.e. the kernel carried it.
  T: d >= 0.99 and R within 15% of 14303 Mbps (2026-10-06's p3 free) -> the rig is as it was; else the campaign is
    printed but labelled RIG CHANGED and nothing is compared to 2026-10-06.
  F1: d <= 0.01 -> REPRODUCED; otherwise NOT REPRODUCED and no hypothesis reading is made (the numbers are printed).
  H, read only when F1 reproduced:
    F3 >> F1 (R_F3 >= 100 R_F1 and d_F3 >= 0.10) and F2 >> F1 (same test) -> SUPPORTED: the loss belongs to
      data-sharing and goes away with a deeper writer pool.
    F3 >> F1 but F2 not -> data-sharing-specific but NOT pool depth: H's mechanism REFUTED.
    F3 ~ F1 (R_F3 <= 3 R_F1) -> REFUTED: the loss is not data-sharing's; suspect the bench's reader / the CPU.
    anything else -> UNDECIDED, numbers printed.
  F4 (treatment applied only if its realized send rate <= 0.20 x F1's - else VOID, the arm did not slow down):
    d_F4 >= 0.90 -> rate-dependent overrun: at a rate the reader follows, KEEP_LAST 1 data-sharing delivers (with H).
    d_F4 <= 0.10 -> loses even when slowed: not an overrun (against H; suspect matching or the bench).
  F1w (only with fdds_warn_total > 0 - a counter that never fired cannot say "no discards"; and its R must stay
    within 10x of F1's, or the witness changed the run and is labelled PERTURBED):
    overridden + dirty + overtook >= 0.01 x our lost -> Fast DDS's own log says it discarded reused payloads: the
      mechanism is CONFIRMED in its own words (which of the three dominates is printed).
    all three 0 -> the discards are not data-sharing reuse: mechanism REFUTED.
  fdds_sample_lost (every arm): Fast DDS's SAMPLE_LOST counts gaps among samples the reader was NOTIFIED of. Printed
    beside our lost=; it is evidence of where the loss happened, not a verdict.

Usage:  fastdds_be_delivery.py <OUTB>          read $OUTB.{T,F1,F2,F3,F4,F1w}.txt and print the verdicts
        fastdds_be_delivery.py --dry <file>    exit 0 only if the dry run's one rep shows everything the campaign needs
"""
import os
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import s6_reps  # noqa: E402  (shares the line parser with s6_transport_cells.sh)

T_REF_MBPS = 14303.0  # TickLE p3 free ON, 2026-10-06 (fair_samehost_9f919c8b_20261006-204050)

# arm -> (file tag, framework, s6 arm, depth, profile, nodsh, warn, data_sharing)
ARMS = {
    'F1': ('F1', 'fastdds', 'ON', 1, 'fastdds_shm_and_eth0.xml', False, False, True),
    'F2': ('F2', 'fastdds', 'ON', 64, 'fastdds_shm_and_eth0.xml', False, False, True),
    'F3': ('F3', 'fastdds', 'OFF', 1, 'fastdds_eth0_only.xml+nodsh', True, False, False),
    'F4': ('F4', 'fastdds', 'ON', 1, 'fastdds_shm_and_eth0.xml', False, False, True),
    'F1w': ('F1w', 'fastdds', 'ON', 1, 'fastdds_shm_and_eth0.xml', False, True, True),
}
# The other arm of each call: printed, never read by a rule.
EXTRAS = {'F1': 'OFF', 'F2': 'OFF', 'F3': 'ON', 'F4': 'OFF', 'F1w': 'OFF'}


def has_dsha(server):
    return any(t.startswith('dds.dsha.') for t in server.get('sched_by_thread', '').split(','))


def identity(rep, depth, profile, warn, dsha):
    """The reasons this rep ran something other than what the arm asked for; empty if none."""
    c, s = rep['client'], rep['server']
    why = []
    if s is None:
        return ['no server RESULT line']
    for role, f in (('client', c), ('server', s)):
        if f.get('transport_profile') != profile:
            why.append('%s transport_profile=%s, wanted %s' % (role, f.get('transport_profile'), profile))
        if f.get('history') != 'keep_last:%d' % depth:
            why.append('%s history=%s, wanted keep_last:%d' % (role, f.get('history'), depth))
        if not s6_reps.window_ok(f):
            why.append('%s window=%s' % (role, f.get('window')))
    if s.get('fdds_warn') != ('on' if warn else 'off'):
        why.append('server fdds_warn=%s, wanted %s' % (s.get('fdds_warn'), 'on' if warn else 'off'))
    if 'fdds_sample_lost' not in s:
        why.append('server prints no fdds_sample_lost (an old server binary?)')
    if has_dsha(s) != dsha:
        why.append('server %s a dds.dsha thread, so data-sharing was %s' %
                   ('has' if has_dsha(s) else 'has no', 'on' if has_dsha(s) else 'off'))
    if not dsha and (s6_reps.num(c, 'wire_packets_per_sample') or 0.0) < 1.5:
        why.append('client wire_packets_per_sample=%s < 1.5: the kernel did not carry the data' %
                   c.get('wire_packets_per_sample'))
    return why


def med(values):
    values = [v for v in values if v is not None]
    return st.median(values) if values else None


def arm_figures(path, fw, arm, check):
    """(rows, refused): rows of per-rep figures that passed `check`, and the refusal reasons of the others."""
    if not os.path.exists(path):
        return [], ['%s does not exist' % path]
    reps, orphans = s6_reps.read_reps(path)
    rows, refused = [], []
    for rep in reps:
        if rep['fw'] != fw or rep['arm'] != arm:
            continue
        why = check(rep) if check else []
        if why:
            refused.append('; '.join(why))
            continue
        fig = s6_reps.figures(rep, 'best_effort_throughput')
        s = rep['server'] or {}
        fig['sent'] = s6_reps.num(rep['client'], 'sent')
        fig['recv'] = s6_reps.num(s, 'recv')
        fig['lost'] = s6_reps.num(s, 'lost')
        for k in ('fdds_sample_lost', 'fdds_warn_overridden', 'fdds_warn_dirty', 'fdds_warn_overtook',
                  'fdds_warn_total'):
            fig[k] = s6_reps.num(s, k)
        rows.append(fig)
    if orphans:
        refused.append('%d orphan line(s) in %s' % (orphans, path))
    return rows, refused


def summary(rows):
    keys = ('send_mbps', 'recv_mbps', 'delivered_ratio', 'sent', 'recv', 'lost', 'fdds_sample_lost',
            'fdds_warn_overridden', 'fdds_warn_dirty', 'fdds_warn_overtook', 'fdds_warn_total')
    return {k: med([r.get(k) for r in rows]) for k in keys}


def fmt(v, digits=3):
    if v is None:
        return 'n/a'
    if isinstance(v, float) and not v.is_integer():
        return '%.*f' % (digits, v)
    return '%d' % v


def show(name, m, n, extra=''):
    print('  %-4s n=%d send_mbps=%s recv_mbps=%s delivered=%s sent=%s recv=%s lost=%s fdds_sample_lost=%s%s%s' % (
        name, n, fmt(m['send_mbps'], 1), fmt(m['recv_mbps'], 3), fmt(m['delivered_ratio'], 6), fmt(m['sent']),
        fmt(m['recv']), fmt(m['lost']), fmt(m['fdds_sample_lost']),
        (' warn: overridden=%s dirty=%s overtook=%s total=%s' % (
            fmt(m['fdds_warn_overridden']), fmt(m['fdds_warn_dirty']), fmt(m['fdds_warn_overtook']),
            fmt(m['fdds_warn_total']))) if m['fdds_warn_total'] is not None else '', extra))


def much_more(a, b):
    """a >> b, on the delivered rate and ratio (rule H)."""
    return (a['recv_mbps'] is not None and b['recv_mbps'] is not None and a['delivered_ratio'] is not None and
            a['recv_mbps'] >= 100.0 * max(b['recv_mbps'], 1e-6) and a['delivered_ratio'] >= 0.10)


def dry(path, reps_needed=1):
    _, fw, arm, depth, prof, _, warn, dsha = ARMS['F1w']
    rows, refused = arm_figures(path, fw, arm, lambda r: identity(r, depth, prof, warn, dsha))
    for why in refused:
        print('  DRY refused a rep: %s' % why)
    if len(rows) < reps_needed:
        print('DRY RUN FAILED: %d usable rep(s) of F1w, need %d. The campaign does not start.' % (len(rows), reps_needed))
        return 1
    m = summary(rows)
    show('dry', m, len(rows))
    if not m['fdds_warn_total']:
        print('DRY RUN FAILED: fdds_warn_total is 0 - the witness counter never fired, so it cannot say anything.')
        return 1
    print('DRY RUN OK: build, data-sharing witness, warning counter and identity fields all present.')
    return 0


def main(outb, reps_needed):
    m, n, valid = {}, {}, {}
    print('=== fastdds_be_delivery verdicts, %s ===' % outb)
    # The control first.
    rows, refused = arm_figures(outb + '.T.txt', 'tickle', 'ON',
                                lambda r: ([] if r['server'] is not None else ['no server line']))
    for why in refused:
        print('  T refused: %s' % why)
    valid['T'] = len(rows) >= reps_needed
    m['T'], n['T'] = summary(rows), len(rows)
    show('T', m['T'], n['T'])
    rig_same = False
    if not valid['T']:
        print('  T VOID: %d usable reps < %d. Nothing below is compared to 2026-10-06.' % (n['T'], reps_needed))
    else:
        d, r = m['T']['delivered_ratio'], m['T']['recv_mbps']
        rig_same = d is not None and d >= 0.99 and r is not None and abs(r - T_REF_MBPS) <= 0.15 * T_REF_MBPS
        print('  T: %s (delivered %s, %s Mbps vs %s on 2026-10-06)' % (
            'rig as on 2026-10-06' if rig_same else 'RIG CHANGED - read within this campaign only', fmt(d, 4),
            fmt(r, 0), fmt(T_REF_MBPS, 0)))

    for name, (tag, fw, arm, depth, prof, _, warn, dsha) in ARMS.items():
        path = '%s.%s.txt' % (outb, tag)
        rows, refused = arm_figures(path, fw, arm, lambda r, d=depth, p=prof, w=warn, s=dsha: identity(r, d, p, w, s))
        for why in refused:
            print('  %s refused a rep: %s' % (name, why))
        m[name], n[name] = summary(rows), len(rows)
        valid[name] = n[name] >= reps_needed
        show(name, m[name], n[name], '' if valid[name] else '   <- VOID (%d < %d usable reps)' % (n[name], reps_needed))
        xrows, _ = arm_figures(path, fw, EXTRAS[name], None)
        if xrows:
            show('  +' + EXTRAS[name], summary(xrows), len(xrows), '   (the call\'s other arm; no rule reads it)')

    print('--- verdicts')
    if not valid['F1']:
        print('  F1 VOID: nothing is read.')
        return 1
    d1 = m['F1']['delivered_ratio']
    if d1 is None or d1 > 0.01:
        print('  F1 NOT REPRODUCED (delivered %s > 0.01): the 2026-10-06 result did not recur; no hypothesis reading.'
              % fmt(d1, 4))
        return 0
    print('  F1 REPRODUCED: delivered %s at KEEP_LAST 1 with data-sharing.' % fmt(d1, 6))

    if valid['F2'] and valid['F3']:
        f3_more, f2_more = much_more(m['F3'], m['F1']), much_more(m['F2'], m['F1'])
        f3_same = m['F3']['recv_mbps'] is not None and m['F3']['recv_mbps'] <= 3.0 * max(m['F1']['recv_mbps'], 1e-6)
        if f3_more and f2_more:
            print('  H SUPPORTED: without data-sharing (F3) and with a 65-payload pool (F2) the reader receives;'
                  ' the loss belongs to data-sharing\'s writer pool at depth 1.')
        elif f3_more:
            print('  H MECHANISM REFUTED: the loss is data-sharing\'s (F3 >> F1) but not the pool depth (F2 !>> F1).')
        elif f3_same:
            print('  H REFUTED: without data-sharing the reader still receives ~nothing (F3 ~ F1);'
                  ' suspect the bench\'s reader loop or the CPU, not data-sharing.')
        else:
            print('  H UNDECIDED: F3/F1 and F2/F1 fall between the pre-registered bands (numbers above).')
    else:
        print('  H not read: F2 or F3 VOID.')

    if valid['F4']:
        s1, s4 = m['F1']['send_mbps'], m['F4']['send_mbps']
        if s1 is None or s4 is None or s4 > 0.20 * s1:
            print('  F4 VOID: realized send %s Mbps is not <= 20%% of F1\'s %s - the treatment did not apply.' % (
                fmt(s4, 1), fmt(s1, 1)))
        else:
            d4 = m['F4']['delivered_ratio']
            if d4 is not None and d4 >= 0.90:
                print('  F4: at %s Mbps (%.1f%% of F1) delivered %s - a rate-dependent overrun, consistent with H.'
                      % (fmt(s4, 1), 100.0 * s4 / s1, fmt(d4, 4)))
            elif d4 is not None and d4 <= 0.10:
                print('  F4: slowed to %.1f%% of F1 and still delivered %s - not an overrun; against H.'
                      % (100.0 * s4 / s1, fmt(d4, 4)))
            else:
                print('  F4: delivered %s at %.1f%% of F1\'s rate - between the bands.' % (fmt(d4, 4), 100.0 * s4 / s1))
    else:
        print('  F4 not read: VOID.')

    if valid['F1w']:
        w = m['F1w']
        if not w['fdds_warn_total']:
            print('  F1w VOID: the warning counter never fired (total 0), so its zeros say nothing.')
        else:
            perturbed = (w['recv_mbps'] is not None and m['F1']['recv_mbps'] is not None and
                         w['recv_mbps'] > 10.0 * max(m['F1']['recv_mbps'], 1e-6))
            disc = sum(w[k] or 0 for k in ('fdds_warn_overridden', 'fdds_warn_dirty', 'fdds_warn_overtook'))
            lost = w['lost'] or 0
            label = ' (PERTURBED: the witness raised delivery >10x; read with care)' if perturbed else ''
            if disc == 0:
                print('  F1w MECHANISM REFUTED: Fast DDS logged no data-sharing discard while %s were lost%s.'
                      % (fmt(lost), label))
            elif disc >= 0.01 * lost:
                top = max(('overridden', 'dirty', 'overtook'), key=lambda k: w['fdds_warn_' + k] or 0)
                print('  F1w MECHANISM CONFIRMED in Fast DDS\'s own log: %s discards (most: %s) against %s lost%s.'
                      % (fmt(disc), top, fmt(lost), label))
            else:
                print('  F1w: %s discards against %s lost - present but below 1%%; between the bands%s.'
                      % (fmt(disc), fmt(lost), label))
    else:
        print('  F1w not read: VOID.')
    return 0


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--dry':
        sys.exit(dry(sys.argv[2]))
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], int(os.environ.get('REPS', '3'))))
