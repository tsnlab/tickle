#!/usr/bin/env python3
"""rx_perf_read.py OUT [OUT ...] [--preflight] | --selftest

Where the rig's subscriber spends its receive CPU per MB: rmw_tickle (hybrid transport) against Fast DDS as shipped, side by
side in one rmw_keepall_rig.sh cell, 0% loss. Reads the invocations ~/rig_queue_rx_perf.sh makes (rmw_keepall_rig.sh
RX_PROFILE=plain|stat|record|strace, one OUT per cell and mode). Written 2026-10-12 before any rig run of it.

WHY. On the Pi 5 rig the subscriber's CPU per MB at 0% loss reads rmw_tickle 2.49-2.70 ms/MB against Fast DDS 2.11-2.36
(13-16% behind; hybridDrain 2026-10-12, perf_test's rusage over the steady seconds = largemsg_rig_summary.py cpu_sub). The
PC profile did not reproduce it (TickLE cheaper there) and named a separate UDP checksum pass (udp_poll ->
first_packet_length -> __skb_checksum_complete, because TickLE polls before it reads) as 45% of TickLE's PC receive cost.
The user keeps checksums unless beating the vendors otherwise becomes too hard. This asks the Pi.

WHAT EACH RUN GIVES (rx_window.sh, a window RX_DELAY s after the publisher's launch, RX_WIN s long, on perf_test's own PID):
  every mode  window MB (perf_test's delivered samples interpolated at the window's wall ends; payload MB of 1e6 B, the
              unit of cpu_sub), the process's run time from schedstat (proc ms/MB), utime+stime, voluntary and
              involuntary context switches per MB, the host's /proc/stat (busy and softirq ms/MB over all CPUs: work the
              kernel did for the receive but charged to no process or to another), NET_RX softirqs per MB, eth0 rx bytes
              per MB (the wire), and perf_test's own cpu_sub (the published metric).
  stat        perf stat --per-thread: instructions and cycles, user and kernel, per MB; context switches per MB.
  record      perf record -g -e cycles: every sample put in ONE category by the fixed rules below; a category's ms/MB =
              its share of the arm's sampled cycles x the arm's PLAIN-mode proc ms/MB (the uninstrumented figure).
  strace      strace -f -c: system calls per MB by name, and their errors (EAGAIN = an empty read). Attached for the
              window where Yama allows it; otherwise (strace_wrap) the whole life, start-up included, labelled so.

CATEGORIES (record; fixed before any rig run). A sample is KERNEL when its leaf address is a kernel one (>= 0xffff0000...).
  Kernel symbols are unresolved where kptr_restrict != 0 (the rig: ci is not root), so kernel samples are split by the
  first user frames of their call chain - the system call they were made under - not by kernel function:
    K_RECV    under recvmsg/recvmmsg/recvfrom/read: the copy to user (with checksum fused where the kernel can), the
              reassembled datagram's free
    K_POLL    under epoll/poll/io_uring_enter: TickLE's separate checksum pass lands here (udp_poll), and wake-up waits
    K_SEND    under send*/write: ACKs, NACKs, HEARTBEATs
    K_SYNC    under futex/nanosleep/sched_yield: wakes between threads
    K_OTHER   under another system call (clock, mmap, open, ...)
    K_INTR    user code interrupted (no system call in the first user frames): IRQ, NET_RX softirq - IP reassembly,
              GRO, the IP/UDP receive - and page faults, charged to this process because it was running
    K_NOUSER  no user frame (a kernel thread of the process, e.g. io_uring's iou-wrk, or a truncated chain)
  A USER sample by its leaf (an inlined memcpy counts as the copy it is):
    U_COPY    memcpy/memmove/memset (any library)
    U_DECODE  a typesupport library, or a symbol named deserialize/decode/cdr/take
    U_MW      the rest of the middleware: librmw_tickle (TickLE's core is compiled into it), libfastrtps/libfastdds,
              libfastcdr, librmw_fastrtps*
    U_APP     perf_test itself (sync_reset - the harness's own loop - and its callback; both arms run the same binary)
    U_ROS     rcl, rclcpp, rmw, rcutils, libstdc++ and the rest of ROS
    U_LIBC    the rest of libc and ld.so (malloc, pthread, the syscall wrappers' user side)
    U_OTHER   anything else ([vdso], unknown)
  The breakdown table groups them as asked: kernel receive (K_RECV + K_POLL + K_INTR + K_NOUSER), syscalls and wakes
  (K_SEND + K_SYNC + K_OTHER, with calls and voluntary switches per MB), copies (U_COPY), rmw decode/take (U_DECODE),
  middleware bookkeeping (U_MW), ROS + app + libc (U_ROS + U_APP + U_LIBC + U_OTHER), and outside the process (the host's
  busy ms/MB minus the process's).

VOID (a run is left out, with its reason): largemsg_rig_summary.py's VOID (the arm's rmw not in perf_test's maps); the
  window helper said RXWIN_VOID, or perf_test was not alive at the window's end, or the instrument failed (rc != 0, or its
  output missing/empty, or record < 500 samples); window MB outside 90-110% of rate x window x size (not steady); a
  RELIABLE run that lost a sample. In the PC preflight a vendor that delivers nothing is reported, not judged (Fast DDS
  over the PC's veth namespaces delivers nothing - not the rig).

HOW TO READ IT (pre-registered 2026-10-12, before the first rig run; applied below, per cell):
  GAP     from plain: G = median(tickle cpu_sub) - median(fastdds cpu_sub), ms/MB. REPRODUCED when G >= 5% of Fast DDS's
          median AND the two arms' ranges do not overlap. Gw = the same for proc ms/MB over the window.
          Not reproduced: everything is printed, the readings are labelled "descriptive", and nothing is claimed.
  INSTRUMENT  per arm, proc ms/MB under record (and stat) vs plain; > 15% higher is printed as PERTURBED (shares from a
          perturbed arm are still scaled by plain, but say so).
  A delta D(x) = median(tickle) - median(fastdds) of x. A category COUNTS when D >= 0.1 ms/MB (the threshold of
  ~/claude_reports/hybrid_gaps_2026-10-12.md, ranked plan 2) and the arms' ranges do not overlap. DK = D(kernel total),
  DU = D(user total), SYS = tickle's / Fast DDS's system calls per MB (strace, all names), WAKE = the same for voluntary
  context switches per MB (window).
  R0 ACCOUNTING  REPRODUCED and D(host busy ms/MB) < 0.5 x Gw: the host does not spend more for TickLE's MB than for Fast
                 DDS's; the gap is WHERE the kernel charges the receive work (Fast DDS's softirq runs while its threads
                 sleep, TickLE's while it runs), not more work. Next: compare host-wide, not per process.
  R1 KERNEL PER BYTE  DK >= 0.1 and DK >= 0.5 x G and SYS <= 1.2: the same calls cost more kernel work per MB ->
                 checksum / reassembly / drain. Named by the K_ component holding >= half of DK: K_POLL -> the separate
                 checksum pass in udp_poll (what UDP checksum 0 on runs would remove: the user's decision); K_RECV -> the
                 read path (copy, reassembled skb free); K_INTR -> IP reassembly/softirq charged to the process while it
                 runs (more of it, or the same charged here instead of to idle); none -> spread.
  R2 KERNEL PER CALL  DK >= 0.1 and SYS > 1.2: more system calls per MB; the names with the largest per-MB excess and
                 their errors (EAGAIN = drain reads that found nothing) name the drain or poll pattern.
  R3 USER        DU >= 0.1 and DU >= 0.5 x G: our code; the U_ category with the largest positive counting delta names
                 it (U_COPY a copy more, U_DECODE rmw's decode/take, U_MW the core's per-unit bookkeeping, U_APP/U_ROS the
                 executor path - waits/wakes of rclcpp, which then shows in WAKE too).
  R4 WAKES       WAKE > 1.5 and D(K_SYNC) + D(K_POLL) >= 0.1: more sleeps and wakes per MB.
  R5             none of the above: the instruments do not localise the gap (could not look - not "no difference").
  Every reading that holds is printed; PRIMARY is the first in the order R0..R5.
  FALSIFIED "the separate checksum pass is the rig gap": REPRODUCED and D(K_POLL) < 0.05 ms/MB.

PMU MODE (added 2026-10-13, before any rig run of it; ~/rig_jobs/rig_queue_rx_pmu.sh). The question: why does our copy
  into the sample cost more than Fast DDS's for the same bytes (~/claude_reports/rx_perf_gap2_2026-10-13.md sections 5, 7
  item 2 and 9: +0.10/+0.11 ms/MB, no page faults in either arm, so cache or TLB misses on the destination)?
  pmu         perf stat --per-thread on the subscriber with cycles, instructions and the memory events the subscriber's
              host accepted (rmw_keepall_rig.sh RX_PROFILE=pmu probes each; meta.txt pmu_events / pmu_missing). Every
              count is per window MB (payload MB of 1e6 B, as everywhere here), summed over the process's threads.
              Cortex-A76 cache lines are 64 B, so l2d_cache_refill x 64 / 1e6 per MB = bytes refilled into L2 per payload
              byte ("refill bytes/B"): 1.0 is one whole cold pass over the payload.
  An event the host did not accept, or that perf printed as <not counted>/<not supported> in a run, is COULD NOT LOOK
  for every rule that needs it - never a zero. A count perf multiplexed (running < 100% of the window) is scaled by perf;
  the lowest running share is printed beside it and a share < 50% is flagged MULTIPLEXED.
  The rules, per cell, over the pmu runs of each arm (>= 2 usable runs per arm, else PMU NOT READ: could not look). A
  ratio is median(tickle) / median(fastdds) of the per-MB value; "separate" means min(tickle) > max(fastdds):
  P1 DEST COLD   l2d_cache_refill/MB ratio >= 1.3, separate, AND instructions/MB ratio within 1/1.2..1.2: the same work
                 meets colder memory - the copy's destination (or source) is not in L2 for us and is for Fast DDS.
                 With the LL event (ll_cache_miss_rd or l3d_cache_refill) ratio >= 1.3 too it is not in L3 either
                 (buffer rotation, or a footprint over the Pi 5's 2 MB L3); below 1.3 the lines come from L3 (the
                 footprint of several copies of a sample on one core, over its 512 KB L2).
  P2 TLB         dtlb_walk/MB ratio >= 2.0 and separate: page walks on the copy (a 4 KB-paged multi-MB destination).
  P3 MORE WORK   l2d ratio >= 1.3 and separate but instructions ratio > 1.2: refills follow more instructions - more
                 work, not a colder destination.
  P4             none of P1-P3, with l2d, instructions and dtlb_walk all looked at: the PMU does not localise the copy
                 gap to L2 refills or TLB walks (looked - not "could not look"). If any of the three could not be
                 looked at, the reading says COULD NOT LOOK and names it.
  Every one that holds is printed; PMU PRIMARY is the first in the order P1, P2, P3, P4.
  FALSIFIED "our copy destination is colder" (the rank-2 hypothesis): l2d_cache_refill/MB ratio < 1.1 with both arms
  measured. Descriptive only (no rule): l1d refills, mem_access, stall_backend per cycle, the LL ratio, the refill
  bytes/B excess (tickle - fastdds), and per tickle run the subscriber's rx counters (direct_reads: reads in place;
  drain_datagrams) beside its l2d refills, so in-place and not-in-place runs can be told apart.
"""
import gzip
import math
import re
import statistics as st
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from largemsg_rig_summary import SIZES, load  # noqa: E402
from rmw_keepall_rig_summary import rows, sample_bytes, whole_counts  # noqa: E402

