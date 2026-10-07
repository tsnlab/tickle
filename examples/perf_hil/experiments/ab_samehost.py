#!/usr/bin/env python3
"""ab_samehost.py - the pre-registration and the reading of ab_samehost.sh (a same-host TickLE-only A/B).

THE PRE-REGISTRATION IS A FILE, WRITTEN BEFORE THE FIRST RUN. `prereg` reads the invocation's environment, refuses
anything it cannot read, prints it, and writes it as JSON. `summary` reads ONLY that JSON - never the environment -
so nothing named after the data has been seen can decide. The driver records the file's sha256 in its log before the
first block, so an edit afterwards is visible.

  A, B, [C]   full commit SHAs (the driver resolves refs). Three arms is the base / fix-only / full design.
  CELLS       "scen:size:extra args;..." e.g. "best_effort_throughput:p3:;reliable_latency:p3:-i 0.005"
              A cell is named everywhere else as scen:size or scen:size:extra args (extra whitespace-normalised).
  PRIMARY     "cell:metric,..."   the ONLY metrics that decide. Required.
  CONTROL     "cell:metric,..."   metrics the change cannot touch. Required: a campaign without a control has nothing
              to tell a drift of the Pi from the change. A control that moves (two-sided) VOIDs the comparison.
              Read from the arms' own runs, a control is only as untouched as its binary: a control whose client
              differs between the two arms (the change compiles into the segment-less build too - a struct field, a
              printf) gets a note saying so. ab_drain 2026-10-07: B's OFF client differed from A's (4c15ffd0 vs
              643c407f), and its 0.33% OFF send-rate move VOIDed B-A.
  SENTINEL    a full commit SHA (the driver resolves refs; usually A's). Then every CONTROL is measured on the
              SENTINEL's build instead of the arm's: the driver runs each control cell at SENTINEL in EVERY block,
              beside that block's arm cells (files b<n>_<armsha8>_sentinel<sha8>_<slug>.txt). The same bytes in every
              block, so the change provably cannot touch it, and its mean over X's blocks minus its mean over Y's
              blocks is exactly the drift the mirrored block order failed to cancel in X - Y. Judged at block level:
              t = that difference / (SD of the sentinel's block means, pooled within each arm's blocks, x
              sqrt(1/nX + 1/nY)), against the Student t threshold (df = blocks - arms) at the Bonferroni level over
              controls x comparisons (pooled over all blocks it would contain the shift it tests for). Block
              level because repetitions within a block share the block's state: on 2026-10-07 the OFF send rate's
              block means spread 0.18% where three reps predict 0.10%, so a rep-level SE reads ordinary block-to-block
              drift as a move. A sentinel whose ON or OFF client sha256 differs between blocks VOIDs; a block without
              its sentinel run is NO VERDICT.
  SECONDARY   "cell:metric,..."   printed and flagged at plain 2 x SE; never decide.
  TREATMENT   "cell:ARMS:expr;..." checked on EVERY recorded repetition of the ON arm of those arms, e.g.
              "best_effort_throughput:p3:B:client_traffic.shm_encoded_in_slot>0;
               best_effort_throughput:p3:A:absent_as(client_traffic.shm_encoded_in_slot,0)==0;
               *:*:client.bells_rung==client.doorbells_sent"
              ARMS is A, B,C or *. A repetition that fails it VOIDs every comparison involving that arm (an arm that
              did not get its treatment is not a measurement of it); one where a named field is absent is "could not
              look" -> NO VERDICT, unless the field is wrapped in absent_as(field, value), which says in advance what
              an absent field means (e.g. a counter the base commit does not have).
  DERIVED     "name=expr;..."     per-repetition figures, referred to as derived.<name>, e.g.
              "stime_per_sleep_us=1e6*(client.stime_s+proc.stime_s)/(client.sleeps+server.sleeps)"
  COMPARE     "B-A,C-A" (default: every other arm against A). Each is one comparison.
  REPS, DUR   per block and cell, and the measured window in seconds (recorded, used by the driver).

A metric is role.field[@OFF][/hi|/lo]. Roles are the lines s6_transport_cells.sh records per repetition:
  client          the client's RESULT line          server          the server's RESULT line (role=server)
  proc            the server's /proc reading (PROC:) - the only server CPU a latency server reports
  delivery        the server's "Subscriber ... delivery:" line
  client_traffic, server_traffic   the core's "Node N traffic:" line of each side (s6_witness_check.sh)
  derived         a DERIVED figure
@OFF reads the arm with the segment compiled out (s6's kernel-path arm); default is the ON arm. /hi or /lo says
which way is better; without it a known name's direction is used (DIRECTION below) and an unknown one is refused.
Arithmetic in expr: + - * / ( ), numbers, comparisons, and/or/not, absent_as(field, number); a text field compares
with a quoted string (client.rx_hint=="epoll").

HOW EACH COMPARISON IS READ (implemented in summary(), not only here):
  per (cell, metric): mean and SE over the repetitions of each arm (both of its blocks pooled), t = (X - A) / SE,
  as ab_compare.py. A primary is judged at the Bonferroni threshold for k = (primaries x comparisons), the
  two-sided z at 0.0455 / k (2 x SE at k = 1); a control at the same rule over (controls x comparisons).
    VOID        a treatment failed in either arm, the two arms built the same client binary for a cell that carries
                a PRIMARY (the change never reached that cell's compiler, so its primary measures nothing), or a
                control moved beyond its threshold. A control cell with one binary in both arms is printed as the
                strongest control there is (ab_frag_fastpath's p3), not refused.
    NO VERDICT  a primary or control has fewer than 2 repetitions in either arm (missing data), or a treatment
                could not be checked - "could not look" must not read as "held"
    WORSE       some primary is beyond its threshold in its bad direction
    IMPROVED    none worse, and some primary beyond it in its good direction
    PASS        every primary held
  A repetition counts only if its client line says window=ok, and only from a file whose header names the arm's
  SHA and the cell's scenario and size. OVERALL is the strongest of VOID > NO VERDICT > WORSE > IMPROVED > PASS.
  Exit status: 0 PASS/IMPROVED, 1 WORSE, 3 NO VERDICT, 4 VOID, 2 refused/usage.

Usage:
  ab_samehost.py prereg OUT.json      validate the environment, print and write the pre-registration
  ab_samehost.py cells PREREG.json    the driver's cell list: scen|size|extra|slug (extra args may not contain |),
                                      then BLOCKS|A B ..., and SENTINEL|scen|size|extra|slug|sha per sentinel cell
  ab_samehost.py summary PREREG.json PREFIX     read PREFIX.b<n>_<sha8>_<slug>.txt and print the verdicts
  ab_samehost.py selftest             the reading shown to decide on ab_samehost_fixture.txt
"""
import ast
import contextlib
import io
import json
import math
import os
import re
import statistics
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ab_compare import bonferroni_t, bonferroni_z, judge, stats  # noqa: E402

