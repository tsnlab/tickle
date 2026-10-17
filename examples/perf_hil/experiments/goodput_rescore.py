#!/usr/bin/env python3
"""Re-score a campaign_sweep.sh result file on GOODPUT, from the raw RESULT lines only (no new measurement).

The user's decision (2026-10-17): throughput is scored as goodput - the payload of the samples the receiving
application got complete and unique, within the same measurement window used today. A partly received large sample
counts 0; duplicates and retransmissions are excluded; the send rate becomes a diagnostic figure; CPU per sample and
wire bytes per sample are divided by DELIVERED samples, not sent ones.

Usage: goodput_rescore.py <campaign.txt> <out_dir>

Writes <out_dir>/<basename> - the same file with the client's scored figures replaced by their goodput forms (the
original send rate kept as diag_*) - and prints the notes and the totals of campaign_summary.py on the ORIGINAL file.

SUPERSEDED 2026-10-17 by campaign_summary.py itself, which now scores goodput natively from the original file with
these same rules (its G1-G6) and refuses a rewritten file (its figures would be rescaled twice). This tool produced the
75d82c42 goodput figures by rewriting the client's send rate and summarising the rewrite with the summary of that
time; the native summary was checked against that reading on the 09a0437d A/T files and the 3ae721aa T6 file: every
scored (cell, metric) has the same value and verdict, the rewrite's client.send_mbps being the native
server.goodput_mbps and its duplicate server.recv_mbps no longer scored. It is kept to reproduce that rewrite.

Why every receive-side count is goodput as it stands (read from the bench sources at 2026-10-17, not assumed):
every server (tickle/cyclonedds/fastdds, reliable_ and best_effort_throughput) counts recv= and BenchWindow_add()s once
per sample its application took with valid data. A large sample reaches the application only reassembled (TickLE's
frag_reassembled, the DDS readers' own fragment reassembly), so a partial sample is never counted. A RELIABLE reader
hands each sequence number to the application once; a BEST_EFFORT duplicate would be counted, which rule G2 bounds.

READING RULES (enforced below, not only stated):
  G1 Goodput needs the receiver's count. A throughput row whose server chunk has no recv= is VOID for goodput
     ("cannot re-score from existing data"), never estimated from the send rate.
  G2 Uniqueness: a row whose server recv= exceeds the client's sent= has counted something twice; it is VOID.
     (A shortfall is loss, which goodput already prices at zero.)
  G3 The window: goodput is the server's own windowed rate (win_recv_mbps), which BenchWindow takes by the sender's
     send_ns with the same edges as the client's win_send_mbps. If the two windows' lengths differ by more than one
     bucket (2^22 ns) the row is VOID: it would not be "the same window used today".
  G4 A file from before the window (no win_* fields) is read on the whole run, as today's rows from it are: goodput
     is the client's send_mbps x recv/sent, the delivered count over the client's own elapsed time - the span today's
     figure uses. Not the server's recv_mbps: that is over the server's first-to-last receive, another span (it moved
     the 3ae721aa vendor figures by up to 2% with recv == sent), and TickLE's reliable server there prints none.
  G5 Per-sample divisors: the client's cpu_s_per_Msample, cpu_s_per_MB, sched_cpu_s_per_Msample and
     wire_bytes_per_sample are divided today by its own sent= (checked per row: G5 fails the row if the published
     figure does not reproduce from utime_s + stime_s and wire_bytes_total over sent= within rounding). They are
     rescaled by sent / delivered, delivered being the server's recv= (throughput) or the client's own recv= (latency
     round trips answered). The server's figures are already over its recv= and are left alone (checked the same way).
     delivered = 0 makes the row VOID.
  G6 Nothing else moves: the sent= and recv= counts, the delivery rule, the boundary gate and the policy check are
     campaign_summary.py's, applied unchanged to the rewritten file.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BUCKET_S = (1 << 22) / 1e9
PER_SAMPLE = ("cpu_s_per_Msample", "cpu_s_per_MB", "sched_cpu_s_per_Msample", "wire_bytes_per_sample")
ROW = re.compile(r"^(c\d+\s+[TL]\s+p\d\s+Q\d\s+.+?\s+(?:tickle|cyclonedds|fastdds)\s+rep\d+\s+\|\s+)([^|]+?)(\s+\|\s*)(.*)$")
PAIR = re.compile(r"([A-Za-z_]\w*)=([-\d.]+)")


def fields(chunk):
    return dict(PAIR.findall(chunk))


def set_field(chunk, key, value):
    """Replace key=<number> in chunk (word-bounded), keeping the old value as diag_<key>=."""
    pat = re.compile(rf"(?<![\w])({re.escape(key)})=([-\d.]+)")
    m = pat.search(chunk)
    if not m:
        return chunk
    return chunk[:m.start()] + f"{key}={value:.6f} diag_{key}={m.group(2)}" + chunk[m.end():]


def check_divisor(own, divisor, tol):
    """G5: the published per-sample CPU and wire figures reproduce from the raw totals over divisor."""
    bad = []
    try:
        cpu = float(own["utime_s"]) + float(own["stime_s"])
        pub = float(own["cpu_s_per_Msample"])
        # utime_s/stime_s are printed to the millisecond: allow that rounding plus tol.
        if abs(cpu / divisor * 1e6 - pub) > pub * tol + 0.002 / divisor * 1e6:
            bad.append(f"cpu {cpu / divisor * 1e6:.4f} != {pub}")
    except (KeyError, ValueError, ZeroDivisionError):
        pass
    try:
        wire = float(own["wire_bytes_total"]) / divisor
        pub = float(own["wire_bytes_per_sample"])
        if abs(wire - pub) > pub * tol + 0.01:
            bad.append(f"wire {wire:.4f} != {pub}")
    except (KeyError, ValueError, ZeroDivisionError):
        pass
    return bad


def rewrite_row(head, verdict, sep, body, notes):
    if not verdict.startswith("ok"):
        return head + verdict + sep + body
    chunks = re.split(r"(?=role=)", body)
    roles = {}
    for i, ch in enumerate(chunks):
        if not ch.strip():
            continue
        m = re.match(r"role=(\w+)", ch)
        roles.setdefault(m.group(1) if m else "client", i)
    ci = roles.get("client", 0)
    client = fields(chunks[ci])
    server = fields(chunks[roles["server"]]) if "server" in roles else {}
    tag = head.split("|")[0].strip()
    shape = head.split()[1]
    void = None
    sent = float(client.get("sent", "nan"))
    if shape == "T":
        if "recv" not in server:
            void = "VOID(goodput G1: no receive count)"
        else:
            delivered = float(server["recv"])
            if delivered > sent:
                void = f"VOID(goodput G2: recv {delivered:.0f} > sent {sent:.0f})"
    else:
        delivered = float(client.get("recv", "nan"))
        if delivered != delivered:
            void = "VOID(goodput G1: no round-trip count)"
    if void is None and not delivered > 0:
        void = "VOID(goodput G5: nothing delivered)"
    if void is None:
        bad = check_divisor(client, sent, 0.01)
        if server and "recv" in server:
            bad += [f"server {b}" for b in check_divisor(server, float(server["recv"]), 0.01)]
        if bad:
            void = "VOID(goodput G5: divisor does not reproduce: " + "; ".join(bad) + ")"
    if void:
        notes.append(f"{tag}: {void}")
        return head + void + sep + body
    out = chunks[ci]
    if shape == "T":
        if "win_send_mbps" in client:
            if "win_recv_mbps" not in server or "win_s" not in server:
                notes.append(f"{tag}: VOID(goodput G3: server has no window)")
                return head + "VOID(goodput G3: server has no window)" + sep + body
            if abs(float(server["win_s"]) - float(client["win_s"])) > BUCKET_S:
                why = f"VOID(goodput G3: windows {client['win_s']} s vs {server['win_s']} s)"
                notes.append(f"{tag}: {why}")
                return head + why + sep + body
            out = set_field(out, "win_send_mbps", float(server["win_recv_mbps"]))
        else:
            goodput = float(client["send_mbps"]) * delivered / sent
            notes.append(f"{tag}: G4 whole run, goodput = send_mbps x recv/sent = {goodput:.3f}")
            out = set_field(out, "send_mbps", goodput)
    if delivered != sent:
        for k in PER_SAMPLE:
            if k in client:
                out = set_field(out, k, float(client[k]) * sent / delivered)
    chunks[ci] = out
    return head + verdict + sep + "".join(chunks)


def rewrite(src, dst):
    notes = []
    with open(src, encoding="utf-8") as fin, open(dst, "w", encoding="utf-8") as fout:
        for line in fin:
            m = ROW.match(line.rstrip("\n"))
            fout.write((rewrite_row(*m.groups(), notes) if m else line.rstrip("\n")) + "\n")
    return notes


METRIC = re.compile(r"^   (\S+)\s+tickle (\S+)\s+(.*?)\s+(WIN|DRAW|TIE|LOSE|VOID)(\s+\(.*\))?$")
NOCMP = re.compile(r"^   (\S+)\s+tickle (\S+)\s+\(no cross-vendor comparison")


def summarise(path):
    out = subprocess.run([sys.executable, f"{HERE}/campaign_summary.py", path], capture_output=True, text=True,
                         check=True).stdout
    cells, cell, totals = {}, None, ""
    for line in out.splitlines():
        h = re.match(r"^== (c\d+ .*)$", line)
        if h and not line.startswith("== totals"):
            cell = h.group(1)
            continue
        if line.startswith("== totals"):
            totals = line[3:].strip()
            continue
        m = METRIC.match(line)
        if m and cell:
            cells[(cell, m.group(1))] = (m.group(2), m.group(3).strip(), m.group(4))
            continue
        m = NOCMP.match(line)
        if m and cell:
            cells[(cell, m.group(1))] = (m.group(2), "", "NO-CMP")
    return cells, totals


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.split("\n\n")[2])
    src, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    dst = os.path.join(out_dir, os.path.basename(src))
    if os.path.abspath(dst) == os.path.abspath(src):
        sys.exit("out_dir must differ from the source file's directory")
    notes = rewrite(src, dst)
    native, native_tot = summarise(src)
    print(f"source    {src}\nrewritten {dst}")
    for n in notes:
        print(f"  note: {n}")
    if not native:
        sys.exit("no metric lines in the summary of the original - wrong file")
    print(f"\n{len(native)} scored metric-cell(s); campaign_summary.py scores goodput natively from the original:")
    print(f"totals: {native_tot}")


if __name__ == "__main__":
    main()
