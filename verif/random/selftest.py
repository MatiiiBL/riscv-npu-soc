#!/usr/bin/env python3
"""Checks that tools/difftest.py catches every kind of mismatch.

    selftest.py --iss build/iss/iss prog.elf

Runs difftest with the ISS on both sides, once unchanged (must match) and
once per fault that fault_sim.py injects into the "RTL" side (must be
reported as a mismatch that names the corrupted line and field). Prints
the full report of one fault (--show) as an example.
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

# Text the report must contain for each fault ({line} = the corrupted line).
EXPECT = {
    "rd_wdata": ["differs at line {line} ", "fields: rd_wdata"],
    "wdata": ["differs at line {line} ", "fields: wdata"],
    "load_addr": ["differs at line {line} ", "fields: mem_addr"],
    "pc": ["differs at line {line} ", "fields: pc"],
    "drop": ["differs at line {line} "],
    "extra": ["differs at line {line} "],
    "truncate": ["differs at line {line} ", "rtl log ends here"],
    "stdout": ["stdout differs"],
    "exit": ["ending differs"],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--iss", required=True, help="ISS binary")
    parser.add_argument("--objdump", help="passed to difftest")
    parser.add_argument("--show", default="rd_wdata", help="fault whose report is printed in full")
    parser.add_argument("elf")
    args = parser.parse_args()
    py = sys.executable

    with tempfile.TemporaryDirectory(prefix="difftest-selftest-") as work:
        log = Path(work) / "clean.commit"
        subprocess.run([args.iss, "--quiet", "--commit-log", str(log), args.elf], stdout=subprocess.DEVNULL,
                       check=True)
        middle = max(1, sum(1 for _ in open(log)) // 2)

        failures = 0
        for fault in ["none"] + list(EXPECT):
            rtl = f"{py} {FAULT_SIM} --fault {fault} --line {middle} -- {args.iss}"
            cmd = [py, str(DIFFTEST), "--rtl", rtl, "--iss", args.iss, "--work-dir", work, args.elf]
            if args.objdump:
                cmd += ["--objdump", args.objdump]
            proc = subprocess.run(cmd, capture_output=True, text=True)
            report = proc.stdout + proc.stderr
            injected = re.search(r"fault_sim: \S+ at commit-log line (\d+)", report)
            line = injected.group(1) if injected else "?"
            if fault == "none":
                ok = proc.returncode == 0
            else:
                ok = proc.returncode == 1 and all(e.format(line=line) in report for e in EXPECT[fault])
            print(f"  {fault:9} {'ok' if ok else 'NOT DETECTED CORRECTLY'} (difftest exit {proc.returncode})")
            if not ok:
                failures += 1
                print(report)
            if fault == args.show:
                show = report
        print(f"\nexample report ({args.show} injected at line {middle}):\n{show}")
    total = len(EXPECT) + 1
    print(f"difftest selftest: {total - failures}/{total} cases behaved as expected")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