ROLES = ("client", "server", "proc", "delivery", "client_traffic", "server_traffic", "derived")
METRIC = re.compile(r"^(?P<role>[a-z_]+)\.(?P<field>[A-Za-z_]\w*)(?P<off>@OFF)?(?:/(?P<dir>hi|lo))?$")
# Which way is better, for names whose direction is not a matter of opinion. Anything else needs /hi or /lo.
HI = {"send_mbps", "recv_mbps", "win_send_mbps", "win_recv_mbps", "delivered", "recv", "win_recv", "win_sent",
      "sent", "measured"}
LO_PREFIX = ("rtt_", "cpu_s", "sched_cpu", "utime", "stime", "loss_pct", "lost", "shm_full_dropped",
             "retransmitted", "gap_abandoned", "peak_rss", "vmhwm", "wire_bytes_per_sample", "segment_head_stalls")
VERDICT_ORDER = ("VOID", "NO VERDICT", "WORSE", "IMPROVED", "PASS")
EXIT = {"PASS": 0, "IMPROVED": 0, "WORSE": 1, "NO VERDICT": 3, "VOID": 4}


class Refused(Exception):
    pass


class Absent(Exception):
    pass


def norm_extra(extra):
    return " ".join(extra.split())


def cell_key(scen, size, extra):
    extra = norm_extra(extra)
    return f"{scen}:{size}" + (f":{extra}" if extra else "")


def slug(key):
    return re.sub(r"[^A-Za-z0-9.]+", "_", key).strip("_")


def parse_cells(spec):
    cells = []
    for raw in spec.split(";"):
        if not raw.strip():
            continue
        parts = raw.strip().split(":", 2)
        if len(parts) < 2 or not re.fullmatch(r"[a-z_]+", parts[0]) or not re.fullmatch(r"p\d", parts[1]):
            raise Refused(f"CELLS item {raw!r}: want scen:size:extra args (e.g. reliable_latency:p3:-i 0.005)")
        extra = parts[2] if len(parts) == 3 else ""
        if "|" in extra:
            raise Refused(f"CELLS item {raw!r}: '|' separates the driver's fields and cannot be in the extra args")
        key = cell_key(parts[0], parts[1], extra)
        if any(c["key"] == key for c in cells):
            raise Refused(f"CELLS names {key} twice")
        cells.append({"key": key, "scen": parts[0], "size": parts[1], "extra": norm_extra(extra), "slug": slug(key)})
    if not cells:
        raise Refused("CELLS is empty")
    if len({c["slug"] for c in cells}) != len(cells):
        raise Refused("two CELLS map to the same file name")
    return cells


def resolve_cell(part, cells, what):
    if part.strip() == "*":
        return [c["key"] for c in cells]
    p = part.strip().split(":", 2)
    if len(p) < 2:
        raise Refused(f"{what}: {part!r} is not a cell")
    key = cell_key(p[0], p[1], p[2] if len(p) == 3 else "")
    if key not in [c["key"] for c in cells]:
        raise Refused(f"{what}: cell {key!r} is not in CELLS ({', '.join(c['key'] for c in cells)})")
    return [key]


def direction(field, explicit):
    if explicit:
        return explicit == "hi"
    if field in HI or field.endswith("_mbps"):
        return True
    if field.startswith(LO_PREFIX):
        return False
    return None


def parse_metric(text, what, derived, need_dir):
    m = METRIC.match(text.strip())
    if not m or m["role"] not in ROLES:
        raise Refused(f"{what}: {text!r} is not role.field[@OFF][/hi|/lo] with role in {', '.join(ROLES)}")
    if m["role"] == "derived" and m["field"] not in derived:
        raise Refused(f"{what}: derived.{m['field']} is not defined in DERIVED")
    hi = direction(m["field"], m["dir"])
    if need_dir and hi is None:
        raise Refused(f"{what}: which way is better for {text!r} is not known - append /hi or /lo")
    return {"role": m["role"], "field": m["field"], "arm": "OFF" if m["off"] else "ON", "hi": hi,
            "name": f"{m['role']}.{m['field']}{m['off'] or ''}"}


def parse_items(spec, cells, derived, what, need_dir):
    items = []
    for raw in (spec or "").split(","):
        if not raw.strip():
            continue
        if ":" not in raw:
            raise Refused(f"{what}: {raw!r} names no cell (want cell:metric)")
        cpart, mpart = raw.rsplit(":", 1)
        met = parse_metric(mpart, what, derived, need_dir)
        for key in resolve_cell(cpart, cells, what):
            item = dict(met, cell=key)
            if any(i["cell"] == key and i["name"] == item["name"] for i in items):
                raise Refused(f"{what}: {key} {item['name']} named twice")
            items.append(item)
    return items


