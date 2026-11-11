#!/usr/bin/env python3
"""Run the Pulse benchmarks on a simulator and tabulate their results.

Every benchmark prints, besides its human-readable report, lines of the form

    BENCH <bench> <case> key=value key=value ...

This script runs each benchmark ELF on the simulator given by --sim, checks
its exit code, collects those lines and writes

    <out-dir>/results.json   every record, plus per-run metadata
    <out-dir>/results.md     one Markdown table per benchmark

It exits nonzero if any benchmark fails or prints no BENCH line, so it can
gate a build. Standard library only.

Usage:
    tools/collect_bench.py --sim build/sim/soc_sim [--elf-dir build/sw]
                           [--out-dir build/bench] [--bench gemm mnist coremark]
"""

import argparse
import json
import pathlib
import re
import subprocess
import sys
import time

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
BENCHMARKS = ("gemm", "mnist", "coremark")
BENCH_LINE = re.compile(r"^BENCH\s+(\S+)\s+(\S+)((?:\s+[^\s=]+=\S*)*)\s*$")
SIM_SUMMARY = re.compile(r"^\[(rtl|iss)\] (.*)$")


def number(text):
    """'42' -> 42, '3.5' -> 3.5, anything else unchanged."""
    for kind in (int, float):
        try:
            return kind(text)
        except ValueError:
            pass
    return text


def parse_pairs(text):
    return dict(pair.split("=", 1) for pair in text.split())


def run_benchmark(sim, elf, max_cycles, timeout):
    cmd = [str(sim)]
    if max_cycles is not None:
        cmd += ["--max-cycles", str(max_cycles)]
    cmd.append(str(elf))
    start = time.monotonic()
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, check=False)
    wall = time.monotonic() - start

    records = []
    for line in proc.stdout.splitlines():
        m = BENCH_LINE.match(line)
        if m:
            records.append({"bench": m.group(1), "case": m.group(2), **parse_pairs(m.group(3))})
    summary = {}
    for line in proc.stderr.splitlines():
        m = SIM_SUMMARY.match(line)
        if m:
            summary = {"model": m.group(1), **parse_pairs(m.group(2))}
    return {
        "elf": str(elf),
        "exit_code": proc.returncode,
        "wall_seconds": round(wall, 2),
        "sim_summary": summary,
        "records": records,
        "stdout": proc.stdout,
    }


def markdown_tables(records):
    """One table per run of consecutive records that share the same keys."""
    groups = []
    for r in records:
        keys = [k for k in r if k not in ("bench", "case")]
        if groups and groups[-1][0] == keys:
            groups[-1][1].append(r)
        else:
            groups.append((keys, [r]))
    tables = []
    for keys, rows in groups:
        lines = [
            "| case | " + " | ".join(keys) + " |",
            "|------|" + "|".join("---:" for _ in keys) + "|",
        ]
        for r in rows:
            lines.append(f"| {r['case']} | " + " | ".join(str(r[k]) for k in keys) + " |")
        tables.append("\n".join(lines) + "\n")
    return tables


def write_markdown(path, sim, runs):
    out = [f"# Pulse benchmark results\n", f"Simulator: `{sim}`\n"]
    if pathlib.Path(sim).name == "iss":
        out.append(
            "> These numbers come from the ISS, which executes NPU commands instantly "
            "and whose cycle counter is not a timing model: cycle counts and speedups "
            "are not performance results. Run on the RTL simulator for those.\n"
        )
    for name, run in runs.items():
        out.append(f"## {name}\n")
        status = "passed" if run["exit_code"] == 0 else f"FAILED (exit code {run['exit_code']})"
        details = ", ".join(f"{k}={v}" for k, v in run["sim_summary"].items() if k != "model")
        out.append(f"Run {status} in {run['wall_seconds']} s wall time; simulator: {details or 'n/a'}.\n")
        out.extend(markdown_tables(run["records"]))
    path.write_text("\n".join(out))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sim", required=True, help="simulator binary (soc_sim or iss)")
    ap.add_argument("--elf-dir", default=str(REPO_ROOT / "build" / "sw"))
    ap.add_argument("--out-dir", default=str(REPO_ROOT / "build" / "bench"))
    ap.add_argument("--bench", nargs="+", default=list(BENCHMARKS), choices=BENCHMARKS)
    ap.add_argument("--max-cycles", type=int, default=None, help="passed through to the simulator")
    ap.add_argument("--timeout", type=float, default=1800, help="seconds per benchmark")
    args = ap.parse_args()

    sim = pathlib.Path(args.sim)
    if not sim.is_file():
        sys.exit(f"simulator not found: {sim}")
    elf_dir = pathlib.Path(args.elf_dir)
    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    runs = {}
    failed = []
    for name in args.bench:
        elf = elf_dir / f"{name}.elf"
        if not elf.is_file():
            sys.exit(f"benchmark not built: {elf} (run make -C sw)")
        print(f"running {name} on {sim.name} ...", flush=True)
        run = run_benchmark(sim, elf, args.max_cycles, args.timeout)
        runs[name] = run
        ok = run["exit_code"] == 0 and run["records"]
        print(f"  {'ok' if ok else 'FAILED'}: exit {run['exit_code']}, "
              f"{len(run['records'])} BENCH lines, {run['wall_seconds']} s")
        if not ok:
            failed.append(name)
            sys.stdout.write(run["stdout"])

    # Markdown keeps the values as printed ("100.00"); JSON gets numbers.
    results = {"simulator": str(sim), "runs": {}}
    for name, run in runs.items():
        entry = {k: v for k, v in run.items() if k not in ("stdout", "records", "sim_summary")}
        entry["sim_summary"] = {k: number(v) for k, v in run["sim_summary"].items()}
        entry["records"] = [{k: number(v) for k, v in r.items()} for r in run["records"]]
        results["runs"][name] = entry
    (out_dir / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    write_markdown(out_dir / "results.md", str(sim), runs)
    print(f"wrote {out_dir / 'results.md'} and {out_dir / 'results.json'}")
    if failed:
        sys.exit(f"failed: {', '.join(failed)}")


if __name__ == "__main__":
    main()
