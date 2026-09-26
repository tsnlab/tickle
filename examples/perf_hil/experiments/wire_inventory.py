#!/usr/bin/env python3
"""Where a TickLE capture's bytes go, by kind (2026-09-26, rmw_tickle/WIRE_PLAN.md W0).

Usage: wire_inventory.py <pcap> [<pcap> ...]   (classic pcap, Ethernet/cooked/raw)

A capture taken with a short snap length (the rig's rmw captures use 256) still counts every datagram's
full size, from the UDP header: what was cut off is reported as `unparsed`, not guessed at.

For every TickLE datagram (magic "KT"/"TK", or since tt_VERSION 10 a single-submessage marker "k"/"t") the
bytes on the wire are split into:
  l2l3l4        Ethernet + IPv4 + UDP headers, counted as 42 per datagram (outside TickLE's control)
  tt_header     4 per datagram: the tt_Header, or the whole 4-byte tt_SingleHeader
  sub_header    4 per submessage of a classic datagram (none in the single form)
  data_header   DATA's 16, FRAG_FIRST's 17, FRAG_CONT's 10 (user endpoints; 20 and 21 before tt_VERSION 10)
  data_payload  what follows those headers inside the submessage (CDR payload and padding)
  hb, acknack   whole HEARTBEAT / ACKNACK submessage bodies on user endpoints
  discovery     every submessage on the built-in discovery endpoint (endpoint_id 0), whole
  call          CALLREQUEST / CALLRESPONSE bodies
  retx_data     DATA/FRAG_FIRST bodies whose (source, endpoint, entity, seq_no) was already seen
                - counted inside data_header/data_payload as well, reported separately
Then what WIRE_PLAN's format candidates would remove from this very capture:
  W1  6 bytes per DATA / FRAG_FIRST (a 2-byte writer handle for endpoint_id + entity_id)
  W2  4 bytes per DATA / FRAG_FIRST (a 32-bit timestamp)
  W4  3.5 bytes per datagram carrying exactly one submessage (one combined header)
All figures are also given per user sample, a sample being one DATA or one FRAG_FIRST that is not a
retransmission.
"""
import struct
import sys
from collections import Counter

DATA, ACKNACK, CALLREQ, CALLRESP, HEARTBEAT, FRAG_FIRST, FRAG_CONT = 2, 3, 4, 5, 6, 8, 9
L2L3L4 = 42


def packets(path):
    with open(path, "rb") as f:
        head = f.read(24)
        magic = struct.unpack("<I", head[:4])[0]
        if magic in (0xA1B2C3D4, 0xA1B23C4D):
            e = "<"
        elif magic in (0xD4C3B2A1, 0x4D3CB2A1):
            e = ">"
        else:
            sys.exit(f"{path}: not a classic pcap")
        linktype = struct.unpack(e + "I", head[20:24])[0]
        while True:
            rec = f.read(16)
            if len(rec) < 16:
                return
            _, _, incl, orig = struct.unpack(e + "IIII", rec)
            buf = f.read(incl)
            yield linktype, buf


def udp_payload(linktype, buf):
    if linktype == 1:
        if struct.unpack(">H", buf[12:14])[0] != 0x0800:
            return None
        ip = buf[14:]
    elif linktype == 113:
        ip = buf[16:]
    elif linktype == 276:
        ip = buf[20:]
    elif linktype == 101:
        ip = buf
    else:
        sys.exit(f"unsupported linktype {linktype}")
    if len(ip) < 20 or ip[0] >> 4 != 4 or ip[9] != 17:
        return None
    ihl = (ip[0] & 0x0F) * 4
    udp_len = struct.unpack(">H", ip[ihl + 4:ihl + 6])[0]
    return ip[ihl + 8:ihl + udp_len], udp_len - 8



def classic_view(p, true_len=None):
    """A TickLE datagram in the classic form (tt_Header + tt_SubmessageHeader), whichever it came in: since
    tt_VERSION 10 one carrying a single submessage to every node has a 4-byte tt_SingleHeader instead - marker
    'k'/'t' (the magic's first byte in lower case), version, source, type - and the submessage runs to the end.
    Returns (classic bytes, byte order, version, single) or None for anything that is not TickLE."""
    if len(p) >= 4 and p[:1] in (b"k", b"t"):
        e = "<" if p[:1] == b"k" else ">"
        magic = b"KT" if e == "<" else b"TK"
        # The submessage runs to the datagram's true end, which a short snap length may have cut from p.
        classic = magic + bytes([p[1], p[2], p[3], 0xFF]) + struct.pack(e + "H", true_len or len(p)) + p[4:]
        return classic, e, p[1], True
    if len(p) >= 4 and p[:2] in (b"KT", b"TK"):
        return p, ("<" if p[:2] == b"KT" else ">"), p[2], False
    return None