# ------------------------------------------------------------------------- expressions, checked before they run
def check_expr(text, derived, what, allow_derived=True):
    try:
        tree = ast.parse(text.strip(), mode="eval")
    except SyntaxError as err:
        raise Refused(f"{what}: cannot parse {text!r}: {err.msg}") from None

    def ref(node):
        if not (isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name)):
            raise Refused(f"{what}: {ast.unparse(node)!r} is not role.field")
        role, field = node.value.id, node.attr
        if role not in ROLES or (role == "derived" and (not allow_derived or field not in derived)):
            raise Refused(f"{what}: {role}.{field} is not a readable field here")

    def walk(node):
        if isinstance(node, ast.Expression):
            walk(node.body)
        elif isinstance(node, ast.BinOp) and isinstance(node.op, (ast.Add, ast.Sub, ast.Mult, ast.Div)):
            walk(node.left)
            walk(node.right)
        elif isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.USub, ast.Not)):
            walk(node.operand)
        elif isinstance(node, ast.BoolOp):
            for v in node.values:
                walk(v)
        elif isinstance(node, ast.Compare):
            walk(node.left)
            for c in node.comparators:
                walk(c)
        elif isinstance(node, ast.Constant) and isinstance(node.value, (int, float, str)):
            pass
        elif isinstance(node, ast.Attribute):
            ref(node)
        elif isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "absent_as":
            if len(node.args) != 2 or node.keywords or not isinstance(node.args[1], ast.Constant):
                raise Refused(f"{what}: absent_as takes (role.field, number)")
            ref(node.args[0])
        else:
            raise Refused(f"{what}: {ast.unparse(node)!r} is not allowed (arithmetic, comparisons, absent_as only)")

    walk(tree)
    return text.strip()


def fields_of(text):
    return sorted({f"{r}.{f}" for r, f in re.findall(r"\b([a-z_]+)\.([A-Za-z_]\w*)", text) if r in ROLES})


def value(rep, role, field):
    try:
        return float(rep[role][field])
    except (KeyError, ValueError):
        raise Absent(f"{role}.{field}") from None


def field_value(rep, role, field):
    """A number if the field reads as one, else its text (rx_hint=uring); Absent if the line or field is missing."""
    try:
        text = rep[role][field]
    except KeyError:
        raise Absent(f"{role}.{field}") from None
    try:
        return float(text)
    except ValueError:
        return text


def evaluate(text, rep):
    """Evaluates a checked expression on one repetition; raises Absent naming the first missing field."""

    def ev(node):
        if isinstance(node, ast.Expression):
            return ev(node.body)
        if isinstance(node, ast.Constant):
            return node.value
        if isinstance(node, ast.Attribute):
            return field_value(rep, node.value.id, node.attr)
        if isinstance(node, ast.Call):
            try:
                return ev(node.args[0])
            except Absent:
                return node.args[1].value
        if isinstance(node, ast.UnaryOp):
            v = ev(node.operand)
            return -v if isinstance(node.op, ast.USub) else not v
        if isinstance(node, ast.BoolOp):
            vals = [ev(v) for v in node.values]
            return all(vals) if isinstance(node.op, ast.And) else any(vals)
        if isinstance(node, ast.BinOp):
            a, b = ev(node.left), ev(node.right)
            if isinstance(node.op, ast.Add):
                return a + b
            if isinstance(node.op, ast.Sub):
                return a - b
            if isinstance(node.op, ast.Mult):
                return a * b
            if b == 0:
                raise Absent("a division by zero")
            return a / b
        if isinstance(node, ast.Compare):
            left = ev(node.left)
            for op, comp in zip(node.ops, node.comparators):
                right = ev(comp)
                ok = {ast.Eq: left == right, ast.NotEq: left != right, ast.Lt: left < right, ast.LtE: left <= right,
                      ast.Gt: left > right, ast.GtE: left >= right}[type(op)]
                if not ok:
                    return False
                left = right
            return True
        raise Absent(f"unsupported {ast.unparse(node)}")

    try:
        return ev(ast.parse(text, mode="eval"))
    except TypeError as err:  # text against a number: the field is not what the expression assumed
        raise Absent(f"a type mismatch ({err})") from None


# ------------------------------------------------------------------------------------------- pre-registration
def build_prereg(env):
    arms = {}
    for letter in ("A", "B", "C"):
        sha = env.get(letter, "").strip()
        if sha:
            if not re.fullmatch(r"[0-9a-f]{40}", sha):
                raise Refused(f"{letter}={sha!r} is not a full commit SHA (the driver resolves refs before this)")
            arms[letter] = sha
    if "A" not in arms or "B" not in arms:
        raise Refused("A and B are required")
    if len(set(arms.values())) != len(arms):
        raise Refused("two arms name the same commit")
    cells = parse_cells(env.get("CELLS", ""))
    derived = {}
    for raw in env.get("DERIVED", "").split(";"):
        if not raw.strip():
            continue
        name, sep, expr = raw.partition("=")
        if not sep or not re.fullmatch(r"[A-Za-z_]\w*", name.strip()):
            raise Refused(f"DERIVED item {raw!r}: want name=expr")
        derived[name.strip()] = check_expr(expr, {}, f"DERIVED {name.strip()}", allow_derived=False)
    primary = parse_items(env.get("PRIMARY", ""), cells, derived, "PRIMARY", True)
    control = parse_items(env.get("CONTROL", ""), cells, derived, "CONTROL", False)
    secondary = parse_items(env.get("SECONDARY", ""), cells, derived, "SECONDARY", False)
    if not primary:
        raise Refused("PRIMARY is empty: name the metrics that decide before the run")
    if not control:
        raise Refused("CONTROL is empty: every campaign needs a control the change provably cannot touch")
    treatment = []
    for raw in env.get("TREATMENT", "").split(";"):
        if not raw.strip():
            continue
        parts = raw.strip().rsplit(":", 2)
        if len(parts) != 3:
            raise Refused(f"TREATMENT item {raw!r}: want cell:ARMS:expr")
        cpart, arm_part, expr = parts
        tarms = sorted(arms) if arm_part.strip() == "*" else [a.strip() for a in arm_part.split(",") if a.strip()]
        for a in tarms:
            if a not in arms:
                raise Refused(f"TREATMENT {raw!r}: arm {a} is not one of {', '.join(arms)}")
        expr = check_expr(expr, derived, f"TREATMENT {raw.strip()!r}")
        treatment.append({"cells": resolve_cell(cpart, cells, "TREATMENT"), "arms": tarms, "expr": expr})
    if not treatment:
        raise Refused("TREATMENT is empty: name a field that shows each arm got what it was meant to")
    compare = []
    spec = env.get("COMPARE", "") or ",".join(f"{a}-A" for a in sorted(arms) if a != "A")
    for raw in spec.split(","):
        m = re.fullmatch(r"\s*([ABC])-([ABC])\s*", raw)
        if not m or m[1] == m[2] or m[1] not in arms or m[2] not in arms:
            raise Refused(f"COMPARE item {raw!r}: want X-Y with X, Y two different arms")
        compare.append([m[1], m[2]])
    try:
        reps, dur = int(env.get("REPS", "5")), int(env.get("DUR", "10"))
    except ValueError:
        raise Refused("REPS and DUR must be integers") from None
    if reps < 2:
        raise Refused("REPS < 2 gives no SE within a block")
    k_p, k_c = len(primary) * len(compare), len(control) * len(compare)
    pre = {"arms": arms, "cells": cells, "derived": derived, "primary": primary, "control": control,
           "secondary": secondary, "treatment": treatment, "compare": compare, "reps": reps,
           "dur": dur, "k_primary": k_p, "z_primary": bonferroni_z(k_p), "k_control": k_c,
           "z_control": bonferroni_z(k_c), "tag": env.get("TAG", "")}
    sentinel = env.get("SENTINEL", "").strip()
    if sentinel:
        if not re.fullmatch(r"[0-9a-f]{40}", sentinel):
            raise Refused(f"SENTINEL={sentinel!r} is not a full commit SHA (the driver resolves refs before this)")
        if any(i["role"] == "derived" for i in control):
            raise Refused("CONTROL with SENTINEL: a derived figure is not read from the sentinel's runs")
        pre["sentinel"] = sentinel
        pre["sentinel_cells"] = [c["key"] for c in cells if any(i["cell"] == c["key"] for i in control)]
        pre["t_control"] = bonferroni_t(k_c, len(blocks(pre)) - len(arms))
    return pre


