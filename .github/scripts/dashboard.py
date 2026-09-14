#!/usr/bin/env python3
# Maintains the per-commit status grid shown on top of the benchmark page
# (https://tsnlab.github.io/tickle/dev/bench/).
#
# Rows are commits on main (newest first, capped at HISTORY_LIMIT); columns are each platform's
# build / test tiers plus the hardware-in-the-loop numbers. Two producers feed it, each owning
# one section of every row it touches, keyed by commit SHA:
#   - "buildtest": .github/workflows/test-all.yml, on a push to main - the Linux x86-64 and
#     FreeRTOS/QEMU compile + test outcomes.
#   - "perf": .github/workflows/performance.yml (via run_perf.sh) - the Raspberry Pi build/test
#     outcome and the measured throughput / latency / small-message rate.
# They run in separate jobs and finish out of order, so `merge` upserts a row by commit and only
# fills in its own section; `inject` re-renders the whole grid from status.json into
# dev/bench/index.html. github-action-benchmark rewrites index.html from scratch on every perf
# run, so the grid is bounded by HTML comment markers and re-inserted (right after </header>)
# whenever they've gone missing.
#
# Usage:
#   dashboard.py merge  <status.json> <section> <fragment.json>
#   dashboard.py inject <status.json> <index.html>

import datetime
import html
import json
import re
import sys

MARK_BEGIN = "<!-- TICKLE-STATUS:BEGIN -->"
MARK_END = "<!-- TICKLE-STATUS:END -->"
HISTORY_LIMIT = 30

ICON = {
    "pass": ("✅", "pass"),
    "fail": ("❌", "fail"),
    "skipped": ("⚠️", "skipped"),
    "n/a": ("–", "na"),
    "": ("·", "pending"),
}


