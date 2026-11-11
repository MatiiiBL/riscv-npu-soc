#!/usr/bin/env python3
"""ECP5 resource report for one RTL top, for CI.

    synth_report.py --top npu_top --filelist rtl/npu/npu.f
    synth_report.py --top soc_top --stat build/synth-check/stat.txt

The first form runs Yosys `synth_ecp5` on the sources of a Verilator-style
file list (one path per line, relative to the list; nested `-F`/`-f` lists
are followed). The second reports the output of a Yosys `stat` that another
flow already ran (text or `stat -json`). Either way the cell counts are
printed and, when $GITHUB_STEP_SUMMARY is set, appended to the job summary
as a Markdown table. Exits nonzero if Yosys fails.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# Capacity of the LFE5U-85F on the ULX3S board, for the cells that map 1:1
# onto a fixed resource. Logic LUTs are LUT4 plus the two LUTs of every
# CCU2C carry cell.
CAPACITY = {"LUTs": 83640, "TRELLIS_FF": 83640, "DP16KD": 208, "MULT18X18D": 156}
ORDER = ["LUT4", "CCU2C", "PFUMX", "L6MUX21", "TRELLIS_FF", "TRELLIS_DPR16X4", "DP16KD", "PDPW16KD",
         "MULT18X18D", "ALU54B"]


def expand_filelist(path):
    sources = []
    for line in Path(path).read_text().splitlines():
        line = line.split("//")[0].strip()
        if not line:
            continue
        parts = line.split()
        if parts[0] in ("-F", "-f") and len(parts) == 2:
            sources += expand_filelist(Path(path).parent / parts[1])
        else:
            sources.append(str(Path(path).parent / parts[0]))
    return sources


def synthesize(top, filelist, log_dir):
    stat = Path(log_dir) / f"{top}.stat.json"
    script = (f"read_verilog -sv {' '.join(expand_filelist(filelist))}; "
              f"synth_ecp5 -top {top}; tee -q -o {stat} stat -json")
    log = Path(log_dir) / f"{top}.yosys.log"
    proc = subprocess.run(["yosys", "-q", "-l", str(log), "-p", script])
    if proc.returncode != 0:
        sys.stdout.write(log.read_text()[-4000:])
        sys.exit(f"synth_report: yosys failed for {top} (log: {log})")
    return stat


def cell_counts(stat_file):
    """Cell type -> count of the top module, from `stat` or `stat -json`."""
    text = Path(stat_file).read_text()
    try:
        design = json.loads(text)["design"]
        return {k: v for k, v in design["num_cells_by_type"].items() if not k.startswith("$")}
    except (json.JSONDecodeError, KeyError):
        pass
    counts = {}
    # Newer Yosys prints "   5960   LUT4", older releases "     LUT4   5960".
    for line in text.splitlines():
        m = re.match(r"^\s+(\d+)\s+([A-Z][A-Z0-9_]+)$", line) or re.match(r"^\s+([A-Z][A-Z0-9_]+)\s+(\d+)$", line)
        if m:
            a, b = m.groups()
            name, count = (b, a) if a.isdigit() else (a, b)
            counts[name] = counts.get(name, 0) + int(count)
    return counts


def report(top, counts):
    names = [n for n in ORDER if n in counts] + sorted(n for n in counts if n not in ORDER)
    luts = counts.get("LUT4", 0) + 2 * counts.get("CCU2C", 0)
    rows = [("LUTs (LUT4 + 2 x CCU2C)", luts, CAPACITY["LUTs"])]
    rows += [(n, counts[n], CAPACITY.get(n)) for n in names]
    lines = [f"### {top}: ECP5 cells (Yosys synth_ecp5)", "", "| Cell | Count | of LFE5U-85F |", "|---|---:|---:|"]
    for name, count, cap in rows:
        share = f"{100 * count / cap:.1f} %" if cap else ""
        lines.append(f"| {name} | {count} | {share} |")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--top", required=True)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--filelist", help="Verilator-style .f file to synthesize")
    source.add_argument("--stat", help="existing Yosys stat output to report")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        log_dir = os.environ.get("RUNNER_TEMP", tmp)
        stat = synthesize(args.top, args.filelist, log_dir) if args.filelist else args.stat
        counts = cell_counts(stat)
    if not counts:
        sys.exit(f"synth_report: no cells found for {args.top} in {stat}")
    text = report(args.top, counts)
    print(text)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as f:
            f.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
