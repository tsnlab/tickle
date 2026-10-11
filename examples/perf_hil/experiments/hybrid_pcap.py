#!/usr/bin/env python3
"""hybrid_pcap.py - what a TickLE writer sent, as one receiver's interface saw it (DESIGN.md section 8, hybrid transport).

Reads a pcap tcpdump wrote on a receiver's interface (Ethernet, IPv4) and classifies every UDP datagram from the
writer's address by the TickLE submessage its first fragment carries:

  first transmissions  addressed to everyone (the single-submessage form): runs (types 14, 15) and MTU fragments
                       (types 8, 9, 11, 12);
  repairs              fragments addressed to one reader (the classic form, receiver != 0xff) - what a writer resends
                       for an ACKNACK. DESIGN.md: always MTU units of the lost range.

and whether IP fragmented it (MF set, or a fragment offset). Prints one line of key=value counts:

  runs run_max_bytes run_max_units mtu_first repairs repair_runs repair_fragmented repair_max_bytes fragmented_frames

A repair that is a run, or that IP fragmented, or that is larger than 1,472 bytes, is what the "repairs are MTU-only"
criterion forbids; this script only counts, the harness judges.

Usage: hybrid_pcap.py FILE.pcap WRITER_IP
"""
import struct
import sys

RUN_TYPES = (14, 15)
FRAG_TYPES = (8, 9, 11, 12)
SINGLE_MARKERS = (ord("k"), ord("t"))
CLASSIC_MAGICS = (b"KT", b"TK")
ETHERNET = 14
UDP = 8


def packets(path):
    with open(path, "rb") as f:
        header = f.read(24)
        if len(header) < 24:
            return
        magic = struct.unpack("<I", header[:4])[0]
        endian = "<" if magic in (0xA1B2C3D4, 0xA1B23C4D) else ">"
        while True:
            record = f.read(16)
            if len(record) < 16:
                return
            _, _, caplen, _ = struct.unpack(endian + "IIII", record)
            yield f.read(caplen)


def main():
    path, writer = sys.argv[1], sys.argv[2]
    writer_ip = bytes(int(x) for x in writer.split("."))
    counts = dict(runs=0, run_max_bytes=0, run_max_units=0, mtu_first=0, repairs=0, repair_runs=0,
                  repair_fragmented=0, repair_max_bytes=0, fragmented_frames=0, unparsed=0)
    for frame in packets(path):
        if len(frame) < ETHERNET + 20 or frame[12:14] != b"\x08\x00":
            continue
        ip = frame[ETHERNET:]
        ihl = (ip[0] & 0x0F) * 4
        if ip[9] != 17 or ip[12:16] != writer_ip:
            continue
        flags_offset = struct.unpack("!H", ip[6:8])[0]
        more = bool(flags_offset & 0x2000)
        offset = flags_offset & 0x1FFF
        if more or offset:
            counts["fragmented_frames"] += 1
        if offset:
            continue  # a later fragment: the first one named the datagram
        udp = ip[ihl:]
        if len(udp) < UDP + 8:
            counts["unparsed"] += 1
            continue
        length = struct.unpack("!H", udp[4:6])[0] - UDP  # the datagram's payload, however IP cut it
        payload = udp[UDP:]
        if payload[0] in SINGLE_MARKERS:
            kind, addressed = payload[3], False
            body = payload[4:]
        elif payload[:2] in CLASSIC_MAGICS:
            kind, addressed = payload[4], payload[5] != 0xFF
            body = payload[8:]
        else:
            counts["unparsed"] += 1
            continue
        if kind not in RUN_TYPES and kind not in FRAG_TYPES:
            continue
        if addressed:
            counts["repairs"] += 1
            counts["repair_runs"] += 1 if kind in RUN_TYPES else 0
            counts["repair_fragmented"] += 1 if more else 0
            counts["repair_max_bytes"] = max(counts["repair_max_bytes"], length)
        elif kind in RUN_TYPES:
            counts["runs"] += 1
            counts["run_max_bytes"] = max(counts["run_max_bytes"], length)
            # FRAG_RUN_L: data header (16), index, count, run_count (16-bit); FRAG_RUN: 8-bit fields.
            units = (struct.unpack("<H", body[20:22])[0] if payload[0] == ord("k") else
                     struct.unpack(">H", body[20:22])[0]) if kind == 14 else body[18]
            counts["run_max_units"] = max(counts["run_max_units"], units)
        else:
            counts["mtu_first"] += 1
    print(" ".join(f"{k}={v}" for k, v in counts.items()))


if __name__ == "__main__":
    main()
