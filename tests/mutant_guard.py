"""Bounded children, refused anchors and a guaranteed restore, for the tests/mutants_*.py harnesses that edit src/ in place.

A mutant's failure mode is unknown by construction: an infinite loop, a log line printed forever, an allocation that
never stops. This PC is shared with other sessions' VMs - on 2026-10-11 a harness that held a looping mutant's output in
memory grew past 11 GB and the global OOM killer took other people's qemu processes before it took the harness. So:

  run_capped()      every child (make, the test binary, sudo ip netns exec ...) runs with RLIMIT_AS (2 GB), a wall
                    clock deadline (its process group killed on expiry), and its output read through a pipe of which at
                    most CAP_BYTES are kept - the head and the tail; the rest is counted and dropped, never held.
  check_anchors()   every mutant's anchor must occur exactly once in the pristine file, checked for ALL mutants before
                    the control runs. An anchor that matches nothing is a mutant that cannot fail; one that matches
                    twice mutates code nobody chose. Either refuses the whole sweep (exit 2).
  SourceGuard       backs up the files a sweep edits, restores them in a finally AND on SIGINT/SIGTERM/SIGHUP (signals
                    blocked while restoring), and gives the restored file a fresh mtime: shutil.copy2() kept the old
                    one, so make reused objects compiled from the mutant (tests/mutants_shm_stage1.py, 2026-09-29).
                    It refuses to start on a file git already reports modified - most likely a mutant a SIGKILLed sweep
                    left behind, which would otherwise be backed up as "the original" (MUTANTS_DIRTY_OK=1 overrides).
  killed_by()       the test functions whose EXPECT_* failed, from their `file.c:LINE:` lines: which test killed it.
  usage_line()      the largest child RSS and the largest child output seen, for the sweep's report.

SIGKILL cannot be caught; after one, `git status` shows the mutant and the next sweep refuses to start on it.
"""
import collections
import os
import pathlib
import re
import resource
import selectors
import shutil
import signal
import subprocess
import sys
import tempfile
import time

MEMORY_BYTES = 2 << 30  # per child process (RLIMIT_AS)
CAP_BYTES = 256 << 10  # kept of a child's combined stdout+stderr: half from the head, half from the tail
_largest_output = 0


class Result:
    def __init__(self, returncode, output, total_bytes, timed_out):
        self.returncode = returncode  # negative: killed by that signal; None: could not be reaped
        self.output = output  # str, at most CAP_BYTES of it plus a marker where bytes were dropped
        self.total_bytes = total_bytes
        self.timed_out = timed_out


def _limit():
    resource.setrlimit(resource.RLIMIT_AS, (MEMORY_BYTES, MEMORY_BYTES))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))  # a crashing mutant must not leave a 2 GB core per run


def be_nice(level=10):
    """Lowers this sweep's priority (children inherit it): it shares the PC with other sessions' builds and VMs."""
    current = os.nice(0)
    if current < level:
        os.nice(level - current)


