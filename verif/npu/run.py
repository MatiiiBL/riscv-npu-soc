#!/usr/bin/env python3
"""Build npu_top with Verilator and run the cocotb suite for each configuration.

    run.py [--config ARR:DMA_BYTES[:KMAX] ...] [--kmax 1024] [--seed 1]
           [--random-ops 300] [--test NAME] [--waves]

A configuration without its own KMAX uses --kmax. The defaults are the
shipped sizes (8:4, 8:8, 8:16, 4:16, all at KMAX 1024) and two corners
(3:4:16, an ARR that is not a power of two; 1:16:16, a single PE on the
widest port, where a whole row of KMAX bytes fits in one block). Exits
nonzero if any build fails or any test fails in any configuration.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from cocotb_tools.runner import get_results, get_runner

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
RTL_LIST = REPO / "rtl" / "npu" / "npu.f"


def rtl_sources() -> list[Path]:
    """Source files from npu.f (one path per line, relative to the .f)."""
    lines = RTL_LIST.read_text().splitlines()
    return [RTL_LIST.parent / line.strip() for line in lines if line.strip()]


DEFAULT_CONFIGS = ["8:4", "8:8", "8:16", "4:16", "3:4:16", "1:16:16"]


def run_config(arr: int, dma_bytes: int, kmax: int,
               args: argparse.Namespace) -> tuple[int, int]:
    build_dir = REPO / "build" / "npu" / f"arr{arr}_k{kmax}_d{dma_bytes}"
    runner = get_runner("verilator")
    runner.build(
        sources=rtl_sources(),
        hdl_toplevel="npu_top",
        parameters={"ARR": arr, "KMAX": kmax, "DMA_BYTES": dma_bytes},
        # Randomize everything without a reset (RAM contents, datapath
        # registers) so the tests cannot pass by relying on zero init.
        build_args=["--x-assign", "unique", "--x-initial", "unique",
                    *(["--trace-fst"] if args.waves else [])],
        build_dir=build_dir,
        waves=args.waves,
    )
    results = runner.test(
        test_module="test_npu",
        hdl_toplevel="npu_top",
        build_dir=build_dir,
        test_dir=build_dir,
        testcase=args.test or None,
        seed=args.seed,
        plusargs=["+verilator+rand+reset+2", f"+verilator+seed+{args.seed}"],
        extra_env={
            "NPU_ARR": str(arr),
            "NPU_KMAX": str(kmax),
            "NPU_DMA_BYTES": str(dma_bytes),
            "NPU_SEED": str(args.seed),
            "NPU_RANDOM_OPS": str(args.random_ops),
            "NPU_REPORT": str(build_dir / "cycles.md"),
        },
        waves=args.waves,
    )
    return get_results(results)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--config", action="append", metavar="ARR:DMA_BYTES[:KMAX]",
                        help="configuration to run (repeatable; default "
                             + " ".join(DEFAULT_CONFIGS) + ")")
    parser.add_argument("--kmax", type=int, default=1024,
                        help="KMAX of configurations that do not give one")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--random-ops", type=int, default=300)
    parser.add_argument("--test", action="append", help="run only this test (repeatable)")
    parser.add_argument("--waves", action="store_true", help="dump an FST waveform")
    args = parser.parse_args()

    summary = []
    for config in args.config or DEFAULT_CONFIGS:
        fields = [int(x) for x in config.split(":")]
        if len(fields) not in (2, 3):
            parser.error(f"--config {config}: expected ARR:DMA_BYTES[:KMAX]")
        arr, dma_bytes, kmax = fields[0], fields[1], fields[2] if len(fields) == 3 else args.kmax
        total, failed = run_config(arr, dma_bytes, kmax, args)
        summary.append((arr, dma_bytes, kmax, total, failed))

    ok = True
    for arr, dma_bytes, kmax, total, failed in summary:
        print(f"npu ARR={arr} KMAX={kmax} DMA_BYTES={dma_bytes} seed={args.seed}: "
              f"{total - failed}/{total} tests passed")
        ok &= failed == 0 and total > 0
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