def _load(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return {}


def _now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")


def _now_iso():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%MZ")


# --------------------------------------------------------------------------- merge


def cmd_merge(status_path, section, fragment_path):
    status = _load(status_path)
    history = status.get("history")
    if not isinstance(history, list):
        history = []
    frag = _load(fragment_path)

    commit = frag.get("commit") or frag.get("commit_short") or "unknown"
    entry = next((e for e in history if e.get("commit") == commit), None)
    if entry is None:
        entry = {"commit": commit}
        history.append(entry)
    entry["commit_short"] = frag.get("commit_short") or commit[:7]
    entry.setdefault("date", frag.get("date") or _now_iso())
    entry[section] = frag

    history.sort(key=lambda e: e.get("date") or "", reverse=True)
    seen, deduped = set(), []
    for e in history:
        if e["commit"] in seen:
            continue
        seen.add(e["commit"])
        deduped.append(e)
    history = deduped[:HISTORY_LIMIT]

    with open(status_path, "w", encoding="utf-8") as f:
        json.dump({"history": history, "updated": _now()}, f, indent=2, sort_keys=True)
        f.write("\n")


# -------------------------------------------------------------------------- render


def _cell(value):
    icon, label = ICON.get(value or "", ICON[""])
    return f'<td class="s-{label}" title="{label or "pending"}">{icon}</td>'


def _txt(value):
    return f"<td>{html.escape(str(value))}</td>" if value not in (None, "") else '<td class="s-pending">·</td>'


def _num(value, fmt):
    try:
        return fmt.format(float(value))
    except (TypeError, ValueError):
        return None


def _repo_base(entry):
    for sec in ("buildtest", "perf"):
        url = (entry.get(sec) or {}).get("run_url", "")
        m = re.match(r"(https://[^/]+/[^/]+/[^/]+)/actions/", url)
        if m:
            return m.group(1)
    return "https://github.com/tsnlab/tickle"


def _fmt_date(s):
    return html.escape((s or "").replace("T", " ").rstrip("Z"))


def _row(entry):
    short = entry.get("commit_short") or entry.get("commit", "")[:7]
    base = _repo_base(entry)
    commit_url = f"{base}/commit/{html.escape(entry.get('commit', ''))}"

    bt = entry.get("buildtest") or {}
    bt_rows = bt.get("rows") or {}
    lin = bt_rows.get("linux") or {}
    fr = bt_rows.get("freertos") or {}
    has_bt = bool(bt)

    pf = entry.get("perf") or {}
    has_pf = bool(pf)
    send = _num(pf.get("throughput_send_mbps"), "{:.0f}")
    recv = _num(pf.get("throughput_recv_mbps"), "{:.0f}")
    tput = f"{send}/{recv}" if send and recv else None
    rtt = _num(pf.get("rtt_avg_ms"), "{:.2f}")
    msg = _num(pf.get("smallmsg_rate_msgs_s"), "{:,.0f}")

    def bt_cell(d, key):
        return _cell(d.get(key, "") if has_bt else "")

    def pf_cell(key):
        return _cell(pf.get(key, "") if has_pf else "")

    return (
        f'<tr><th><a href="{commit_url}"><code>{html.escape(short)}</code></a></th>'
        f"<td>{_fmt_date(entry.get('date'))}</td>"
        + bt_cell(lin, "build") + bt_cell(lin, "unit") + bt_cell(lin, "integration")
        + bt_cell(fr, "build") + bt_cell(fr, "integration")
        + pf_cell("build") + pf_cell("integration")
        + (_txt(tput) if has_pf else '<td class="s-pending">·</td>')
        + (_txt(rtt) if has_pf else '<td class="s-pending">·</td>')
        + (_txt(msg) if has_pf else '<td class="s-pending">·</td>')
        + "</tr>"
    )


def render_block(status):
    history = status.get("history") or []
    body = "\n".join(_row(e) for e in history) or (
        '<tr><td colspan="12" style="text-align:center;color:#999">no runs recorded yet</td></tr>'
    )
    updated = html.escape(status.get("updated", ""))
    return f"""{MARK_BEGIN}
<style>
  #tickle-status {{ margin: 8px 0 16px; overflow-x: auto; }}
  #tickle-status table {{ border-collapse: collapse; font-size: 0.9em; white-space: nowrap; }}
  #tickle-status th, #tickle-status td {{ border: 1px solid #dbdbdb; padding: 3px 8px; text-align: center; }}
  #tickle-status thead th {{ background: #f5f5f5; }}
  #tickle-status tbody th {{ background: #fafafa; font-weight: normal; }}
  #tickle-status td.s-fail {{ background: #fff0f0; }}
  #tickle-status td.s-pass {{ background: #f3fbf3; }}
  #tickle-status td.s-pending {{ color: #bbb; }}
  #tickle-status .status-caption {{ font-size: 0.82em; color: #7a7a7a; margin-top: 4px; }}
</style>
<section id="tickle-status">
  <h2>Platform status &mdash; last {HISTORY_LIMIT} commits on <code>main</code></h2>
  <table>
    <thead>
      <tr>
        <th rowspan="2">Commit</th><th rowspan="2">Date (UTC)</th>
        <th colspan="3">Linux x86-64</th>
        <th colspan="2">FreeRTOS RISC-V (QEMU)</th>
        <th colspan="5">Raspberry Pi (HIL, arm64)</th>
      </tr>
      <tr>
        <th>Build</th><th>Unit</th><th>Integ.</th>
        <th>Build</th><th>Integ.</th>
        <th>Build</th><th>Integ.</th><th>Tput ↑/↓<br>Mbps</th><th>RTT<br>ms</th><th>Small-msg<br>msg/s</th>
      </tr>
    </thead>
    <tbody>
{body}
    </tbody>
  </table>
  <div class="status-caption">✅ pass &nbsp; ❌ fail &nbsp; ⚠️ skipped &nbsp; – n/a &nbsp; · not run yet
  &nbsp;·&nbsp; updated {updated}</div>
</section>
{MARK_END}"""


def cmd_inject(status_path, index_path):
    block = render_block(_load(status_path))
    with open(index_path, encoding="utf-8") as f:
        page = f.read()

    if MARK_BEGIN in page and MARK_END in page:
        page = page.split(MARK_BEGIN, 1)[0] + block + page.split(MARK_END, 1)[1]
    else:
        needle = "</header>"
        idx = page.find(needle)
        if idx == -1:
            raise SystemExit(f"{index_path}: no </header> to anchor the status grid to")
        cut = idx + len(needle)
        page = page[:cut] + "\n" + block + "\n" + page[cut:]

    with open(index_path, "w", encoding="utf-8") as f:
        f.write(page)


def main(argv):
    if len(argv) == 5 and argv[1] == "merge":
        cmd_merge(argv[2], argv[3], argv[4])
    elif len(argv) == 4 and argv[1] == "inject":
        cmd_inject(argv[2], argv[3])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