TICKLE, VENDOR = "tickle@head", "fastdds"
COUNT_MS = 0.1
REPRO_FRAC = 0.05
PERTURB = 1.15
MIN_SAMPLES = 500
KERNEL_IP = 0xFFFF000000000000
USER_IP_MAX = 1 << 48

K_RULES = (  # first user frames' symbol -> category, first match wins
    ("K_RECV", re.compile(r"recv|^(__libc_|__)?read(v)?$|^__read_nocancel")),
    ("K_POLL", re.compile(r"poll|select|io_uring|epoll")),
    ("K_SEND", re.compile(r"send|^(__libc_|__)?write(v)?$")),
    ("K_SYNC", re.compile(r"futex|nanosleep|sched_yield|pthread_cond|sem_(timed)?wait|lll_lock|pthread_mutex|__lll")),
    ("K_OTHER", re.compile(r"syscall|clock_|ioctl|mmap|munmap|madvise|mprotect|brk|open|close|stat|getrandom|"
                           r"timerfd|eventfd|fcntl|getpid|gettid|prctl|setsockopt|getsockopt|sigaction|sigprocmask")),
)
U_COPY = re.compile(r"^(__)?mem(cpy|move|set)|memcpy|memmove|memset")
U_DECODE_SYM = re.compile(r"deserializ|decode|[Cc]dr|_take|take_|^take|rmw_take")
U_DECODE_DSO = re.compile(r"typesupport|rosidl")
U_MW_DSO = re.compile(r"librmw_tickle|libfastrtps|libfastdds|libfastcdr|librmw_fastrtps|libfoonathan")
U_APP_DSO = re.compile(r"(^|/)perf_test$")
U_ROS_DSO = re.compile(r"librcl|librmw|librcutils|librcpputils|libtracetools|libstdc\+\+|libspdlog|libfmt|libyaml|"
                       r"libament|libclass_loader|librosidl|libtinyxml|/opt/ros/")
U_LIBC_DSO = re.compile(r"libc\.so|libc-|ld-linux|libpthread|libm\.so|libgcc")
KERNEL_CATS = ("K_RECV", "K_POLL", "K_INTR", "K_NOUSER", "K_SEND", "K_SYNC", "K_OTHER")
USER_CATS = ("U_COPY", "U_DECODE", "U_MW", "U_APP", "U_ROS", "U_LIBC", "U_OTHER")
GROUPS = (  # the breakdown table asked for: (label, categories)
    ("kernel receive (K_RECV+K_POLL+K_INTR+K_NOUSER)", ("K_RECV", "K_POLL", "K_INTR", "K_NOUSER")),
    ("  under recv* (copy, skb free)", ("K_RECV",)),
    ("  under poll/epoll/io_uring (TickLE's checksum pass)", ("K_POLL",)),
    ("  interrupted user code (irq/softirq: IP reassembly)", ("K_INTR",)),
    ("  no user frame (kernel threads of the process)", ("K_NOUSER",)),
    ("syscalls and wakes (K_SEND+K_SYNC+K_OTHER)", ("K_SEND", "K_SYNC", "K_OTHER")),
    ("copies (U_COPY: memcpy/memmove/memset)", ("U_COPY",)),
    ("rmw decode/take (U_DECODE)", ("U_DECODE",)),
    ("middleware bookkeeping (U_MW)", ("U_MW",)),
    ("ROS + perf_test + libc (U_ROS+U_APP+U_LIBC+U_OTHER)", ("U_ROS", "U_APP", "U_LIBC", "U_OTHER")),
)


# ---------------------------------------------------------------------------------------------------------- parsers
def kv(text):
    return dict(re.findall(r"(\w+)=(\S+)", text))


def parse_rxwin(path):
    """{'t0': {...}, 't1': {...}, 'tasks': {label: {tid: {...}}}, 'end': {...} or None, 'void': reason or ''}."""
    out = {"t0": None, "t1": None, "tasks": {"t0": {}, "t1": {}}, "end": None, "void": ""}
    try:
        text = Path(path).read_text(errors="replace")
    except OSError:
        out["void"] = "no window output"
        return out
    for line in text.splitlines():
        if line.startswith("SNAP "):
            label = line.split()[1]
            out[label] = kv(line)
        elif line.startswith("TASK "):
            label = line.split()[1]
            d = kv(line)
            out["tasks"].setdefault(label, {})[d.get("tid", "?")] = d
        elif line.startswith("RXWIN_VOID"):
            out["void"] = line
        elif line.startswith("RXWIN "):
            out["end"] = kv(line.split("err=")[0])
    if not out["void"]:
        if out["end"] is None or out["t0"] is None or out["t1"] is None:
            out["void"] = "window output incomplete"
        elif out["end"].get("rc") != "0":
            out["void"] = f"instrument rc={out['end'].get('rc')}"
        elif out["end"].get("alive") != "1":
            out["void"] = "perf_test not alive at the window's end"
    return out