def run_capped(argv, timeout, cwd=None, env=None, cap=CAP_BYTES):
    """Runs argv bounded in memory, time and kept output. Never raises for the child's behaviour."""
    global _largest_output
    proc = subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, start_new_session=True, preexec_fn=_limit)
    head = bytearray()
    tail = collections.deque()
    tail_bytes = 0
    total = 0
    timed_out = False
    deadline = time.monotonic() + timeout
    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)
    try:
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                timed_out = True
                break
            if not selector.select(timeout=min(left, 1.0)):
                continue
            chunk = os.read(proc.stdout.fileno(), 65536)
            if not chunk:
                break  # EOF: every writer closed its end
            total += len(chunk)
            room = cap // 2 - len(head)
            if room > 0:
                head += chunk[:room]
                chunk = chunk[room:]
            if chunk:
                tail.append(chunk)
                tail_bytes += len(chunk)
                while tail and tail_bytes - len(tail[0]) >= cap // 2:
                    tail_bytes -= len(tail.popleft())
    except BaseException:
        _kill_group(proc)  # interrupted (SourceGuard's signal): the child is in its own session, so take it down too
        raise
    finally:
        selector.close()
        if timed_out:
            _kill_group(proc)
        proc.stdout.close()
    try:
        returncode = proc.wait(timeout=30 if not timed_out else 10)
    except subprocess.TimeoutExpired:
        # A child of another uid (sudo) outlived the kill: say so rather than wait for it unbounded.
        _kill_group(proc)
        returncode = None
    _largest_output = max(_largest_output, total)
    kept = bytes(head)
    tail_data = b"".join(tail)[-(cap // 2):] if tail else b""
    dropped = total - len(kept) - len(tail_data)
    if dropped > 0:
        kept += f"\n[... {dropped} bytes dropped ...]\n".encode()
    kept += tail_data
    return Result(returncode, kept.decode(errors="replace"), total, timed_out)


def _kill_group(proc):
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        try:
            proc.kill()
        except (ProcessLookupError, PermissionError):
            pass


_FAILURE = re.compile(r"([\w./-]*?(\w+\.c)):(\d+):")
_VERDICT = re.compile(r"^(\w+): (FAIL|SETUP FAILED|NO DRAIN) - (.*)$", re.MULTILINE)
_FUNCTION = re.compile(r"^(?:static\s+)?(?:void|int|bool)\s+(\w+)\s*\(")


def killed_by(output, root, limit=6):
    """The test functions whose checks failed, from the `file.c:LINE:` lines tests/test_common.h's EXPECT_* print:
    each line mapped to the function it sits in. Says which test killed a mutant, not only that one did."""
    found = []
    functions = {}
    for match in _FAILURE.finditer(output):
        name, line = match.group(2), int(match.group(3))
        if name not in functions:
            path = next((p for p in (pathlib.Path(root) / "tests" / name, pathlib.Path(root) / "platform/linux" / name)
                         if p.is_file()), None)
            starts = []
            if path is not None:
                for number, text in enumerate(path.read_text(errors="replace").splitlines(), 1):
                    hit = _FUNCTION.match(text)
                    if hit:
                        starts.append((number, hit.group(1)))
            functions[name] = starts
        owner = [fn for start, fn in functions[name] if start <= line]
        label = f"{name}:{owner[-1]}" if owner else f"{name}:{line}"
        if label not in found:
            found.append(label)
            if len(found) == limit:
                return found
    # The HAL checks (platform/linux/rx_*_check.c) print a verdict line of their own instead: "<check>: FAIL - <what>".
    # SETUP FAILED is shown as such - a mutant that breaks the check's own setup is caught, but not by the check proper.
    for match in _VERDICT.finditer(output):
        label = f"{match.group(1)}: {match.group(2)} - {match.group(3)[:100]}"
        if label not in found:
            found.append(label)
            if len(found) == limit:
                break
    return found


def usage_line():
    peak_kb = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    return (f"usage: peak child RSS {peak_kb / 1024:.1f} MB (limit {MEMORY_BYTES >> 20} MB address space), "
            f"largest child output {_largest_output} bytes (kept at most {CAP_BYTES})")


def check_anchors(root, anchors):
    """anchors: (name, relative path, old, new). Exits 2, before anything runs, unless every old occurs exactly once."""
    bad = []
    texts = {}
    for name, rel, old, new in anchors:
        if rel not in texts:
            texts[rel] = (pathlib.Path(root) / rel).read_text()
        count = texts[rel].count(old)
        if count != 1:
            bad.append(f"  {name}: anchor occurs {count} times in {rel}, not once")
        elif old == new:
            bad.append(f"  {name}: the replacement is the anchor itself")
    if bad:
        print("REFUSED: these mutants cannot be applied as written - re-anchor them to the current source:")
        print("\n".join(bad))
        sys.exit(2)


class SourceGuard:
    """with SourceGuard(root, ["src/tickle.c"]) as guard: ... guard.restore() after each mutant."""

    SIGNALS = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)

    def __init__(self, root, paths):
        self.root = pathlib.Path(root)
        self.paths = sorted(set(paths))
        self.backup = None
        self.previous = {}

    def __enter__(self):
        if os.environ.get("MUTANTS_DIRTY_OK") != "1":
            dirty = subprocess.run(["git", "-C", str(self.root), "status", "--porcelain", "--", *self.paths],
                                   capture_output=True, text=True, check=True, timeout=60).stdout.strip()
            if dirty:
                print(f"REFUSED: git reports these files modified - a mutant left by a killed sweep?\n{dirty}\n"
                      "Inspect them (git diff); MUTANTS_DIRTY_OK=1 runs against them as they are.")
                sys.exit(2)
        self.backup = pathlib.Path(tempfile.mkdtemp(prefix="mutants_backup_", dir=os.environ.get("TMPDIR")))
        for rel in self.paths:
            shutil.copy(self.root / rel, self.backup / rel.replace("/", "_"))
        for sig in self.SIGNALS:
            self.previous[sig] = signal.signal(sig, self._on_signal)
        return self

    def _on_signal(self, signum, _frame):
        # Unwinds through every finally (and __exit__), which restores; the sweep's exit status says it was cut short.
        raise SystemExit(128 + signum)

    def original(self, rel):
        return (self.backup / rel.replace("/", "_")).read_text()

    def restore(self):
        blocked = signal.pthread_sigmask(signal.SIG_BLOCK, self.SIGNALS)
        try:
            for rel in self.paths:
                target = self.root / rel
                shutil.copy(self.backup / rel.replace("/", "_"), target)  # copy(), not copy2(): no old mtime
                os.utime(target)  # newer than any object built from the mutant, so make rebuilds
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, blocked)

    def __exit__(self, *_exc):
        try:
            self.restore()
            shutil.rmtree(self.backup, ignore_errors=True)
        finally:
            for sig, handler in self.previous.items():
                signal.signal(sig, handler)
        return False
