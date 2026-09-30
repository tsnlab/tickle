#!/usr/bin/env python3
"""A result file's name must not claim a build it did not measure.

Found by hand on 2026-09-30: of 190 files in examples/perf_hil/results, two carried the PREVIOUS run's
SHA in their name while their header recorded the build actually measured - the block-wait file was named
ea910ae7 and had measured 934f90de, the poll-wait file was named 934f90de and had measured bbc783ee. Both
are cited in COMPARISON.md as the provenance of rows. A reader checking which build produced a row got a
different commit, and nothing failed.

The rule is deliberately narrow, because a narrow rule that always decides beats a broad one that has to be
argued with:

    if the FILENAME names a commit and the HEADER names a commit, they must be the same commit.

Out of scope by design, so that 150 older files do not have to be rewritten tonight:
  - a name with no SHA. Many results are named by experiment and date, and that is fine.
  - a header with no SHA. Harnesses that do not print one are a separate gap; naming them here would turn
    this into a nag instead of a check.
  - an all-digit token such as 20260924. Those are dates in names like tickle_comparison_resweep_20260924-
    225439.txt, and they are valid hex, so the rule is "a SHA candidate must contain a hex letter". An
    all-digit short SHA would be missed; that is a known and accepted hole, stated rather than hidden.

Local gate only, and deliberately not a CI step - the same reason check-doc-shas is local. It resolves
abbreviated commit ids, and CI checks out at depth 1, so every older SHA would come back "not a commit"
and the gate would be red for a reason that has nothing to do with the commit under test.

DO NOT MAKE THIS CHECK CLEVERER. It exists because a human typed one SHA where another belonged. Every
extra inference it makes is another way for it to be wrong about a file it does not understand.
"""
import re
import subprocess
import sys
from pathlib import Path

RESULTS = Path("examples/perf_hil/results")
# A SHA in a header: the harnesses write "SHA <hex>" or "TickLE <hex>" on their first lines.
HEADER_SHA = re.compile(r"\b(?:SHA|TickLE)\s+([0-9a-f]{8,40})\b")
# A SHA candidate in a filename: 8-40 hex characters containing at least one letter, between separators.
NAME_SHA = re.compile(r"(?<![0-9a-zA-Z])((?=[0-9a-f]*[a-f])[0-9a-f]{8,40})(?![0-9a-zA-Z])")


def commit_of(sha: str):
    """The full commit id this abbreviation resolves to, or None if it is not a commit here."""
    r = subprocess.run(["git", "rev-parse", "--verify", "--quiet", f"{sha}^{{commit}}"],
                       capture_output=True, text=True)
    return r.stdout.strip() or None


def main() -> int:
    if not RESULTS.is_dir():
        print(f"{RESULTS} is not a directory - nothing to check")
        return 0
    failures, checked = [], 0
    for path in sorted(RESULTS.glob("*.txt")):
        name_candidates = [m.group(1) for m in NAME_SHA.finditer(path.stem)]
        if not name_candidates:
            continue
        try:
            head = "".join(path.open(errors="replace").readlines()[:12])
        except OSError as exc:
            failures.append(f"{path.name}: cannot be read ({exc}) - a file that cannot be checked is"
                            " reported, not skipped")
            continue
        m = HEADER_SHA.search(head)
        if not m:
            continue
        header_commit = commit_of(m.group(1))
        if header_commit is None:
            failures.append(f"{path.name}: header names {m.group(1)}, which is not a commit in this"
                            " repository. Either the build was never pushed or the id is wrong; both make"
                            " the row it backs unverifiable.")
            continue
        # The name is right if ANY of its SHA-shaped tokens is the measured commit: names legitimately
        # carry more than one (an A/B pair names both arms).
        resolved = {c: commit_of(c) for c in name_candidates}
        if header_commit in resolved.values():
            checked += 1
            continue
        unknown = [c for c, v in resolved.items() if v is None]
        others = [c[:8] for c, v in resolved.items() if v is not None]
        detail = f"header measured {header_commit[:8]}"
        if others:
            detail += f", but the name says {', '.join(others)}"
        if unknown:
            detail += f"; {', '.join(unknown)} in the name is not a commit here"
        failures.append(f"{path.name}: {detail}.")
    for f in failures:
        print(f"FAIL {f}")
    print(f"checked {checked} result file(s) whose name and header both name a commit;"
          f" {len(failures)} disagreed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
