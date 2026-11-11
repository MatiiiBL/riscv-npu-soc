#!/usr/bin/env python3
"""A simulator wrapper that injects one known fault, to test difftest.

    fault_sim.py --fault rd_wdata --line 100 -- build/iss/iss [sim args] prog.elf

Runs the real simulator, then corrupts its result the way a buggy RTL
model would, so selftest.py can check that tools/difftest.py notices and
points at the right place:

  none       pass everything through unchanged
  rd_wdata   flip bit 0 of rd_wdata on the first register write at/after --line
  wdata      flip bit 0 of the store data on the first store at/after --line
  load_addr  move the address of the first load at/after --line to the next
             word (a load that read the right data from the wrong place)
  pc         add 4 to the pc of line --line
  drop       delete line --line (an instruction that never retired)
  extra      duplicate line --line (an instruction that retired twice)
  truncate   cut the log after line --line - 1 (the model stopped early)
  stdout     change the last UART byte (or emit one if there was none)
  exit       report a different exit code
"""

import argparse
import subprocess
import sys

FAULTS = ["none", "rd_wdata", "wdata", "load_addr", "pc", "drop", "extra", "truncate", "stdout", "exit"]
LOG_FAULTS = ["rd_wdata", "wdata", "load_addr", "pc", "drop", "extra", "truncate"]


def flip_field(line, index, bit=1):
    parts = line.split(" ")
    parts[index] = f"{int(parts[index], 16) ^ bit:08x}"
    return " ".join(parts)


def is_mem(line, store):
    """A load or store line (spec 7.2): stores have a nonzero byte mask."""
    parts = line.split(" ")
    return len(parts) == 8 and (parts[6] != "0") == store


def corrupt_log(path, fault, at):
    with open(path) as f:
        lines = f.read().splitlines()
    i = at - 1
    if not 0 <= i < len(lines):
        sys.exit(f"fault_sim: --line {at} is outside the {len(lines)}-line commit log")
    if fault == "rd_wdata":
        i = next(k for k in range(i, len(lines)) if lines[k].split(" ")[2] != "00")
        lines[i] = flip_field(lines[i], 3)
    elif fault == "wdata":
        i = next(k for k in range(i, len(lines)) if is_mem(lines[k], store=True))
        lines[i] = flip_field(lines[i], 7)
    elif fault == "load_addr":
        i = next(k for k in range(i, len(lines)) if is_mem(lines[k], store=False))
        lines[i] = flip_field(lines[i], 5, bit=4)
    elif fault == "pc":
        parts = lines[i].split(" ")
        parts[0] = f"{int(parts[0], 16) + 4:08x}"
        lines[i] = " ".join(parts)
    elif fault == "drop":
        del lines[i]
    elif fault == "extra":
        lines.insert(i, lines[i])
    elif fault == "truncate":
        del lines[i:]
    with open(path, "w") as f:
        f.write("".join(line + "\n" for line in lines))
    # The first line that now differs, which difftest must report. A
    # duplicated line only shows up on the copy after it.
    first = i + 2 if fault == "extra" else i + 1
    print(f"fault_sim: {fault} at commit-log line {first}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--fault", choices=FAULTS, required=True)
    parser.add_argument("--line", type=int, default=1, help="commit-log line to corrupt (1-based)")
    parser.add_argument("sim", nargs=argparse.REMAINDER, help="-- simulator command and its arguments")
    args = parser.parse_args()
    sim = args.sim[1:] if args.sim[:1] == ["--"] else args.sim
    if not sim:
        parser.error("missing simulator command")

    proc = subprocess.run(sim, stdout=subprocess.PIPE)
    stdout, code = proc.stdout, proc.returncode
    if args.fault in LOG_FAULTS:
        corrupt_log(sim[sim.index("--commit-log") + 1], args.fault, args.line)
    elif args.fault == "stdout":
        stdout = stdout[:-1] + bytes([stdout[-1] ^ 0x20]) if stdout else b"?"
    elif args.fault == "exit":
        code = code + 1
    sys.stdout.buffer.write(stdout)
    return code


if __name__ == "__main__":
    sys.exit(main())
