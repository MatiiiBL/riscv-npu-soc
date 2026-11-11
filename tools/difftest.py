#!/usr/bin/env python3
"""Differential test: run one program on the RTL simulator and on the ISS.

    difftest.py --rtl build/sim/soc_sim --iss build/iss/iss prog.elf

Both simulators run with --commit-log (docs/spec.md section 7). The run
matches when the commit logs are identical line for line, stdout is
identical byte for byte, and the exit codes are equal. On a mismatch the
report shows the first differing commit-log line from each model with the
lines before it, which fields differ, and the disassembly of the
instruction at that pc.

Exit status: 0 match, 1 mismatch, 2 usage or tool error.
"""

import argparse
import collections
import filecmp
import itertools
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
FIELDS = ["pc", "insn", "rd", "rd_wdata", "mem_addr", "wmask", "wdata"]
TOOL_ERROR = 125  # simulator exit code for usage/ELF/I-O errors (spec 7.1)


class ToolError(Exception):
    pass


class Run:
    """One simulator run: exit code, stdout bytes, stderr text and log path."""

    def __init__(self, name, commit, code, stdout, stderr, timed_out):
        self.name = name
        self.commit = commit
        self.code = code
        self.stdout = stdout
        self.stderr = stderr
        self.timed_out = timed_out


def run_model(name, command, elf, work, stem, args):
    commit = work / f"{stem}.{name}.commit"
    argv = list(command) + ["--commit-log", str(commit)]
    if args.max_cycles is not None:
        argv += ["--max-cycles", str(args.max_cycles)]
    argv.append(str(elf))
    timed_out = False
    try:
        proc = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=args.timeout)
        code, stdout, stderr = proc.returncode, proc.stdout, proc.stderr
    except FileNotFoundError:
        raise ToolError(f"{name} simulator not found: {argv[0]}")
    except PermissionError:
        raise ToolError(f"{name} simulator is not executable: {argv[0]}")
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        code, stdout, stderr = None, exc.stdout or b"", exc.stderr or b""
    (work / f"{stem}.{name}.stdout").write_bytes(stdout)
    (work / f"{stem}.{name}.stderr").write_bytes(stderr)
    if code == TOOL_ERROR:
        raise ToolError(f"{name} simulator failed to run {elf}:\n" + stderr.decode(errors="replace").rstrip())
    if not commit.exists():
        commit.write_bytes(b"")
    return Run(name, commit, code, stdout, stderr.decode(errors="replace"), timed_out)


def first_difference(path_a, path_b, context):
    """None if the files are identical, else (line number, matching lines
    before it, line from a, line from b, next lines of a, next lines of b).
    A line is None where that file has already ended."""
    if filecmp.cmp(path_a, path_b, shallow=False):
        return None
    with open(path_a, encoding="ascii", errors="replace") as fa, \
         open(path_b, encoding="ascii", errors="replace") as fb:
        before = collections.deque(maxlen=context)
        for number, (la, lb) in enumerate(itertools.zip_longest(fa, fb), start=1):
            if la != lb:
                after_a = [line for line in itertools.islice(fa, 2)]
                after_b = [line for line in itertools.islice(fb, 2)]
                return number, list(before), la, lb, after_a, after_b
            before.append((number, la))
    return None


def parse_line(line):
    """Commit-log fields as a dict, or None if the line is malformed."""
    parts = line.split()
    if len(parts) == 4:
        parts += [None, None, None]
    elif len(parts) == 8 and parts[4] == "M":
        del parts[4]
    else:
        return None
    return dict(zip(FIELDS, parts))


def differing_fields(la, lb):
    a, b = parse_line(la), parse_line(lb)
    if a is None or b is None:
        return None
    return [f for f in FIELDS if a[f] != b[f]]


