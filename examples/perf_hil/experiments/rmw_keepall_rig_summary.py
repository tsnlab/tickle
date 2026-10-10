#!/usr/bin/env python3
"""Reads rmw_keepall_rig.sh's runs and applies the reading rules written in its header.

Usage: rmw_keepall_rig_summary.py <OUT.runs dir> <DUR> | --selftest [SCRIPT]
  --selftest  read perf_test_counts_fixture/keepall.runs (real Ke runs, 2026-10-08) with SCRIPT (default this file)
              and exit 1 unless its whole counts are right (COUNTS below); with the pre-2026-10-10 reader it fails.

COUNTS (corrected 2026-10-10). perf_test's received, sent and lost columns are NOT counts: sync_reset()
(data_runner.hpp) writes floor(count / D), D the time since the previous sync, so each row is a per-second rate. The
data_received column is floor(count x sizeof(msg) / D) (sizeof: payload + 16 B, sample_bytes()). Until 2026-10-10
this reader summed the rate columns as counts, which under-counts by the floor and by D - 1 per row: 0.02-0.11%
for rmw_tickle, and up to 3.6% for CycloneDDS at 5% loss, whose subscriber rows run at D up to 1.10 (Ke's
cyclonedds Array1k r1 read sent 36,236, recv 34,974 with lost 0; whole counts give 36,260 and 36,260). That
overstated CPU per sample by the same fraction and made a lossless run look 1,262 samples short. whole_counts()
rebuilds them: D from the row stamps (span()), received from data_received (rmw_tickle's own delivered= counter
agrees within 2 of 1.5M per run), sent and lost as the lower end of [v x D, (v + 1) x D). The lost column can still
hide one sample per row at D > 1: a "lost 0" from perf_test alone means at most one per row, and rmw_tickle's
gap_evicted / gap_abandoned counters are the witness that settles it. Rates were per-second figures all along
(the steady mean of the column, now of count / D: +0.5/s, the floor).

Loss (2026-10-06): perf_test counts every id below the first one it receives as lost (its previous id starts at 0),
and --expected_num_subs is compiled out in the rig's build, so whatever the publisher sent before the match showed
as loss for every arm. loss_split() therefore reports three figures from the subscriber's per-second rows:
  lost_all   the old figure, every row summed (kept for comparison);
  prematch   the first delivered second's count: first received id - 1, plus any gap inside that same second
             (perf_test's rows cannot separate the two; under RELIABLE a later gap is a delivery defect anyway);
  lost       every row after that one - the loss the verdicts use.
The three are whole counts (COUNTS above), so lost_all == prematch + lost.

EQUAL_BOUND (rmw_keepall_rig.sh's header): when <runs>/bounds.txt exists, every run is also VOID unless its
publisher received the bound recorded for its topic, read from /proc/PID/environ at 2 s (the .treat file):
  tickle@head  RMW_TICKLE_KEEP_ALL_BYTES == tickle_bytes, and min(bytes // footprint - 1, window, ring) == n.
               Derived from the env and core's sizing: rmw_tickle logs no realized arena, so this is the closest
               read-back there is, and it is labelled "derived".
  fastdds      FASTRTPS_DEFAULT_PROFILES_FILE == the n profile, its sha256 == the recorded one, no
               RMW_FASTRTPS_USE_QOS_FROM_XML, and no Fast DDS XML-parser or QoS-check error in the publisher's log
               (a rejected profile leaves Fast DDS at its 5,000-sample default with the hash still right).
  cyclonedds   cannot be bounded in samples (header); printed as "default, NOT equal", never VOID for it.
The rate section then prints each arm's median per cell instead of the pre/ack/head verdicts.
--dry (the dry run): prints the runs and VOIDs and exits 1 on any VOID or any tickle@head correctness failure.
"""
import math
import re
import statistics
import sys
from pathlib import Path

INDEX_RE = r"RUN (\S+) pub_maps=\[(.*?)\] sub_maps=\[(.*?)\] pub_killed=(\S+) sub_killed=(\S+) want=(\S+)"
ERROR_MARKERS = ("terminate", "exception", "what():", "timeout", "Segmentation", "Aborted")


