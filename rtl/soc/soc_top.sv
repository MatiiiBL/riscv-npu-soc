// Pulse SoC top level (docs/spec.md 1 and 3).
//
//   rv32_core --imem--> IMEM 64 KiB (single-port block RAM)
//             --dmem--> soc_bus --+--> DMEM 256 KiB port A, 32 bits
//                                 +--> soc_mmio (UART, SIM_EXIT, LEDS)
//                                 +--> npu_top registers
//   npu_top DMA --------> soc_bus ----> DMEM port B, NPU_DMA_BYTES wide
//
// DMEM (soc_dmem) is NPU_DMA_BYTES / 4 word-interleaved banks of true
// dual-port block RAM, so the NPU moves one aligned NPU_DMA_BYTES block per
// cycle while the CPU still sees a plain 32-bit memory.
//
// The sim_* outputs, the retirement port and the performance counters exist
// for the Verilator harness; an FPGA wrapper leaves them unconnected.
module soc_top #(
  parameter int BP_MODE     = 1,
  parameter int BTB_ENTRIES = 64,
  parameter int UART_DIV    = 217,    // clocks per UART bit (25 MHz / 115200 baud)
  parameter bit SIM         = 1'b0,   // UART_STATUS always reads ready
  parameter int NPU_DMA_BYTES = 16,   // NPU DMA port width: 4, 8 or 16 bytes
  // $readmemh images for the FPGA build (tools/elf2hex.py). IMEM_INIT is one
  // file; DMEM_INIT is a path prefix, and DMEM bank b loads
  // <DMEM_INIT><b>.hex (elf2hex --dmem-banks NPU_DMA_BYTES/4).
  parameter     IMEM_INIT   = "",
  parameter     DMEM_INIT   = ""
) (
  input  logic        clk,
  input  logic        rst_n,

  output logic        uart_txd,
  output logic [7:0]  leds,

  // Simulation taps
  output logic        sim_uart_valid,    // byte written to UART_TX this cycle
  output logic [7:0]  sim_uart_data,
  output logic        sim_exit_valid,    // SIM_EXIT written this cycle
  output logic [31:0] sim_exit_code,

  output logic        rvfi_valid,
  output logic [31:0] rvfi_pc,
  output logic [31:0] rvfi_insn,
  output logic [4:0]  rvfi_rd,
  output logic [31:0] rvfi_rd_wdata,
  output logic [31:0] rvfi_mem_addr,
  output logic [3:0]  rvfi_mem_wmask,
  output logic [31:0] rvfi_mem_wdata,
  output logic        halted,

  output logic [63:0] perf_cycles,
  output logic [63:0] perf_instret,
  output logic [31:0] perf_load_use,
  output logic [31:0] perf_mispredict,
  output logic [31:0] perf_div_stall,
  output logic [31:0] perf_ctrl
);

  localparam int IMEM_WORDS = 16384;   // 64 KiB
  localparam int DMEM_WORDS = 65536;   // 256 KiB
  localparam int DMEM_BANKS = NPU_DMA_BYTES / 4;
  localparam int IMEM_AW    = $clog2(IMEM_WORDS);
  localparam int DMEM_AW    = $clog2(DMEM_WORDS);
  localparam int BLOCK_AW   = $clog2(DMEM_WORDS / DMEM_BANKS);

  if (NPU_DMA_BYTES != 4 && NPU_DMA_BYTES != 8 && NPU_DMA_BYTES != 16) begin : g_bad_dma_bytes
    $error("soc_top: NPU_DMA_BYTES must be 4, 8 or 16");
  end

  // ------------------------------------------------------------------ core
  logic        imem_req;
  logic [31:0] imem_addr, imem_rdata;
  logic        dmem_req, dmem_we;
  logic [31:0] dmem_addr, dmem_wdata, dmem_rdata;
  logic [3:0]  dmem_be;

  rv32_core #(
    .BP_MODE     (BP_MODE),
    .BTB_ENTRIES (BTB_ENTRIES)
  ) u_core (
    .clk             (clk),
    .rst_n           (rst_n),
    .imem_req        (imem_req),
    .imem_addr       (imem_addr),
    .imem_rdata      (imem_rdata),
    .dmem_req        (dmem_req),
    .dmem_we         (dmem_we),
    .dmem_addr       (dmem_addr),
    .dmem_be         (dmem_be),
    .dmem_wdata      (dmem_wdata),
    .dmem_rdata      (dmem_rdata),
    .rvfi_valid      (rvfi_valid),
    .rvfi_pc         (rvfi_pc),
    .rvfi_insn       (rvfi_insn),
    .rvfi_rd         (rvfi_rd),
    .rvfi_rd_wdata   (rvfi_rd_wdata),
    .rvfi_mem_addr   (rvfi_mem_addr),
    .rvfi_mem_wmask  (rvfi_mem_wmask),
    .rvfi_mem_wdata  (rvfi_mem_wdata),
    .halted          (halted),
    .perf_cycles     (perf_cycles),
    .perf_instret    (perf_instret),
    .perf_load_use   (perf_load_use),
    .perf_mispredict (perf_mispredict),
    .perf_div_stall  (perf_div_stall),
    .perf_ctrl       (perf_ctrl)
  );

  // ------------------------------------------------------------------ IMEM
  // Fetches outside the 64 KiB wrap around; nothing legal produces them.
  bram_sp #(
    .WORDS     (IMEM_WORDS),
    .INIT_FILE (IMEM_INIT)
  ) u_imem (
    .clk   (clk),
    .en    (imem_req),
    .addr  (imem_addr[IMEM_AW+1:2]),
    .rdata (imem_rdata)
  );

  // ---------------------------------------------------------- interconnect
  logic                     dma_req, dma_we;
  logic [31:0]              dma_addr;
  logic [NPU_DMA_BYTES-1:0] dma_be;
  logic [8*NPU_DMA_BYTES-1:0] dma_wdata, dma_rdata;

  logic                     dmem_a_en, dmem_a_we;
  logic [3:0]               dmem_a_be;
  logic [DMEM_AW-1:0]       dmem_a_addr;
  logic [31:0]              dmem_a_wdata, dmem_a_rdata;

  logic                     dmem_b_en, dmem_b_we;
  logic [NPU_DMA_BYTES-1:0] dmem_b_be;
  logic [BLOCK_AW-1:0]      dmem_b_addr;
  logic [8*NPU_DMA_BYTES-1:0] dmem_b_wdata, dmem_b_rdata;

  logic                mmio_req, mmio_we;
  logic [11:0]         mmio_addr;
  logic [3:0]          mmio_be;
  logic [31:0]         mmio_wdata, mmio_rdata;

  logic                npu_req, npu_we;
  logic [11:0]         npu_addr;
  logic [31:0]         npu_wdata, npu_rdata;

  soc_bus #(
    .DMEM_WORDS (DMEM_WORDS),
    .DMA_BYTES  (NPU_DMA_BYTES)
  ) u_bus (
    .clk          (clk),
    .rst_n        (rst_n),
    .cpu_req      (dmem_req),
    .cpu_we       (dmem_we),
    .cpu_addr     (dmem_addr),
    .cpu_be       (dmem_be),
    .cpu_wdata    (dmem_wdata),
    .cpu_rdata    (dmem_rdata),
    .dma_req      (dma_req),
    .dma_we       (dma_we),
    .dma_addr     (dma_addr),
    .dma_be       (dma_be),
    .dma_wdata    (dma_wdata),
    .dma_rdata    (dma_rdata),
    .dmem_a_en    (dmem_a_en),
    .dmem_a_we    (dmem_a_we),
    .dmem_a_be    (dmem_a_be),
    .dmem_a_addr  (dmem_a_addr),
    .dmem_a_wdata (dmem_a_wdata),
    .dmem_a_rdata (dmem_a_rdata),
    .dmem_b_en    (dmem_b_en),
    .dmem_b_we    (dmem_b_we),
    .dmem_b_be    (dmem_b_be),
    .dmem_b_addr  (dmem_b_addr),
    .dmem_b_wdata (dmem_b_wdata),
    .dmem_b_rdata (dmem_b_rdata),
    .mmio_req     (mmio_req),
    .mmio_we      (mmio_we),
    .mmio_addr    (mmio_addr),
    .mmio_be      (mmio_be),
    .mmio_wdata   (mmio_wdata),
    .mmio_rdata   (mmio_rdata),
    .npu_req      (npu_req),
    .npu_we       (npu_we),
    .npu_addr     (npu_addr),
    .npu_wdata    (npu_wdata),
    .npu_rdata    (npu_rdata)
  );

  // ------------------------------------------------------------------ DMEM
  soc_dmem #(
    .WORDS     (DMEM_WORDS),
    .BANKS     (DMEM_BANKS),
    .INIT_FILE (DMEM_INIT)
  ) u_dmem (
    .clk     (clk),
    .a_en    (dmem_a_en),
    .a_we    (dmem_a_we),
    .a_be    (dmem_a_be),
    .a_addr  (dmem_a_addr),
    .a_wdata (dmem_a_wdata),
    .a_rdata (dmem_a_rdata),
    .b_en    (dmem_b_en),
    .b_we    (dmem_b_we),
    .b_be    (dmem_b_be),
    .b_addr  (dmem_b_addr),
    .b_wdata (dmem_b_wdata),
    .b_rdata (dmem_b_rdata)
  );

  // ----------------------------------------------------------- peripherals
  soc_mmio #(
    .UART_DIV (UART_DIV),
    .SIM      (SIM)
  ) u_mmio (
    .clk        (clk),
    .rst_n      (rst_n),
    .req        (mmio_req),
    .we         (mmio_we),
    .addr       (mmio_addr),
    .be         (mmio_be),
    .wdata      (mmio_wdata),
    .rdata      (mmio_rdata),
    .uart_txd   (uart_txd),
    .leds       (leds),
    .tx_valid   (sim_uart_valid),
    .tx_data    (sim_uart_data),
    .exit_valid (sim_exit_valid),
    .exit_code  (sim_exit_code)
  );

  // ------------------------------------------------------------------- NPU
  // irq is not wired to the core (no interrupts); software polls STATUS.
  logic npu_irq;

  npu_top #(
    .DMA_BYTES (NPU_DMA_BYTES)
  ) u_npu (
    .clk       (clk),
    .rst_n     (rst_n),
    .cfg_req   (npu_req),
    .cfg_we    (npu_we),
    .cfg_addr  (npu_addr),
    .cfg_wdata (npu_wdata),
    .cfg_rdata (npu_rdata),
    .dma_req   (dma_req),
    .dma_we    (dma_we),
    .dma_addr  (dma_addr),
    .dma_be    (dma_be),
    .dma_wdata (dma_wdata),
    .dma_rdata (dma_rdata),
    .irq       (npu_irq)
  );

  logic unused;
  assign unused = ^{imem_addr[31:IMEM_AW+2], imem_addr[1:0], npu_irq};

endmodule
