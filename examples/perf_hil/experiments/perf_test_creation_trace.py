#!/usr/bin/env python3
"""Creation-time trace for apex performance_test: apply, revert or check it in a performance_test source tree.

Why (the user's decision of 2026-10-17): latency is the receiver's arrival time minus the data's CREATION time, and in a
fixed-rate cell the creation time is the scheduled publish time (start + k / rate), which does not slip when the send
loop falls behind (the coordinated-omission correction). perf_test stamps a sample with the time read just BEFORE its
publish() call (data_runner.hpp: epoc_time, then m_com.publish), for every rmw alike. A DDS publish() of a large sample
blocks until the sample is in the socket, so when the loop is behind, the NEXT sample is stamped only after that wait:
the wait is outside every later sample's latency. rmw_tickle's publish does not block, its next sample is stamped on
time and waits inside the rmw, inside its latency (docs/RESULTS.md, L4 table, I4s note). Measured from the stamp, the
two are not the same quantity.

What the change does, and only this. With PERF_TEST_CREATION_TRACE=<file> in perf_test's environment it writes one line
per sample to <file>; without it nothing is opened and nothing is written (two steady_clock reads and an assignment per
publish remain). The send loop's pacing, the stamp in the sample and perf_test's own latency statistics are unchanged,
so the old latency stays exactly what it was and is the secondary figure.
  P <stamp> <gap_ns> <id> <realtime_ns>   the publisher, right after publish() returned. stamp = the value perf_test
        put in the sample (its own clock, whichever that is); gap_ns = steady_clock at the stamp minus the time the
        loop was scheduled to publish this sample (the next_run it slept to, or was too late to sleep to); "nan" when
        the rate is 0 (no schedule).
  S <stamp> <lat_ns> <realtime_ns>        the subscriber, in Communicator::add_latency_to_statistics (every
        communicator, the round-trip Main's too): stamp = the sample's own; lat_ns = the latency perf_test itself
        added to its statistics, in ns.
  # ...                                   a header (both clocks at open) and, at a normal exit, "# end lines= dropped=".
The creation latency of a sample is lat_ns + gap_ns (its stamp is gap_ns after its schedule); the send-stamp latency is
lat_ns. Both use perf_test's own clock and its own subtraction, so the trace needs to know neither which clock
perf_test stamps with (steady_clock in this PC's copy, system_clock on the rig per clock_offset_probe.py) nor how the
two hosts' clocks differ: the summary corrects both by the same probe offset, and their difference, gap_ns, is one
host's steady_clock.

perf_test's schedule (data_runner.hpp thread_function, read 2026-10-17 at 5b1035d): sample 1 is published at once,
sample 2 after sleeping to first_run + 1/rate, sample 3 without sleeping (next_run was already first_run + 1/rate),
then sample c at first_run + (c - 2)/rate. The trace takes the target the loop actually slept to (ctl_target), so
samples 2 and 3 share one schedule; the summary's warm-up excludes both.

  apply  <performance_test package dir>   insert the trace (anchored lines, each found exactly once, or nothing is
                                          changed and it exits 1); the originals are kept as <file>.ctl_orig
  revert <performance_test package dir>   restore the originals and remove the header
  check  <performance_test package dir>   exit 0 if applied, 3 if not, 1 if half-applied or the anchors moved
The package dir is the one holding src/data_running/data_runner.hpp (~/rmw_perf_ws/src/performance_test/performance_test).
"""
import sys
from pathlib import Path