def parse_stat(path):
    """event -> summed count over threads; and per-thread cycles (u+k) by 'comm-tid'."""
    ev, per_thread = {}, {}
    try:
        lines = Path(path).read_text(errors="replace").splitlines()
    except OSError:
        return None, None
    for line in lines:
        if not line or line.startswith("#"):
            continue
        f = line.split(",")
        if len(f) < 4:
            continue
        thread, val, event = f[0], f[1], f[3]
        try:
            v = float(val)
        except ValueError:
            continue  # <not counted> / <not supported>
        ev[event] = ev.get(event, 0.0) + v
        if event.startswith("cycles"):
            per_thread[thread] = per_thread.get(thread, 0.0) + v
    return (ev or None), per_thread


def stat_event_name(event):
    """'armv8_pmuv3_0/l2d_cache_refill/' and 'l2d_cache_refill' are the same event; a :u/:k modifier is kept."""
    m = re.match(r"^[\w.-]+/([^/,]+)/(:\w+)?$", event)
    return (m.group(1) + (m.group(2) or "")).lower() if m else event.lower()


def parse_stat_pmu(path):
    """pmu mode: {event: summed count} over threads, {event: lowest running share %}, {events perf could not count},
    {event: {thread: count}}; (None, ...) when the file is missing or holds no count at all.

    --per-thread prints "<not counted>" for a thread that did not run in the window (seen on the PC preflight: perf_test's
    idle threads, every event). That thread adds nothing and hides nothing. An event is COULD NOT LOOK (in the returned
    set) when perf said "<not supported>" for it, when no thread has a count for it, or when it is "<not counted>" on a
    thread that another event shows running (a count > 0) - that thread's share of it is unknown, not zero."""
    ev, pct, uncounted, per_thread = {}, {}, set(), {}
    try:
        lines = Path(path).read_text(errors="replace").splitlines()
    except OSError:
        return None, {}, set(), {}
    status = {}  # (thread, event) -> count or the "<...>" text
    for line in lines:
        if not line or line.startswith("#"):
            continue
        f = line.split(",")
        if len(f) < 4:
            continue
        thread, val, name = f[0], f[1], stat_event_name(f[3])
        try:
            v = float(val)
        except ValueError:
            status[(thread, name)] = val.strip()
            continue
        status[(thread, name)] = v
        ev[name] = ev.get(name, 0.0) + v
        per_thread.setdefault(name, {})[thread] = per_thread.get(name, {}).get(thread, 0.0) + v
        try:
            share = float(f[5]) if len(f) > 5 and f[5] else 100.0
        except ValueError:
            share = 100.0
        pct[name] = min(pct.get(name, 100.0), share)
    running = {t for (t, _e), v in status.items() if isinstance(v, float) and v > 0}
    for (t, e), v in status.items():
        if isinstance(v, str) and ("supported" in v or t in running):
            uncounted.add(e)
    uncounted |= {e for _t, e in status if e not in ev}
    return (ev or None), pct, uncounted, per_thread


def sub_rx_counters(path):
    """The subscriber's last 'Node N rx:' line (rmw_tickle only): direct_reads, drain_datagrams, ... as strings."""
    out = {}
    try:
        for line in Path(path).read_text(errors="replace").splitlines():
            if re.search(r"Node \d+ rx:", line):
                out = dict(re.findall(r"(\w+)=(\S+)", line.split(" rx:", 1)[1]))
    except OSError:
        pass
    return out


def parse_strace(path):
    """{name: (calls, errors)} from strace -c's table, or None."""
    out = {}
    try:
        lines = Path(path).read_text(errors="replace").splitlines()
    except OSError:
        return None
    for line in lines:
        m = re.match(r"\s*[\d.]+\s+[\d.]+\s+\d+\s+(\d+)\s+(\d+\s+)?(\w+)\s*$", line)
        if m and m.group(3) != "total":
            out[m.group(3)] = (int(m.group(1)), int(m.group(2) or 0))
    return out or None


FRAME_RE = re.compile(r"^\s+([0-9a-f]+)\s+(.*?)\s+\((.*)\)\s*$")


def iter_samples(lines):
    """(comm, period, [(ip, sym, dso)]) per perf script sample (-F comm,tid,period,ip,sym,dso, call chains)."""
    head, chain = None, []
    for line in lines:
        if not line.strip():
            if head is not None:
                yield head[0], head[1], chain
            head, chain = None, []
            continue
        if line[0] not in " \t":
            if head is not None:
                yield head[0], head[1], chain
            parts = line.split()
            try:
                head = (" ".join(parts[:-2]) or "?", int(parts[-1]))
            except (ValueError, IndexError):
                head = None
            chain = []
            continue
        m = FRAME_RE.match(line)
        if m and head is not None:
            chain.append((int(m.group(1), 16), m.group(2), m.group(3)))
    if head is not None:
        yield head[0], head[1], chain


def clean(sym):
    return re.sub(r"\+0x[0-9a-f]+$", "", sym)


def categorize(chain):
    """One category per sample (module docstring, CATEGORIES)."""
    if not chain:
        return "U_OTHER"
    ip, sym, dso = chain[0]
    if ip >= KERNEL_IP:
        user = [(clean(s), d) for i, s, d in chain if 0x1000 < i < USER_IP_MAX]
        if not user:
            return "K_NOUSER"
        # the first three user frames; a specific call (recv, poll, send, sync) wins over a generic wrapper that only
        # matches K_OTHER (__syscall_cancel_arch above epoll_pwait2)
        for cat, rx in K_RULES:
            if any(rx.search(s) for s, _d in user[:3]):
                return cat
        return "K_INTR"
    sym = clean(sym)
    if dso == "inlined":  # the frame that holds the inlined code names the library
        dso = next((d for _i, _s, d in chain[1:] if d != "inlined"), dso)
    if U_COPY.search(sym):
        return "U_COPY"
    if U_DECODE_DSO.search(dso) or U_DECODE_SYM.search(sym):
        return "U_DECODE"
    if U_MW_DSO.search(dso):
        return "U_MW"
    if U_APP_DSO.search(dso):
        return "U_APP"
    if U_ROS_DSO.search(dso):
        return "U_ROS"
    if U_LIBC_DSO.search(dso):
        return "U_LIBC"
    return "U_OTHER"


def parse_script(path):
    """({category: cycles}, samples, {kernel leaf address: cycles}, kernel symbols resolved?, {comm: cycles})."""
    cats, kaddr, comms = {}, {}, {}
    n = 0
    resolved = False
    try:
        fh = gzip.open(path, "rt", errors="replace")
        with fh:
            for comm, period, chain in iter_samples(fh):
                n += 1
                c = categorize(chain)
                cats[c] = cats.get(c, 0) + period
                comms[comm] = comms.get(comm, 0) + period
                if chain and chain[0][0] >= KERNEL_IP:
                    kaddr[chain[0][0]] = kaddr.get(chain[0][0], 0) + period
                    if chain[0][1] not in ("[unknown]", "") and not chain[0][1].startswith("0x"):
                        resolved = True
    except (OSError, EOFError):
        return None, 0, {}, False, {}
    return cats, n, kaddr, resolved, comms


# ---------------------------------------------------------------------------------------------------------- one run
def window_mb(runs, stem, topic, t0_ns, t1_ns):
    """Delivered payload MB between two wall times, perf_test's per-second rows interpolated (module docstring)."""
    sub, _text = rows(runs / f"{stem}_sub.log")
    if not sub:
        return float("nan")
    whole_counts(sub, sample_bytes(topic))
    try:
        start = int((runs / f"{stem}_sub.start").read_text().split()[0]) / 1e9
    except (OSError, ValueError, IndexError):
        return float("nan")
    times = [start + r["T_experiment"] for r in sub]
    n = [r.get("n_received", 0) for r in sub]

    def cum(t):
        total, prev_t = 0.0, start
        for ti, ni in zip(times, n):
            if t <= ti:
                return total + ni * max(0.0, (t - prev_t)) / (ti - prev_t) if ti > prev_t else total
            total += ni
            prev_t = ti
        return total

    return (cum(t1_ns / 1e9) - cum(t0_ns / 1e9)) * SIZES.get(topic, 0) / 1e6


