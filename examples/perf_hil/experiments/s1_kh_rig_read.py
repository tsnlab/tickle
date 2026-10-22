#!/usr/bin/env python3
"""s1_kh_rig_read.py - the reading rules of rig_queue_s1_kh.sh, written 2026-10-22 before any of its runs.

Two modes, one per part of the queue. Nothing here computes a statistic the existing readers already compute: the S1
rows' verdicts (WORSE / better / held: Bonferroni Student t AND the placement floor |P - A|) are gate_v2.py's, as
campaign_ab_chain.sh GATE=v2 wrote them into its compare file, and the KH numbers are rmw_pcap_split.py's wake summary.
This file decides what those verdicts say about the two questions asked.

--s1  STAGE 1 / S1, THE CPU-TIME HALF ON THE RIG (ROADMAP "Wired work"; the PC half is stage1_payg.sh, d131d82e,
      whose CPU time was unresolvable on the PC: load 14 on 16 cores, so its amendments moved to instructions).
  The claim (the user's decision 6, stage1_payg.sh's UNUSED): a default build whose peers are all on other hosts costs
  no CPU time against the build with the shared-memory module compiled out. On the rig the two processes are on two
  Pis, so every datagram crosses eth0 and the segment cannot be found - the real "on but unused" case.
  Arms (one commit, flags only): A = alloff (-Dtt_SEGMENT_ENABLED=0 -Dtt_SAMPLE_LENDING=0) on the layout-stable build,
  P = alloff on the placement build (A's floor), B = on (the default build) on the layout-stable build. Cells 1-4 =
  RELIABLE KEEP_ALL throughput p1-p4, stage1_payg.sh's four shapes with its KEEP_ALL bounds.
  VOID (nothing is judged; exit 3):
    S1V0 core_ab_read.read() says VOID: compare or done file missing or empty, the chain did not end done (its own
         VOID / REFUSED: flags not echoed or not verified per Pi, no INSN_COUNT=1), gate v2 NO VERDICT, k not equal to
         the rows parsed, a cell with no CPU witness row.
    S1V1 SAMPLES FIRST: a cell's client or server cpu_s_per_Msample row with n < 5 in A or B, or missing.
    S1V2 THE TREATMENT, from the arm files (every ok TickLE row of cells 1-4): tx_shm, rx_shm and shm_attach_attempts
         present on every line; tx_shm == rx_shm == 0 everywhere (a sample that did not cross the link makes the run
         not cross-host shaped); A and P lines shm_attach_attempts == 0 (the flag reached the compiler); every B run
         (client + server line of one cell and repetition) shm_attach_attempts > 0 (the on-but-unused path ran); fewer
         than 5 ok client lines of an arm in a cell.
  THE CLAIM (WIRE 8.3: one WORSE cell among many is a candidate, not a finding), on the cpu_s_per_Msample rows, B vs A:
    COST SHOWN       WORSE in at least 2 of the 4 cells for one role (client or server). Refutes "no CPU-time cost".
    CANDIDATE        WORSE in exactly one cell for some role, and in no role at 2 or more.
    NO COST SHOWN    no cpu_s_per_Msample row WORSE.
  Reported, never deciding: send_mbps / recv_mbps rows, the instruction rows (the PC residual was +1.7-1.9% at p4,
  full5), gate v2's own verdict line.
  Exit 0 NO COST SHOWN or CANDIDATE, 1 COST SHOWN, 3 VOID.

--kh  KH RE-MEASURED AFTER THE WAIT RING (769bd13c..71ed68f4): the pong's tap -> receive return, rmw_tickle@libc against
      both vendors, the same run as KH (rmw_holdover_rig.sh part K: bench, both QoS, 3 reps, sysstamp off/on, default
      and libc builds, capture). Reads the new split file and KH's (--prev).
  Per cell (bench best_effort decides; bench reliable is reported, as KH's was NOT READ there):
    KHV  NOT READ: a K0 NOT READ line for rmw_tickle@libc or a vendor, no K1 line, K1 CONTROL FAIL, or a K2
         tap->recv line missing for a vendor (in the new file; and for the comparison with KH, in --prev).
    CLOSED       K2 tap->recv is not SLOWER against either vendor (NOT RESOLVED or FASTER).
    NARROWED     SLOWER against a vendor, and against each vendor the new gap (tickle - vendor medians) is at least
                 1.0 us below KH's gap for that vendor, with that vendor's own tap->recv within 1.0 us of its KH value
                 (the vendors are the control: nothing in them changed, so their move is the rig's).
    NOT COMPARABLE  SLOWER, and a vendor's own tap->recv moved 1.0 us or more since KH (the rig moved; compare deltas,
                 not a bare historical point).
    NOT CLOSED   otherwise.
  Exit 0 when the deciding cell was read (any of the four verdicts), 3 when it is NOT READ.

  s1_kh_rig_read.py --s1 --compare F --done F --arm-a F --arm-b F --arm-p F [--cells "1 2 3 4"]
  s1_kh_rig_read.py --kh NEW.split.txt --prev KH.split.txt
  s1_kh_rig_read.py --selftest
"""
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import core_ab_read  # noqa: E402  (the gate v2 row grammar and the chain's VOID rules, not duplicated here)

