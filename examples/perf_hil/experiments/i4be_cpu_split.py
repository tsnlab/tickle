#!/usr/bin/env python3
"""Split the CPU of an rmw large-message cell (I4 best effort) into user and system time, per side and per arm.

Reads <OUT>.runs/ directories written by rmw_keepall_rig.sh (KCOUNTERS on, the default) and prints, per arm, the
median over reps (with min..max) of:

  sub_u / sub_s / pub_u / pub_s   ms of ru_utime / ru_stime per MB, over the steady window (the same window
                                  largemsg_rig_summary.py takes: WARM rows after the first live row, COOL before
                                  the last), subscriber per MB delivered, publisher per MB sent
  *_minflt                        minor page faults per sample, same window
  *_vcs / *_ivcs                  voluntary / involuntary context switches per sample, same window; a voluntary
                                  switch is a sleep in the kernel, so on the subscriber it counts its wakes
  pub_udp_out, pub_frag           system-wide Udp:OutDatagrams and Ip:FragCreates per delivered sample on the
                                  publisher's host, from <stem>_pub_kpre/_kpost (whole run): sends and IP fragments
  sub_udp_in, sub_reasm, sub_ip   Udp:InDatagrams, Ip:ReasmOKs, Ip:InReceives per delivered sample on the
                                  subscriber's host
  sub_softnet / pub_softnet       packets processed by the softirq (/proc/net/softnet_stat column 0, all CPUs)
  insn                            perf stat instructions:u/:k per sample on each side, when the run counted them
                                  (pub_insn_u= ... on the index line, INSN_COUNT=1)

The system-wide counters include the clock probe and everything else on the host; their control is the vendor arm
in the same file, which runs under the same probe. Usage: i4be_cpu_split.py OUT.runs [OUT.runs ...]
"""
import re
import statistics as st
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rmw_keepall_rig_summary import INDEX_RE, insn_of, rows, sample_bytes, whole_counts  # noqa: E402

WARM, COOL = 2, 1
SIZE = 4194304


def steady(rs, key):
    live = [i for i, r in enumerate(rs) if r.get(key, 0) > 0]
    if len(live) <= WARM + COOL + 1:
        return None
    idx = live[WARM:-COOL]
    if idx[0] == 0:
        return None
    return rs[idx[0] - 1], rs[idx[-1]], sum(rs[i][key] for i in idx)


def snmp(path):
    text = path.read_text()
    out = {}
    lines = text.splitlines()
    for a, b in zip(lines, lines[1:]):
        ka, kb = a.split(), b.split()
        if len(ka) > 1 and len(kb) == len(ka) and ka[0] == kb[0] and ka[0].endswith(":") \
                and not ka[1].lstrip("-").isdigit() and kb[1].lstrip("-").isdigit():
            for k, v in zip(ka[1:], kb[1:]):
                out[ka[0] + k] = int(v)
    m = re.search(r"--- softnet\n(.*?)(?:\n---|\Z)", text, re.S)
    if m:
        out["softnet"] = sum(int(x.split()[0], 16) for x in m.group(1).splitlines() if x.strip())
    return out


def kdelta(runs, stem, side):
    pre, post = runs / f"{stem}_{side}_kpre.txt", runs / f"{stem}_{side}_kpost.txt"
    if not pre.exists() or not post.exists():
        return {}
    a, b = snmp(pre), snmp(post)
    return {k: b[k] - a[k] for k in b if k in a}


def run(runs, stem, line):
    sub, _ = rows(runs / f"{stem}_sub.log")
    pub, _ = rows(runs / f"{stem}_pub.log")
    whole_counts(sub, sample_bytes("Array4m"))
    whole_counts(pub)
    s, p = steady(sub, "n_received"), steady(pub, "n_sent")
    if not s or not p:
        return None
    rec = {}
    for side, (b, e, n) in (("sub", s), ("pub", p)):
        mb = n * SIZE / 1e6
        rec[side + "_u"] = (e["ru_utime"] - b["ru_utime"]) * 1e3 / mb
        rec[side + "_s"] = (e["ru_stime"] - b["ru_stime"]) * 1e3 / mb
        rec[side + "_vcs"] = (e["ru_nvcsw"] - b["ru_nvcsw"]) / n
        rec[side + "_ivcs"] = (e["ru_nivcsw"] - b["ru_nivcsw"]) / n
        rec[side + "_minflt"] = (e["ru_minflt"] - b["ru_minflt"]) / n
        rec[side + "_n"] = n
    recv = sum(r["n_received"] for r in sub)
    rec["recv"] = recv
    kp, ks = kdelta(runs, stem, "pub"), kdelta(runs, stem, "sub")
    if kp and ks and recv:
        rec["pub_udp_out"] = kp.get("Udp:OutDatagrams", 0) / recv
        rec["pub_frag"] = kp.get("Ip:FragCreates", 0) / recv
        rec["pub_softnet"] = kp.get("softnet", 0) / recv
        rec["sub_udp_in"] = ks.get("Udp:InDatagrams", 0) / recv
        rec["sub_reasm"] = ks.get("Ip:ReasmOKs", 0) / recv
        rec["sub_ip"] = ks.get("Ip:InReceives", 0) / recv
        rec["sub_softnet"] = ks.get("softnet", 0) / recv
    ins = insn_of(line) if line else {}
    for side in ("pub", "sub"):
        d = ins.get(side, {})
        if "u" in d and "k" in d:
            n = sum(r["n_received"] for r in sub) if side == "sub" else sum(r["n_sent"] for r in pub)
            rec[side + "_insn_u"] = d["u"] / n
            rec[side + "_insn_k"] = d["k"] / n
    return rec


KEYS = ["sub_n", "sub_u", "sub_s", "pub_u", "pub_s", "sub_vcs", "sub_ivcs", "pub_vcs", "pub_ivcs", "sub_minflt", "pub_minflt", "pub_udp_out",
        "pub_frag", "sub_udp_in", "sub_reasm", "sub_ip", "sub_softnet", "pub_softnet",
        "pub_insn_u", "pub_insn_k", "sub_insn_u", "sub_insn_k"]


def main(argv):
    for d in argv:
        runs = Path(d)
        lines = {}
        idx = runs / "index.txt"
        if idx.exists():
            for line in idx.read_text().splitlines():
                m = re.match(INDEX_RE, line)
                if m:
                    lines[m.group(1)] = line
        arms = {}
        for sub in sorted(runs.glob("*_Array4m_l0_r*_sub.log")):
            stem = sub.name[: -len("_sub.log")]
            rec = run(runs, stem, lines.get(stem, ""))
            if rec:
                arms.setdefault(stem.split("_Array4m")[0], []).append(rec)
        print(f"== {runs}")
        for arm, recs in arms.items():
            print(f"  {arm} ({len(recs)} reps)")
            for k in KEYS:
                xs = [r[k] for r in recs if k in r]
                if xs:
                    print(f"    {k:12} {st.median(xs):12.4f}   {min(xs):12.4f} .. {max(xs):12.4f}")


if __name__ == "__main__":
    main(sys.argv[1:])
