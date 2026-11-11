import gzip
import struct
import sys
from collections import Counter, defaultdict

# 802.11 pcap (linktype 105, no radiotap): per transmitter, MAC attempts per data frame from sequence number + retry bit.
path = sys.argv[1]
f = gzip.open(path, "rb")
hdr = f.read(24)
magic = struct.unpack("<I", hdr[:4])[0]
assert magic == 0xA1B2C3D4, hex(magic)
link = struct.unpack("<I", hdr[20:24])[0]
assert link == 105, link

last = {}  # transmitter -> (seq, attempts)
att = {"down": Counter(), "up": Counter()}
frames = Counter()
retry_frames = Counter()
tx_count = Counter()
first_ts = None
last_ts = 0.0
# time series: per 1 s bin, data frames and retry-flagged frames (all data frames)
bins_n = defaultdict(int)
bins_r = defaultdict(int)
while True:
    ph = f.read(16)
    if len(ph) < 16:
        break
    sec, usec, cap, _ = struct.unpack("<IIII", ph)
    data = f.read(cap)
    if len(data) < 24:
        continue
    t = sec + usec / 1e6
    if first_ts is None:
        first_ts = t
    last_ts = t
    b0, b1 = data[0], data[1]
    ftype = (b0 >> 2) & 3
    if ftype != 2:
        continue
    to_ds = b1 & 1
    from_ds = (b1 >> 1) & 1
    retry = (b1 >> 3) & 1
    tx = data[10:16]
    sc = struct.unpack("<H", data[22:24])[0]
    seq = sc >> 4
    kind = "down" if from_ds and not to_ds else ("up" if to_ds and not from_ds else None)
    if kind is None:
        continue
    frames[kind] += 1
    tx_count[tx] += 1
    if retry:
        retry_frames[kind] += 1
    b = int(t - first_ts)
    bins_n[(kind, b)] += 1
    bins_r[(kind, b)] += retry
    prev = last.get(tx)
    if prev is not None and prev[0] == seq and retry:
        last[tx] = (seq, prev[1] + 1)
    else:
        if prev is not None:
            att[kind][min(prev[1], 8)] += 1
        last[tx] = (seq, 1)

print(path.split("/")[-1], "duration %.0f s" % (last_ts - first_ts))
for kind in ("down", "up"):
    n = frames[kind]
    if not n:
        continue
    tot = sum(att[kind].values())
    mean_att = sum(k * v for k, v in att[kind].items()) / tot if tot else 0
    print(" ", kind, "data frames", n, "retry-flag %.1f%%" % (100.0 * retry_frames[kind] / n),
          "msdus", tot, "mean attempts %.2f" % mean_att,
          "attempts:count", sorted(att[kind].items()))
# per-second retry rate spread (down + up)
rates = []
for b in range(int(last_ts - first_ts) + 1):
    n = bins_n[("down", b)] + bins_n[("up", b)]
    if n >= 20:
        r = bins_r[("down", b)] + bins_r[("up", b)]
        rates.append(r / n)
rates.sort()
if rates:
    q = lambda p: rates[int(p * (len(rates) - 1))]
    print("  per-second retry rate (seconds with >=20 data frames: %d): p10 %.2f p50 %.2f p90 %.2f p99 %.2f max %.2f"
          % (len(rates), q(0.1), q(0.5), q(0.9), q(0.99), rates[-1]))
