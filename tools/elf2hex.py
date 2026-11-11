#!/usr/bin/env python3
"""Convert a Pulse firmware ELF into $readmemh images for IMEM and DMEM.

The images initialise the block RAMs of the FPGA build through the IMEM_INIT
and DMEM_INIT parameters of soc_top. Every file has one 32-bit little-endian
word per line as 8 hex digits, and every word that no PT_LOAD segment covers
is zero: the same contents the simulators load (docs/spec.md section 7.1).

IMEM is one file. DMEM is split into word-interleaved banks like the RTL
(rtl/soc/soc_dmem.sv): with --dmem PREFIX --dmem-banks N, bank b is written
to PREFIX<b>.hex and holds DMEM words b, b + N, b + 2N, ..., which is the
file soc_top's bank b loads for DMEM_INIT = PREFIX. N must be
NPU_DMA_BYTES / 4 of the soc_top being built (4 for the default 16-byte
NPU port).

    tools/elf2hex.py build/sw/hello.elf --imem imem.hex --dmem dmem --dmem-banks 4
    # writes imem.hex and dmem0.hex .. dmem3.hex
"""

import argparse
import struct
import sys

IMEM_BASE, IMEM_SIZE = 0x0000_0000, 64 * 1024
DMEM_BASE, DMEM_SIZE = 0x1000_0000, 256 * 1024

EM_RISCV = 243
PT_LOAD = 1


class ElfError(Exception):
    pass


def load_segments(path):
    """Returns [(paddr, bytes)] for the non-empty PT_LOAD segments of an ELF32."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise ElfError(f"{path}: not an ELF file")
    if data[4] != 1 or data[5] != 1:
        raise ElfError(f"{path}: not a 32-bit little-endian ELF")
    (machine,) = struct.unpack_from("<H", data, 18)
    if machine != EM_RISCV:
        raise ElfError(f"{path}: not a RISC-V ELF")
    (phoff,) = struct.unpack_from("<I", data, 28)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)

    segments = []
    for i in range(phnum):
        off = phoff + i * phentsize
        if off + 32 > len(data):
            raise ElfError(f"{path}: program header table is truncated")
        p_type, p_offset, _vaddr, p_paddr, p_filesz, p_memsz = struct.unpack_from(
            "<6I", data, off
        )
        if p_type != PT_LOAD or p_memsz == 0:
            continue
        if p_filesz > p_memsz or p_offset + p_filesz > len(data):
            raise ElfError(f"{path}: PT_LOAD segment lies outside the file")
        body = data[p_offset : p_offset + p_filesz] + bytes(p_memsz - p_filesz)
        segments.append((p_paddr, body))
    return segments


def build_images(segments):
    """Places each segment into IMEM or DMEM by physical address."""
    imem = bytearray(IMEM_SIZE)
    dmem = bytearray(DMEM_SIZE)
    for paddr, body in segments:
        end = paddr + len(body)
        if IMEM_BASE <= paddr and end <= IMEM_BASE + IMEM_SIZE:
            imem[paddr - IMEM_BASE : end - IMEM_BASE] = body
        elif DMEM_BASE <= paddr and end <= DMEM_BASE + DMEM_SIZE:
            dmem[paddr - DMEM_BASE : end - DMEM_BASE] = body
        else:
            raise ElfError(f"segment 0x{paddr:08x}-0x{end:08x} is outside IMEM and DMEM")
    return imem, dmem


def image_words(image):
    return struct.unpack(f"<{len(image) // 4}I", image)


def write_hex(path, words):
    with open(path, "w") as f:
        f.writelines(f"{w:08x}\n" for w in words)


def main():
    summary, _, details = __doc__.partition("\n\n")
    parser = argparse.ArgumentParser(
        description=summary, epilog=details, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("elf", help="input ELF file")
    parser.add_argument("--imem", metavar="HEX", help="write the IMEM image to this file")
    parser.add_argument(
        "--dmem", metavar="PREFIX", help="write DMEM bank b to PREFIX<b>.hex (PREFIX0.hex for one bank)"
    )
    parser.add_argument(
        "--dmem-banks",
        metavar="N",
        type=int,
        choices=(1, 2, 4),
        default=1,
        help="number of DMEM banks, NPU_DMA_BYTES / 4 (default 1)",
    )
    args = parser.parse_args()
    if args.imem is None and args.dmem is None:
        parser.error("give --imem and/or --dmem")

    try:
        imem, dmem = build_images(load_segments(args.elf))
    except (OSError, ElfError) as e:
        print(f"elf2hex: {e}", file=sys.stderr)
        return 1
    if args.imem is not None:
        write_hex(args.imem, image_words(imem))
    if args.dmem is not None:
        words = image_words(dmem)
        for b in range(args.dmem_banks):
            write_hex(f"{args.dmem}{b}.hex", words[b :: args.dmem_banks])
    return 0


if __name__ == "__main__":
    sys.exit(main())