MIN_N = 5
CPU = "cpu_s_per_Msample"
N_FIELD = re.compile(r"\(se \S+, n(\d+)\)")
ARM_ROW = re.compile(r"^c(?P<num>\d+)\s+[A-Z]\s+p\d\s+Q\d\s+.+?\s+tickle\s+rep(?P<rep>\d+)\s+\|\s+ok\s+\|\s+(?P<kv>.*)$")
KV = re.compile(r"(\w+)=(\S+)")


def compare_rows(path):
    rows = []
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for raw in fh:
                m = core_ab_read.ROW.match(raw)
                if m:
                    ns = [int(x) for x in N_FIELD.findall(raw)]
                    rows.append(dict(m.groupdict(), n=min(ns) if ns else 0, line=raw.rstrip()))
    except OSError:
        pass
    return rows


def treatment(arms, cells):
    """S1V2: the VOID reasons from the arm files, and a one-line summary per arm."""
    void, summary = [], []
    for arm in ("A", "P", "B"):
        path = arms.get(arm)
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except (OSError, TypeError):
            void.append(f"arm {arm} file {path} not read: the treatment could not be looked at")
            continue
        runs, clients = [], {c: 0 for c in cells}
        bad_fields = shm_moved = 0
        attempts_lines = []
        for line in text.splitlines():
            m = ARM_ROW.match(line)
            if not m or int(m["num"]) not in cells:
                continue
            # One line per run: the client's fields, then " role=server ..." and the server's.
            run = 0
            for part in re.split(r"(?=\brole=)", m["kv"]):
                kv = dict(KV.findall(part))
                if "role" not in kv:
                    continue
                try:
                    tx, rx, att = int(kv["tx_shm"]), int(kv["rx_shm"]), int(kv["shm_attach_attempts"])
                except (KeyError, ValueError):
                    bad_fields += 1
                    continue
                shm_moved += tx + rx
                attempts_lines.append(att)
                run += att
                if kv["role"] == "client":
                    clients[int(m["num"])] += 1
            runs.append((int(m["num"]), int(m["rep"]), run))
        if bad_fields:
            void.append(f"arm {arm}: {bad_fields} role(s) without tx_shm/rx_shm/shm_attach_attempts")
        if shm_moved:
            void.append(f"arm {arm}: tx_shm + rx_shm = {shm_moved}, not 0 - samples did not cross the link")
        if arm in ("A", "P") and any(attempts_lines):
            void.append(f"arm {arm} (module compiled out) made attach attempts by {sum(1 for a in attempts_lines if a)} "
                        "role(s): the flag never reached the compiler")
        if arm == "B":
            idle = [r for r in runs if r[2] == 0]
            if idle:
                void.append(f"arm B (on) made no attach attempt in {len(idle)} run(s), e.g. c{idle[0][0]} rep{idle[0][1]}:"
                            " the on-but-unused path was never exercised")
        short = [c for c, n in clients.items() if n < MIN_N]
        if short:
            void.append(f"arm {arm}: fewer than {MIN_N} ok client lines in cell(s) "
                        + ", ".join(f"c{c} ({clients[c]})" for c in short))
        summary.append(f"  treatment arm {arm}: {sum(clients.values())} ok client lines, {len(runs)} runs, attach "
                       f"attempts per role min {min(attempts_lines, default=0)} max {max(attempts_lines, default=0)}, "
                       f"tx_shm+rx_shm {shm_moved}")
    return void, summary