def num(d, k):
    try:
        return float(d[k])
    except (KeyError, TypeError, ValueError):
        return float("nan")


def run_metrics(runs, base, stem, meta):
    """Every per-run metric; base = largemsg_rig_summary's record."""
    mode = meta.get("rx_mode", meta.get("rx_profile", "?"))
    rate = float(meta.get("pub_rate", "0") or 0)
    topic = base["topic"]
    size = SIZES.get(topic, 0)
    m = dict(arm=base["arm"], rep=base["rep"], mode=mode, stem=stem, cpu_sub=base.get("cpu_sub", float("nan")),
             void=base.get("void", ""), recv=base.get("recv", 0))
    why = [m["void"]] if m["void"] else []
    if "--reliable" in meta.get("qos_args", "") and base.get("lost", 0) > 0:
        why.append(f"RELIABLE lost {base['lost']}")
    w = parse_rxwin(runs / f"{stem}_rxwin.txt")
    if w["void"]:
        why.append(w["void"])
    t0, t1 = w["t0"] or {}, w["t1"] or {}
    secs = (num(t1, "wall_ns") - num(t0, "wall_ns")) / 1e9
    mb = window_mb(runs, stem, topic, num(t0, "wall_ns"), num(t1, "wall_ns")) if not w["void"] else float("nan")
    expect = rate * secs * size / 1e6 if rate > 0 else float("nan")
    m.update(win_s=secs, mb=mb, mb_expected=expect)
    if not w["void"] and not (mb == mb and expect == expect and 0.9 * expect <= mb <= 1.1 * expect):
        why.append(f"window MB {mb:.1f} not 90-110% of {expect:.1f} (not steady)")
    clk = num(t0, "clk_tck") or 100.0
    ok = mb == mb and mb > 0

    def per_mb(key, scale=1.0):
        return (num(t1, key) - num(t0, key)) * scale / mb if ok else float("nan")

    m["proc_ms_mb"] = per_mb("task_ns", 1e-6)
    m["rusage_ms_mb"] = ((num(t1, "utime") + num(t1, "stime") - num(t0, "utime") - num(t0, "stime")) / clk * 1e3 / mb
                         if ok else float("nan"))
    busy_keys = ("cpu_user", "cpu_nice", "cpu_system", "cpu_irq", "cpu_softirq", "cpu_steal")
    m["host_busy_ms_mb"] = (sum(num(t1, k) - num(t0, k) for k in busy_keys) / clk * 1e3 / mb) if ok else float("nan")
    m["host_softirq_ms_mb"] = per_mb("cpu_softirq", 1e3 / clk)
    m["host_irq_ms_mb"] = per_mb("cpu_irq", 1e3 / clk)
    m["outside_ms_mb"] = m["host_busy_ms_mb"] - m["proc_ms_mb"]
    m["vol_per_mb"] = per_mb("vol")
    m["nvol_per_mb"] = per_mb("nvol")
    m["net_rx_per_mb"] = per_mb("net_rx_softirqs")
    m["wire_per_payload"] = per_mb("eth0_rx_bytes", 1e-6)
    m["boot_id"] = t0.get("boot_id", "")
    if t0.get("threads") != t1.get("threads"):
        m["note"] = f"threads {t0.get('threads')} -> {t1.get('threads')} (a thread born or ended in the window)"
    # per-thread run time in the window, the busiest few
    tt = []
    for tid, d1 in w["tasks"].get("t1", {}).items():
        d0 = w["tasks"].get("t0", {}).get(tid)
        if d0 and ok:
            tt.append(((num(d1, "rt_ns") - num(d0, "rt_ns")) / 1e6 / mb, d1.get("comm", "?"),
                       (num(d1, "vol") - num(d0, "vol")) / mb))
    m["threads_top"] = sorted(tt, reverse=True)[:4]
    if mode == "stat":
        ev, per_thread = parse_stat(runs / f"{stem}_rx.stat")
        if not ev:
            why.append("no perf stat output")
        else:
            for k_out, k_in in (("insn_u", "instructions:u"), ("insn_k", "instructions:k"), ("cyc_u", "cycles:u"),
                                ("cyc_k", "cycles:k"), ("ctx", "context-switches"), ("migr", "cpu-migrations"),
                                ("faults", "page-faults"), ("task_clock", "task-clock")):
                m[k_out + "_per_mb"] = ev.get(k_in, float("nan")) / mb if ok else float("nan")
            m["stat_threads"] = sorted(((v, t) for t, v in per_thread.items()), reverse=True)[:4]
    elif mode == "pmu":
        ev, pct, uncounted, per_thread = parse_stat_pmu(runs / f"{stem}_rx.stat")
        asked = [stat_event_name(e) for e in meta.get("pmu_events", "").split(",") if e]
        m["pmu_host_missing"] = [e for e in meta.get("pmu_missing", "").split(",") if e]
        if not ev:
            why.append("no perf stat output")
        else:
            # per MB, or None = could not look (not accepted by the host, or not counted in this run)
            m["pmu"] = {e: (ev[e] / mb if ok and e in ev and e not in uncounted else None) for e in asked}
            m["pmu_pct"] = {e: pct.get(e, float("nan")) for e in asked}
            m["pmu_uncounted"] = sorted(uncounted)
            l2 = per_thread.get("l2d_cache_refill", {})
            m["pmu_l2_threads"] = sorted(((v / mb, t) for t, v in l2.items()), reverse=True)[:3] if ok else []
        if base["arm"] == TICKLE:
            m["rx_counters"] = sub_rx_counters(runs / f"{stem}_sub.log")
    elif mode == "record":
        cats, n, kaddr, resolved, comms = parse_script(runs / f"{stem}_rx.script.gz")
        if not cats or n < MIN_SAMPLES:
            why.append(f"record: {n} samples (< {MIN_SAMPLES})")
        else:
            tot = sum(cats.values())
            m["shares"] = {c: cats.get(c, 0) / tot for c in KERNEL_CATS + USER_CATS}
            m["samples"] = n
            m["kaddr"] = {a: v / tot for a, v in kaddr.items()}
            m["k_resolved"] = resolved
            m["comm_shares"] = sorted(((v / tot, c) for c, v in comms.items()), reverse=True)[:4]
    elif mode in ("strace", "strace_wrap"):
        calls = parse_strace(runs / f"{stem}_rx.strace")
        if not calls:
            why.append("no strace table")
        else:
            # attached: the window's MB; wrapped: the whole life's (start-up included, labelled)
            div = mb if mode == "strace" else base.get("recv", 0) * size / 1e6
            if div and div == div:
                m["sys_per_mb"] = {k: (c / div, e / div) for k, (c, e) in calls.items()}
                m["sys_total_per_mb"] = sum(c for c, _e in calls.values()) / div
                m["sys_errors_per_mb"] = sum(e for _c, e in calls.values()) / div
            else:
                why.append("strace: no MB to divide by")
    m["void"] = "; ".join(x for x in why if x)
    return m


# ---------------------------------------------------------------------------------------------------------- pmu rules
PMU_MIN_RUNS = 2
PMU_COLD = 1.3  # P1/P3: l2d refills per MB, tickle / fastdds
PMU_INSN_WITHIN = 1.2  # P1: instructions per MB within this factor either way
PMU_TLB = 2.0  # P2: dtlb walks per MB
PMU_FALSIFY = 1.1  # the cold-destination hypothesis is falsified below this l2d ratio
PMU_MUX_WARN = 50.0  # running share (%) below which a count is flagged MULTIPLEXED
LINE_BYTES = 64  # Cortex-A76 cache line
LL_EVENTS = ("ll_cache_miss_rd", "l3d_cache_refill")


