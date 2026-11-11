// Pulse NPU: an ARR x ARR output-stationary systolic array of INT8 MACs with
// INT32 accumulators, local operand buffers, a DMA engine on DMEM port B and
// a fused bias / requantize / ReLU write-back stage.
//
// Programming model: docs/spec.md section 5. Design notes: docs/npu.md.
//
// An operation runs four phases back to back:
//   LOAD     npu_dma copies M rows of A, N rows of B and the N bias words
//            (when used) into the buffers: one bus word per cycle
//   COMPUTE  npu_opbuf feeds the array with skewed rows; K + M + N + 1
//            cycles, including the fill and drain of the wavefront
//   WB       npu_wb post-processes and writes C, one element per cycle
//   finish   BUSY drops, DONE and irq rise
// Rejected parameters finish immediately with ERR and touch no memory.
//
// ARR must be 1..255 and KMAX 1..65535 so they fit the INFO fields.
module npu_top #(
  parameter int ARR  = 8,
  parameter int KMAX = 1024
) (
  input  logic        clk,
  input  logic        rst_n,
  // register port, driven by the SoC decoder (offset within the NPU window)
  input  logic        cfg_req,
  input  logic        cfg_we,
  input  logic [11:0] cfg_addr,
  input  logic [31:0] cfg_wdata,
  output logic [31:0] cfg_rdata,
  // DMA master on DMEM port B
  output logic        dma_req,
  output logic        dma_we,
  output logic [31:0] dma_addr,
  output logic [3:0]  dma_be,
  output logic [31:0] dma_wdata,
  input  logic [31:0] dma_rdata,
  output logic        irq
);

  localparam int RW     = (ARR > 1) ? $clog2(ARR) : 1;
  localparam int CW     = $clog2(ARR + 1);
  localparam int KW     = $clog2(KMAX + 1);
  localparam int WDEPTH = (KMAX + 3) / 4;
  localparam int WAW    = (WDEPTH > 1) ? $clog2(WDEPTH) : 1;
  localparam int TW0    = $clog2(KMAX + 2 * ARR + 1);
  localparam int TW     = (TW0 > WAW + 2) ? TW0 : WAW + 2;

  localparam logic [31:0] VERSION = 32'd1;
  localparam logic [31:0] INFO    = (VERSION << 24) | (32'(KMAX) << 8) | 32'(ARR);

  // Register word offsets (cfg_addr[5:2]).
  localparam logic [3:0] R_CTRL      = 4'h0;
  localparam logic [3:0] R_STATUS    = 4'h1;
  localparam logic [3:0] R_A_ADDR    = 4'h2;
  localparam logic [3:0] R_B_ADDR    = 4'h3;
  localparam logic [3:0] R_C_ADDR    = 4'h4;
  localparam logic [3:0] R_M         = 4'h5;
  localparam logic [3:0] R_N         = 4'h6;
  localparam logic [3:0] R_K         = 4'h7;
  localparam logic [3:0] R_A_STRIDE  = 4'h8;
  localparam logic [3:0] R_B_STRIDE  = 4'h9;
  localparam logic [3:0] R_C_STRIDE  = 4'hA;
  localparam logic [3:0] R_QUANT     = 4'hB;
  localparam logic [3:0] R_BIAS_ADDR = 4'hC;
  localparam logic [3:0] R_CYCLES    = 4'hD;
  localparam logic [3:0] R_INFO      = 4'hE;
  localparam logic [3:0] R_QMULT     = 4'hF;

  // ------------------------------------------------------------ registers
  logic [31:0] a_addr_q, b_addr_q, c_addr_q, bias_addr_q;
  logic [31:0] m_q, n_q, k_q;
  logic [31:0] a_stride_q, b_stride_q, c_stride_q;
  logic [31:0] quant_q, qmult_q, cycles_q;
  logic        busy_q, done_q, err_q;
  logic        keep_q, nowb_q;   // CTRL.ACC_KEEP / CTRL.NO_WB of the running op

  logic [3:0] reg_idx;
  logic       reg_hit, reg_wr, start;
  assign reg_idx = cfg_addr[5:2];
  assign reg_hit = (cfg_addr[11:6] == 6'd0);
  // Every register write is dropped while an operation runs, START included.
  assign reg_wr  = cfg_req && cfg_we && reg_hit && !busy_q;
  assign start   = reg_wr && (reg_idx == R_CTRL) && cfg_wdata[0];

  // cfg_addr is word aligned by contract.
  logic unused_cfg_addr;
  assign unused_cfg_addr = ^cfg_addr[1:0];

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      a_addr_q    <= '0;
      b_addr_q    <= '0;
      c_addr_q    <= '0;
      m_q         <= '0;
      n_q         <= '0;
      k_q         <= '0;
      a_stride_q  <= '0;
      b_stride_q  <= '0;
      c_stride_q  <= '0;
      quant_q     <= '0;
      bias_addr_q <= '0;
      qmult_q     <= 32'd1;
    end else if (reg_wr) begin
      unique case (reg_idx)
        R_A_ADDR:    a_addr_q    <= cfg_wdata;
        R_B_ADDR:    b_addr_q    <= cfg_wdata;
        R_C_ADDR:    c_addr_q    <= cfg_wdata;
        R_M:         m_q         <= cfg_wdata;
        R_N:         n_q         <= cfg_wdata;
        R_K:         k_q         <= cfg_wdata;
        R_A_STRIDE:  a_stride_q  <= cfg_wdata;
        R_B_STRIDE:  b_stride_q  <= cfg_wdata;
        R_C_STRIDE:  c_stride_q  <= cfg_wdata;
        R_QUANT:     quant_q     <= cfg_wdata;
        R_BIAS_ADDR: bias_addr_q <= cfg_wdata;
        R_QMULT:     qmult_q     <= cfg_wdata;
        default: ;   // CTRL acts below; STATUS, CYCLES, INFO are read-only
      endcase
    end
  end

  logic [31:0] rd_val;
  always_comb begin
    rd_val = '0;
    if (reg_hit) begin
      unique case (reg_idx)
        R_STATUS:    rd_val = {29'd0, err_q, done_q, busy_q};
        R_A_ADDR:    rd_val = a_addr_q;
        R_B_ADDR:    rd_val = b_addr_q;
        R_C_ADDR:    rd_val = c_addr_q;
        R_M:         rd_val = m_q;
        R_N:         rd_val = n_q;
        R_K:         rd_val = k_q;
        R_A_STRIDE:  rd_val = a_stride_q;
        R_B_STRIDE:  rd_val = b_stride_q;
        R_C_STRIDE:  rd_val = c_stride_q;
        R_QUANT:     rd_val = quant_q;
        R_BIAS_ADDR: rd_val = bias_addr_q;
        R_CYCLES:    rd_val = cycles_q;
        R_INFO:      rd_val = INFO;
        R_QMULT:     rd_val = qmult_q;
        default:     rd_val = '0;   // CTRL is write-only
      endcase
    end
  end

  always_ff @(posedge clk) begin
    if (!rst_n)                  cfg_rdata <= '0;
    else if (cfg_req && !cfg_we) cfg_rdata <= rd_val;
  end

  // Parameter check of spec 5.1, on the full 32-bit register values.
  logic shape_ok, align_ok, params_ok;
  assign shape_ok  = (m_q != 0) && (m_q <= 32'(ARR)) &&
                     (n_q != 0) && (n_q <= 32'(ARR)) &&
                     (k_q != 0) && (k_q <= 32'(KMAX));
  assign align_ok  = ~|{a_addr_q[1:0], b_addr_q[1:0], c_addr_q[1:0], bias_addr_q[1:0],
                        a_stride_q[1:0], b_stride_q[1:0], c_stride_q[1:0]};
  assign params_ok = shape_ok && align_ok;

  // After the check these narrow copies hold the values exactly.
  logic [CW-1:0] m_rows, n_rows;
  logic [KW-1:0] k_len;
  assign m_rows = m_q[CW-1:0];
  assign n_rows = n_q[CW-1:0];
  assign k_len  = k_q[KW-1:0];

  logic bias_en;
  assign bias_en = (bias_addr_q != 32'd0);

  // ------------------------------------------------------------ sequencer
  typedef enum logic [1:0] {S_IDLE, S_LOAD, S_COMPUTE, S_WB} state_t;
  state_t        state_q;
  logic [TW-1:0] step_q, step_last;
  logic          dma_done, wb_done;
  logic          dma_start, wb_start, compute_end, acc_clr;

  // The last product reaches PE (M-1, N-1) at step K + M + N - 1 and is
  // accumulated one cycle later (see npu_opbuf and npu_pe for the stages).
  assign step_last   = TW'(k_len) + TW'(m_rows) + TW'(n_rows);
  assign compute_end = (state_q == S_COMPUTE) && (step_q == step_last);
  assign dma_start   = (state_q == S_IDLE) && start && params_ok;
  assign wb_start    = compute_end && !nowb_q;
  assign acc_clr     = (state_q == S_COMPUTE) && (step_q == '0) && !keep_q;

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      state_q  <= S_IDLE;
      step_q   <= '0;
      busy_q   <= 1'b0;
      done_q   <= 1'b0;
      err_q    <= 1'b0;
      keep_q   <= 1'b0;
      nowb_q   <= 1'b0;
      cycles_q <= '0;
    end else begin
      if (busy_q) cycles_q <= cycles_q + 32'd1;

      unique case (state_q)
        S_IDLE: begin
          if (start) begin
            busy_q   <= params_ok;
            done_q   <= !params_ok;
            err_q    <= !params_ok;
            keep_q   <= cfg_wdata[1];
            nowb_q   <= cfg_wdata[2];
            cycles_q <= '0;
            if (params_ok) state_q <= S_LOAD;
          end
        end
        S_LOAD: begin
          step_q <= '0;
          if (dma_done) state_q <= S_COMPUTE;
        end
        S_COMPUTE: begin
          step_q <= step_q + TW'(1);
          if (compute_end) begin
            if (nowb_q) begin
              state_q <= S_IDLE;
              busy_q  <= 1'b0;
              done_q  <= 1'b1;
            end else begin
              state_q <= S_WB;
            end
          end
        end
        S_WB: begin
          if (wb_done) begin
            state_q <= S_IDLE;
            busy_q  <= 1'b0;
            done_q  <= 1'b1;
          end
        end
        default: state_q <= S_IDLE;
      endcase
    end
  end

  assign irq = done_q;

  // ------------------------------------------------------------ datapath
  logic           rd_req, fill_a, fill_b, fill_bias;
  logic [31:0]    rd_addr;
  logic [RW-1:0]  fill_row;
  logic [WAW-1:0] fill_word;

  npu_dma #(.ARR(ARR), .KMAX(KMAX)) u_dma (
    .clk,
    .rst_n,
    .start     (dma_start),
    .a_addr    (a_addr_q),
    .b_addr    (b_addr_q),
    .bias_addr (bias_addr_q),
    .a_stride  (a_stride_q),
    .b_stride  (b_stride_q),
    .m_rows,
    .n_rows,
    .k_len,
    .load_bias (bias_en && !nowb_q),
    .rd_req,
    .rd_addr,
    .fill_a,
    .fill_b,
    .fill_bias,
    .fill_row,
    .fill_word,
    .done      (dma_done)
  );

  logic             run;
  logic [ARR*8-1:0] a_feed, b_feed;
  assign run = (state_q == S_COMPUTE);

  npu_opbuf #(.ARR(ARR), .KMAX(KMAX)) u_abuf (
    .clk,
    .rst_n,
    .wr_en   (fill_a),
    .wr_bank (fill_row),
    .wr_addr (fill_word),
    .wr_data (dma_rdata),
    .run,
    .step    (step_q),
    .k_len,
    .rows    (m_rows),
    .feed    (a_feed)
  );

  npu_opbuf #(.ARR(ARR), .KMAX(KMAX)) u_bbuf (
    .clk,
    .rst_n,
    .wr_en   (fill_b),
    .wr_bank (fill_row),
    .wr_addr (fill_word),
    .wr_data (dma_rdata),
    .run,
    .step    (step_q),
    .k_len,
    .rows    (n_rows),
    .feed    (b_feed)
  );

  logic [RW-1:0] bias_raddr;
  logic [31:0]   bias_rdata;

  npu_sdp_ram #(.WIDTH(32), .DEPTH(ARR)) u_bias (
    .clk,
    .we    (fill_bias),
    .waddr (fill_row),
    .wdata (dma_rdata),
    .raddr (bias_raddr),
    .rdata (bias_rdata)
  );

  logic [ARR*ARR*32-1:0] acc;

  npu_array #(.ARR(ARR)) u_array (
    .clk,
    .rst_n,
    .clr    (acc_clr),
    .a_feed,
    .b_feed,
    .acc
  );

  logic        wr_req;
  logic [31:0] wr_addr, wr_data;
  logic [3:0]  wr_be;

  npu_wb #(.ARR(ARR)) u_wb (
    .clk,
    .rst_n,
    .start      (wb_start),
    .m_rows,
    .n_rows,
    .c_addr     (c_addr_q),
    .c_stride   (c_stride_q),
    .bias_en,
    .out_int8   (quant_q[8]),
    .relu       (quant_q[9]),
    .shift      (quant_q[4:0]),
    .qmult      (qmult_q[15:0]),
    .acc,
    .bias_raddr,
    .bias_rdata,
    .wr_req,
    .wr_addr,
    .wr_be,
    .wr_data,
    .done       (wb_done)
  );

  // The load and write-back phases never overlap, so the two masters share
  // the port through a plain mux. Reads fetch whole words.
  assign dma_req   = rd_req || wr_req;
  assign dma_we    = wr_req;
  assign dma_addr  = wr_req ? wr_addr : rd_addr;
  assign dma_be    = wr_req ? wr_be : 4'b1111;
  assign dma_wdata = wr_data;

endmodule