def read_s1(compare, done, arms, cells):
    verdict, why, lines = core_ab_read.read(compare, done, cells)
    out = [f"core_ab_read (its VOID rules; its PASS/FAIL is not this question's): {verdict} - {why[:200]}"]
    void = [f"S1V0 {why}"] if verdict == "VOID" else []
    rows = compare_rows(compare)
    for c in cells:
        for role in ("client", "server"):
            r = [x for x in rows if x["num"] == str(c) and x["metric"] == f"{role}.{CPU}"]
            if not r:
                void.append(f"S1V1 c{c} has no {role}.{CPU} row")
            elif r[0]["n"] < MIN_N:
                void.append(f"S1V1 c{c} {role}.{CPU} n={r[0]['n']} < {MIN_N}")
    tv, ts = treatment(arms, cells)
    void += [f"S1V2 {v}" for v in tv]
    out += ts
    worse = {"client": [], "server": []}
    for r in rows:
        if r["metric"].endswith("." + CPU) and int(r["num"]) in cells:
            out.append(f"  CPU   {r['line']}")
            if r["verdict"] == "WORSE":
                worse[r["metric"].split(".")[0]].append(int(r["num"]))
    for r in rows:
        name = r["metric"].split(".", 1)[-1]
        if int(r["num"]) in cells and (name.endswith("_mbps") or name.startswith("insn_")):
            out.append(f"  rep.  {r['line']}")
    if void:
        return "VOID", "; ".join(void), out
    half = (len(cells) + 1) // 2
    counts = ", ".join(f"{role} {len(v)}/{len(cells)}" for role, v in worse.items())
    if any(len(v) >= half for v in worse.values()):
        return "COST SHOWN", f"cpu_s_per_Msample WORSE (on vs alloff): {counts}", out
    if any(worse.values()):
        return "CANDIDATE", f"cpu_s_per_Msample WORSE in one cell: {counts}", out
    return "NO COST SHOWN", f"no cpu_s_per_Msample row WORSE (on vs alloff): {counts}", out


K2 = re.compile(r"^\s+K2 (?P<s>rmw_tickle\S*) vs (?P<v>rmw_\S+) pong tap->recv: (?P<x>[\d.]+) vs (?P<y>[\d.]+) us "
                r"\(diff (?P<d>[+-][\d.]+)\) -> (?P<verdict>.+?)\s*$")


def wake_cells(path):
    """{cell: {"k0_bad": [...], "k1": "PASS"|"FAIL"|None, "k2": {vendor: (x, y, diff, verdict)}}} from a split file."""
    cells, cur, inside = {}, None, False
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return None
    for line in text.splitlines():
        if line.startswith("=== kernel wake summary"):
            inside = True
            continue
        if not inside:
            continue
        if line.startswith("=== "):
            break
        if line.startswith("-- "):
            cur = line[3:].strip()
            cells[cur] = {"k0_bad": [], "k1": None, "k2": {}}
            continue
        if cur is None:
            continue
        c = cells[cur]
        m = re.match(r"^\s+K0 (\S+)\s+NOT READ", line)
        if m:
            c["k0_bad"].append(m.group(1))
        m = re.search(r"K1 .*CONTROL (PASS|FAIL)", line)
        if m:
            c["k1"] = m.group(1)
        m = K2.match(line)
        if m:
            c["k2"][m["v"]] = (float(m["x"]), float(m["y"]), float(m["d"]), m["verdict"], m["s"])
    return cells


VENDORS = ("rmw_cyclonedds_cpp", "rmw_fastrtps_cpp")