def pmu_values(runs, event):
    """Per-MB values of `event` over usable runs; None when any run could not look at it (or there are no runs)."""
    vals = [r.get("pmu", {}).get(event) for r in runs]
    if not vals or any(v is None for v in vals):
        return None
    return vals


def pmu_reading(t_runs, f_runs):
    """The pre-registered PMU rules (module docstring, PMU MODE). Returns (held lines, falsification line or '',
    descriptive lines). Pure: the selftest drives it with made-up runs."""
    if len(t_runs) < PMU_MIN_RUNS or len(f_runs) < PMU_MIN_RUNS:
        return ([f"PMU NOT READ: {len(t_runs)} tickle and {len(f_runs)} fastdds usable pmu runs (< {PMU_MIN_RUNS} each) "
                 "- could not look"], "", [])

    def both(event):
        tv, fv = pmu_values(t_runs, event), pmu_values(f_runs, event)
        return (tv, fv) if tv is not None and fv is not None else None

    def ratio(pair):
        tm, fm = med(pair[0]), med(pair[1])
        return tm / fm if fm == fm and fm > 0 else float("nan")

    def separate(pair):
        return min(pair[0]) > max(pair[1])

    l2, insn, tlb = both("l2d_cache_refill"), both("instructions"), both("dtlb_walk")
    ll_name = next((e for e in LL_EVENTS if both(e) is not None), None)
    ll = both(ll_name) if ll_name else None
    held, cannot = [], []
    fals = ""
    if l2 is None or insn is None:
        cannot.append("P1/P3 need " + " and ".join(n for n, p in (("l2d_cache_refill", l2), ("instructions", insn))
                                                    if p is None))
    else:
        r2, ri = ratio(l2), ratio(insn)
        where = ""
        if ll is not None:
            rl = ratio(ll)
            where = (f"; {ll_name} ratio {rl:.2f}: " +
                     ("not in L3 either (rotation, or a footprint over L3)" if rl >= PMU_COLD
                      else "the lines come from L3 (footprint over L2)"))
        else:
            where = "; LL event: could not look"
        if r2 >= PMU_COLD and separate(l2) and 1 / PMU_INSN_WITHIN <= ri <= PMU_INSN_WITHIN:
            held.append(f"P1 DEST COLD: l2d refills/MB ratio {r2:.2f} (separate), instructions/MB ratio {ri:.2f}{where}")
        elif r2 >= PMU_COLD and separate(l2) and ri > PMU_INSN_WITHIN:
            held.append(f"P3 MORE WORK: l2d refills/MB ratio {r2:.2f} with instructions/MB ratio {ri:.2f} - refills "
                        "follow more instructions, not a colder destination")
        fals = (f"DEST COLD HYPOTHESIS {'FALSIFIED' if r2 < PMU_FALSIFY else 'NOT FALSIFIED'}: l2d refills/MB ratio "
                f"{r2:.2f} (falsified below {PMU_FALSIFY})")
    if tlb is None:
        cannot.append("P2 needs dtlb_walk")
    else:
        rt = ratio(tlb)
        if rt >= PMU_TLB and separate(tlb):
            held.append(f"P2 TLB: dtlb walks/MB ratio {rt:.2f} (separate)")
    held.sort(key=lambda h: h[:2])  # P1, P2, P3 in that order
    if not held:
        if cannot:
            held.append("P4 COULD NOT LOOK: " + "; ".join(cannot) + " (not accepted on the host or not counted)")
        else:
            held.append("P4 NOT LOCALISED: l2d refills, instructions and dtlb walks were looked at and none of P1-P3 "
                        "holds")
    elif cannot:
        held.append("note - could not look: " + "; ".join(cannot))
    desc = []
    if l2 is not None:
        ex = (med(l2[0]) - med(l2[1])) * LINE_BYTES / 1e6
        desc.append(f"refill bytes/B (l2d x {LINE_BYTES} per payload byte): tickle {med(l2[0]) * LINE_BYTES / 1e6:.3f}, "
                    f"fastdds {med(l2[1]) * LINE_BYTES / 1e6:.3f}, excess {ex:+.3f}")
    for e in ("l1d_cache_refill", "mem_access"):
        p = both(e)
        if p is not None:
            desc.append(f"{e}/MB ratio {ratio(p):.2f}")
    sb, cy = both("stall_backend"), both("cycles")
    if sb is not None and cy is not None:
        desc.append(f"stall_backend per cycle: tickle {med(sb[0]) / med(cy[0]):.3f}, fastdds {med(sb[1]) / med(cy[1]):.3f}")
    return held, fals, desc


# ---------------------------------------------------------------------------------------------------------- reading
def finite(xs):
    return [x for x in xs if isinstance(x, (int, float)) and not math.isnan(x)]


def med(xs):
    xs = finite(xs)
    return st.median(xs) if xs else float("nan")


def rng(xs):
    xs = finite(xs)
    return (min(xs), max(xs)) if xs else (float("nan"), float("nan"))


def f2(x, w=7, p=3):
    return f"{x:{w}.{p}f}" if isinstance(x, (int, float)) and not math.isnan(x) else f"{'-':>{w}}"


def delta(tv, fv, thr):
    """(D, counts?) for two arms' per-run values: D = median difference; counts when D >= thr and ranges separate."""
    d = med(tv) - med(fv)
    lo_t, _hi_t = rng(tv)
    _lo_f, hi_f = rng(fv)
    return d, (d == d and d >= thr and lo_t > hi_f)


def cat_ms(runs_rec, plain_ms, cats):
    """Per record run: the categories' share x the arm's plain proc ms/MB median."""
    return [sum(r["shares"][c] for c in cats) * plain_ms for r in runs_rec if "shares" in r]