def blocks(pre):
    order = sorted(pre["arms"])
    return order + order[::-1]


def describe(pre):
    out = ["=== PRE-REGISTRATION (written before any run; summary() reads only this) ==="]
    for a, sha in pre["arms"].items():
        out.append(f"  arm {a} = {sha}")
    out.append(f"  blocks: {' '.join(blocks(pre))}, REPS={pre['reps']} per block and cell, DUR={pre['dur']} s")
    out.append("  cells:")
    out += [f"    {c['key']}" for c in pre["cells"]]
    for name, expr in pre["derived"].items():
        out.append(f"  derived.{name} = {expr}")
    out.append(f"  comparisons: {', '.join(x + '-' + y for x, y in pre['compare'])}")
    out.append(f"  PRIMARY (decide): k = {pre['k_primary']}, Bonferroni |t| > {pre['z_primary']:.2f}")
    out += [f"    {i['cell']}  {i['name']}  ({'higher' if i['hi'] else 'lower'} is better)" for i in pre["primary"]]
    if pre.get("sentinel"):
        out.append(f"  CONTROL (a move either way VOIDs), measured on SENTINEL {pre['sentinel']} in every block and "
                   f"judged on its block means: k = {pre['k_control']}, df = {len(blocks(pre)) - len(pre['arms'])}, "
                   f"|t| > {pre['t_control']:.2f}")
    else:
        out.append(f"  CONTROL (a move either way VOIDs): k = {pre['k_control']}, |t| > {pre['z_control']:.2f}")
    out += [f"    {i['cell']}  {i['name']}" for i in pre["control"]]
    out.append("  SECONDARY (never decide, flagged at 2 x SE):")
    out += [f"    {i['cell']}  {i['name']}" for i in pre["secondary"]] or ["    (none)"]
    out.append("  TREATMENT (every recorded ON repetition; a failure VOIDs the arm, an absent field is NO VERDICT):")
    out += [f"    arms {','.join(t['arms'])}  {', '.join(t['cells'])}:  {t['expr']}" for t in pre["treatment"]]
    out.append("  RULES: VOID (treatment failed / same client binary in a primary's cell / control moved / the "
               "sentinel not one binary) > "
               "NO VERDICT (n < 2 in an arm, "
               "or a treatment not checkable) > WORSE > IMPROVED > PASS; a repetition counts only with window=ok.")
    return "\n".join(out)


# ------------------------------------------------------------------------------------------------- reading data
LINE = re.compile(r"^\s*arm=(ON|OFF) (RESULT:|PROC:|server-delivery|client-traffic|server-traffic)(.*)$")
HEADER = re.compile(r"=== S6 transport cells .* sha=(\S+) scen=(\S+) size=(\S+)")
BUILT = re.compile(r"arm (ON|OFF) \(extra='[^']*'\) built, client sha256=([0-9a-f]+)")
# A cell that failed says so in its file (s6_transport_cells.sh / s6_witness_check.sh); the first such line is kept so
# an arm with no repetitions reads as "its cells failed: <why>", not as "its treatment could not be checked".
FAILED = re.compile(r"FAILED to run|FATAL|BUILD_FAILED|error:")
ROLE_OF = {"PROC:": "proc", "server-delivery": "delivery", "client-traffic": "client_traffic",
           "server-traffic": "server_traffic"}


def read_file(path, sha, cell, info=None):
    """-> (reps by arm tag, ON client sha256 or None, problem or None). `info`, if given, gets "ON"/"OFF": each arm's
    client sha256, and "why": the first two lines saying the cell failed to build or run."""
    reps, cur, binary, header = {"ON": [], "OFF": []}, {}, None, None
    info = {} if info is None else info
    with open(path, encoding="utf-8", errors="replace") as fh:
        for text in fh:
            if header is None:
                h = HEADER.search(text)
                if h:
                    header = h.groups()
                    continue
            b = BUILT.search(text)
            if b:
                info[b[1]] = b[2]
                if b[1] == "ON":
                    binary = b[2]
                continue
            if info.get("why", "").count(" | ") < 1 and FAILED.search(text):
                info["why"] = (info["why"] + " | " if "why" in info else "") + text.strip()
            m = LINE.match(text.rstrip("\n"))
            if not m:
                continue
            tag, kind, rest = m.groups()
            fields = dict(re.findall(r"([A-Za-z_]\w*)=(\S+)", rest))
            if kind == "RESULT:" and fields.get("role") != "server":
                cur[tag] = {"client": fields}
                reps[tag].append(cur[tag])
                continue
            role = "server" if kind == "RESULT:" else ROLE_OF[kind]
            if tag in cur:
                cur[tag][role] = fields
    if header is None:
        return reps, binary, "no S6 header line"
    hsha, scen, size = header
    if not (sha.startswith(hsha) or hsha.startswith(sha)) or len(hsha) < 7:
        return reps, binary, f"header sha={hsha}, not this arm's {sha[:8]}"
    if (scen, size) != (cell["scen"], cell["size"]):
        return reps, binary, f"header scen={scen} size={size}, not {cell['scen']} {cell['size']}"
    return reps, binary, None


