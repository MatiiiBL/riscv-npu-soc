#!/usr/bin/env python3
"""Utilization and timing of a nextpnr-ecp5 run, from its --report JSON.

    report.py [--label TEXT] [--paths N] build/fpga/hello/report.json
    report.py [--label TEXT] --seeds 1=build/fpga/hello/seed1/report.json 2=...

The first form prints the resources used (as counts and as a share of the
LFE5U-85F), the achieved Fmax of each clock against its constraint and,
with --paths, the critical register-to-register path of up to N clock pairs
(nextpnr reports one per pair). The second prints one row of Fmax per seed
and the spread. Output is Markdown; when
$GITHUB_STEP_SUMMARY is set it is also appended to the CI job summary.
Exits nonzero if a clock misses its constraint (first form only).
"""

import argparse
import json
import os
import statistics
import sys
from pathlib import Path

# nextpnr bel types, in the order and with the names the report uses. Each
# TRELLIS_COMB is one LUT4 (the two halves of a CCU2C carry cell count as
# two); TRELLIS_RAMW is the write port of a distributed-RAM slice. The first
# four are always listed, the others only when the design uses them.
RESOURCES = [
    ("TRELLIS_COMB", "LUT4 (TRELLIS_COMB)"),
    ("TRELLIS_FF", "FF"),
    ("DP16KD", "DP16KD (18 Kbit block RAM)"),
    ("MULT18X18D", "MULT18X18D"),
    ("TRELLIS_RAMW", "distributed RAM write ports"),
    ("TRELLIS_IO", "I/O pins"),
    ("EHXPLLL", "PLL"),
]
ALWAYS = 4


def load(path):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, json.JSONDecodeError) as e:
        sys.exit(f"report: cannot read {path}: {e}")


def clock_name(net):
    """The design's name for a clock net: nextpnr reports the global it
    promoted, e.g. $glbnet$clk_25mhz$TRELLIS_IO_IN for the clk_25mhz pin."""
    return net.removeprefix("$glbnet$").split("$")[0]


def clocks(report):
    """[(clock, achieved MHz, constraint MHz)] in name order."""
    return sorted((clock_name(net), c["achieved"], c["constraint"])
                  for net, c in report["fmax"].items())


def utilization_table(report):
    util = report["utilization"]
    lines = ["| Resource | Used | Available | Share |", "|---|---:|---:|---:|"]
    for i, (bel, label) in enumerate(RESOURCES):
        used, avail = util.get(bel, {}).get("used", 0), util.get(bel, {}).get("available", 0)
        if avail and (used or i < ALWAYS):
            lines.append(f"| {label} | {used} | {avail} | {100 * used / avail:.1f} % |")
    return lines


def timing_table(report):
    lines = ["| Clock | Fmax (MHz) | Constraint (MHz) | Slack (ns) | |", "|---|---:|---:|---:|---|"]
    for name, achieved, constraint in clocks(report):
        slack = 1e3 / constraint - 1e3 / achieved
        verdict = "met" if achieved >= constraint else "FAILED"
        lines.append(f"| `{name}` | {achieved:.2f} | {constraint:.2f} | {slack:.2f} | {verdict} |")
    return lines


def cell_kind(port):
    """What a logic step on a path is, from the cell output it ends at."""
    if port == "FCO":
        return "carry"
    if port in ("F", "OFX"):
        return "LUT/mux"
    if port.startswith("P"):
        return "multiplier"
    return "other"


def path_summary(path):
    """One critical path: its end points, the split of its delay, and the
    kinds of cells on it (a carry chain counts one step per CCU2C half)."""
    steps = path["path"]
    cells = sum(s["delay"] for s in steps if s["type"] in ("clk-to-q", "logic", "setup"))
    routing = sum(s["delay"] for s in steps if s["type"] == "routing")
    kinds = {}
    for s in steps:
        if s["type"] == "logic":
            kind = cell_kind(s["to"]["port"])
            kinds[kind] = kinds.get(kind, 0) + 1
    start, end = steps[0]["from"], steps[-1]["to"]
    mix = ", ".join(f"{n} {k}" for k, n in sorted(kinds.items()))
    return [
        f"- from `{start['cell']}` ({start['port']}) to `{end['cell']}` ({end['port']})",
        f"- {cells + routing:.2f} ns: {cells:.2f} ns in cells, {routing:.2f} ns in routing",
        f"- logic steps: {mix}",
    ]


def heading(title, label):
    return f"### {title}" + (f" ({label})" if label else "")


def single(args):
    report = load(args.report)
    lines = [heading("ULX3S (LFE5U-85F) place and route", args.label), ""]
    lines += utilization_table(report) + [""] + timing_table(report)
    # Pad-to-register paths (from or to "<async>") are unconstrained.
    worst = sorted((p for p in report.get("critical_paths", [])
                    if "<async>" not in (p["from"], p["to"])),
                   key=lambda p: -sum(s["delay"] for s in p["path"]))
    for i, path in enumerate(worst[: args.paths]):
        ends = [f"{edge} {clock_name(net)}" for edge, net in
                (path["from"].split(" ", 1), path["to"].split(" ", 1))]
        lines += ["", f"Critical path {i + 1} ({ends[0]} -> {ends[1]}):", ""]
        lines += path_summary(path)
    failed = [name for name, achieved, constraint in clocks(report) if achieved < constraint]
    return lines, 1 if failed else 0


def seeds(args):
    runs = []
    for item in args.seeds:
        seed, sep, path = item.partition("=")
        if not sep:
            sys.exit(f"report: --seeds takes SEED=REPORT, not {item}")
        runs.append((seed, clocks(load(path))))
    names = [name for name, _, _ in runs[0][1]]
    lines = [heading("ULX3S Fmax per nextpnr seed (MHz)", args.label), "",
             "| Seed | " + " | ".join(f"`{n}`" for n in names) + " |",
             "|---:|" + "---:|" * len(names)]
    for seed, clks in runs:
        lines.append(f"| {seed} | " + " | ".join(f"{a:.2f}" for _, a, _ in clks) + " |")
    for label, fn in (("min", min), ("median", statistics.median), ("max", max)):
        values = [fn([clks[i][1] for _, clks in runs]) for i in range(len(names))]
        lines.append(f"| {label} | " + " | ".join(f"{v:.2f}" for v in values) + " |")
    return lines, 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("report", nargs="?", help="nextpnr --report JSON")
    parser.add_argument("--paths", type=int, default=0, help="also summarize the N worst paths")
    parser.add_argument("--seeds", nargs="+", metavar="SEED=REPORT", help="compare seed runs")
    parser.add_argument("--label", help="what was built, for the heading")
    args = parser.parse_args()
    if bool(args.report) == bool(args.seeds):
        parser.error("give one report, or --seeds")

    lines, status = seeds(args) if args.seeds else single(args)
    text = "\n".join(lines) + "\n"
    print(text, end="")
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as f:
            f.write(text + "\n")
    return status


if __name__ == "__main__":
    sys.exit(main())