def kh_cell(new, prev):
    """(verdict, lines) for one cell's dicts (prev may be None)."""
    lines = []
    bad = [r for r in new["k0_bad"] if r == "rmw_tickle@libc" or r in VENDORS]
    if bad:
        return "NOT READ", [f"    K0 NOT READ for {', '.join(bad)}"]
    if new["k1"] != "PASS":
        return "NOT READ", [f"    K1 control {new['k1'] or 'absent'}"]
    if any(v not in new["k2"] for v in VENDORS):
        return "NOT READ", ["    K2 tap->recv missing for a vendor"]
    for v in VENDORS:
        x, y, d, verdict, s = new["k2"][v]
        lines.append(f"    now  {s} {x:.1f} vs {v} {y:.1f} us: gap {d:+.1f} -> {verdict}")
        if prev and v in prev["k2"]:
            px, py, pd, pv, _ = prev["k2"][v]
            lines.append(f"    KH   tickle {px:.1f} vs {py:.1f} us: gap {pd:+.1f} -> {pv}; vendor moved {y - py:+.1f}, "
                         f"tickle moved {x - px:+.1f}, gap moved {d - pd:+.1f} us")
    if not any(new["k2"][v][3] == "SLOWER" for v in VENDORS):
        return "CLOSED", lines
    if not prev or any(v not in prev["k2"] for v in VENDORS):
        return "NOT READ", lines + ["    KH's own K2 rows are missing: no gap to compare against"]
    if any(abs(new["k2"][v][1] - prev["k2"][v][1]) >= 1.0 for v in VENDORS):
        return "NOT COMPARABLE", lines
    if all(new["k2"][v][2] <= prev["k2"][v][2] - 1.0 for v in VENDORS):
        return "NARROWED", lines
    return "NOT CLOSED", lines


def read_kh(new_path, prev_path):
    new, prev = wake_cells(new_path), wake_cells(prev_path)
    if not new:
        return "NOT READ", f"no kernel wake summary in {new_path}", []
    out, verdicts = [], {}
    for cell in sorted(new):
        v, ls = kh_cell(new[cell], (prev or {}).get(cell))
        verdicts[cell] = v
        out.append(f"  {cell}: {v}{'' if cell == 'bench best_effort' else ' (reported)'}")
        out += ls
    dec = verdicts.get("bench best_effort", "NOT READ")
    return dec, f"bench best_effort {dec}; " + ", ".join(f"{k} {v}" for k, v in verdicts.items()), out


