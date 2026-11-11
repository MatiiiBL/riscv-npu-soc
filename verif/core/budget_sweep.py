#!/usr/bin/env python3
"""Cuts a run at every cycle and checks that stdout agrees with retirement.

    budget_sweep.py --rtl SIM [--iss ISS] program.elf WORKDIR

The RTL simulator prints a UART byte when its store retires (spec 7.1), so
wherever a --max-cycles budget ends a run, stdout must hold exactly the
bytes of the UART stores in the commit log. The program first runs without
a budget to find its length C in cycles; then every budget from 1 to C + 2
is tried, so the run is cut in every cycle, including each one in which a
UART store is between MEM and WB, and the last budgets let it finish.

With --iss, every budget is a tools/difftest.py run, which must pass: the
ISS is stopped after the instructions the RTL retired, and its stdout and
exit code are the reference. Without --iss (for the serial-UART build,
whose UART_STATUS reads differ from the ISS's), the RTL's stdout is compared
with the UART_TX stores in its own commit log, and the run must exit with
124 or, once the budget covers the whole run, as the unbudgeted run did.

Each budget's logs go to WORKDIR/<budget>, and are deleted when it passes.
Exits nonzero if any budget fails.
"""

import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

DIFFTEST = Path(__file__).resolve().parents[2] / "tools" / "difftest.py"
TIMEOUT = 124
UART_TX = 0x20000000
MARGIN = 2  # budgets past the run's length, which must let it finish
SHOWN = 5   # failing budgets reported in detail


def full_run(sim, elf):
    """Cycle count and exit code of the run without a budget."""
    r = subprocess.run([sim, elf], capture_output=True)
    m = re.search(rb"^\[rtl\] cycles=(\d+) ", r.stderr, re.MULTILINE)
    if m is None:
        sys.exit(f"budget_sweep: {sim} printed no cycle count for {elf}")
    return int(m.group(1)), r.returncode


def retired_uart_bytes(log):
    """The bytes of the UART_TX stores in a commit log (spec 7.2)."""
    out = bytearray()
    for line in log.read_text().splitlines():
        f = line.split()
        if len(f) == 8 and f[4] == "M" and int(f[5], 16) == UART_TX and int(f[6], 16) & 1:
            out.append(int(f[7], 16) & 0xFF)
    return bytes(out)


def check_difftest(args, budget, work, _full_rc):
    r = subprocess.run([sys.executable, str(DIFFTEST), "--rtl", args.rtl, "--iss", args.iss,
                        "--max-cycles", str(budget), "--work-dir", str(work), args.elf],
                       capture_output=True, text=True)
    if r.returncode == 0:
        return None
    # The report's first line names the program; the next ones say what differs.
    lines = (r.stdout + r.stderr).splitlines()
    return " / ".join(line.strip() for line in lines[1:3]) or f"difftest exited with {r.returncode}"


def check_self(args, budget, work, full_rc):
    work.mkdir(parents=True, exist_ok=True)
    log = work / "commit.log"
    r = subprocess.run([args.rtl, "--quiet", "--max-cycles", str(budget), "--commit-log", str(log),
                        args.elf], capture_output=True)
    (work / "stdout").write_bytes(r.stdout)
    (work / "stderr").write_bytes(r.stderr)
    want = retired_uart_bytes(log)
    if r.returncode not in (TIMEOUT, full_rc):
        return f"exit code {r.returncode}: {r.stderr.decode(errors='replace').strip()}"
    if r.stdout != want:
        return f"stdout {r.stdout!r}, but the retired UART stores wrote {want!r}"
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--rtl", required=True, help="RTL simulator")
    parser.add_argument("--iss", help="ISS; without it stdout is checked against the RTL's own log")
    parser.add_argument("elf", help="program to run")
    parser.add_argument("workdir", help="directory for the logs of failing budgets")
    args = parser.parse_args()

    cycles, full_rc = full_run(args.rtl, args.elf)
    last = cycles + MARGIN
    workdir = Path(args.workdir)
    shutil.rmtree(workdir, ignore_errors=True)

    # The sweep only covers the end of the run if the last budget lets the
    # run finish as it does without a budget.
    r = subprocess.run([args.rtl, "--quiet", "--max-cycles", str(last), args.elf], capture_output=True)
    if r.returncode != full_rc:
        sys.exit(f"budget_sweep: with a budget of {last} cycles {args.rtl} exits with "
                 f"{r.returncode}, without one with {full_rc}")

    check = check_difftest if args.iss else check_self
    budgets = range(1, last + 1)
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        results = list(pool.map(lambda b: check(args, b, workdir / str(b), full_rc), budgets))

    failed = []
    for budget, problem in zip(budgets, results):
        if problem is None:
            shutil.rmtree(workdir / str(budget), ignore_errors=True)
        else:
            failed.append(budget)
            if len(failed) <= SHOWN:
                print(f"FAIL --max-cycles {budget}: {problem}")
    if len(failed) > SHOWN:
        print(f"FAIL also at --max-cycles {', '.join(map(str, failed[SHOWN:]))}")
    how = "difftest" if args.iss else "stdout vs commit log"
    print(f"budget sweep ({how}) of {Path(args.elf).name} on {Path(args.rtl).name}: "
          f"{len(budgets) - len(failed)} of budgets 1..{last} passed, {len(failed)} failed")
    if not failed:
        shutil.rmtree(workdir, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
