#!/usr/bin/env python3
"""Timeline analysis of a T1-Pi diagnosis run (WIFI_DIAG_PLAN.md sections 3 and 4). SIMULATION ONLY data.
Reads RUNBASE.cli.pcap (sender side: hwsta in t1p_sta) and RUNBASE.srv.pcap (receiver side: t1pgs in t1p_srv), RUNBASE.rc (1 Hz sampler),
RUNBASE.near/.far/.meta; own pcap parser (no scapy). Recognises TickLE (4-byte single header or tt_Header) and RTPS (Fast DDS, CycloneDDS).
Usage: t1pi_wdiag.py run RUNBASE [--json]      one run
       t1pi_wdiag.py dir DIR                    every run under DIR (r*_*_A.cli.pcap), one summary row each, JSON lines to stdout with --json
"""
import glob, json, math, os, re, struct, sys

PUB, SUB = "10.90.0.2", "10.90.0.3"


def read_pcap(path):
    """-> list of (ts, src, dst, ipid, first, ul, payload, iplen): one entry per IP packet on the wire (a non-first IP fragment has first=False, ul=0)."""
    out = []
    try:
        f = open(path, "rb")
    except OSError:
        return out
    with f:
        g = f.read(24)
        if len(g) < 24:
            return out
        magic = struct.unpack("<I", g[:4])[0]
        if magic in (0xA1B2C3D4, 0xA1B23C4D):
            en = "<"
        elif magic in (0xD4C3B2A1, 0x4D3CB2A1):
            en = ">"
        else:
            return out
        nano = magic in (0xA1B23C4D, 0x4D3CB2A1)
        link = struct.unpack(en + "I", g[20:24])[0]
        while True:
            h = f.read(16)
            if len(h) < 16:
                break
            sec, frac, caplen, wirelen = struct.unpack(en + "IIII", h)
            d = f.read(caplen)
            if len(d) < caplen:
                break
            ts = sec + frac / (1e9 if nano else 1e6)
            off = 14 if link == 1 else (16 if link == 113 else 0)
            if link == 1 and len(d) > 14 and d[12:14] == b"\x81\x00":
                off = 18
            if len(d) < off + 20 or (d[off] >> 4) != 4 or d[off + 9] != 17:
                continue
            ihl = (d[off] & 15) * 4
            iplen, ipid, ff = struct.unpack("!HHH", d[off + 2:off + 8])
            src = ".".join(map(str, d[off + 12:off + 16]))
            dst = ".".join(map(str, d[off + 16:off + 20]))
            if ff & 0x1FFF:
                out.append((ts, src, dst, ipid, False, 0, b"", iplen))
                continue
            if len(d) < off + ihl + 8:
                continue
            sp, dp, ul = struct.unpack("!HHH", d[off + ihl:off + ihl + 6])
            out.append((ts, src, dst, ipid, True, ul - 8, d[off + ihl + 8:], iplen))
    return out


RTPS_NAMES = {0x01: "PAD", 0x06: "ACKNACK", 0x07: "HEARTBEAT", 0x08: "GAP", 0x09: "INFO_TS", 0x0e: "INFO_DST", 0x0c: "INFO_SRC", 0x0d: "INFO_REPLY_IP4", 0x0f: "INFO_REPLY",
              0x12: "NACK_FRAG", 0x13: "HEARTBEAT_FRAG", 0x15: "DATA", 0x16: "DATA_FRAG"}
RTPS_PRIO = ["DATA_FRAG", "DATA", "NACK_FRAG", "ACKNACK", "HEARTBEAT_FRAG", "HEARTBEAT", "GAP"]
TT_NAMES = {2: "DATA", 3: "ACKNACK", 4: "CALLREQ", 5: "CALLRESP", 6: "HEARTBEAT", 8: "FRAG", 9: "FRAG", 11: "FRAG", 12: "FRAG", 14: "FRAG", 15: "FRAG"}