def windowed(pre, reps):
    """The repetitions with window=ok (derived figures added), and how many were dropped."""
    got, dropped = {"ON": [], "OFF": []}, 0
    for tag in got:
        for r in reps[tag]:
            if r["client"].get("window") == "ok":
                for name, expr in pre["derived"].items():
                    try:
                        r.setdefault("derived", {})[name] = str(evaluate(expr, r))
                    except Absent:
                        pass
                got[tag].append(r)
            else:
                dropped += 1
    return got, dropped


def load(pre, prefix):
    """-> data[(arm, cell)] = {"ON": reps, "OFF": reps}, ON client binaries per (arm, cell), notes, and extra:
    "off" OFF client binaries per (arm, cell), "failed" per arm the files that held no repetition and why,
    "sentinel" per cell {block number: (arm, {"ON": reps, "OFF": reps})} and "sentinel_bin" per cell {"ON"/"OFF": set}."""
    data, binaries, notes = {}, {}, []
    extra = {"off": {}, "failed": {}, "sentinel": {}, "sentinel_bin": {}}
    folder, base = os.path.split(prefix)
    names = sorted(os.listdir(folder or "."))
    for a, sha in pre["arms"].items():
        for cell in pre["cells"]:
            pat = re.compile(re.escape(base) + r"\.b(\d+)_" + re.escape(sha[:8]) + "_" + re.escape(cell["slug"])
                             + r"\.txt$")
            files = [n for n in names if pat.match(n)]
            got = {"ON": [], "OFF": []}
            dropped = refused = 0
            for n in files:
                info = {}
                reps, binary, problem = read_file(os.path.join(folder, n), sha, cell, info)
                if problem:
                    notes.append(f"IDENTITY FAILED, not read: {n}: {problem}")
                    refused += 1
                    continue
                if binary:
                    binaries.setdefault((a, cell["key"]), set()).add(binary)
                if "OFF" in info:
                    extra["off"].setdefault((a, cell["key"]), set()).add(info["OFF"])
                if not reps["ON"] and not reps["OFF"]:
                    extra["failed"].setdefault(a, []).append(f"{n}: {info.get('why', 'no RESULT line, no reason given')}")
                w, d = windowed(pre, reps)
                dropped += d
                for tag in got:
                    got[tag] += w[tag]
            notes.append(f"arm {a} {cell['key']}: {len(files) - refused} file(s) read, ON n={len(got['ON'])} "
                         f"OFF n={len(got['OFF'])}"
                         + (f", {dropped} repetition(s) dropped (window not ok)" if dropped else ""))
            data[(a, cell["key"])] = got
    for a, why in extra["failed"].items():
        notes.append(f"arm {a}: {len(why)} file(s) with no repetition at all - the cell failed; first: {why[0]}")
    if pre.get("sentinel"):
        s = pre["sentinel"]
        letter = {sha[:8]: a for a, sha in pre["arms"].items()}
        for cell in (c for c in pre["cells"] if c["key"] in pre["sentinel_cells"]):
            pat = re.compile(re.escape(base) + r"\.b(\d+)_([0-9a-f]{8})_sentinel" + re.escape(s[:8]) + "_"
                             + re.escape(cell["slug"]) + r"\.txt$")
            per_block, bins = {}, {"ON": set(), "OFF": set()}
            for n in names:
                m = pat.match(n)
                if not m or m[2] not in letter:
                    continue
                info = {}
                reps, _, problem = read_file(os.path.join(folder, n), s, cell, info)
                if problem:
                    notes.append(f"IDENTITY FAILED, not read: {n}: {problem}")
                    continue
                for tag in bins:
                    if tag in info:
                        bins[tag].add(info[tag])
                w, d = windowed(pre, reps)
                per_block[int(m[1])] = (letter[m[2]], w)
                notes.append(f"sentinel {s[:8]} {cell['key']} block {m[1]} (arm {letter[m[2]]}): ON n={len(w['ON'])} "
                             f"OFF n={len(w['OFF'])}" + (f", {d} dropped (window not ok)" if d else "")
                             + (f" - {info.get('why', 'no reason given')}" if not reps["ON"] and not reps["OFF"] else ""))
            extra["sentinel"][cell["key"]] = per_block
            extra["sentinel_bin"][cell["key"]] = bins
    return data, binaries, notes, extra


def sentinel_control(pre, extra, item, x, y):
    """The sentinel's drift between X's and Y's blocks, at block level -> (line, verdict) with verdict one of
    "held", "MOVED", "missing", "NOT ONE BINARY"."""
    per_block = extra["sentinel"].get(item["cell"], {})
    bins = extra["sentinel_bin"].get(item["cell"], {})
    order = blocks(pre)
    means, by_arm, missing = {}, {a: [] for a in order}, []
    for n, arm in enumerate(order, 1):
        got = per_block.get(n)
        vals = []
        if got and got[0] == arm:
            for r in got[1][item["arm"]]:
                try:
                    vals.append(value(r, item["role"], item["field"]))
                except Absent:
                    pass
        if not vals:
            missing.append(n)
            continue
        means[n] = sum(vals) / len(vals)
        by_arm[arm].append(means[n])
    shown = " ".join(f"b{n}{order[n - 1]} {v:.5g}" for n, v in sorted(means.items()))
    head = f"  CONTROL   %-10s {item['cell']:36s} {item['name']:36s} sentinel {pre['sentinel'][:8]} blocks: {shown}"
    if len(bins.get(item["arm"], ())) > 1:
        return head % "NOT ONE" + f"  client sha256 {sorted(bins[item['arm']])}", "NOT ONE BINARY"
    if missing:
        return head % "missing" + f"  (no sentinel value in block(s) {missing})", "missing"
    # The block-to-block spread is pooled WITHIN each arm's blocks (df = blocks - arms): the spread over all blocks
    # would contain the very shift between arms being tested, and could never call it (the self-test's "moved" case
    # read held at t 1.7 that way).
    df = len(order) - len(by_arm)
    ss = sum((v - statistics.fmean(vs)) ** 2 for vs in by_arm.values() for v in vs)
    sd = math.sqrt(ss / df) if df > 0 else float("nan")
    mx, my = statistics.fmean(by_arm[x]), statistics.fmean(by_arm[y])
    se = sd * math.sqrt(1 / len(by_arm[x]) + 1 / len(by_arm[y]))
    t = (mx - my) / se if se else (0.0 if mx == my else math.copysign(math.inf, mx - my))
    moved = abs(t) > pre["t_control"]
    pct = 100 * (mx - my) / my if my else float("nan")
    return (head % ("MOVED" if moved else "held") + f"  {x}-{y} {mx - my:+.4g} ({pct:+.2f}%), block SD {sd:.3g}, "
            f"t {t:+.2f} (|t| > {pre['t_control']:.2f} moves)"), ("MOVED" if moved else "held")


