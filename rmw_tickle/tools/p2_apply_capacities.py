#!/usr/bin/env python3
"""Apply a capacities proposal (p2_capacities_proposal.tsv) to copies of ROS 2 interface packages
as '# @capacity N' annotations, so p2_inventory.py can check that the proposal actually makes each
interface generate and fit.

Research tool, not the shipping mechanism: TickLE Dev's --capacities file replaces in-.msg
annotations for real builds. The annotation is used here only because the generator already
understands it, so the proposal can be validated today.

Fails loudly if a row names a field that has no unbounded-array line to annotate. A silent miss
would leave the proposal looking complete while the generator never saw that capacity.

Usage: p2_apply_capacities.py PROPOSAL.tsv OUT_DIR SRC_ROOT [SRC_ROOT ...]
"""
import pathlib
import re
import shutil
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from p2_inventory import packages  # noqa: E402


def main(argv):
    proposal, out = pathlib.Path(argv[1]), pathlib.Path(argv[2])
    pkgs = packages(argv[3:])
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    for name, path in pkgs.items():
        shutil.copytree(path, out / name, ignore=shutil.ignore_patterns("test", ".git"))

    missing = []
    for line in proposal.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        cols = line.split("\t")
        iface, field, cap = cols[0], cols[1], int(cols[2])
        pkg, sub, name = iface.split("/")
        f = out / pkg / sub / f"{name}.{sub}"
        if not f.is_file():
            missing.append(f"{iface}: no such file")
            continue
        pat = re.compile(rf"^(\s*[\w/]+(?:<=\d+)?\[\]\s+{re.escape(field)})(\s*)(#.*)?$")
        lines = f.read_text(encoding="utf-8").splitlines()
        hits = 0
        for i, text in enumerate(lines):
            m = pat.match(text)
            if m:
                lines[i] = f"{m.group(1)}  # @capacity {cap}"
                hits += 1
        if hits != 1:
            missing.append(f"{iface}.{field}: {hits} matching unbounded-array lines (expected 1)")
            continue
        f.write_text("\n".join(lines) + "\n", encoding="utf-8")
    if missing:
        print("proposal rows that did not apply:", *missing, sep="\n  ", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main(sys.argv)