def rows(path):
    """perf_test's per-second data rows as dicts, or [] when the log has none."""
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return [], ""
    lines = text.splitlines()
    try:
        start = lines.index("---EXPERIMENT-START---")
    except ValueError:
        return [], text
    header = [h.strip() for h in lines[start + 1].split(",") if h.strip()]
    out = []
    for line in lines[start + 2:]:
        cells = [c.strip() for c in line.split(",") if c.strip()]
        if len(cells) < len(header) or not re.match(r"^[0-9.]+$", cells[0]):
            continue
        try:
            out.append({h: float(c) for h, c in zip(header, cells)})
        except ValueError:
            continue
    return out, text


def sample_bytes(topic):
    """sizeof() of perf_test's ROS 2 message, which its data_received column counts: ArrayNk / ArrayNm is N KiB / MiB
    of payload plus the int64 time and uint64 id fields (Array1k 1,040 B, Array4k 4,112 B). None for another type."""
    m = re.match(r"Array(\d+)([km])$", topic or "")
    if not m:
        return None
    return int(m.group(1)) * (1024 if m.group(2) == "k" else 1048576) + 16


def span(rs, i):
    """D for row i: the time its counts were taken over (module docstring, COUNTS).

    sync_reset() stamps `now` before it takes the runner's lock; the row's T_loop runs from the loop's start, through
    sleep(1) and that lock wait, to after the sync, and T_experiment is stamped after the sync. So between two
    syncs D_i = (T_experiment_i - T_experiment_{i-1}) - T_loop_i + T_loop_{i-1}, exact up to the difference of two
    sleep(1) overshoots. Row 0 has no previous stamp; its T_loop stands in (it is a pre-match row in every run read).
    """
    if i == 0:
        return rs[0].get("T_loop", 1.0)
    d = rs[i]["T_experiment"] - rs[i - 1]["T_experiment"] - rs[i]["T_loop"] + rs[i - 1]["T_loop"]
    return d if d > 0 else rs[i].get("T_loop", 1.0)


def whole_counts(rs, size=None):
    """Turn perf_test's per-row RATE columns back into whole counts, in place, once (module docstring, COUNTS).

    Row i's received, sent and lost are floor(n_i / D_i). received comes back from data_received, which is
    floor(n_i x size / D_i) and so fine enough to give n_i to the sample (checked against rmw_tickle's own
    delivered= counter: within 2 of 1.5M per run, selftest); sent and lost have no byte column, so n_<key> is the
    lower end of [v x D, (v + 1) x D) and nhi_<key> its upper end - a row reading lost 0 at D > 1 may hide one.
    """
    if rs and "D" in rs[0]:
        return rs
    for i, r in enumerate(rs):
        d = span(rs, i)
        for key in ("sent", "lost", "received"):
            v = r.get(key, 0)
            r["n_" + key] = math.ceil(v * d - 1e-9) if v > 0 else 0
            r["nhi_" + key] = math.ceil((v + 1) * d - 1e-9) - 1
        if size and r.get("data_received", 0) > 0:
            r["n_received"] = r["nhi_received"] = round(r["data_received"] * d / size)
        r["D"] = d
    return rs


def loss_split(sub, size=None):
    """(lost_all, prematch, lost_after) from the subscriber's rows, whole counts; see the module docstring."""
    whole_counts(sub, size)
    lost_all = sum(r["n_lost"] for r in sub)
    first = next((i for i, r in enumerate(sub) if r["n_received"] > 0), None)
    if first is None:
        return lost_all, 0, lost_all
    prematch = sum(r["n_lost"] for r in sub[:first + 1])
    return lost_all, prematch, lost_all - prematch


def loaded(want, maps):
    """A tickle arm names its library's exact path; a vendor arm names the file, found anywhere on the path."""
    paths = maps.split()
    if want.startswith("/"):
        return want in paths
    return any(p.endswith("/" + want) for p in paths)


FASTDDS_CONFIG_ERROR_RE = re.compile(r"\[(XML\w*|\w*QOS\w*) Error\]|Problem loading XML file")


