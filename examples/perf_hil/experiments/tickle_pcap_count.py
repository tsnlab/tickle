#!/usr/bin/env python3
"""Count TickLE wire traffic in a pcap, per sending node: datagrams, bytes and submessages.

Written for rmw_heartbeat_wire_cost.sh, to measure what a Heartbeat costs on the wire exactly
rather than infer it from throughput. No third-party pcap library: the format is small and this
box has none installed.

Per source node (tt_Header.source) it reports:
  datagrams, udp_payload_bytes
  data            DATA submessages
  data_retx       DATA whose (endpoint, entity, seq_no) was already seen from that node - a
                  retransmission (with no injected loss, every one is a duplicate the reader asked
                  for, or one nobody needed)
  hb_alone        HEARTBEAT in a datagram with no DATA (periodic, or an eviction/solicited reply)
  hb_piggyback    HEARTBEAT in the same datagram as a DATA
  acknack         ACKNACK submessages
  other           any other submessage type (UPDATE discovery, RPC)
Only TickLE datagrams count - a magic "KT"/"TK", or since tt_VERSION 10 a single-submessage marker "k"/"t" -, so unrelated UDP on the same
interface cannot leak in.
"""
import struct
import sys
from collections import defaultdict

DATA, ACKNACK, HEARTBEAT = 2, 3, 6


def packets(path):
    with open(path, "rb") as f:
        head = f.read(24)
        magic = struct.unpack("<I", head[:4])[0]
        if magic in (0xA1B2C3D4, 0xA1B23C4D):
            e = "<"
        elif magic in (0xD4C3B2A1, 0x4D3CB2A1):
            e = ">"
        else:
            sys.exit(f"{path}: not a classic pcap (pcapng is not supported; tcpdump -w writes pcap)")
        linktype = struct.unpack(e + "I", head[20:24])[0]
        while True:
            rec = f.read(16)
            if len(rec) < 16:
                return
            _, _, incl, orig = struct.unpack(e + "IIII", rec)
            buf = f.read(incl)
            if incl != orig:
                sys.exit("truncated packet in capture: rerun tcpdump with -s 0")
            yield linktype, buf


def udp_payload(linktype, buf):
    if linktype == 1:  # Ethernet (Linux lo)
        if struct.unpack(">H", buf[12:14])[0] != 0x0800:
            return None
        ip = buf[14:]
    elif linktype == 113:  # Linux cooked
        ip = buf[16:]
    elif linktype == 276:  # Linux cooked v2
        ip = buf[20:]
    elif linktype == 101:  # raw IP
        ip = buf
    else:
        sys.exit(f"unsupported linktype {linktype}")
    if len(ip) < 20 or ip[0] >> 4 != 4 or ip[9] != 17:
        return None
    ihl = (ip[0] & 0x0F) * 4
    udp_len = struct.unpack(">H", ip[ihl + 4:ihl + 6])[0]
    return ip[ihl + 8:ihl + udp_len]



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

def main():
    stats = defaultdict(lambda: defaultdict(int))
    seen = defaultdict(set)
    for linktype, buf in packets(sys.argv[1]):
        wire = udp_payload(linktype, buf)
        view = classic_view(wire) if wire else None
        if view is None:
            continue
        p, e, version, _ = view
        data_len, _, entity_at = data_header_layout(version)
        src = p[3]
        s = stats[src]
        s["datagrams"] += 1
        s["udp_payload_bytes"] += len(wire)
        off, has_data, hbs = 4, False, 0
        while off + 4 <= len(p):
            typ = p[off]
            # In bytes, header included and padded to 4 (end_encode(), tickle.c). The field's own
            # comment in tickle.h says "in 4 bytes"; the encoder writes bytes.
            length = struct.unpack(e + "H", p[off + 2:off + 4])[0]
            if length < 4 or off + length > len(p):
                s["malformed"] += 1
                break
            body = p[off + 4:off + length]
            if typ == DATA and len(body) >= data_len:
                has_data = True
                s["data"] += 1
                endpoint, seq = struct.unpack(e + "II", body[:8])
                entity = struct.unpack(e + "I", body[entity_at:entity_at + 4])[0]
                key = (endpoint, entity, seq)
                if key in seen[src]:
                    s["data_retx"] += 1
                else:
                    seen[src].add(key)
            elif typ == HEARTBEAT:
                hbs += 1
            elif typ == ACKNACK:
                s["acknack"] += 1
            else:
                s["other"] += 1
            off += length
        s["hb_piggyback" if has_data else "hb_alone"] += hbs
    fields = ["datagrams", "udp_payload_bytes", "data", "data_retx", "hb_alone", "hb_piggyback",
              "acknack", "other", "malformed"]
    for src in sorted(stats):
        print(f"node{src} " + " ".join(f"{k}={stats[src][k]}" for k in fields))


if __name__ == "__main__":
    main()
