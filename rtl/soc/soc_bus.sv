// SoC address decoder (docs/spec.md 3 and 4).
//
// CPU data port: the request is routed to DMEM port A, the MMIO block or the
// NPU register file by address; only the listed region sizes are backed, and
// any other address reads 0 and drops writes. Every slave answers one cycle
// later, so the selection is registered and steers the returned read data in
// the following cycle.
//
// NPU DMA port: each access is one aligned DMA_BYTES block, mapped straight
// onto DMEM port B as block (addr - DMEM base) / DMA_BYTES, with the same
// rule for addresses outside DMEM.
module soc_bus #(
  parameter int DMEM_WORDS = 65536,
  parameter int DMA_BYTES  = 16
) (
  input  logic        clk,
  input  logic        rst_n,

  // Core data port
  input  logic        cpu_req,
  input  logic        cpu_we,
  input  logic [31:0] cpu_addr,
  input  logic [3:0]  cpu_be,
  input  logic [31:0] cpu_wdata,
  output logic [31:0] cpu_rdata,

  // NPU DMA master
  input  logic                   dma_req,
  input  logic                   dma_we,
  input  logic [31:0]            dma_addr,   // DMA_BYTES aligned
  input  logic [DMA_BYTES-1:0]   dma_be,
  input  logic [8*DMA_BYTES-1:0] dma_wdata,
  output logic [8*DMA_BYTES-1:0] dma_rdata,

  // DMEM port A (CPU)
  output logic                          dmem_a_en,
  output logic                          dmem_a_we,
  output logic [3:0]                    dmem_a_be,
  output logic [$clog2(DMEM_WORDS)-1:0] dmem_a_addr,
  output logic [31:0]                   dmem_a_wdata,
  input  logic [31:0]                   dmem_a_rdata,

  // DMEM port B (NPU DMA), one DMA_BYTES block per access
  output logic                                      dmem_b_en,
  output logic                                      dmem_b_we,
  output logic [DMA_BYTES-1:0]                      dmem_b_be,
  output logic [$clog2(DMEM_WORDS*4/DMA_BYTES)-1:0] dmem_b_addr,   // block address
  output logic [8*DMA_BYTES-1:0]                    dmem_b_wdata,
  input  logic [8*DMA_BYTES-1:0]                    dmem_b_rdata,

  // MMIO peripherals (byte offset within the 4 KiB window)
  output logic                          mmio_req,
  output logic                          mmio_we,
  output logic [11:0]                   mmio_addr,
  output logic [3:0]                    mmio_be,
  output logic [31:0]                   mmio_wdata,
  input  logic [31:0]                   mmio_rdata,

  // NPU register port
  output logic                          npu_req,
  output logic                          npu_we,
  output logic [11:0]                   npu_addr,
  output logic [31:0]                   npu_wdata,
  input  logic [31:0]                   npu_rdata
);

  localparam int DMEM_AW = $clog2(DMEM_WORDS);
  localparam int DMEM_HI = DMEM_AW + 2;    // first byte-address bit above DMEM
  localparam int BLK_LO  = $clog2(DMA_BYTES);   // first byte-address bit of a block

  localparam logic [31:0] DMEM_BASE = 32'h1000_0000;
  localparam logic [31:0] MMIO_BASE = 32'h2000_0000;
  localparam logic [31:0] NPU_BASE  = 32'h3000_0000;

  // ------------------------------------------------------------- CPU port
  logic sel_dmem, sel_mmio, sel_npu;

  assign sel_dmem = (cpu_addr[31:DMEM_HI] == DMEM_BASE[31:DMEM_HI]);
  assign sel_mmio = (cpu_addr[31:12] == MMIO_BASE[31:12]);
  assign sel_npu  = (cpu_addr[31:12] == NPU_BASE[31:12]);

  assign dmem_a_en    = cpu_req && sel_dmem;
  assign dmem_a_we    = cpu_we;
  assign dmem_a_be    = cpu_be;
  assign dmem_a_addr  = cpu_addr[DMEM_HI-1:2];
  assign dmem_a_wdata = cpu_wdata;

  assign mmio_req   = cpu_req && sel_mmio;
  assign mmio_we    = cpu_we;
  assign mmio_addr  = cpu_addr[11:0];
  assign mmio_be    = cpu_be;
  assign mmio_wdata = cpu_wdata;

  assign npu_req   = cpu_req && sel_npu;
  assign npu_we    = cpu_we;
  assign npu_addr  = cpu_addr[11:0];
  assign npu_wdata = cpu_wdata;

  typedef enum logic [1:0] {
    RD_NONE,
    RD_DMEM,
    RD_MMIO,
    RD_NPU
  } rd_src_e;

  rd_src_e rd_src;

  always_ff @(posedge clk) begin
    if (!rst_n || !cpu_req || cpu_we) rd_src <= RD_NONE;
    else if (sel_dmem)                rd_src <= RD_DMEM;
    else if (sel_mmio)                rd_src <= RD_MMIO;
    else if (sel_npu)                 rd_src <= RD_NPU;
    else                              rd_src <= RD_NONE;
  end

  always_comb begin
    case (rd_src)
      RD_DMEM: cpu_rdata = dmem_a_rdata;
      RD_MMIO: cpu_rdata = mmio_rdata;
      RD_NPU:  cpu_rdata = npu_rdata;
      default: cpu_rdata = '0;
    endcase
  end

  // ------------------------------------------------------------- DMA port
  logic dma_in_dmem, dma_rd_ok;

  assign dma_in_dmem = (dma_addr[31:DMEM_HI] == DMEM_BASE[31:DMEM_HI]);

  assign dmem_b_en    = dma_req && dma_in_dmem;
  assign dmem_b_we    = dma_we;
  assign dmem_b_be    = dma_be;
  assign dmem_b_addr  = dma_addr[DMEM_HI-1:BLK_LO];
  assign dmem_b_wdata = dma_wdata;

  always_ff @(posedge clk) begin
    if (!rst_n) dma_rd_ok <= 1'b0;
    else        dma_rd_ok <= dma_req && dma_in_dmem && !dma_we;
  end

  assign dma_rdata = dma_rd_ok ? dmem_b_rdata : '0;

  logic unused;
  assign unused = ^{dma_addr[BLK_LO-1:0]};

endmodule
