#!/usr/bin/env python3
"""Checks that soc_sim and the ISS accept and reject the same inputs.

    sim_cli.py GOOD.elf WORKDIR SIM...

Spec 7.1 gives both simulators one command line and one set of exit codes:
usage and ELF-load errors exit with 125, -h/--help with 0. From a good ELF
this crafts malformed ones (wrong type or machine, missing or truncated
program headers, segments outside IMEM/DMEM, a 4 GiB p_memsz) and runs each
case, plus bad command lines, on every simulator given. A case passes when
every simulator returns the expected exit code. Each run gets a 1 GiB
address-space limit where the OS enforces one (Linux), so a loader that
allocates p_memsz bytes before checking the segment aborts instead of
exiting with 125. Exits nonzero if any case fails.
"""

import resource
import struct
import subprocess
import sys
from pathlib import Path

TIME_LIMIT = 10  # seconds per run; a good ELF here finishes in well under 1
MEMORY_LIMIT = 1 << 30
USAGE_ERROR = 125


def load_headers(data):
    """Offsets of the PT_LOAD program headers of an ELF32 image."""
    (phoff,) = struct.unpack_from("<I", data, 28)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    offsets = [phoff + i * phentsize for i in range(phnum)]
    return [off for off in offsets if struct.unpack_from("<I", data, off)[0] == 1]


def patched(data, *fields):
    """A copy of `data` with each (offset, struct format, value) written in."""
    out = bytearray(data)
    for offset, fmt, value in fields:
        struct.pack_into(fmt, out, offset, value)
    return out


def elf_cases(good):
    """(name, image, expected exit code) for the crafted ELF files."""
    first = load_headers(good)[0]
    return [
        ("good", good, 0),
        ("not ELF", b"\x7fELG" + good[4:], USAGE_ERROR),
        ("too short", good[:40], USAGE_ERROR),
        ("ELF64 class", patched(good, (4, "<B", 2)), USAGE_ERROR),
        ("big-endian", patched(good, (5, "<B", 2)), USAGE_ERROR),
        ("e_type ET_REL", patched(good, (16, "<H", 1)), USAGE_ERROR),
        ("e_type ET_DYN", patched(good, (16, "<H", 3)), USAGE_ERROR),
        ("e_machine x86", patched(good, (18, "<H", 3)), USAGE_ERROR),
        ("no program headers", patched(good, (42, "<H", 0), (44, "<H", 0)), USAGE_ERROR),
        ("short phentsize", patched(good, (42, "<H", 16)), USAGE_ERROR),
        ("program headers past EOF", patched(good, (28, "<I", len(good) - 8)), USAGE_ERROR),
        ("p_memsz 4 GiB", patched(good, (first + 20, "<I", 0xFFFFFFF0)), USAGE_ERROR),
        ("p_filesz > p_memsz", patched(good, (first + 16, "<I", 0x100000)), USAGE_ERROR),
        ("p_offset past EOF", patched(good, (first + 4, "<I", len(good) + 4)), USAGE_ERROR),
        ("segment in MMIO", patched(good, (first + 12, "<I", 0x20000000)), USAGE_ERROR),
        ("segment crossing IMEM end", patched(good, (first + 12, "<I", 0xFFF0)), USAGE_ERROR),
        ("segment crossing DMEM end", patched(good, (first + 12, "<I", 0x1003FFF0)), USAGE_ERROR),
    ]


def cli_cases(elf):
    """(name, arguments, expected exit code) for command lines."""
    return [
        ("-h", ["-h"], 0),
        ("--help", ["--help"], 0),
        ("--max-cycles 100", ["--max-cycles", "100", elf], 124),
        ("--max-cycles -1", ["--max-cycles", "-1", elf], USAGE_ERROR),
        ("--max-cycles 0x10", ["--max-cycles", "0x10", elf], USAGE_ERROR),
        ("--max-cycles 1e3", ["--max-cycles", "1e3", elf], USAGE_ERROR),
        ("--max-cycles empty", ["--max-cycles", "", elf], USAGE_ERROR),
        ("--max-cycles too big", ["--max-cycles", "18446744073709551616", elf], USAGE_ERROR),
        ("--max-cycles missing", [elf, "--max-cycles"], USAGE_ERROR),
        ("--commit-log missing", [elf, "--commit-log"], USAGE_ERROR),
        ("unknown option", ["--bogus", elf], USAGE_ERROR),
        ("two programs", [elf, elf], USAGE_ERROR),
        ("no program", [], USAGE_ERROR),
        ("missing file", ["/nonexistent/program.elf"], USAGE_ERROR),
    ]


def limit_memory():
    try:
        resource.setrlimit(resource.RLIMIT_AS, (MEMORY_LIMIT, MEMORY_LIMIT))
    except (ValueError, OSError):
        pass  # not supported here (macOS); the exit codes are still checked


def run(sim, args):
    try:
        r = subprocess.run([sim, "--quiet", *args], capture_output=True, timeout=TIME_LIMIT,
                           preexec_fn=limit_memory)
        return r.returncode
    except subprocess.TimeoutExpired:
        return "timeout"


def main():
    good_path, workdir, sims = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3:]
    workdir.mkdir(parents=True, exist_ok=True)
    good = good_path.read_bytes()

    runs = []
    for i, (name, image, want) in enumerate(elf_cases(good)):
        path = workdir / f"case{i}.elf"
        path.write_bytes(bytes(image))
        runs.append((f"ELF: {name}", [str(path)], want))
    runs += [(f"CLI: {name}", args, want) for name, args, want in cli_cases(str(good_path))]

    failed = 0
    for name, args, want in runs:
        got = {Path(sim).name: run(sim, args) for sim in sims}
        if any(code != want for code in got.values()):
            failed += 1
            print(f"FAIL {name}: expected {want}, got {got}")
    print(f"simulator command line and ELF checks: {len(runs) - failed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