def classify(pl, fw, dst):
    """-> (kind, units, disc): kind in DATA_FRAG/DATA/NACK/ACK/HB/OTHER; units = [(sample_key, idx, count)] or None; disc = discovery-ish flag"""
    n = len(pl)
    bcast = dst.endswith(".255") or dst.startswith("239.") or dst.startswith("224.")
    if fw == "tickle":
        units = []
        kinds = []
        if n >= 4 and pl[0] in (0x6B, 0x74):  # 'k' / 't' single header: body at 4
            subs = [(pl[3], pl[4:], "<" if pl[0] == 0x6B else ">")]
        elif n >= 8 and pl[:2] in (b"TK", b"KT"):  # tt_Header + submessages
            en = "<" if pl[:2] == b"KT" else ">"
            subs, o = [], 4
            while o + 4 <= n:
                t, rcv, ln = pl[o], pl[o + 1], struct.unpack(en + "H", pl[o + 2:o + 4])[0]
                subs.append((t, pl[o + 4:o + ln] if ln >= 4 else pl[o + 4:], en))
                if ln < 4:
                    break
                o += ln
        else:
            return "OTHER", None, bcast
        for t, b, en in subs:
            nm = TT_NAMES.get(t, "T%d" % t)
            kinds.append(nm)
            u = None
            if t == 12 and len(b) >= 12:
                ent, seq, idx, cnt = struct.unpack(en + "IIHH", b[:12])
                u = [((ent, seq - idx), idx, cnt)] if ent != 0xFFFFFFFF else None
            elif t == 11 and len(b) >= 18:
                ep, seq, ts, ent, cnt = struct.unpack(en + "IIIIH", b[:18])
                u = [((ent, seq), 0, cnt)] if ent != 0xFFFFFFFF else None
            elif t == 9 and len(b) >= 10:
                ent, seq, idx, cnt = struct.unpack(en + "IIBB", b[:10])
                u = [((ent, seq - idx), idx, cnt)] if ent != 0xFFFFFFFF else None
            elif t == 8 and len(b) >= 17:
                ep, seq, ts, ent, cnt = struct.unpack(en + "IIIIB", b[:17])
                u = [((ent, seq), 0, cnt)] if ent != 0xFFFFFFFF else None
            elif t == 14 and len(b) >= 24:
                ep, seq, ts, ent, idx, cnt, rc, z = struct.unpack(en + "IIIIHHHH", b[:24])
                u = [((ent, seq - idx), idx + j, cnt) for j in range(rc)] if ent != 0xFFFFFFFF else None
            elif t == 15 and len(b) >= 19:
                ep, seq, ts, ent, idx, cnt, rc = struct.unpack(en + "IIIIBBB", b[:19])
                u = [((ent, seq - idx), idx + j, cnt) for j in range(rc)] if ent != 0xFFFFFFFF else None
            if u:
                units += u
            elif nm == "FRAG":
                kinds[-1] = "DISC_FRAG"
        if units:
            return "DATA_FRAG", units, bcast
        for p in ("DISC_FRAG", "ACKNACK", "HEARTBEAT", "DATA"):
            if p in kinds:
                return {"DISC_FRAG": "DISC_FRAG", "ACKNACK": "ACK", "HEARTBEAT": "HB", "DATA": "DATA"}[p], None, bcast
        return "OTHER", None, bcast
    # RTPS
    if n < 24 or pl[:4] != b"RTPS":
        return "OTHER", None, bcast
    o = 20
    kinds = []
    unit = None
    disc = bcast
    while o + 4 <= n:
        sid, fl = pl[o], pl[o + 1]
        en = "<" if fl & 1 else ">"
        ln = struct.unpack(en + "H", pl[o + 2:o + 4])[0]
        nm = RTPS_NAMES.get(sid, "S%02x" % sid)
        kinds.append(nm)
        b = o + 4
        if sid == 0x16 and b + 32 <= n:
            wid = struct.unpack(">I", pl[b + 8:b + 12])[0]
            hi, lo = struct.unpack(en + "iI", pl[b + 12:b + 20])
            fsn, nf, fsz, ssz = struct.unpack(en + "IHHI", pl[b + 20:b + 32])
            cnt = max(1, (ssz + fsz - 1) // fsz) if fsz else 1
            if (wid & 0xFF) in (0xC2, 0xC3, 0xC4, 0xC7):
                disc = True
            else:
                unit = [(((wid, ssz), (hi << 32) | lo), fsn - 1 + j, cnt) for j in range(max(1, nf))]
        elif sid == 0x15 and b + 20 <= n:
            wid = struct.unpack(">I", pl[b + 8:b + 12])[0]
            if (wid & 0xFF) in (0xC2, 0xC3, 0xC4, 0xC7):
                disc = True
        if ln == 0:
            break
        o = b + ln
    k = "OTHER"
    for p in RTPS_PRIO:
        if p in kinds:
            k = p
            break
    if k == "NACK_FRAG":
        k = "NACK"
    elif k == "ACKNACK":
        k = "ACK"
    elif k in ("HEARTBEAT", "HEARTBEAT_FRAG"):
        k = "HB"
    return k, unit, disc


def q(v, p):
    if not v:
        return None
    v = sorted(v)
    return v[min(len(v) - 1, int(p * (len(v) - 1) + 0.5))]


def datagrams(pk, fw):
    """Group IP packets into UDP datagrams (IP fragments by src,dst,ipid within 3 s). -> list of dicts, time-ordered by first frame."""
    dg, open_ = [], {}
    for ts, s, d, ipid, first, ul, pl, iplen in pk:
        if s not in (PUB, SUB):
            continue
        key = (s, d, ipid)
        if first:
            if s == PUB and not (d == SUB or d.endswith(".255") or d.startswith("239.")):
                continue
            kind, units, disc = classify(pl, fw, d)
            nf = max(1, -(-(ul + 8) // 1480)) if ul + 8 > 1480 else 1
            x = {"t": ts, "tl": ts, "kind": kind, "units": units, "disc": disc, "dir": "f" if s == PUB else "r", "ul": ul, "nf": nf, "seen": 1, "frames": [ts]}
            dg.append(x)
            if nf > 1:
                open_[key] = x
        else:
            x = open_.get(key)
            if x is not None and ts - x["t"] < 3.0:
                x["seen"] += 1
                x["tl"] = ts
                x["frames"].append(ts)
                if x["seen"] >= x["nf"]:
                    del open_[key]
    return dg


def analyse(base, fw):
    cli = read_pcap(base + ".cli.pcap")
    srv = read_pcap(base + ".srv.pcap")
    R = {"base": base, "fw": fw}
    if not cli:
        R["error"] = "no sender pcap"
        return R
    ec, es = datagrams(cli, fw), datagrams(srv, fw)
    data_c = [x for x in ec if x["kind"] == "DATA_FRAG" and x["dir"] == "f"]
    data_s = [x for x in es if x["kind"] == "DATA_FRAG" and x["dir"] == "f" and x["seen"] >= x["nf"]]
    if not data_c:
        R["error"] = "no data units on sender side"
        return R
    t0 = data_c[0]["t"]
    R["t0_first_data_s"] = t0 - ec[0]["t"]
    # clock offset (receiver minus sender) from the first 20 units seen on both sides; the smallest difference is the least delayed
    first_c = {}
    for x in data_c:
        for u in x["units"]:
            first_c.setdefault((u[0], u[1]), x["t"])
    offs, seen = [], set()
    for x in data_s:
        for u in x["units"]:
            k = (u[0], u[1])
            if k in first_c and k not in seen:
                seen.add(k)
                offs.append(x["tl"] - first_c[k])
        if len(offs) >= 20:
            break
    off = min(offs) if offs else 0.0
    R["clock_off_ms"] = off * 1e3
    sent, recv, samples = {}, {}, {}
    for x in data_c:
        for sk, idx, cnt in x["units"]:
            sent.setdefault((sk, idx), []).append(x["t"])
            s_ = samples.setdefault(sk, {"cnt": 0, "first": 1e18, "sent": 0, "uniq_sent": 0})
            s_["cnt"] = max(s_["cnt"], cnt)
            s_["first"] = min(s_["first"], x["t"])
            s_["sent"] += 1
    for (sk, idx) in sent:
        samples[sk]["uniq_sent"] += 1
    for x in data_s:
        for sk, idx, cnt in x["units"]:
            recv.setdefault((sk, idx), []).append(x["tl"] - off)
            samples.setdefault(sk, {"cnt": cnt, "first": 1e18, "sent": 0, "uniq_sent": 0})
    ords = sorted(samples, key=lambda k: samples[k]["first"])
    got = {}
    for (sk, idx), ts in recv.items():
        got.setdefault(sk, {})[idx] = min(ts)
    comp = []
    for i, sk in enumerate(ords):
        s_ = samples[sk]
        have = got.get(sk, {})
        done = len(have) >= s_["cnt"] > 0
        s_["t_done"] = max(have.values()) if done else None
        s_["recv_units"] = len(have)
        comp.append((i + 1, s_["cnt"], s_["sent"], s_["uniq_sent"], len(have), (s_["t_done"] - s_["first"]) * 1e3 if done else None, s_["first"] - t0))
    R["samples"] = comp
    R["n_samples_seen"] = len(ords)
    R["n_done"] = sum(1 for c in comp if c[5] is not None)
    cd = [c[5] for c in comp if c[5] is not None]
    R["compl_ms_p50"], R["compl_ms_max"] = q(cd, 0.5), (max(cd) if cd else None)
    ups = sorted(c[1] for c in comp)
    R["units_per_sample"] = ups[len(ups) // 2] if ups else None
    R["dg_udp_payload_median"] = q([x["ul"] for x in data_c], 0.5)
    R["dg_nframes_max"] = max(x["nf"] for x in data_c)
    R["units_per_dg_mean"] = sum(len(x["units"]) for x in data_c) / len(data_c)
    R["data_dg_sent"], R["data_dg_recv_complete"] = len(data_c), len(data_s)
    fr_c = sum(x["nf"] for x in data_c)
    fr_s = sum(x["seen"] for x in es if x["kind"] == "DATA_FRAG" and x["dir"] == "f")
    R["data_frames_sent"], R["data_frames_seen_at_server"] = fr_c, fr_s
    uniq = len(sent)
    R["units_unique_sent"] = uniq
    nu = sum(len(x["units"]) for x in data_c)
    R["units_sent_total"] = nu
    R["send_amplification"] = nu / uniq if uniq else None
    # bursts of data frames (gap < 5 ms) on the sender side: frames time-ordered
    ft = sorted(t for x in data_c for t in x["frames"])
    bursts, cur = [], [0, 0]
    for i, t in enumerate(ft):
        if i and t - ft[i - 1] > 0.005:
            bursts.append((ft[cur[0]], cur[1] - cur[0] + 1))
            cur = [i, i]
        else:
            cur[1] = i
    bursts.append((ft[cur[0]], cur[1] - cur[0] + 1))
    bs = [b[1] for b in bursts]
    R["bursts"] = len(bs)
    R["burst_frames_p50"], R["burst_frames_p90"], R["burst_frames_max"] = q(bs, 0.5), q(bs, 0.9), max(bs)
    gaps = [ft[i] - ft[i - 1] for i in range(1, len(ft)) if ft[i] - ft[i - 1] <= 0.005]
    R["intra_gap_us_p50"], R["intra_gap_us_p90"] = (q(gaps, 0.5) or 0) * 1e6, (q(gaps, 0.9) or 0) * 1e6
    def bins(xs, w):
        b = {}
        for t in xs:
            k = int((t - t0) / w)
            b[k] = b.get(k, 0) + 1
        return b
    fs_srv = sorted(t - off for x in es if x["kind"] == "DATA_FRAG" and x["dir"] == "f" for t in x["frames"])
    b1s, b1r, b01r = bins(ft, 1.0), bins(fs_srv, 1.0), bins(fs_srv, 0.1)
    R["recv_peak_100ms_fps"] = max(b01r.values()) * 10 if b01r else 0
    rv = [b1r.get(i, 0) for i in range(0, (max(b1r) + 1) if b1r else 0)]
    R["recv_bins_1s"] = rv
    busy = [v for v in rv if v >= 5]
    R["recv_busy_1s_p50"], R["recv_busy_1s_p90"] = q(busy, 0.5), q(busy, 0.9)
    R["send_bins_1s"] = [b1s.get(i, 0) for i in range(0, (max(b1s) + 1) if b1s else 0)]
    best = None
    for i in range(0, max(1, len(rv) - 19)):
        w = rv[i:i + 20]
        m = sum(w) / len(w)
        if best is None or m > best[0]:
            sd = math.sqrt(sum((x - m) ** 2 for x in w) / len(w))
            best = (m, sd / m if m else 0.0, i)
    R["win20_mean_fps"], R["win20_cv"], R["win20_start"] = best if best else (0, 0, 0)
    def cnt(e, k, d):
        return sum(1 for x in e if x["kind"] == k and x["dir"] == d)
    for side, e in (("c", ec), ("s", es)):
        R["ctl_" + side] = {"nack_r": cnt(e, "ACK", "r") + cnt(e, "NACK", "r"), "hb_f": cnt(e, "HB", "f"), "ack_f": cnt(e, "ACK", "f"), "hb_r": cnt(e, "HB", "r"),
                            "disc_all": sum(1 for x in e if x["disc"] or x["kind"] == "DISC_FRAG"), "disc_first10s": sum(1 for x in e if (x["disc"] or x["kind"] == "DISC_FRAG") and x["t"] - e[0]["t"] < 10)}
    nk = [x["t"] for x in ec if x["dir"] == "r" and x["kind"] in ("ACK", "NACK")]
    cl = []
    for t in nk:
        if not cl or t - cl[-1] > 0.05:
            cl.append(t)
    R["nack_clusters"] = len(cl)
    iv = [cl[i] - cl[i - 1] for i in range(1, len(cl))]
    R["nack_interval_s_p50"], R["nack_interval_s_p90"] = q(iv, 0.5), q(iv, 0.9)
    bst = [b[0] for b in bursts if b[1] >= 5]
    ib = [bst[i] - bst[i - 1] for i in range(1, len(bst))]
    R["burst_interval_s_p50"], R["burst_interval_s_p90"] = q(ib, 0.5), q(ib, 0.9)
    R["first_sends"], R["repeat_sends"] = uniq, nu - uniq
    try:
        rc = [json.loads(l) for l in open(base + ".rc")]
        names = [next((x[0] for x in r["sta"] if "A" in x[1]), None) for r in rc]
        R["rc_sta_best_first"], R["rc_sta_best_last"] = names[0], names[-1]
        R["rc_sta_best_set"] = sorted(set(n for n in names if n))
        R["rc_sta_sum_att_first"] = sum(x[4] for x in rc[0]["sta"])
        R["rc_sta_sum_att_last"] = sum(x[4] for x in rc[-1]["sta"])
        R["rc_ap_best_last"] = next((x[0] for x in rc[-1]["ap"] if "A" in x[1]), None)
        R["rc_sta_aqm_drops_last"] = (rc[-1].get("sta_aqm") or {}).get("drops")
        R["rc_sta_aqm_overlimit_last"] = (rc[-1].get("sta_aqm") or {}).get("overlimit")
        R["rc_sta_aqm_backlog_max"] = max(((r.get("sta_aqm") or {}).get("backlog_pkts", 0) for r in rc), default=0)
        w0 = [r["wm_ticks"] for r in rc if r.get("wm_ticks") is not None]
        R["wm_cpu_pct_mean"] = (w0[-1] - w0[0]) / (len(w0) - 1) if len(w0) > 1 else None
        R["wm_cpu_pct_max"] = max((w0[i] - w0[i - 1] for i in range(1, len(w0))), default=None)
    except (OSError, ValueError, KeyError, IndexError):
        pass
    txt = ""
    for ext in (".near", ".far"):
        try:
            txt += open(base + ext, errors="replace").read()
        except OSError:
            pass
    R["peer_registered_pub"] = len(re.findall(r"Publisher peer registered", txt))
    R["writer_proxy_created"] = len(re.findall(r"Writer proxy created", txt))
    for key, pat in (("retransmitted", r"role=pub size=.*? retransmitted=(\d+)"), ("delivered", r"role=sub size=\S+ reliable=1 delivered=(\d+)"), ("acknack_received", r"acknack_received=(\d+)"),
                     ("acknack_sent", r"acknack_tx immediate=\d+ timer=\d+ new_gap=\d+ reveal=\d+ sent=(\d+)"), ("tx_udp_broadcast", r"tx_udp_broadcast=(\d+)"),
                     ("send_waits", r"large_send_waits=(\d+)"), ("summary_copies", r"summary_copies=(\d+)"), ("hybrid_runs", r"hybrid_runs=(\d+)"), ("hybrid_run_units", r"hybrid_run_units=(\d+)"),
                     ("hybrid_mtu_units", r"hybrid_mtu_units=(\d+)"), ("hybrid_collapses", r"hybrid_collapses=(\d+)")):
        m = re.search(pat, txt)
        if m:
            R[key] = int(m.group(1))
    m = re.search(r"hybrid_n_hist=(\S+)", txt)
    if m:
        R["hybrid_n_hist"] = m.group(1)
    gaps_opened = re.findall(r"gaps_opened=(\d+)", txt)
    if gaps_opened:
        R["gaps_opened"] = max(int(x) for x in gaps_opened)
    if "delivered" not in R:
        m = re.search(r"delivered=(\d+)", txt)
        if m:
            R["delivered"] = int(m.group(1))
    return R


def fw_of(path):
    m = re.search(r"_(tickle[^_/]*|fastdds|cyclonedds)_(A2?)$", path)
    return m.group(1) if m else "tickle"


def fmt(v, p=1):
    if v is None:
        return "-"
    if isinstance(v, float):
        return ("%." + str(p) + "f") % v
    return str(v)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    if sys.argv[1] == "run":
        r = analyse(sys.argv[2], fw_of(sys.argv[2]))
        if "--json" in sys.argv:
            print(json.dumps(r))
        else:
            for k, v in r.items():
                if k not in ("samples", "recv_bins_1s", "send_bins_1s"):
                    print("%-28s %s" % (k, v))
            print("samples (ord, units, sent, uniq_sent, recv_units, completion_ms, start_s):")
            for c in r.get("samples", []):
                print("  ", c)
            print("recv 1s bins:", r.get("recv_bins_1s"))
            print("send 1s bins:", r.get("send_bins_1s"))
    elif sys.argv[1] == "dir":
        for p in sorted(glob.glob(os.path.join(sys.argv[2], "**", "r*_A.cli.pcap"), recursive=True)):
            base = p[:-len(".cli.pcap")]
            r = analyse(base, fw_of(base))
            r["arm"] = os.path.basename(os.path.dirname(base))
            r["run"] = os.path.basename(base)
            print(json.dumps(r))
