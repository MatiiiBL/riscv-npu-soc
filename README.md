# Pulse

A RISC-V SoC with a systolic-array neural network accelerator, written in SystemVerilog.

- **CPU:** 5-stage pipelined RV32IM core with forwarding and branch prediction
- **NPU:** 8x8 INT8 systolic array with DMA, bias, requantization and ReLU
- **Verification:** official riscv-tests, random differential testing against a C++ ISS, cocotb unit tests
- **Workloads:** quantized MNIST MLP, GEMM and CoreMark, compiled with clang

Work in progress. The design contract is in [docs/spec.md](docs/spec.md).