MARK = "CTL-CREATION-TRACE"
HEADER_NAME = "src/data_running/creation_trace.hpp"
HEADER = r'''// CTL-CREATION-TRACE: TickLE harness addition, written by examples/perf_hil/experiments/perf_test_creation_trace.py
// (see its docstring). One line per sample to $PERF_TEST_CREATION_TRACE; nothing at all when it is unset.
#ifndef DATA_RUNNING__CREATION_TRACE_HPP_
#define DATA_RUNNING__CREATION_TRACE_HPP_

#include <time.h>

#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace perf_test_ctl
{
inline std::int64_t clock_ns(clockid_t id)
{
  struct timespec ts;
  clock_gettime(id, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

struct Trace
{
  std::FILE * f = nullptr;
  std::uint64_t lines = 0;
  std::uint64_t max_lines = 4000000ULL;
  std::uint64_t dropped = 0;
  std::mutex mu;
  Trace()
  {
    const char * path = std::getenv("PERF_TEST_CREATION_TRACE");
    if (path == nullptr || *path == '\0') {
      return;
    }
    f = std::fopen(path, "w");
    if (f == nullptr) {
      return;
    }
    const char * m = std::getenv("PERF_TEST_CREATION_TRACE_MAX");
    if (m != nullptr && *m != '\0') {
      max_lines = std::strtoull(m, nullptr, 10);
    }
    // Line-buffered: every line is in the file when written, so a perf_test ended by a signal loses only "# end".
    std::setvbuf(f, nullptr, _IOLBF, 1 << 16);
    std::fprintf(
      f, "# ctl creation trace v1 realtime_ns=%" PRId64 " steady_ns=%" PRId64 " max_lines=%" PRIu64 "\n",
      clock_ns(CLOCK_REALTIME), clock_ns(CLOCK_MONOTONIC), max_lines);
  }
  ~Trace()
  {
    if (f != nullptr) {
      std::fprintf(f, "# end lines=%" PRIu64 " dropped=%" PRIu64 "\n", lines, dropped);
      std::fclose(f);
    }
  }
  bool take()
  {
    if (lines >= max_lines) {
      ++dropped;
      return false;
    }
    ++lines;
    return true;
  }
};

inline Trace & trace()
{
  static Trace t;
  return t;
}

// The publisher, right after publish() returned: the sample's stamp, how late that stamp was against the schedule
// (INT64_MIN: no schedule, rate 0), and the sample's id.
inline void trace_pub(std::int64_t stamp, std::int64_t gap_ns, std::uint64_t id)
{
  Trace & t = trace();
  if (t.f == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> g(t.mu);
  if (!t.take()) {
    return;
  }
  if (gap_ns == INT64_MIN) {
    std::fprintf(t.f, "P %" PRId64 " nan %" PRIu64 " %" PRId64 "\n", stamp, id, clock_ns(CLOCK_REALTIME));
  } else {
    std::fprintf(
      t.f, "P %" PRId64 " %" PRId64 " %" PRIu64 " %" PRId64 "\n", stamp, gap_ns, id,
      clock_ns(CLOCK_REALTIME));
  }
}

// The subscriber, where perf_test adds a latency to its statistics: the sample's stamp and that latency in ns.
inline void trace_sub(std::int64_t stamp, double sec_diff)
{
  Trace & t = trace();
  if (t.f == nullptr) {
    return;
  }
  const std::int64_t lat_ns = std::llround(sec_diff * 1e9);
  std::lock_guard<std::mutex> g(t.mu);
  if (!t.take()) {
    return;
  }
  std::fprintf(t.f, "S %" PRId64 " %" PRId64 " %" PRId64 "\n", stamp, lat_ns, clock_ns(CLOCK_REALTIME));
}
}  // namespace perf_test_ctl

#endif  // DATA_RUNNING__CREATION_TRACE_HPP_
'''