def selftest():
    tmp = tempfile.mkdtemp(prefix="s1kh_selftest_")
    ok = True

    def w(name, text):
        p = os.path.join(tmp, name)
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(text)
        return p

    def crow(cell, metric, verdict, n=6, fam="WITNESS"):
        return (f"{fam}   {verdict:22s} c{cell} T p{cell} Q0 none                          {metric:30s} A       5.0000 "
                f"(se 0.0100, n{n})  B       5.1000 (se 0.0100, n{n})   +2.00%  t +7.0 (> 5.00)  floor 0.01 (0.20%)\n")

    def compare(worse_cells, n=6):
        body = ""
        for c in (1, 2, 3, 4):
            for role in ("client", "server"):
                body += crow(c, f"{role}.{CPU}", "WORSE" if (role, c) in worse_cells else "held", n)
                body += crow(c, f"{role}.insn_u_per_sample", "held", n, "PRIMARY")
        k = body.count("\n")
        return (f"--- gate v2: {k // 2} primary (instructions) + {k // 2} witness comparison(s), k = {k}; WORSE needs\n"
                + body + "VERDICT: PASS\n")

    def arm(att, shm=0, reps=6):
        s = ""
        for c in (1, 2, 3, 4):
            for rep in range(1, reps + 1):
                s += (f"c{c}  T p{c} Q0 none                   tickle     rep{(rep - 1) % 3 + 1} | ok | role=client "
                      f"sent=10 tx_shm={shm} rx_shm=0 shm_attach_attempts={att} role=server recv=10 tx_shm=0 rx_shm=0 "
                      f"shm_attach_attempts=0\n")
        return s

    done = w("done", "=== chain done 2026: VERDICT: PASS ===\n")
    good = {"A": w("A", arm(0)), "P": w("P", arm(0)), "B": w("B", arm(7))}
    cases = [
        ("no cost", compare(set()), good, "NO COST SHOWN"),
        ("one cell", compare({("client", 2)}), good, "CANDIDATE"),
        ("two cells", compare({("server", 1), ("server", 3)}), good, "COST SHOWN"),
        ("n too small", compare(set(), n=4), good, "VOID"),
        ("off arm attached", compare(set()), dict(good, A=w("A2", arm(3))), "VOID"),
        ("on arm idle", compare(set()), dict(good, B=w("B2", arm(0))), "VOID"),
        ("samples via shm", compare(set()), dict(good, P=w("P2", arm(0, shm=5))), "VOID"),
        ("too few runs", compare(set()), dict(good, B=w("B3", arm(7, reps=4))), "VOID"),
    ]
    for name, cmp_text, arms, want in cases:
        got, why, _ = read_s1(w("cmp_" + name.replace(" ", "_"), cmp_text), done, arms, [1, 2, 3, 4])
        ok &= got == want
        print(f"selftest s1 {name}: {got} (want {want}){'' if got == want else ' - ' + why}")
    got, _, _ = read_s1(w("cmp_x", compare(set())), w("done_v", "=== chain VOID x ===\n"), good, [1, 2, 3, 4])
    ok &= got == "VOID"
    print(f"selftest s1 chain void: {got} (want VOID)")

    def split(t_be, cy, fa, k1="PASS", k0=""):
        return ("x\n=== kernel wake summary (K0-K3) ===\n-- bench best_effort\n" + k0
                + f"   K1 wire p50 per rmw a 1 us: spread 1.0 us -> CONTROL {k1}\n"
                + f"   K2 rmw_tickle@libc vs rmw_cyclonedds_cpp pong tap->recv: {t_be:.1f} vs {cy:.1f} us (diff "
                  f"{t_be - cy:+.1f}) -> {'SLOWER' if t_be - cy > 1 else 'NOT RESOLVED'}\n"
                + f"   K2 rmw_tickle@libc vs rmw_fastrtps_cpp pong tap->recv: {t_be:.1f} vs {fa:.1f} us (diff "
                  f"{t_be - fa:+.1f}) -> {'SLOWER' if t_be - fa > 1 else 'NOT RESOLVED'}\n")

    prev = w("prev", split(18.0, 12.8, 13.8))
    kcases = [
        ("closed", split(13.5, 12.8, 13.6), "CLOSED"),
        ("narrowed", split(15.5, 12.9, 13.7), "NARROWED"),
        ("not closed", split(17.6, 12.8, 13.8), "NOT CLOSED"),
        ("vendor moved", split(15.5, 11.0, 13.8), "NOT COMPARABLE"),
        ("control failed", split(15.5, 12.8, 13.8, k1="FAIL"), "NOT READ"),
        ("tickle not read", split(15.5, 12.8, 13.8, k0="   K0 rmw_tickle@libc  NOT READ: 0\n"), "NOT READ"),
    ]
    for name, text, want in kcases:
        got, why, _ = read_kh(w("new_" + name.replace(" ", "_"), text), prev)
        ok &= got == want
        print(f"selftest kh {name}: {got} (want {want}){'' if got == want else ' - ' + why}")
    print("SELFTEST OK" if ok else "SELFTEST FAILED")
    return 0 if ok else 1


def main(argv):
    args = argv[1:]
    if args == ["--selftest"]:
        return selftest()
    if args[:1] == ["--kh"] and len(args) == 4 and args[2] == "--prev":
        verdict, why, out = read_kh(args[1], args[3])
        print("\n".join(out))
        print(f"KH VERDICT: {verdict} ({why})")
        return 3 if verdict == "NOT READ" else 0
    if args[:1] == ["--s1"]:
        opt = {"--compare": None, "--done": None, "--arm-a": None, "--arm-b": None, "--arm-p": None,
               "--cells": "1 2 3 4"}
        rest = args[1:]
        while rest:
            if rest[0] not in opt or len(rest) < 2:
                print(__doc__)
                return 2
            opt[rest[0]] = rest[1]
            rest = rest[2:]
        if not opt["--compare"] or not opt["--done"]:
            print(__doc__)
            return 2
        cells = [int(c) for c in opt["--cells"].split()]
        arms = {"A": opt["--arm-a"], "B": opt["--arm-b"], "P": opt["--arm-p"]}
        verdict, why, out = read_s1(opt["--compare"], opt["--done"], arms, cells)
        print("\n".join(out))
        print(f"S1 RIG VERDICT: {verdict} ({why})")
        return {"VOID": 3, "COST SHOWN": 1}.get(verdict, 0)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
