#!/usr/bin/env python3
"""Checks that tools/difftest.py catches every kind of mismatch.

    selftest.py --iss build/iss/iss prog.elf

Runs difftest with the ISS on both sides, once unchanged (must match), once
per fault that fault_sim.py injects into the "RTL" side (must be reported
as a mismatch that names the corrupted line and field), and with an "RTL"
that runs out of --max-cycles before the ISS would (must compare the same
instructions instead of reporting a spurious ending). Prints the full
report of one case (--show) as an example.
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
DIFFTEST = HERE.parent.parent / "tools" / "difftest.py"
FAULT_SIM = HERE / "fault_sim.py"

# (name, fault, extra difftest arguments, expected exit, text the report
# must contain). {line} is the corrupted line and {budget} a --max-cycles
# value just above the program's instruction count.
CASES = [
    ("none", "none", [], 0, []),
    ("rd_wdata", "rd_wdata", [], 1, ["differs at line {line} ", "fields: rd_wdata"]),
    ("wdata", "wdata", [], 1, ["differs at line {line} ", "fields: wdata"]),
    ("load_addr", "load_addr", [], 1, ["differs at line {line} ", "fields: mem_addr"]),
    ("pc", "pc", [], 1, ["differs at line {line} ", "fields: pc"]),
    ("drop", "drop", [], 1, ["differs at line {line} "]),
    ("extra", "extra", [], 1, ["differs at line {line} "]),
    ("truncate", "truncate", [], 1, ["differs at line {line} ", "rtl log ends here"]),
    ("stdout", "stdout", [], 1, ["stdout differs"]),
    ("exit", "exit", [], 1, ["ending differs"]),
    # The CPI-2 "RTL" retires only budget / 2 instructions, fewer than the
    # program needs; the ISS must stop at the same instruction.
    ("cycle budget", "cpi2", ["--max-cycles", "{budget}"], 0, ["used its whole budget"]),
    ("budget, must exit", "cpi2", ["--max-cycles", "{budget}", "--expect-exit", "0"], 1,
     ["used its whole budget", "both models exited with 124, expected 0"]),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--iss", required=True, help="ISS binary")
    parser.add_argument("--objdump", help="passed to difftest")
    parser.add_argument("--show", default="rd_wdata", help="case whose report is printed in full")
    parser.add_argument("elf")
    args = parser.parse_args()
    py = sys.executable

    with tempfile.TemporaryDirectory(prefix="difftest-selftest-") as work:
        log = Path(work) / "clean.commit"
        subprocess.run([args.iss, "--quiet", "--commit-log", str(log), args.elf], stdout=subprocess.DEVNULL,
                       check=True)
        lines = sum(1 for _ in open(log))
        middle = max(1, lines // 2)
        budget = str(lines + 50)

        failures = 0
        show = ""
        for name, fault, extra, want_rc, texts in CASES:
            rtl = f"{py} {FAULT_SIM} --fault {fault} --line {middle} -- {args.iss}"
            cmd = [py, str(DIFFTEST), "--rtl", rtl, "--iss", args.iss, "--work-dir", work, args.elf]
            cmd += [a.format(budget=budget) for a in extra]
            if args.objdump:
                cmd += ["--objdump", args.objdump]
            proc = subprocess.run(cmd, capture_output=True, text=True)
            report = proc.stdout + proc.stderr
            injected = re.search(r"fault_sim: \S+ at commit-log line (\d+)", report)
            line = injected.group(1) if injected else "?"
            ok = proc.returncode == want_rc and all(t.format(line=line) in report for t in texts)
            print(f"  {name:17} {'ok' if ok else 'NOT DETECTED CORRECTLY'} (difftest exit {proc.returncode})")
            if not ok:
                failures += 1
                print(report)
            if name == args.show:
                show = report
        print(f"\nexample report ({args.show}, line {middle} of {lines}):\n{show}")
    print(f"difftest selftest: {len(CASES) - failures}/{len(CASES)} cases behaved as expected")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