# (file, anchor, match mode, where, inserted lines). Anchors are compared stripped; "eq" whole line, "start" prefix.
EDITS = [
    ("src/data_running/data_runner.hpp", '#include "../utilities/spin_lock.hpp"', "eq", "after",
     ['#include "creation_trace.hpp"  // CTL-CREATION-TRACE']),
    ("src/data_running/data_runner.hpp", "auto first_run = ", "start", "after",
     ["    // CTL-CREATION-TRACE: the time the loop is scheduled to publish the next sample (start + k / rate).",
      "    std::chrono::steady_clock::time_point ctl_target = std::chrono::steady_clock::now();"]),
    ("src/data_running/data_runner.hpp", "std::chrono::nanoseconds epoc_time =", "eq", "before",
     ["        const auto ctl_stamp_steady = std::chrono::steady_clock::now();  // CTL-CREATION-TRACE"]),
    ("src/data_running/data_runner.hpp", "m_com.publish(*data, epoc_time);", "eq", "after",
     ["        perf_test_ctl::trace_pub(  // CTL-CREATION-TRACE",
      "          epoc_time.count(),",
      "          m_ec.rate() > 0 ? std::chrono::duration_cast<std::chrono::nanoseconds>(",
      "            ctl_stamp_steady - ctl_target).count() : INT64_MIN,",
      "          static_cast<std::uint64_t>(data->id));"]),
    ("src/data_running/data_runner.hpp", "next_run = first_run + loop_counter *", "start", "before",
     ["      ctl_target = next_run;  // CTL-CREATION-TRACE: the target this iteration slept to, or was too late for"]),
    ("src/communication_abstractions/communicator.cpp", '#include "communicator.hpp"', "eq", "after",
     ['#include "../data_running/creation_trace.hpp"  // CTL-CREATION-TRACE']),
    ("src/communication_abstractions/communicator.cpp", "m_latency.add_sample(sec_diff);", "eq", "before",
     ["  perf_test_ctl::trace_sub(sample_timestamp, sec_diff);  // CTL-CREATION-TRACE"]),
]
FILES = sorted({e[0] for e in EDITS})


def state(pkg):
    marked = [f for f in FILES if MARK in (pkg / f).read_text()]
    hdr = (pkg / HEADER_NAME).exists()
    if not marked and not hdr:
        return "clean"
    if len(marked) == len(FILES) and hdr:
        return "applied"
    return "half"


def apply(pkg):
    st = state(pkg)
    if st == "applied":
        print(f"already applied in {pkg}")
        return 0
    if st == "half":
        print(f"REFUSED: {pkg} is half-applied; revert first", file=sys.stderr)
        return 1
    texts = {f: (pkg / f).read_text().split("\n") for f in FILES}
    plan = []
    for f, anchor, mode, where, lines in EDITS:
        hits = [i for i, ln in enumerate(texts[f])
                if (ln.strip() == anchor if mode == "eq" else ln.strip().startswith(anchor))]
        if len(hits) != 1:
            print(f"REFUSED: anchor {anchor!r} found {len(hits)} times in {f} (want 1); nothing changed",
                  file=sys.stderr)
            return 1
        plan.append((f, hits[0], where, lines))
    # Insert bottom-up per file so earlier indexes stay valid.
    for f, i, where, lines in sorted(plan, key=lambda p: (p[0], -p[1])):
        at = i + 1 if where == "after" else i
        texts[f][at:at] = lines
    for f in FILES:
        p = pkg / f
        orig = p.with_name(p.name + ".ctl_orig")
        orig.write_text(p.read_text())
        p.write_text("\n".join(texts[f]))
    (pkg / HEADER_NAME).write_text(HEADER)
    print(f"applied in {pkg}: {len(EDITS)} insertions in {', '.join(FILES)} + {HEADER_NAME}")
    return 0


def revert(pkg):
    st = state(pkg)
    if st == "clean":
        print(f"not applied in {pkg}")
        return 0
    bad = 0
    for f in FILES:
        p = pkg / f
        orig = p.with_name(p.name + ".ctl_orig")
        if orig.exists():
            p.write_text(orig.read_text())
            orig.unlink()
        elif MARK in p.read_text():
            print(f"cannot revert {f}: no {orig.name}", file=sys.stderr)
            bad = 1
    h = pkg / HEADER_NAME
    if h.exists():
        h.unlink()
    print(f"reverted in {pkg}" + (" (incompletely)" if bad else ""))
    return bad


def main(argv):
    if len(argv) != 3 or argv[1] not in ("apply", "revert", "check"):
        print(__doc__)
        return 2
    pkg = Path(argv[2])
    if not (pkg / "src/data_running/data_runner.hpp").exists():
        print(f"no src/data_running/data_runner.hpp under {pkg}", file=sys.stderr)
        return 2
    if argv[1] == "apply":
        return apply(pkg)
    if argv[1] == "revert":
        return revert(pkg)
    st = state(pkg)
    print(f"creation trace: {st} in {pkg}")
    return {"applied": 0, "clean": 3}.get(st, 1)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