def values(data, arm, item):
    out = []
    for r in data.get((arm, item["cell"]), {}).get(item["arm"], []):
        try:
            out.append(value(r, item["role"], item["field"]))
        except Absent:
            pass
    return out


def fmt(s):
    return f"{s[0]:12.5g} (se {s[1]:.3g}, n{s[2]})" if s else f"{'-':>12s} (n0)"


def summary(pre, prefix):
    data, binaries, notes, extra = load(pre, prefix)
    print(describe(pre))
    print("=== DATA ===")
    for n in notes:
        print("  " + n)
    # Treatments, per arm: every recorded ON repetition, with what each one actually reported.
    print("=== TREATMENT ===")
    t_fail, t_blind = set(), set()
    for t in pre["treatment"]:
        for a in t["arms"]:
            ok = bad = absent = 0
            first_bad = None
            for key in t["cells"]:
                for r in data.get((a, key), {}).get("ON", []):
                    try:
                        if evaluate(t["expr"], r):
                            ok += 1
                        else:
                            bad += 1
                            if first_bad is None:
                                seen = []
                                for f in fields_of(t["expr"]):
                                    role, field = f.split(".", 1)
                                    seen.append(f"{f}={r.get(role, {}).get(field, 'absent')}")
                                first_bad = f"{key}: " + " ".join(seen)
                    except Absent as err:
                        absent += 1
                        first_bad = first_bad or f"{key}: {err} absent"
            state = "FAILED" if bad else ("NOT CHECKABLE" if absent or ok == 0 else "ok")
            print(f"  {state:13s} arm {a}  {t['expr']}  [{', '.join(t['cells'])}]: {ok} ok, {bad} failed, "
                  f"{absent} could not look" + (f"  (e.g. {first_bad})" if first_bad else ""))
            if bad:
                t_fail.add(a)
            elif absent or ok == 0:
                t_blind.add(a)
    overall = []
    for x, y in pre["compare"]:
        print(f"=== {x} vs {y}: {x}={pre['arms'][x][:8]} against {y}={pre['arms'][y][:8]} ===")
        void, blind = [], []
        if t_fail & {x, y}:
            void.append(f"treatment failed in arm(s) {','.join(sorted(t_fail & {x, y}))}")
        for a in sorted({x, y} & set(extra["failed"])):
            if not any(data[(a, c["key"])]["ON"] for c in pre["cells"]):
                blind.append(f"arm {a} has no repetition in any cell - its cells failed ({extra['failed'][a][0]})")
        if t_blind & {x, y}:
            blind.append(f"treatment could not be checked in arm(s) {','.join(sorted(t_blind & {x, y}))}")
        for cell in pre["cells"]:
            same = binaries.get((x, cell["key"]), set()) & binaries.get((y, cell["key"]), set())
            if not same:
                continue
            if any(i["cell"] == cell["key"] for i in pre["primary"]):
                void.append(f"{cell['key']}: both arms built client sha256 {min(same)} - the change never compiled "
                            "into a cell whose primary is meant to measure it")
            else:
                print(f"  note: {cell['key']}: both arms built client sha256 {min(same)} - byte-identical, so any move "
                      "there is the Pi's, not the change's")
        for item in pre["control"]:
            if pre.get("sentinel"):
                line, how = sentinel_control(pre, extra, item, x, y)
                print(line)
                if how == "missing":
                    blind.append(f"control {item['cell']} {item['name']}: a block has no sentinel run")
                elif how == "NOT ONE BINARY":
                    void.append(f"control {item['cell']} {item['name']}: the sentinel built different clients in "
                                "different blocks - not one instrument")
                elif how == "MOVED":
                    void.append(f"control {item['cell']} {item['name']} (sentinel) moved beyond "
                                f"|t| > {pre['t_control']:.2f}")
                continue
            own = binaries if item["arm"] == "ON" else extra["off"]
            bx, by = own.get((x, item["cell"]), set()), own.get((y, item["cell"]), set())
            if bx and by and not bx & by:
                print(f"  note: control {item['cell']} {item['name']}: the two arms built different {item['arm']} "
                      f"clients ({y} {','.join(sorted(by))}, {x} {','.join(sorted(bx))}) - the change compiles into "
                      "it, so it is not provably untouched (a SENTINEL is)")
            a, b = stats(values(data, y, item)), stats(values(data, x, item))
            if not a or not b or a[2] < 2 or b[2] < 2:
                blind.append(f"control {item['cell']} {item['name']} has n < 2 in an arm")
                print(f"  CONTROL   missing    {item['cell']:36s} {item['name']:36s} {y} {fmt(a)}  {x} {fmt(b)}")
                continue
            _, t, _ = judge(a, b, True, pre["z_control"])
            moved = abs(t) > pre["z_control"]
            print(f"  CONTROL   {'MOVED' if moved else 'held':10s} {item['cell']:36s} {item['name']:36s} {y} {fmt(a)}  "
                  f"{x} {fmt(b)}  t {t:+.2f}")
            if moved:
                void.append(f"control {item['cell']} {item['name']} moved (t {t:+.2f}, |t| > {pre['z_control']:.2f})")
        counts = {"IMPROVED": 0, "PASS": 0, "WORSE": 0}
        for kind, items, z in (("PRIMARY", pre["primary"], pre["z_primary"]), ("secondary", pre["secondary"], 2.0)):
            for item in items:
                a, b = stats(values(data, y, item)), stats(values(data, x, item))
                if not a or not b or a[2] < 2 or b[2] < 2:
                    if kind == "PRIMARY":
                        blind.append(f"primary {item['cell']} {item['name']} has n < 2 in an arm")
                    print(f"  {kind:9s} missing    {item['cell']:36s} {item['name']:36s} {y} {fmt(a)}  {x} {fmt(b)}")
                    continue
                hi = item["hi"] if item["hi"] is not None else False
                diff, t, v = judge(a, b, hi, z)
                v = {"better": "IMPROVED", "held": "PASS"}.get(v, v)
                if kind == "PRIMARY":
                    counts[v] += 1
                pct = 100 * diff / a[0] if a[0] else float("nan")
                note = "" if item["hi"] is not None else " (direction unknown, read as lower-better)"
                print(f"  {kind:9s} {v:10s} {item['cell']:36s} {item['name']:36s} {y} {fmt(a)}  {x} {fmt(b)}  "
                      f"{pct:+6.1f}%  t {t:+.2f}{note}")
        if void:
            verdict = "VOID"
            why = "; ".join(void)
        elif blind:
            verdict = "NO VERDICT"
            why = "; ".join(blind)
        elif counts["WORSE"]:
            verdict = "WORSE"
            why = f"{counts['WORSE']} of {len(pre['primary'])} primary beyond |t| > {pre['z_primary']:.2f}"
        elif counts["IMPROVED"]:
            verdict, why = "IMPROVED", f"{counts['IMPROVED']} better, {counts['PASS']} held, none worse"
        else:
            verdict, why = "PASS", f"all {counts['PASS']} primary held at |t| <= {pre['z_primary']:.2f}"
        print(f"VERDICT {x}-{y}: {verdict} ({why})")
        overall.append(verdict)
    final = min(overall, key=VERDICT_ORDER.index)
    print(f"OVERALL: {final} ({', '.join(f'{x}-{y} {v}' for (x, y), v in zip(pre['compare'], overall))})")
    return EXIT[final]