def find_objdump(explicit):
    """llvm-objdump: --objdump, then $RV_OBJDUMP, then the path that
    mk/toolchain.mk detects, then PATH."""
    for candidate in (explicit, os.environ.get("RV_OBJDUMP")):
        if candidate:
            return candidate
    toolchain = REPO_ROOT / "mk" / "toolchain.mk"
    if toolchain.exists():
        env = {k: v for k, v in os.environ.items() if k not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL")}
        try:
            out = subprocess.run(
                ["make", "-s", "--no-print-directory", "-f", str(toolchain), "-f", "-", "print-objdump"],
                input="print-objdump:\n\t@echo $(RV_OBJDUMP)\n", capture_output=True, text=True,
                timeout=20, env=env)
            path = out.stdout.strip()
            if out.returncode == 0 and path and shutil.which(path):
                return path
        except (OSError, subprocess.SubprocessError):
            pass
    return shutil.which("llvm-objdump")


def disassemble(objdump, elf, pc):
    """'<symbol+offset>: <encoding>  <instruction>' for the word at pc."""
    if objdump is None:
        return "(no llvm-objdump found; pass --objdump)"
    try:
        out = subprocess.run([objdump, "-d", f"--start-address={pc:#x}", f"--stop-address={pc + 4:#x}", str(elf)],
                             capture_output=True, text=True, timeout=20).stdout
    except (OSError, subprocess.SubprocessError) as exc:
        return f"(objdump failed: {exc})"
    symbol = ""
    for line in out.splitlines():
        header = re.match(r"^[0-9a-f]+ <(.+)>:$", line)
        if header:
            symbol = f"<{header.group(1)}> "
        insn = re.match(r"^\s*[0-9a-f]+:\s+([0-9a-f]{8})\s+(.*)$", line)
        if insn:
            return f"{symbol}{insn.group(1)}  {' '.join(insn.group(2).split())}"
    return "(no instruction at this address in the ELF)"


def show(line):
    return line.rstrip("\n") if line is not None else "<end of log>"


def report_commit(diff, runs, objdump, elf, out):
    number, before, la, lb, after_a, after_b = diff
    rtl, iss = runs
    out.append(f"  commit log differs at line {number} ({number - 1} instructions matched)")
    for n, line in before:
        out.append(f"          {n:8d}  {show(line)}")
    out.append(f"    {rtl.name} > {number:8d}  {show(la)}")
    out.append(f"    {iss.name} > {number:8d}  {show(lb)}")
    if la is not None and lb is not None:
        fields = differing_fields(la, lb)
        out.append("          fields: " + (", ".join(fields) if fields else "malformed line"))
    elif la is None:
        out.append(f"          the {rtl.name} log ends here; the {iss.name} log goes on")
    else:
        out.append(f"          the {iss.name} log ends here; the {rtl.name} log goes on")
    for name, after in ((rtl.name, after_a), (iss.name, after_b)):
        for line in after:
            out.append(f"    {name}   then    {show(line)}")
    pcs = []
    for line in (la, lb):
        fields = parse_line(line) if line is not None else None
        if fields is not None and fields["pc"] not in pcs:
            pcs.append(fields["pc"])
    for pc in pcs:
        out.append(f"  pc {pc}: {disassemble(objdump, elf, int(pc, 16))}")


def report_stdout(rtl, iss, out):
    a, b = rtl.stdout, iss.stdout
    if a == b:
        return True
    at = next((i for i, (ca, cb) in enumerate(zip(a, b)) if ca != cb), min(len(a), len(b)))
    out.append(f"  stdout differs at byte {at} ({rtl.name} {len(a)} bytes, {iss.name} {len(b)} bytes)")
    for run, data in ((rtl, a), (iss, b)):
        window = data[max(0, at - 16):at + 16]
        out.append(f"    {run.name:>4}: {window!r}")
    return False


def describe_ending(run):
    if run.timed_out:
        return "timed out"
    if run.code < 0:
        return f"killed by signal {-run.code}"
    return f"exit {run.code}"


def compare(args):
    elf = Path(args.elf)
    if not elf.is_file():
        raise ToolError(f"no such program: {elf}")
    keep = args.work_dir is not None
    work = Path(args.work_dir) if keep else Path(tempfile.mkdtemp(prefix="difftest-"))
    work.mkdir(parents=True, exist_ok=True)
    stem = elf.stem

    try:
        runs = (run_model("rtl", shlex.split(args.rtl), elf, work, stem, args),
                run_model("iss", shlex.split(args.iss), elf, work, stem, args))
    except ToolError:
        if not keep:
            shutil.rmtree(work)
        raise
    rtl, iss = runs
    out = []
    diff = first_difference(rtl.commit, iss.commit, args.context)
    if diff is not None:
        report_commit(diff, runs, find_objdump(args.objdump), elf, out)
    stdout_ok = report_stdout(rtl, iss, out)
    endings_ok = not rtl.timed_out and not iss.timed_out and rtl.code == iss.code and rtl.code >= 0
    if not endings_ok:
        out.append(f"  ending differs: {rtl.name} {describe_ending(rtl)}, {iss.name} {describe_ending(iss)}")
    elif args.expect_exit is not None and rtl.code != args.expect_exit:
        endings_ok = False
        out.append(f"  both models exited with {rtl.code}, expected {args.expect_exit}")

    if diff is None and stdout_ok and endings_ok:
        with open(rtl.commit, "rb") as f:
            lines = sum(1 for _ in f)
        if not args.quiet:
            print(f"difftest PASS: {elf} ({lines} instructions, {len(rtl.stdout)} stdout bytes, exit {rtl.code})")
        if not keep:
            shutil.rmtree(work)
        return 0

    print(f"difftest FAIL: {elf}")
    print("\n".join(out))
    for run in runs:
        tail = run.stderr.strip().splitlines()[-3:]
        if tail:
            print(f"  {run.name} stderr: " + "\n               ".join(tail))
    print(f"  logs: {work}/{stem}.{{rtl,iss}}.{{commit,stdout,stderr}}")
    return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     epilog="Simulator commands are split like shell words, so they may carry "
                                            "extra arguments, e.g. --rtl 'build/sim/soc_sim --stats'.")
    parser.add_argument("--rtl", required=True, help="RTL simulator command")
    parser.add_argument("--iss", required=True, help="ISS command")
    parser.add_argument("--objdump", help="llvm-objdump for disassembly (default: auto-detect)")
    parser.add_argument("--work-dir", help="keep the logs in this directory (default: a temporary "
                                           "directory, deleted when the models match)")
    parser.add_argument("--max-cycles", type=int, help="passed to both simulators")
    parser.add_argument("--timeout", type=float, default=600, help="seconds per simulator (default 600)")
    parser.add_argument("--context", type=int, default=5, help="matching lines shown before a mismatch")
    parser.add_argument("--expect-exit", type=int, help="also fail unless both models exit with this code")
    parser.add_argument("-q", "--quiet", action="store_true", help="print nothing when the models match")
    parser.add_argument("elf", help="program to run")
    args = parser.parse_args()
    try:
        return compare(args)
    except ToolError as exc:
        print(f"difftest: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
