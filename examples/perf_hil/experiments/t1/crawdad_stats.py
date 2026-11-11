import os
import sys
from collections import Counter

root = sys.argv[1]
N = 301  # sequence numbers 0..300 per file
for level in ["dbm0", "dbm-5", "dbm-10", "dbm-15", "dbm-20"]:
    base = os.path.join(root, level)
    files = 0
    sent = 0
    lost = 0
    bursts = Counter()
    lossy_files = 0
    per_file = []
    for dirpath, _, names in os.walk(base):
        for nm in names:
            seqs = set()
            with open(os.path.join(dirpath, nm)) as f:
                for line in f:
                    p = line.split()
                    if len(p) == 2:
                        try:
                            seqs.add(int(p[0]))
                        except ValueError:
                            pass
            if len(seqs) < 100:  # too short to be a link trace
                continue
            files += 1
            run = 0
            nl = 0
            for s in range(N):
                if s in seqs:
                    if run:
                        bursts[run] += 1
                        run = 0
                else:
                    run += 1
                    nl += 1
            if run:
                bursts[run] += 1
            sent += N
            lost += nl
            per_file.append(nl / N)
            if nl:
                lossy_files += 1
    nb = sum(bursts.values())
    mean_b = sum(k * v for k, v in bursts.items()) / nb if nb else 0
    top = sorted(bursts.items())[:8]
    mx = max(bursts) if bursts else 0
    print(level, "files", files, "loss %.3f%%" % (100.0 * lost / sent), "lossy_files", lossy_files,
          "bursts", nb, "mean_burst %.2f" % mean_b, "max_burst", mx, "len:count", top)