def read_cell(name, by_mode, preflight):
    """by_mode: mode -> list of usable run metrics (both arms). Prints the cell's table and its reading."""
    print(f"\n==================== {name} ====================")

    def arm(mode, a):
        return [r for r in by_mode.get(mode, []) if r["arm"] == a and not r["void"]]

    for mode, rs in sorted(by_mode.items()):
        for r in rs:
            if r["void"]:
                print(f"  VOID {mode} {r['arm']} r{r['rep']}: {r['void']}")
    plain = {a: arm("plain", a) for a in (TICKLE, VENDOR)}
    print("--- plain (no instrument): ms/MB unless named; median [min..max] over reps ---")
    keys = (("cpu_sub", "cpu_sub (published: perf_test rusage, steady s)"), ("proc_ms_mb", "proc run time (window)"),
            ("rusage_ms_mb", "proc utime+stime (window)"), ("host_busy_ms_mb", "host busy, all CPUs (window)"),
            ("outside_ms_mb", "host busy - proc (outside the process)"), ("host_softirq_ms_mb", "host softirq"),
            ("host_irq_ms_mb", "host irq"), ("vol_per_mb", "voluntary switches /MB"),
            ("nvol_per_mb", "involuntary switches /MB"), ("net_rx_per_mb", "NET_RX softirqs /MB"),
            ("wire_per_payload", "eth0 rx bytes / payload byte"), ("mb", "window MB"))
    print(f"  {'metric':46} {TICKLE:>26} {VENDOR:>26}   D")
    for k, label in keys:
        cols = []
        for a in (TICKLE, VENDOR):
            xs = [r.get(k, float("nan")) for r in plain[a]]
            lo, hi = rng(xs)
            cols.append(f"{f2(med(xs))} [{f2(lo, 6, 2)}..{f2(hi, 6, 2)}] n{len(finite(xs))}")
        d = med([r.get(k) for r in plain[TICKLE]]) - med([r.get(k) for r in plain[VENDOR]])
        print(f"  {label:46} {cols[0]:>26} {cols[1]:>26} {f2(d)}")
    for a in (TICKLE, VENDOR):
        for r in plain[a][:1]:
            print(f"  busiest threads {a}: " + ", ".join(f"{c} {v:.3f} ms/MB vol {s:.1f}/MB" for v, c, s in r["threads_top"]))
    tc, fc = [r["cpu_sub"] for r in plain[TICKLE]], [r["cpu_sub"] for r in plain[VENDOR]]
    G = med(tc) - med(fc)
    Gw = med([r["proc_ms_mb"] for r in plain[TICKLE]]) - med([r["proc_ms_mb"] for r in plain[VENDOR]])
    repro = (G == G and G >= REPRO_FRAC * med(fc) and rng(tc)[0] > rng(fc)[1])
    if preflight:
        print(f"GAP (preflight, not judged): G = {f2(G)} ms/MB, Gw = {f2(Gw)}")
    elif G != G:
        print("GAP NOT READ: an arm has no usable plain run")
    else:
        print(f"GAP {'REPRODUCED' if repro else 'NOT REPRODUCED'}: G = {f2(G)} ms/MB "
              f"({100 * G / med(fc):+.1f}% of Fast DDS), Gw (window proc) = {f2(Gw)}")

    pms = {a: med([r["proc_ms_mb"] for r in plain[a]]) for a in (TICKLE, VENDOR)}
    for mode in ("stat", "record", "pmu"):
        for a in (TICKLE, VENDOR):
            x = med([r["proc_ms_mb"] for r in arm(mode, a)])
            if x == x and pms[a] == pms[a] and pms[a] > 0:
                ratio = x / pms[a]
                tag = "PERTURBED" if ratio > PERTURB else "ok"
                print(f"INSTRUMENT {mode} {a}: proc {f2(x)} ms/MB vs plain {f2(pms[a])} ({100 * (ratio - 1):+.1f}%) {tag}")

    st_ = {a: arm("stat", a) for a in (TICKLE, VENDOR)}
    if st_[TICKLE] or st_[VENDOR]:
        print("--- perf stat (window), per MB ---")
        for k in ("insn_u_per_mb", "insn_k_per_mb", "cyc_u_per_mb", "cyc_k_per_mb", "ctx_per_mb", "migr_per_mb",
                  "faults_per_mb", "task_clock_per_mb"):
            vals = [med([r.get(k) for r in st_[a]]) for a in (TICKLE, VENDOR)]
            scale, unit = (1e6, "M") if "insn" in k or "cyc" in k else (1.0, "")
            print(f"  {k:22} {f2(vals[0] / scale, 10, 3)}{unit:1} {f2(vals[1] / scale, 10, 3)}{unit:1}")
        for a in (TICKLE, VENDOR):
            for r in st_[a][:1]:
                print(f"  cycles by thread {a}: " + ", ".join(f"{t} {v / 1e6:.0f}M" for v, t in r.get("stat_threads", [])))

    pm = {a: arm("pmu", a) for a in (TICKLE, VENDOR)}
    if by_mode.get("pmu"):
        label_p = " (preflight: not judged)" if preflight else ("" if repro else " (descriptive: gap not reproduced)")
        print("--- PMU (perf stat, window): counts per MB, median [min..max]; lowest running share; ratio tickle/fastdds ---")
        host_missing = sorted({e for rs in by_mode["pmu"] for e in rs.get("pmu_host_missing", [])})
        print(f"  not accepted on the subscriber's host (could not look): {', '.join(host_missing) or 'none'}")
        events = []
        for a in (TICKLE, VENDOR):
            for r in pm[a]:
                events += [e for e in r.get("pmu", {}) if e not in events]
        for e in events:
            cols = []
            for a in (TICKLE, VENDOR):
                vals = pmu_values(pm[a], e)
                share = min((r.get("pmu_pct", {}).get(e, float("nan")) for r in pm[a]), default=float("nan"))
                mux = " MULTIPLEXED" if share == share and share < PMU_MUX_WARN else ""
                if vals is None:
                    cols.append(f"{'could not look':>34}")
                else:
                    lo, hi = rng(vals)
                    cols.append(f"{f2(med(vals) / 1e6, 8)}M [{f2(lo / 1e6, 6, 2)}..{f2(hi / 1e6, 6, 2)}] {share:3.0f}%{mux}")
            tv, fv = pmu_values(pm[TICKLE], e), pmu_values(pm[VENDOR], e)
            rt = med(tv) / med(fv) if tv is not None and fv is not None and med(fv) > 0 else float("nan")
            print(f"  {e:22} {cols[0]:>34} {cols[1]:>34} {f2(rt, 6, 2)}")
        for r in pm[TICKLE]:
            c = r.get("rx_counters", {})
            l2 = r.get("pmu", {}).get("l2d_cache_refill")
            print(f"  tickle r{r['rep']}: l2d {f2((l2 or float('nan')) / 1e6, 7)}M/MB  rx: " +
                  " ".join(f"{k}={c.get(k, '-')}" for k in ("direct_reads", "direct_runs", "drain_datagrams", "drain_ring")))
        for a in (TICKLE, VENDOR):
            for r in pm[a][:1]:
                print(f"  l2d refills by thread {a} r{r['rep']}: " +
                      ", ".join(f"{t} {v / 1e6:.3f}M/MB" for v, t in r.get("pmu_l2_threads", [])))
        held_p, fals_p, desc_p = pmu_reading(pm[TICKLE], pm[VENDOR])
        for d in desc_p:
            print(f"  {d}")
        if fals_p:
            print(f"{fals_p}{label_p}")
        for h in held_p:
            print(f"PMU READING{label_p} {h}")
        print(f"PMU PRIMARY{label_p} {held_p[0].split(':')[0]}")

    rec = {a: arm("record", a) for a in (TICKLE, VENDOR)}
    D = {}
    if rec[TICKLE] or rec[VENDOR]:
        resolved = any(r.get("k_resolved") for a in rec for r in rec[a])
        print("--- perf record: ms/MB per category = share of the arm's sampled cycles x its plain proc ms/MB ---")
        if not resolved:
            print("  kernel symbols UNRESOLVED (kptr_restrict): kernel split by the system call it ran under, not by "
                  "function; IP reassembly and checksum are inside K_INTR / K_POLL / K_RECV, not named")
        print(f"  {'category':52} {TICKLE:>16} {VENDOR:>16}   D   counts")
        for c in KERNEL_CATS + USER_CATS:
            tv, fv = cat_ms(rec[TICKLE], pms[TICKLE], (c,)), cat_ms(rec[VENDOR], pms[VENDOR], (c,))
            D[c] = delta(tv, fv, COUNT_MS)
            print(f"  {c:52} {f2(med(tv), 16)} {f2(med(fv), 16)} {f2(D[c][0])} {'COUNTS' if D[c][1] else ''}")
        print("--- BREAKDOWN per MB (ms/MB; the table asked for) ---")
        for label, cats in GROUPS:
            tv, fv = cat_ms(rec[TICKLE], pms[TICKLE], cats), cat_ms(rec[VENDOR], pms[VENDOR], cats)
            d, counts = delta(tv, fv, COUNT_MS)
            print(f"  {label:52} {f2(med(tv), 16)} {f2(med(fv), 16)} {f2(d)} {'COUNTS' if counts else ''}")
        outs = [med([r["outside_ms_mb"] for r in plain[a]]) for a in (TICKLE, VENDOR)]
        print(f"  {'outside the process (host busy - proc, plain)':52} {f2(outs[0], 16)} {f2(outs[1], 16)} "
              f"{f2(outs[0] - outs[1])}")
        D["KERNEL"] = delta(cat_ms(rec[TICKLE], pms[TICKLE], KERNEL_CATS), cat_ms(rec[VENDOR], pms[VENDOR], KERNEL_CATS),
                            COUNT_MS)
        D["USER"] = delta(cat_ms(rec[TICKLE], pms[TICKLE], USER_CATS), cat_ms(rec[VENDOR], pms[VENDOR], USER_CATS),
                          COUNT_MS)
        print(f"  kernel total D {f2(D['KERNEL'][0])}  user total D {f2(D['USER'][0])}")
        boots = {r.get("boot_id") for a in rec for r in rec[a]}
        if len(boots) == 1 and rec[TICKLE] and rec[VENDOR]:  # one boot: a kernel address is the same function in both arms
            agg = {}
            for i, a in enumerate((TICKLE, VENDOR)):
                for r in rec[a]:
                    for addr, sh in r["kaddr"].items():
                        agg.setdefault(addr, [[], []])[i].append(sh * pms[a])
            n_t, n_f = len(rec[TICKLE]), len(rec[VENDOR])
            top = sorted(agg.items(), key=lambda kv_: -abs(sum(kv_[1][0]) / n_t - sum(kv_[1][1]) / n_f))[:12]
            print("  kernel leaf addresses with the largest |D| (one boot: same address, same function), ms/MB:")
            for addr, (tv, fv) in top:
                print(f"    {addr:#x} {sum(tv) / n_t:8.4f} {sum(fv) / n_f:8.4f} D {sum(tv) / n_t - sum(fv) / n_f:+.4f}")
        for a in (TICKLE, VENDOR):
            for r in rec[a][:1]:
                print(f"  cycles by thread {a}: " + ", ".join(f"{c} {100 * s:.0f}%" for s, c in r["comm_shares"]))

    sc = {a: [r for r in by_mode.get("strace", []) + by_mode.get("strace_wrap", []) if r["arm"] == a and not r["void"]]
          for a in (TICKLE, VENDOR)}
    SYS = float("nan")
    if sc[TICKLE] or sc[VENDOR]:
        wrap = any(r["mode"] == "strace_wrap" for a in sc for r in sc[a])
        print(f"--- strace: system calls per MB ({'WHOLE LIFE, start-up included (strace_wrap)' if wrap else 'window'}) ---")
        names = set()
        for a in sc:
            for r in sc[a]:
                names |= set(r["sys_per_mb"])
        rows_ = []
        for nm in names:
            tv = med([r["sys_per_mb"].get(nm, (0, 0))[0] for r in sc[TICKLE]])
            fv = med([r["sys_per_mb"].get(nm, (0, 0))[0] for r in sc[VENDOR]])
            te = med([r["sys_per_mb"].get(nm, (0, 0))[1] for r in sc[TICKLE]])
            fe = med([r["sys_per_mb"].get(nm, (0, 0))[1] for r in sc[VENDOR]])
            rows_.append((max(tv, fv), nm, tv, fv, te, fe))
        print(f"  {'syscall':20} {TICKLE + ' calls/MB':>22} {'(errors)':>9} {VENDOR + ' calls/MB':>22} {'(errors)':>9}")
        for _k, nm, tv, fv, te, fe in sorted(rows_, reverse=True)[:14]:
            print(f"  {nm:20} {f2(tv, 22, 1)} {f2(te, 9, 1)} {f2(fv, 22, 1)} {f2(fe, 9, 1)}")
        tt, ft = med([r["sys_total_per_mb"] for r in sc[TICKLE]]), med([r["sys_total_per_mb"] for r in sc[VENDOR]])
        SYS = tt / ft if ft == ft and ft else float("nan")
        print(f"  total {f2(tt, 10, 1)} vs {f2(ft, 10, 1)} calls/MB: SYS = {f2(SYS, 5, 2)}")
    WAKE = (med([r["vol_per_mb"] for r in plain[TICKLE]]) / med([r["vol_per_mb"] for r in plain[VENDOR]])
            if med([r["vol_per_mb"] for r in plain[VENDOR]]) else float("nan"))
    print(f"  WAKE (voluntary switches per MB, plain) = {f2(WAKE, 5, 2)}")

    # ---- the pre-registered reading ----
    label = "" if repro else " (descriptive: gap not reproduced)"
    if preflight:
        label = " (preflight: not judged)"
    held = []
    Dh = med([r["host_busy_ms_mb"] for r in plain[TICKLE]]) - med([r["host_busy_ms_mb"] for r in plain[VENDOR]])
    if repro and Dh == Dh and Dh < 0.5 * Gw:
        held.append(f"R0 ACCOUNTING: host busy D {f2(Dh)} < half of Gw {f2(Gw)} - the gap is where the kernel charges the "
                    "receive work, not more work")
    if D and rec[TICKLE] and rec[VENDOR]:
        DK, DU = D["KERNEL"][0], D["USER"][0]
        g = G if G == G else 0.0
        if DK >= COUNT_MS and DK >= 0.5 * g and (SYS != SYS or SYS <= 1.2):
            comp = max(("K_POLL", "K_RECV", "K_INTR"), key=lambda c: D[c][0])
            what = {"K_POLL": "the separate checksum pass under poll (udp_poll) - what UDP checksum 0 on runs removes",
                    "K_RECV": "the read path (copy, reassembled skb free)",
                    "K_INTR": "IP reassembly/softirq charged to the process while it runs"}[comp]
            if D[comp][0] < 0.5 * DK:
                what = "spread over the kernel components (none holds half)"
            held.append(f"R1 KERNEL PER BYTE: DK {f2(DK)} ms/MB, SYS {f2(SYS, 4, 2)}{' (not measured)' if SYS != SYS else ''}"
                        f" -> {what}")
        if DK >= COUNT_MS and SYS == SYS and SYS > 1.2:
            held.append(f"R2 KERNEL PER CALL: DK {f2(DK)} ms/MB, SYS {f2(SYS, 4, 2)} - see the strace table's excess names")
        if DU >= COUNT_MS and DU >= 0.5 * g:
            ucat = max(USER_CATS, key=lambda c: D[c][0] if D[c][1] else -1e9)
            held.append(f"R3 USER: DU {f2(DU)} ms/MB -> {ucat if D[ucat][1] else 'no single user category counts'}")
        if WAKE == WAKE and WAKE > 1.5 and D["K_SYNC"][0] + D["K_POLL"][0] >= COUNT_MS:
            held.append(f"R4 WAKES: WAKE {f2(WAKE, 4, 2)}, D(K_SYNC)+D(K_POLL) {f2(D['K_SYNC'][0] + D['K_POLL'][0])}")
        if repro:
            fals = D["K_POLL"][0] < 0.05
            print(f"CHECKSUM HYPOTHESIS {'FALSIFIED' if fals else 'NOT FALSIFIED'}: D(K_POLL) = {f2(D['K_POLL'][0])} ms/MB")
    else:
        print("READING INCOMPLETE: no usable record runs for both arms")
    if not held:
        held.append("R5 the instruments do not localise the gap (could not look - not 'no difference')")
    for h in held:
        print(f"READING{label} {h}")
    print(f"PRIMARY{label} {held[0].split(':')[0]}")