# --------------------------------------------------------------------------------------------------- self-test
FIXTURE_SHAS = dict({k: k * 40 for k in "abcdef"}, g="1" * 40, h="2" * 40, i="3" * 40)
FIXTURE_ENV = {
    "CELLS": "best_effort_throughput:p3:;reliable_latency:p3:-i 0.005;best_effort_throughput:p4:",
    "PRIMARY": "best_effort_throughput:p3:client.cpu_s_per_Msample,reliable_latency:p3:-i 0.005:client.rtt_p50_ms,"
               "reliable_latency:p3:-i 0.005:derived.stime_per_sleep_us/lo",
    "CONTROL": "best_effort_throughput:p4:client.win_send_mbps",
    "SECONDARY": "best_effort_throughput:p3:server.win_recv_mbps",
    "TREATMENT": "best_effort_throughput:p3:{BC}:client_traffic.shm_encoded_in_slot>0;"
                 "best_effort_throughput:p3:A:absent_as(client_traffic.shm_encoded_in_slot,0)==0;"
                 "*:*:client.bells_rung==client.doorbells_sent",
    "DERIVED": "stime_per_sleep_us=1e6*(client.stime_s+proc.stime_s)/(client.sleeps+server.sleeps)",
    "REPS": "4", "TAG": "selftest",
}
# (arms, expected OVERALL, why) - each line of the fixture's README is one of these.
FIXTURE_CASES = [
    ({"A": "a", "B": "g"}, "PASS", "g applies the treatment and moves nothing; its p4 binary is a's (a note)"),
    ({"A": "a", "B": "i"}, "VOID", "i improves p3, but its p3 client binary is a's: never compiled in"),
    ({"A": "a", "B": "b"}, "IMPROVED",
     "b cuts best_effort p3 cpu_s_per_Msample 0.80 -> 0.70; its sha=a impostor is refused"),
    ({"A": "a", "B": "c"}, "WORSE", "c raises reliable_latency p3 rtt_p50 30 -> 36 us"),
    ({"A": "a", "B": "d"}, "VOID", "d improves p3 but its p4 control moved 14000 -> 12000 Mbps"),
    ({"A": "a", "B": "e"}, "NO VERDICT", "e has no reliable_latency file at all (a missing arm)"),
    ({"A": "a", "B": "h"}, "NO VERDICT", "h has no shm_encoded_in_slot at all: its treatment cannot be looked at"),
    ({"A": "a", "B": "f"}, "VOID", "f improves p3 but reports shm_encoded_in_slot=0 (treatment not applied)"),
    ({"A": "a", "B": "b", "C": "d"}, "VOID", "three arms: B-A IMPROVED, C-A VOID"),
]


