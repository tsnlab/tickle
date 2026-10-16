"""Tells a mutant the build compiled from one it left out, for the tests/mutants_*.py harnesses.

A mutant that edits code under a preprocessor branch the build leaves out (or a comment) builds binaries identical to the
control's. It can neither die nor survive, and without this check it reads as a survivor - or, worse, a mutant that was
meant to die and "did" for another reason is never noticed to be aimed at nothing. tests/mutants_large_sample.py's "a lent
buffer handed back" sat in the #else of LARGE_DIRECT that way (2026-10-15).

Usage, from a harness run from the repository root (its directory, tests/, is on sys.path):

    from mutant_digest import NOT_COMPILED, code_digest, unchanged
    control = code_digest(binary)                  # after the control build, of every binary a mutant is judged by
    ...
    if unchanged(control, binary):                 # after the mutant's build, the same binaries in the same order
        return NOT_COMPILED, ""

A harness that builds several binaries per mutant passes all of them, in the same order both times: the mutant is
NOT_COMPILED only if every one of them is the control's. A harness whose binaries are built with different flags keys
each control digest by the binary it belongs to.
"""
import hashlib
import os
import pathlib
import subprocess
import tempfile

NOT_COMPILED = "NOT_COMPILED"


def code_digest(*paths):
    """sha256 over the binaries' code and data, debug sections aside, in the order given.

    Debug sections are left out because they carry line numbers: a mutant that moves code without changing it would
    otherwise look compiled. Everything the compiler kept - code, data, symbols - is in the digest.
    """
    if not paths:
        raise ValueError("code_digest needs at least one binary")
    total = hashlib.sha256()
    for path in paths:
        fd, stripped = tempfile.mkstemp(prefix="mutant_strip_")
        os.close(fd)
        try:
            subprocess.run(["objcopy", "--strip-debug", str(path), stripped], check=True, capture_output=True)
            total.update(hashlib.sha256(pathlib.Path(stripped).read_bytes()).digest())
        finally:
            os.unlink(stripped)
    return total.hexdigest()


def unchanged(control, *paths):
    """True when `control` is a digest and the binaries at `paths` have it: the mutant changed nothing compiled."""
    return control is not None and code_digest(*paths) == control


def label(verdict, caught, killed="ok", missed="SURVIVED"):
    """The harness's own word for a caught or a missed mutant, and DEAD for one that was never compiled."""
    if caught:
        return killed
    return "DEAD" if verdict == NOT_COMPILED else missed