def main(argv):
    if "--selftest" in argv:
        return selftest()
    preflight = "--preflight" in argv
    outs = [a for a in argv if not a.startswith("--")]
    if not outs:
        print(__doc__)
        return 64
    cells = {}
    bad = 0
    for out in outs:
        runs = Path(out + ".runs") if not out.endswith(".runs") else Path(out)
        if not (runs / "index.txt").exists():
            print(f"NOT READ: {runs}/index.txt missing")
            bad = 1
            continue
        meta, _samehost, recs = load(runs)
        q = "RELIABLE" if "--reliable" in meta.get("qos_args", "") else "BEST_EFFORT"
        stems = {f"{r['arm']}_{r['topic']}_l{r['loss']}_r{r['rep']}": r for r in recs}
        for stem, base in stems.items():
            if base["arm"] not in (TICKLE, VENDOR):
                continue
            mt = run_metrics(runs, base, stem, meta)
            name = f"{base['topic']} {meta.get('pub_rate', '?')} Hz {q} loss {base['loss']}%"
            cells.setdefault(name, {}).setdefault(mt["mode"], []).append(mt)
        print(f"read {runs}: mode {meta.get('rx_mode', meta.get('rx_profile', '?'))}, {len(stems)} runs")
    if preflight:  # every mode must parse for the subject; the vendor may deliver nothing on the PC (docstring)
        for name, by_mode in cells.items():
            for mode, rs in by_mode.items():
                for r in rs:
                    if r["arm"] == TICKLE and r["void"]:
                        print(f"PREFLIGHT FAIL {name} {mode} {r['arm']}: {r['void']}")
                        bad = 1
                    elif r["void"]:
                        print(f"PREFLIGHT note {name} {mode} {r['arm']}: {r['void']}")
    for name, by_mode in sorted(cells.items()):
        read_cell(name, by_mode, preflight)
    if preflight:
        print(f"PREFLIGHT {'FAILED' if bad else 'OK'}")
    return bad