def selftest():
    here = os.path.dirname(os.path.abspath(__file__))
    fixture = os.path.join(here, "ab_samehost_fixture.txt")
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        prefix = os.path.join(tmp, "fx")
        files, name = {}, None
        with open(fixture, encoding="utf-8") as fh:
            for text in fh:
                m = re.match(r"^### file (\S+)$", text.strip())
                if m:
                    name = m[1]
                    files[name] = []
                elif name:
                    files[name].append(text)
        for name, lines in files.items():
            with open(f"{prefix}.{name}", "w", encoding="utf-8") as fh:
                fh.writelines(lines)
        for arms, want, why in FIXTURE_CASES:
            env = dict(FIXTURE_ENV, **{k: FIXTURE_SHAS[v] for k, v in arms.items()})
            env["TREATMENT"] = env["TREATMENT"].replace("{BC}", "B,C" if "C" in arms else "B")
            pre = build_prereg(env)
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                summary(pre, prefix)
            got = re.search(r"^OVERALL: (VOID|NO VERDICT|WORSE|IMPROVED|PASS)", buf.getvalue(), re.M)[1]
            lines = [ln for ln in buf.getvalue().splitlines() if ln.startswith(("VERDICT", "OVERALL"))]
            ok = got == want
            failures += not ok
            print(f"{'ok  ' if ok else 'FAIL'} {'/'.join(f'{k}={v}' for k, v in arms.items()):14s} want {want:10s} "
                  f"got {got:10s} - {why}")
            for ln in lines:
                print(f"       {ln}")
            if not ok:
                print(buf.getvalue())
        # The SENTINEL: A's build run in every block (A B B A) for the control cell, written here rather than in the
        # fixture because only its block means matter. Each case must be decided by the sentinel alone: arms a/g
        # read PASS without it (the first case above).
        def sentinel_files(sub, means, bins=None, skip=()):
            os.makedirs(os.path.join(tmp, sub), exist_ok=True)
            for n, (arm, mean) in enumerate(zip("ABBA", means), 1):
                if n in skip:
                    continue
                arm_sha = FIXTURE_SHAS["a" if arm == "A" else "g"]
                on = (bins or {}).get(n, "aaaaaaaaaaaaaaaa")
                with open(os.path.join(tmp, sub, f"fx.b{n}_{arm_sha[:8]}_sentinel{FIXTURE_SHAS['a'][:8]}_"
                                       "best_effort_throughput_p4.txt"), "w", encoding="utf-8") as fh:
                    fh.write(f"=== S6 transport cells 2026-10-07T00:00:00+09:00 sha={FIXTURE_SHAS['a']} "
                             "scen=best_effort_throughput size=p4 dur=10s reps=2 host=10.1.1.214 iface=lo ===\n"
                             f"    arm ON (extra='') built, client sha256={on}\n"
                             "    arm OFF (extra=' -Dtt_SEGMENT_ENABLED=0') built, client sha256=0aaaaaaaaaaaaaaa\n")
                    for d in (-7.0, 7.0):
                        fh.write("  arm=ON RESULT: framework=tickle scenario=best_effort_throughput role=client "
                                 f"sent=2000000 win_send_mbps={mean + d} window=ok cpu_s_per_Msample=2.0\n")
            for name, lines in files.items():
                with open(os.path.join(tmp, sub, f"fx.{name}"), "w", encoding="utf-8") as fh:
                    fh.writelines(lines)
            return os.path.join(tmp, sub, "fx")

        sentinel_cases = [
            ("held", [14000, 14012, 13995, 14006], None, (), "PASS",
             "block means within 0.1% (block SD 9 within the arms): the sentinel holds"),
            ("moved", [14000, 14420, 14430, 14010], None, (), "VOID",
             "the sentinel is 3% higher in B's blocks than in A's: drift the A B B A order did not cancel"),
            ("rebuilt", [14000, 14012, 13995, 14006], {3: "deadbeefdeadbeef"}, (), "VOID",
             "the sentinel's ON client differs in block 3: not one instrument"),
            ("gap", [14000, 14012, 13995, 14006], None, (3,), "NO VERDICT", "block 3 has no sentinel run"),
        ]
        for sub, means, bins, skip, want, why in sentinel_cases:
            env = dict(FIXTURE_ENV, A=FIXTURE_SHAS["a"], B=FIXTURE_SHAS["g"], SENTINEL=FIXTURE_SHAS["a"])
            env["TREATMENT"] = env["TREATMENT"].replace("{BC}", "B")
            pre = build_prereg(env)
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                summary(pre, sentinel_files(sub, means, bins, skip))
            got = re.search(r"^OVERALL: (VOID|NO VERDICT|WORSE|IMPROVED|PASS)", buf.getvalue(), re.M)[1]
            ok = got == want
            failures += not ok
            print(f"{'ok  ' if ok else 'FAIL'} sentinel {sub:8s} want {want:10s} got {got:10s} - {why}")
            for ln in buf.getvalue().splitlines():
                if ln.startswith(("VERDICT", "  CONTROL")):
                    print(f"       {ln.strip()[:220]}")
            if not ok:
                print(buf.getvalue())
        # The refusals: a pre-registration that cannot be read must stop the run, not be read leniently.
        bad = [
            (dict(FIXTURE_ENV, PRIMARY=""), "no primary"),
            (dict(FIXTURE_ENV, CONTROL=""), "no control"),
            (dict(FIXTURE_ENV, TREATMENT=""), "no treatment"),
            (dict(FIXTURE_ENV, PRIMARY="reliable_latency:p2:client.rtt_p50_ms"), "a primary on a cell not in CELLS"),
            (dict(FIXTURE_ENV, PRIMARY="best_effort_throughput:p3:client.bells_rung"),
             "a primary of unknown direction"),
            (dict(FIXTURE_ENV, TREATMENT="*:*:__import__('os')"), "code in a treatment"),
            (dict(FIXTURE_ENV, PRIMARY="best_effort_throughput:p3:derived.nope/lo"), "an undefined derived figure"),
        ]
        for env, why in bad:
            env = dict(env, A=FIXTURE_SHAS["a"], B=FIXTURE_SHAS["b"])
            env["TREATMENT"] = env["TREATMENT"].replace("{BC}", "B")
            try:
                build_prereg(env)
                print(f"FAIL refusal not raised: {why}")
                failures += 1
            except Refused as err:
                print(f"ok   refused ({why}): {err}")
    print(f"SELFTEST {'PASSED' if failures == 0 else f'FAILED ({failures})'}")
    return 1 if failures else 0


def main(argv):
    try:
        if len(argv) == 3 and argv[1] == "prereg":
            pre = build_prereg(os.environ)
            text = describe(pre)
            print(text)
            with open(argv[2], "w", encoding="utf-8") as fh:
                json.dump(pre, fh, indent=1, sort_keys=True)
            return 0
        if len(argv) == 3 and argv[1] == "cells":
            with open(argv[2], encoding="utf-8") as fh:
                pre = json.load(fh)
            for c in pre["cells"]:
                print(f"{c['scen']}|{c['size']}|{c['extra']}|{c['slug']}")
            print("BLOCKS|" + " ".join(blocks(pre)))
            if pre.get("sentinel"):
                for c in pre["cells"]:
                    if c["key"] in pre["sentinel_cells"]:
                        print(f"SENTINEL|{c['scen']}|{c['size']}|{c['extra']}|{c['slug']}|{pre['sentinel']}")
            return 0
        if len(argv) == 4 and argv[1] == "summary":
            with open(argv[2], encoding="utf-8") as fh:
                pre = json.load(fh)
            return summary(pre, argv[3])
        if len(argv) == 2 and argv[1] == "selftest":
            return selftest()
    except Refused as err:
        print(f"REFUSED: {err}", file=sys.stderr)
        return 2
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
