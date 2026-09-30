#!/usr/bin/env python3
"""Show that check_results_provenance.py decides, by giving it files it must reject and files it must not.

The check passed the moment it was written, on a tree that had just been repaired. That proves nothing: the
two files it was written for had already been renamed. So each case below is a file the checker has to
disagree with, plus the controls that would make it a nag if it fired on them.

Run from the repository root: python3 tests/mutants_check_results_provenance.py
"""
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

CHECK = Path(".github/scripts/check_results_provenance.py")
RESULTS = Path("examples/perf_hil/results")


def head_of(path: Path) -> str:
    return "".join(path.open(errors="replace").readlines()[:12])


def run_in(tree: Path):
    r = subprocess.run([sys.executable, str(CHECK.resolve())], cwd=tree, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def make_tree(base: Path, files):
    """A throwaway checkout-shaped tree: the real .git (so SHAs resolve) plus only the files given."""
    tree = base / "tree"
    (tree / RESULTS).mkdir(parents=True)
    (tree / ".github/scripts").mkdir(parents=True)
    shutil.copy(CHECK, tree / CHECK)
    (tree / ".git").symlink_to(Path(".git").resolve())
    for name, text in files:
        (tree / RESULTS / name).write_text(text)
    return tree


def main() -> int:
    real = sorted(RESULTS.glob("*.txt"))
    donor = next((p for p in real if re.search(r"\bSHA [0-9a-f]{8,40}\b", head_of(p))), None)
    if donor is None:
        print("no result file carries a 'SHA <hex>' header - cannot build the cases from real data")
        return 1
    head = head_of(donor)
    measured = re.search(r"\bSHA ([0-9a-f]{8,40})\b", head).group(1)[:8]
    other = subprocess.run(["git", "rev-parse", "--short=8", f"{measured}~1"],
                           capture_output=True, text=True).stdout.strip()
    if not other or other == measured:
        print("cannot find a second commit to mis-name with")
        return 1

    cases = [
        # (label, files, must_fail)
        ("a name carrying a different commit than the header measured",
         [(f"case_{other}_2026-09-30.txt", head)], True),
        ("a name carrying a SHA-shaped token that is not a commit here",
         [("case_deadbee1_2026-09-30.txt", head)], True),
        ("a header naming a commit this repository does not have",
         [(f"case_{measured}_2026-09-30.txt", re.sub(r"\bSHA [0-9a-f]{8,40}\b", "SHA deadbee1cafe", head))], True),
        # controls: each of these must NOT fail, or the check becomes a nag nobody will keep
        ("the correct name (control)",
         [(f"case_{measured}_2026-09-30.txt", head)], False),
        ("an A/B pair name carrying two commits, one of them the measured one (control)",
         [(f"case_abba_A_{measured}_B_{other}_2026-09-30.txt", head)], False),
        ("a date in the name and no SHA, header has one (control)",
         [("case_resweep_20260924-225439.txt", head)], False),
        ("no SHA anywhere (control)",
         [("case_plain_2026-09-30.txt", "some output\nwith no build id\n")], False),
        ("a header with no SHA but a SHA in the name (control: out of scope by design)",
         [(f"case_{measured}_2026-09-30.txt", "rows only, no header sha\n")], False),
    ]

    bad = 0
    for label, files, must_fail in cases:
        with tempfile.TemporaryDirectory() as td:
            tree = make_tree(Path(td), files)
            code, out = run_in(tree)
        failed = code != 0
        ok = failed == must_fail
        print(f"  {'ok  ' if ok else 'BAD '} {'must reject' if must_fail else 'must accept '}: {label}"
              f" -> {'rejected' if failed else 'accepted'}")
        if not ok:
            bad += 1
            print("    " + out.strip().replace("\n", "\n    "))
    print(f"{len(cases) - bad}/{len(cases)} cases behaved as required")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