def selftest():
    ok = True

    def check(what, cond):
        nonlocal ok
        print(f"{'ok  ' if cond else 'FAIL'} {what}")
        ok = ok and cond

    script = """perf_test  815217     500500
\t    780b42b71883 memcpy (inlined)
\t    780b42b71883 large_place (/x/librmw_tickle.so)

perf_test  815217     600000
\tffffffff9fb14db7 [unknown] ([unknown])
\t    780b43eac802 __syscall_cancel_arch (inlined)
\t    780b43f37882 epoll_pwait2 (/usr/lib/x86_64-linux-gnu/libc.so.6)

dds.udp.7410  815300     100
\tffffa00084edf5f8 [unknown] ([unknown])
\t    ffff8c0a1234 recvmsg (/usr/lib/aarch64-linux-gnu/libc.so.6)

perf_test  815217     7
\tffffffff9fb14db7 [unknown] ([unknown])
\t    780b42b71883 large_place (/x/librmw_tickle.so)

iou-wrk-1  9     5
\tffffffff9fb14db7 [unknown] ([unknown])

perf_test 1 3
\t    780b42b71883 rmw_take_with_info (/x/librmw_tickle.so)
"""
    got = [(c, p, categorize(ch)) for c, p, ch in iter_samples(script.splitlines())]
    check(f"perf script categories {[g[2] for g in got]}",
          [g[2] for g in got] == ["U_COPY", "K_POLL", "K_RECV", "K_INTR", "K_NOUSER", "U_DECODE"])
    check("comm with a dot and period parsed", got[2][0] == "dds.udp.7410" and got[2][1] == 100)
    tab = """% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
 58.54    0.209298          10     20313           epoll_pwait2
  0.09    0.000319           6        46        46 recvmmsg
------ ----------- ----------- --------- --------- ----------------
100.00    0.357540           9     40000        50 total
"""
    p = Path("/tmp") / f"rx_perf_selftest_{__import__('os').getpid()}.strace"
    p.write_text(tab)
    s = parse_strace(p)
    p.unlink()
    check(f"strace table {s}", s == {"epoll_pwait2": (20313, 0), "recvmmsg": (46, 46)})
    st_text = "# started\n\nperf_test-1,100,,cycles:k,5,100.00,,\nperf_test-2,<not counted>,,cycles:k,0,100.00,,\n" \
              "dds.ev.0-3,50,,cycles:k,5,100.00,,\nperf_test-1,7,,instructions:u,5,100.00,,\n"
    p = Path("/tmp") / f"rx_perf_selftest_{__import__('os').getpid()}.stat"
    p.write_text(st_text)
    ev, per_thread = parse_stat(p)
    p.unlink()
    check(f"perf stat sums {ev}", ev == {"cycles:k": 150.0, "instructions:u": 7.0} and per_thread["dds.ev.0-3"] == 50)
    d, counts = delta([2.6, 2.7, 2.65], [2.2, 2.3, 2.25], COUNT_MS)
    check(f"delta counts when ranges separate ({d:.2f})", counts)
    d, counts = delta([2.6, 2.2], [2.3, 2.25], COUNT_MS)
    check("delta does not count when ranges overlap", not counts)

    # pmu: a sysfs-named event, a multiplexed share, and a count perf could not make (unknown, never 0)
    pmu_text = "# started\n\nperf_test-1,300,,armv8_pmuv3_0/l2d_cache_refill/,5,62.50,,\n" \
               "dds.udp-2,100,,l2d_cache_refill,5,100.00,,\nperf_test-1,<not supported>,,dtlb_walk,0,0.00,,\n" \
               "perf_test-1,9,,instructions,5,100.00,,\n"
    p = Path("/tmp") / f"rx_perf_selftest_{__import__('os').getpid()}.pmu"
    p.write_text(pmu_text)
    ev, pct, unc, per_thread = parse_stat_pmu(p)
    p.unlink()
    check(f"pmu stat parse {ev} {pct} {unc}",
          ev == {"l2d_cache_refill": 400.0, "instructions": 9.0} and pct["l2d_cache_refill"] == 62.5 and
          unc == {"dtlb_walk"} and per_thread["l2d_cache_refill"]["dds.udp-2"] == 100.0)
    # an idle thread's <not counted> hides nothing; <not counted> on a thread another event shows running does
    idle_text = "perf_test-1,500,,cycles,5,100.00,,\nperf_test-1,400,,instructions,5,100.00,,\n" \
                "perf_test-ust-2,<not counted>,,cycles,0,100.00,,\nperf_test-ust-2,<not counted>,,instructions,0,100.00,,\n" \
                "perf_test-1,<not counted>,,l2d_cache_refill,0,0.00,,\nperf_test-3,20,,l2d_cache_refill,5,100.00,,\n"
    p = Path("/tmp") / f"rx_perf_selftest_{__import__('os').getpid()}.idle"
    p.write_text(idle_text)
    ev, _pct, unc, _pt = parse_stat_pmu(p)
    p.unlink()
    check(f"idle thread is not blindness, a blind counter on a running thread is {ev} {unc}",
          ev == {"cycles": 500.0, "instructions": 400.0, "l2d_cache_refill": 20.0} and unc == {"l2d_cache_refill"})

    def runs_(vals):  # {event: [per-MB per run]} -> run dicts
        n = len(next(iter(vals.values())))
        return [{"pmu": {e: v[i] for e, v in vals.items()}} for i in range(n)]

    base = {"instructions": [4.3e6, 4.4e6, 4.2e6], "cycles": [4.5e6] * 3, "dtlb_walk": [1e3, 1.1e3, 1.05e3]}
    f_ = runs_({**base, "l2d_cache_refill": [20e3, 21e3, 19e3], "ll_cache_miss_rd": [5e3] * 3})
    t_cold = runs_({**base, "l2d_cache_refill": [30e3, 31e3, 29e3], "ll_cache_miss_rd": [5.2e3] * 3})
    held, fals, _d = pmu_reading(t_cold, f_)
    check(f"P1 DEST COLD from L3 {held}", held[0].startswith("P1 DEST COLD") and "from L3" in held[0] and
          "NOT FALSIFIED" in fals)
    t_more = runs_({**base, "instructions": [6e6] * 3, "l2d_cache_refill": [30e3, 31e3, 29e3],
                    "ll_cache_miss_rd": [5e3] * 3})
    held, _f, _d = pmu_reading(t_more, f_)
    check(f"P3 MORE WORK {held}", held[0].startswith("P3 MORE WORK"))
    t_tlb = runs_({**base, "dtlb_walk": [3e3] * 3, "l2d_cache_refill": [20.5e3] * 3, "ll_cache_miss_rd": [5e3] * 3})
    held, fals, _d = pmu_reading(t_tlb, f_)
    check(f"P2 TLB, cold falsified {held} {fals}", held[0].startswith("P2 TLB") and "HYPOTHESIS FALSIFIED" in fals)
    t_same = runs_({**base, "l2d_cache_refill": [20.5e3] * 3, "ll_cache_miss_rd": [5e3] * 3})
    held, _f, _d = pmu_reading(t_same, f_)
    check(f"P4 NOT LOCALISED {held}", held == ["P4 NOT LOCALISED: l2d refills, instructions and dtlb walks were "
                                              "looked at and none of P1-P3 holds"])
    t_blind = [{"pmu": {**r["pmu"], "dtlb_walk": None}} for r in t_same]
    held, _f, _d = pmu_reading(t_blind, f_)
    check(f"missing event is COULD NOT LOOK, not zero {held}", held[0].startswith("P4 COULD NOT LOOK") and
          "dtlb_walk" in held[0])
    held, fals, _d = pmu_reading(t_cold[:1], f_)
    check(f"one run is not enough {held}", held[0].startswith("PMU NOT READ") and fals == "")
    print("SELFTEST " + ("OK" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