def data_header_layout(version):
    """(DATA header bytes, FRAG_FIRST header bytes, entity_id offset in them): the timestamp is 32-bit
    microseconds since tt_VERSION 10, 64-bit nanoseconds before."""
    return (16, 17, 12) if version >= 10 else (20, 21, 16)

def inventory(path):
    b = Counter()
    n = Counter()
    seen = set()
    for linktype, buf in packets(path):
        got = udp_payload(linktype, buf)
        if not got:
            continue
        wire, full = got
        view = classic_view(wire, full)
        if view is None:
            continue
        p, e, version, single = view
        data_len, first_len, entity_at = data_header_layout(version)
        full += 4 if single else 0  # the classic view is 4 bytes longer than the wire
        src = p[3]
        n["datagrams"] += 1
        b["l2l3l4"] += L2L3L4
        b["tt_header"] += 4  # the tt_Header, or the whole tt_SingleHeader
        if single:
            n["single_form_datagrams"] += 1
        off, subs = 4, 0
        while off + 4 <= len(p):
            typ = p[off]
            length = struct.unpack(e + "H", p[off + 2:off + 4])[0]
            if length < 4 or off + length > full:
                n["malformed"] += 1
                break
            if off + length > len(p):  # cut off by the snap length: counted, not parsed
                b["unparsed"] += full - off
                n["truncated"] += 1
                off = full
                break
            subs += 1
            b["sub_header"] += 0 if single else 4  # the single form has no submessage header of its own
            body = p[off + 4:off + length]
            endpoint = struct.unpack(e + "I", body[:4])[0] if len(body) >= 4 else None
            if endpoint == 0 and typ in (DATA, FRAG_FIRST, FRAG_CONT, HEARTBEAT, ACKNACK):
                b["discovery"] += len(body)
                n["discovery_sub"] += 1
            elif typ in (DATA, FRAG_FIRST) and len(body) >= data_len:
                hlen = data_len if typ == DATA else first_len
                b["data_header"] += hlen
                b["data_payload"] += len(body) - hlen
                seq = struct.unpack(e + "I", body[4:8])[0]
                entity = struct.unpack(e + "I", body[entity_at:entity_at + 4])[0]
                key = (src, endpoint, entity, seq)
                if key in seen:
                    b["retx_data"] += len(body)
                    n["retx"] += 1
                else:
                    seen.add(key)
                    n["samples"] += 1
                n["data_or_first"] += 1
            elif typ == FRAG_CONT and len(body) >= 10:
                b["data_header"] += 10
                b["data_payload"] += len(body) - 10
                n["frag_cont"] += 1
            elif typ == HEARTBEAT:
                b["hb"] += len(body)
                n["hb"] += 1
            elif typ == ACKNACK:
                b["acknack"] += len(body)
                n["acknack"] += 1
            elif typ in (CALLREQ, CALLRESP):
                b["call"] += len(body)
            else:
                b["other"] += len(body)
            off += length
        if off < full and len(p) < full:
            b["unparsed"] += full - off
            n["truncated"] += 1
        if subs == 1:
            n["single_sub_datagrams"] += 1
    return b, n


def main():
    for path in sys.argv[1:]:
        b, n = inventory(path)
        total = sum(v for k, v in b.items() if k != "retx_data")
        samples = max(n["samples"], 1)
        print(f"== {path}: {n['datagrams']} datagrams, {n['samples']} samples, {total} bytes on the wire "
              f"({total / samples:.1f} per sample)")
        for k in ("l2l3l4", "tt_header", "sub_header", "data_header", "data_payload", "hb", "acknack",
                  "discovery", "call", "other", "unparsed", "retx_data"):
            if b[k]:
                print(f"   {k:13s} {b[k]:>12d} B  {100.0 * b[k] / total:5.1f}%  {b[k] / samples:8.2f} B/sample")
        w1 = 6 * n["data_or_first"]
        w2 = 4 * n["data_or_first"]
        w4 = 3.5 * n["single_sub_datagrams"]
        for name, saved in (("W1", w1), ("W2", w2), ("W4", w4)):
            print(f"   candidate {name}: -{saved:.0f} B = {100.0 * saved / total:.2f}% of bytes, "
                  f"{saved / samples:.2f} B/sample")
        print(f"   counts: {dict(n)}")


if __name__ == "__main__":
    main()