def load_bounds(runs):
    """topic -> {key: value} from bounds.txt, or {} when the run was not EQUAL_BOUND."""
    out = {}
    try:
        text = (runs / "bounds.txt").read_text()
    except OSError:
        return out
    for line in text.splitlines():
        if line.startswith("BOUND "):
            fields = dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv)
            out[fields["topic"]] = fields
    return out


def proc_env(path):
    """The [/proc/PID/environ] section of a .treat file as {name: value} (profile_sha256 included), or None."""
    try:
        lines = path.read_text(errors="replace").splitlines()
    except OSError:
        return None
    env, inside, seen = {}, False, False
    for line in lines:
        if line.startswith("[/proc/"):
            if "unreadable" in line:
                return None
            inside = seen = True
            continue
        if line.startswith("["):
            inside = False
            continue
        if inside and line.startswith("profile_sha256="):
            sha, _, prof = line.partition(" ")
            env["profile_sha256"] = sha.split("=", 1)[1]
            env["profile_sha256_of"] = prof
        elif inside and "=" in line:
            name, _, value = line.partition("=")
            env[name] = value
    return env if seen else None


def bound_check(arm, topic, bounds, treat_path, ptext):
    """(VOID reason or None, label) for one run under EQUAL_BOUND; see the module docstring."""
    b = bounds.get(topic)
    if b is None:
        return f"no recorded bound for {topic}", "?"
    n = int(b["n"])
    if arm == "cyclonedds":
        return None, "default, NOT equal (no sample bound reachable)"
    env = proc_env(treat_path)
    if env is None:
        return "publisher's /proc environ unreadable at 2 s - its treatment is unverified", "?"
    if arm == "tickle@head":
        got = env.get("RMW_TICKLE_KEEP_ALL_BYTES")
        if got != b["tickle_bytes"]:
            return f"RMW_TICKLE_KEEP_ALL_BYTES={got}, not {b['tickle_bytes']}", "?"
        realized = min(int(got) // int(b["footprint"]) - 1, int(b["window_samples"]), int(b["depth_samples"]))
        label = f"{realized} samples (derived: {got} B / {b['footprint']} B footprint - 1)"
        return (None if realized == n else f"realized bound {realized}, not {n}"), label
    if arm == "fastdds":
        why = []
        if env.get("FASTRTPS_DEFAULT_PROFILES_FILE") != b["fastdds_profile"]:
            why.append(f"profile {env.get('FASTRTPS_DEFAULT_PROFILES_FILE')}, not {b['fastdds_profile']}")
        if env.get("profile_sha256") != b["fastdds_sha256"]:
            why.append(f"profile sha256 {env.get('profile_sha256')}, not {b['fastdds_sha256']}")
        if "RMW_FASTRTPS_USE_QOS_FROM_XML" in env:
            why.append("RMW_FASTRTPS_USE_QOS_FROM_XML set")
        bad = FASTDDS_CONFIG_ERROR_RE.search(ptext)
        if bad:
            why.append(f"Fast DDS rejected configuration: '{bad.group(0)}'")
        return ("; ".join(why) or None), f"max_samples {n} (profile sha256 {str(env.get('profile_sha256'))[:12]})"
    return f"arm {arm} has no EQUAL_BOUND treatment", "?"


def equal_bound_rates(cells, bounds):
    """EQUAL_BOUND's rate section: each arm's median delivered rate per cell, refused runs counted apart."""
    print("\n=== RATE at equal KEEP_ALL bounds (median delivered msgs/s of usable reps; cyclonedds NOT bounded) ===")
    for topic, b in sorted(bounds.items()):
        print(f"{topic}: N = {b['n']} samples; rmw_tickle RMW_TICKLE_KEEP_ALL_BYTES = {b['tickle_bytes']} "
              f"({b['footprint']} B per sample); fastdds max_samples = {b['n']}; cyclonedds default")
    for (topic, loss), arms in sorted(cells.items()):
        parts = []
        for arm in sorted(arms):
            ok = [r["rate"] for r in arms[arm] if not r["refused"]]
            refused = sum(r["refused"] for r in arms[arm])
            med = f"{statistics.median(ok):.0f}/s" if ok else "-"
            parts.append(f"{arm} {med} ({len(ok)} usable, {refused} refused)")
        print(f"  {topic} l{loss}: " + "; ".join(parts))


def mean_se(xs):
    if len(xs) < 2:
        return (xs[0] if xs else float("nan")), float("inf")
    return statistics.mean(xs), statistics.stdev(xs) / math.sqrt(len(xs))


def main():
    runs, dur = Path(sys.argv[1]), int(sys.argv[2])
    dry = "--dry" in sys.argv[3:]
    bounds = load_bounds(runs)
    index = {}
    for line in (runs / "index.txt").read_text().splitlines():
        m = re.match(INDEX_RE, line)
        if m:
            index[m.group(1)] = m.groups()[1:]

    cells = {}  # (topic, loss) -> arm -> list of per-rep dicts
    voids, failures = [], []
    for stem, (pmaps, smaps, pkilled, skilled, want) in sorted(index.items()):
        arm, topic, loss, rep = re.match(r"(.+?)_(Array\w+|Struct\w+)_l(\d+)_r(\d+)$", stem).groups()
        sub, _ = rows(runs / f"{stem}_sub.log")
        pub, ptext = rows(runs / f"{stem}_pub.log")
        refused = "failed to publish" in ptext
        why = []
        header_rmw = re.search(r"RMW Implementation: (\S+)", ptext)
        vendor_by_log = not want.startswith("/") and header_rmw and want.startswith("lib" + header_rmw.group(1))
        if not loaded(want, pmaps) and not vendor_by_log:
            why.append(f"publisher did not load {want} (maps: {pmaps.strip() or 'none'})")
        if not loaded(want, smaps):
            why.append(f"subscriber did not load {want} (maps: {smaps.strip() or 'none'})")
        size = sample_bytes(topic)
        whole_counts(sub, size)
        whole_counts(pub)
        live = [r for r in sub if r["n_received"] > 0]
        if not live:
            why.append("subscriber received nothing")
        bound_label = ""
        if bounds:
            bad, bound_label = bound_check(arm, topic, bounds, runs / f"{stem}_pub.treat", ptext)
            if bad:
                why.append("bound: " + bad)
        if why:
            voids.append(f"{stem}: " + "; ".join(why))
            continue
        steady = live[2:-1] if len(live) > 4 else live
        rate = statistics.mean(r["n_received"] / r["D"] for r in steady)
        lost_all, prematch, lost = loss_split(sub, size)
        sent = sum(r["n_sent"] for r in pub)
        recv = sum(r["n_received"] for r in sub)
        errors = [mk for mk in ERROR_MARKERS if mk in ptext]
        errors += ["publisher killed at deadline"] if pkilled != "no" else []
        cpu_pub = (pub[-1]["ru_utime"] + pub[-1]["ru_stime"]) / sent * 1e6 if pub and sent else float("nan")
        cpu_sub = (sub[-1]["ru_utime"] + sub[-1]["ru_stime"]) / recv * 1e6 if recv else float("nan")
        lat = statistics.mean(r["latency_mean (ms)"] for r in steady) if steady else float("nan")
        rec = dict(rep=int(rep), refused=refused, rate=rate, lost=lost, lost_all=lost_all, prematch=prematch,
                   sent=sent, recv=recv, errors=errors,
                   cpu_pub=cpu_pub, cpu_sub=cpu_sub, lat=lat, seconds=len(live), bound=bound_label)
        cells.setdefault((topic, int(loss)), {}).setdefault(arm, []).append(rec)
        if arm in ("tickle@ack", "tickle@head") and (lost or errors):
            failures.append(f"{stem}: lost={lost} errors={errors}")

    print()
    print(f"=== per run, {dur} s publisher runtime (delivered msgs/s over steady seconds; CPU us per sample; "
          "latency not judged; lost = after the first delivered second) ===")
    for (topic, loss), arms in sorted(cells.items()):
        for arm, recs in sorted(arms.items()):
            for r in sorted(recs, key=lambda r: r["rep"]):
                print(f"{topic:8} l{loss:<2} {arm:12} r{r['rep']} rate {r['rate']:9.1f}/s  sent {r['sent']:8}  recv "
                      f"{r['recv']:8}  lost {r['lost']:6} (pre-match {r['prematch']:7}, old all-rows "
                      f"{r['lost_all']:7})  cpu pub {r['cpu_pub']:6.2f} sub {r['cpu_sub']:6.2f}  "
                      f"lat {r['lat']:.3f} ms  {'REFUSED after ' + str(r['seconds']) + ' s ' if r['refused'] else ''}"
                      f"{('ERR ' + ','.join(r['errors'])) if r['errors'] else ''}"
                      f"{('  bound ' + r['bound']) if r['bound'] else ''}")

    if voids:
        print("\nVOID runs:")
        for v in voids:
            print("  " + v)

    print("\n=== CORRECTNESS (tickle@ack, tickle@head: lost == 0, clean publisher) ===")
    judged = sum(len(arms.get(a, [])) for arms in cells.values() for a in ("tickle@ack", "tickle@head"))
    if failures:
        print("FAIL:\n  " + "\n  ".join(failures))
    elif judged == 0:
        print("NO VERDICT: no usable tickle@ack or tickle@head run - nothing was checked")
    else:
        print(f"PASS - no loss and no publisher error in any of {judged} ack/head runs")

    if dry:
        bad = bool(voids or failures or not index)
        print(f"\nDRY RUN {'FAILED' if bad else 'CLEAN'}: {len(index)} runs, {len(voids)} VOID, "
              f"{len(failures)} correctness failures")
        sys.exit(1 if bad else 0)
    if bounds:
        equal_bound_rates(cells, bounds)
        return

    ack_better_under_loss = []
    print("\n=== RATE verdicts (2 x SE and beyond the controls' own rep-to-rep spread) ===")
    for (topic, loss), arms in sorted(cells.items()):
        floors, unusable = [], []
        for ctl in ("fastdds", "cyclonedds"):
            rates = [r["rate"] for r in arms.get(ctl, []) if not r["refused"]]
            if len(rates) < 2:
                unusable.append(f"{ctl} ({len(rates)} usable, {sum(r['refused'] for r in arms.get(ctl, []))} refused)")
                continue
            floors.append(max(rates) / min(rates))
        if not floors:
            print(f"{topic} l{loss}: VOID - no control has two usable reps: " + ", ".join(unusable))
            continue
        floor = max(floors)
        without = f" (without {', '.join(unusable)})" if unusable else ""
        line = [f"{topic} l{loss}: drift floor {floor:.3f}x{without}"]
        for a, b in (("tickle@pre", "tickle@ack"), ("tickle@ack", "tickle@head")):
            ra = [r["rate"] for r in arms.get(a, []) if not r["refused"]]
            rb = [r["rate"] for r in arms.get(b, []) if not r["refused"]]
            if len(ra) < 2 or len(rb) < 2:
                line.append(f"{b} vs {a}: too few reps")
                continue
            ma, sa = mean_se(ra)
            mb, sb = mean_se(rb)
            diff, se = mb - ma, math.sqrt(sa ** 2 + sb ** 2)
            ratio = mb / ma if ma else float("inf")
            if diff > 2 * se and ratio > floor:
                v = "BETTER"
            elif -diff > 2 * se and (1 / ratio if ratio else float("inf")) > floor:
                v = "WORSE"
            else:
                v = "HELD"
            line.append(f"{b} vs {a}: {mb:.0f} vs {ma:.0f}/s = {ratio:.3f}x {v}")
        ctl_txt = ", ".join(f"{c} {statistics.mean(r['rate'] for r in arms[c]):.0f}/s"
                            for c in ("fastdds", "cyclonedds") if arms.get(c))
        print("  ".join(line) + f"  [{ctl_txt}]")
        if loss > 0 and any("tickle@ack vs tickle@pre" in x and x.endswith("BETTER") for x in line):
            ack_better_under_loss.append(f"{topic} l{loss}")

    print()
    decidable = [c for c, arms in cells.items() if c[1] > 0
                 and sum(not r["refused"] for r in arms.get("tickle@pre", [])) >= 2
                 and sum(not r["refused"] for r in arms.get("tickle@ack", [])) >= 2]
    if not decidable:
        print("NO VERDICT: no lossy cell has two usable reps of both tickle@pre and tickle@ack - this run cannot "
              "say whether the fix reaches the rmw layer")
    elif ack_better_under_loss:
        print("FIX REACHES THE RMW LAYER: ack is BETTER than pre in " + ", ".join(ack_better_under_loss))
    else:
        print("FALSIFIED for this rig and rate: ack is not BETTER than pre in any lossy cell - the fix does not reach "
              "the rmw layer at max rate here")


def selftest(script):
    """The fixture (perf_test_counts_fixture/keepall.runs: three Ke runs, 2026-10-08) must read as below, or exit 1.

    Truth that does not come from perf_test's rate columns: rmw_tickle's subscriber prints its own delivered= count.
    For CycloneDDS, the balance: a RELIABLE KEEP_ALL run with nothing published before the match and no id gap leaves
    published - received = what was in flight when the publisher exited, a handful at ~350-1,750/s (whole counts give
    0 or -5 in all ten of Ke's cyclonedds runs; the floored sums gave 148-1,262). The control: the fixture holds the
    artefact - the floored received rows sum short.
    """
    import subprocess
    fx = Path(__file__).resolve().parent / "perf_test_counts_fixture" / "keepall.runs"
    out = subprocess.run([sys.executable, str(script), str(fx), "20"], capture_output=True, text=True).stdout
    fails = []

    def check(what, ok):
        print(f"  {'ok  ' if ok else 'FAIL'} {what}")
        if not ok:
            fails.append(what)

    got = {}
    for line in out.splitlines():
        m = re.match(r"(Array\w+)\s+l(\d+)\s+(\S+)\s+r(\d+) rate\s+([\d.]+)/s\s+sent\s+(\d+)\s+recv\s+(\d+)\s+lost\s+(\d+)"
                     r" \(pre-match\s+(\d+)", line)
        if m:
            got[f"{m.group(3)}_{m.group(1)}_l{m.group(2)}_r{m.group(4)}"] = dict(
                rate=float(m.group(5)), sent=int(m.group(6)), recv=int(m.group(7)), lost=int(m.group(8)),
                prematch=int(m.group(9)))
    print(f"reading {fx} with {script}")
    check(f"three runs read ({sorted(got)})", len(got) == 3)
    stem = "tickle@head_Array1k_l5_r1"
    sub, text = rows(fx / f"{stem}_sub.log")
    delivered = max(int(x) for x in re.findall(r"delivery: delivered=(\d+)", text))
    floored = sum(r["received"] for r in sub)
    check(f"control: the floored received rows sum to {floored:.0f}, short of rmw_tickle's delivered={delivered}",
          floored < delivered - 100)
    r = got.get(stem, {})
    check(f"{stem}: recv {r.get('recv')} within 3 of rmw_tickle's delivered={delivered}, lost 0",
          r and abs(r["recv"] - delivered) <= 3 and r["lost"] == 0)
    for stem in ("cyclonedds_Array1k_l5_r1", "cyclonedds_Array4k_l5_r2"):
        r = got.get(stem, {})
        check(f"{stem}: lossless KEEP_ALL - sent {r.get('sent')} recv {r.get('recv')} within 5, lost 0, pre-match 0",
              r and abs(r["sent"] - r["recv"]) <= 5 and r["lost"] == 0 and r["prematch"] == 0)
        sub, _ = rows(fx / f"{stem}_sub.log")
        column = statistics.mean(x["received"] for x in [x for x in sub if x["received"] > 0][2:-1])
        check(f"{stem}: rate {r.get('rate')} within 1/s of the steady mean of perf_test's column ({column:.1f}): "
              "rates were never the error", r and abs(r["rate"] - column) <= 1.0)
    print(f"selftest: {'PASS' if not fails else 'FAIL (' + str(len(fails)) + ')'}")
    return 1 if fails else 0


if __name__ == "__main__":
    if sys.argv[1:2] == ["--selftest"]:
        sys.exit(selftest(Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve()))
    main()
