#!/usr/bin/env python3
# Maintains the status table shown on top of the benchmark page
# (https://tsnlab.github.io/tickle/dev/bench/).
#
# Two producers feed it, each owning one section of dev/bench/status.json:
#   - "buildtest": written by .github/workflows/test-all.yml on a push to main - per-platform
#     compile + test results for the self-contained tiers (Linux x86-64, FreeRTOS/QEMU).
#   - "perf": written by .github/workflows/performance.yml after a hardware-in-the-loop run -
#     the Raspberry Pi build/test result plus the throughput/latency numbers.
#
# Neither producer knows the other's data; each just calls `merge` with its own section, then
# `inject` re-renders the whole table from status.json into dev/bench/index.html. index.html is
# regenerated from scratch by github-action-benchmark on every perf run, so the table is bounded
# by HTML comment markers and re-inserted (right after </header>) whenever they've gone missing.
#
# Usage:
#   dashboard.py merge  <status.json> <section> <fragment.json>
#   dashboard.py inject <status.json> <index.html>

import datetime
import html
import json
import sys

MARK_BEGIN = "<!-- TICKLE-STATUS:BEGIN -->"
MARK_END = "<!-- TICKLE-STATUS:END -->"

# platform key -> (row label, which status.json section + path holds its cells)
PLATFORMS = [
    ("linux", "Linux x86-64"),
    ("freertos", "FreeRTOS RISC-V (QEMU)"),
    ("hil", "Raspberry Pi (HIL, arm64)"),
]

ICON = {
    "pass": ("✅", "pass"),
    "fail": ("❌", "fail"),
    "skipped": ("⚠️", "skipped"),
    "n/a": ("–", "na"),
}


def _load(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return {}


def _now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")


def cmd_merge(status_path, section, fragment_path):
    status = _load(status_path)
    fragment = _load(fragment_path)
    fragment.setdefault("updated", _now())
    status[section] = fragment
    with open(status_path, "w", encoding="utf-8") as f:
        json.dump(status, f, indent=2, sort_keys=True)
        f.write("\n")


def _cell(value):
    """Render a status token (pass/fail/skipped/n/a) as an icon cell."""
    icon, label = ICON.get(value, ICON["skipped"])
    return f'<td class="s-{label}" title="{label}">{icon}</td>'


def _text_cell(value):
    return f"<td>{html.escape(str(value))}</td>" if value not in (None, "") else "<td>–</td>"


def _num(value, fmt="{:.0f}"):
    if value is None or value == "":
        return None
    try:
        return fmt.format(float(value))
    except (TypeError, ValueError):
        return None


def _rows(status):
    bt = status.get("buildtest", {})
    bt_rows = bt.get("rows", {})
    perf = status.get("perf", {})

    out = []
    for key, label in PLATFORMS:
        if key == "hil":
            build = perf.get("build", "skipped")
            unit = "n/a"
            integ = perf.get("integration", "skipped")
            send = _num(perf.get("throughput_send_mbps"), "{:.0f}")
            recv = _num(perf.get("throughput_recv_mbps"), "{:.0f}")
            tput = f"{send} ↑ / {recv} ↓" if send and recv else None
            rtt_avg = _num(perf.get("rtt_avg_ms"), "{:.2f}")
            rtt_mdev = _num(perf.get("rtt_mdev_ms"), "{:.2f}")
            rtt = f"{rtt_avg} ± {rtt_mdev}" if rtt_avg else None
            smsg = _num(perf.get("smallmsg_rate_msgs_s"), "{:,.0f}")
            smsg = f"{smsg}/s" if smsg else None
        else:
            row = bt_rows.get(key, {})
            build = row.get("build", "skipped")
            unit = row.get("unit", "n/a")
            integ = row.get("integration", "skipped")
            tput = rtt = smsg = None

        out.append(
            "<tr><th>"
            + html.escape(label)
            + "</th>"
            + _cell(build)
            + _cell(unit)
            + _cell(integ)
            + _text_cell(tput)
            + _text_cell(rtt)
            + _text_cell(smsg)
            + "</tr>"
        )
    return "\n".join(out)


def _caption(status):
    parts = []
    for section, name in (("buildtest", "build/test"), ("perf", "perf")):
        s = status.get(section)
        if not s:
            parts.append(f"{name}: (no data yet)")
            continue
        commit = s.get("commit_short") or s.get("commit", "")[:7] or "?"
        updated = s.get("updated", "?")
        run_url = s.get("run_url")
        commit_txt = f'<a href="{html.escape(run_url)}">{html.escape(commit)}</a>' if run_url else html.escape(commit)
        parts.append(f"{name} @ {commit_txt} ({html.escape(updated)})")
    return " &nbsp;·&nbsp; ".join(parts)


def render_block(status):
    return f"""{MARK_BEGIN}
<style>
  #tickle-status {{ margin: 8px 0 16px; overflow-x: auto; }}
  #tickle-status table {{ border-collapse: collapse; font-size: 0.95em; }}
  #tickle-status th, #tickle-status td {{ border: 1px solid #dbdbdb; padding: 4px 10px; text-align: center; }}
  #tickle-status thead th {{ background: #f5f5f5; }}
  #tickle-status tbody th {{ text-align: left; background: #fafafa; white-space: nowrap; }}
  #tickle-status td.s-fail {{ background: #fff0f0; }}
  #tickle-status td.s-pass {{ background: #f2fbf2; }}
  #tickle-status .status-caption {{ font-size: 0.85em; color: #7a7a7a; margin-top: 4px; }}
</style>
<section id="tickle-status">
  <h2>Platform status</h2>
  <table>
    <thead>
      <tr>
        <th>Platform</th><th>Build</th><th>Unit tests</th><th>Integration test</th>
        <th>Throughput (Mbps)</th><th>Latency RTT (ms)</th><th>Small-msg rate</th>
      </tr>
    </thead>
    <tbody>
{_rows(status)}
    </tbody>
  </table>
  <div class="status-caption">{_caption(status)}</div>
</section>
{MARK_END}"""


def cmd_inject(status_path, index_path):
    status = _load(status_path)
    block = render_block(status)
    with open(index_path, encoding="utf-8") as f:
        page = f.read()

    if MARK_BEGIN in page and MARK_END in page:
        pre = page.split(MARK_BEGIN, 1)[0]
        post = page.split(MARK_END, 1)[1]
        page = pre + block + post
    else:
        needle = "</header>"
        idx = page.find(needle)
        if idx == -1:
            raise SystemExit(f"{index_path}: no </header> to anchor the status table to")
        cut = idx + len(needle)
        page = page[:cut] + "\n" + block + "\n" + page[cut:]

    with open(index_path, "w", encoding="utf-8") as f:
        f.write(page)


def main(argv):
    if len(argv) >= 5 and argv[1] == "merge":
        cmd_merge(argv[2], argv[3], argv[4])
    elif len(argv) == 4 and argv[1] == "inject":
        cmd_inject(argv[2], argv[3])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
