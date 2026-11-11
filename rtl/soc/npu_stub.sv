// Placeholder for the NPU (docs/spec.md 4.4 and 5) so the SoC can be built
// and tested before the accelerator is integrated. Same module name and
// ports as the real npu_top in rtl/npu/; soc.f lists this file until then.
//
// The configuration registers are plain 32-bit storage, INFO and CYCLES read
// 0, and a START completes immediately with DONE | ERR set, so a driver
// polling for DONE terminates and can see that no work was done. The DMA
// port is idle.
module npu_top #(
  parameter int ARR  = 8,
  parameter int KMAX = 1024
) (
  input  logic        clk,
  input  logic        rst_n,

  input  logic        cfg_req,
  input  logic        cfg_we,
  input  logic [11:0] cfg_addr,
  input  logic [31:0] cfg_wdata,
  output logic [31:0] cfg_rdata,

  output logic        dma_req,
  output logic        dma_we,
  output logic [31:0] dma_addr,
  output logic [3:0]  dma_be,
  output logic [31:0] dma_wdata,
  input  logic [31:0] dma_rdata,

  output logic        irq
);

  localparam logic [11:0] REG_CTRL      = 12'h000;
  localparam logic [11:0] REG_STATUS    = 12'h004;
  localparam logic [11:0] REG_A_ADDR    = 12'h008;
  localparam logic [11:0] REG_B_ADDR    = 12'h00C;
  localparam logic [11:0] REG_C_ADDR    = 12'h010;
  localparam logic [11:0] REG_M         = 12'h014;
  localparam logic [11:0] REG_N         = 12'h018;
  localparam logic [11:0] REG_K         = 12'h01C;
  localparam logic [11:0] REG_A_STRIDE  = 12'h020;
  localparam logic [11:0] REG_B_STRIDE  = 12'h024;
  localparam logic [11:0] REG_C_STRIDE  = 12'h028;
  localparam logic [11:0] REG_QUANT     = 12'h02C;
  localparam logic [11:0] REG_BIAS_ADDR = 12'h030;
  localparam logic [11:0] REG_QMULT     = 12'h03C;

  localparam logic [2:0] STATUS_DONE_ERR = 3'b110;   // {ERR, DONE, BUSY}

  logic [31:0] a_addr, b_addr, c_addr, m, n, k;
  logic [31:0] a_stride, b_stride, c_stride, quant, bias_addr, qmult;
  logic [2:0]  status;

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      a_addr    <= '0;
      b_addr    <= '0;
      c_addr    <= '0;
      m         <= '0;
      n         <= '0;
      k         <= '0;
      a_stride  <= '0;
      b_stride  <= '0;
      c_stride  <= '0;
      quant     <= '0;
      bias_addr <= '0;
      qmult     <= 32'd1;
      status    <= '0;
    end else if (cfg_req && cfg_we) begin
      case (cfg_addr)
        REG_CTRL:      if (cfg_wdata[0]) status <= STATUS_DONE_ERR;
        REG_A_ADDR:    a_addr    <= cfg_wdata;
        REG_B_ADDR:    b_addr    <= cfg_wdata;
        REG_C_ADDR:    c_addr    <= cfg_wdata;
        REG_M:         m         <= cfg_wdata;
        REG_N:         n         <= cfg_wdata;
        REG_K:         k         <= cfg_wdata;
        REG_A_STRIDE:  a_stride  <= cfg_wdata;
        REG_B_STRIDE:  b_stride  <= cfg_wdata;
        REG_C_STRIDE:  c_stride  <= cfg_wdata;
        REG_QUANT:     quant     <= cfg_wdata;
        REG_BIAS_ADDR: bias_addr <= cfg_wdata;
        REG_QMULT:     qmult     <= cfg_wdata;
        default: ;
      endcase
    end
  end

  always_ff @(posedge clk) begin
    if (cfg_req && !cfg_we) begin
      case (cfg_addr)
        REG_STATUS:    cfg_rdata <= {29'b0, status};
        REG_A_ADDR:    cfg_rdata <= a_addr;
        REG_B_ADDR:    cfg_rdata <= b_addr;
        REG_C_ADDR:    cfg_rdata <= c_addr;
        REG_M:         cfg_rdata <= m;
        REG_N:         cfg_rdata <= n;
        REG_K:         cfg_rdata <= k;
        REG_A_STRIDE:  cfg_rdata <= a_stride;
        REG_B_STRIDE:  cfg_rdata <= b_stride;
        REG_C_STRIDE:  cfg_rdata <= c_stride;
        REG_QUANT:     cfg_rdata <= quant;
        REG_BIAS_ADDR: cfg_rdata <= bias_addr;
        REG_QMULT:     cfg_rdata <= qmult;
        default:       cfg_rdata <= '0;   // CTRL, CYCLES, INFO, undefined
      endcase
    end
  end

  assign dma_req   = 1'b0;
  assign dma_we    = 1'b0;
  assign dma_addr  = '0;
  assign dma_be    = '0;
  assign dma_wdata = '0;
  assign irq       = status[1];

  logic unused;
  assign unused = ^{dma_rdata, ARR, KMAX};

endmodule
