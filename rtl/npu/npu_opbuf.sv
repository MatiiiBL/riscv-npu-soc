// Operand buffer and skewing feeder for one edge of the systolic array.
//
// One bank per array row (A side) or column (B side). Bank i holds row i of
// the operand matrix as KMAX/4 little-endian 32-bit words, so the DMA writes
// one bus word per cycle, and each bank has its own read address so it can
// run i cycles behind bank 0: at compute step t, bank i presents element
// k = t - i. That offset is the systolic skew; no shift-register triangle is
// needed. Each bank is a plain simple dual-port RAM, so KMAX = 1024 maps to
// one block RAM per bank instead of flip-flops.
//
// Read pipeline for bank i (element k = t - i):
//   step t      word address k / 4 presented to the RAM, byte lane and
//               validity registered alongside
//   step t + 1  RAM word available, byte lane selected
//   step t + 2  feed[i] = A[i][k], or 0 when k is outside 0..K-1 or the row
//               is not part of this operation (i >= rows)
// Zero-gating the edges means idle PEs multiply by zero, so the array needs
// no valid bits and rows/columns beyond M/N keep their accumulators.
module npu_opbuf #(
  parameter  int ARR    = 8,
  parameter  int KMAX   = 1024,
  localparam int RW     = (ARR > 1) ? $clog2(ARR) : 1,
  localparam int CW     = $clog2(ARR + 1),
  localparam int KW     = $clog2(KMAX + 1),
  localparam int WDEPTH = (KMAX + 3) / 4,
  localparam int WAW    = (WDEPTH > 1) ? $clog2(WDEPTH) : 1,
  localparam int TW0    = $clog2(KMAX + 2 * ARR + 1),
  localparam int TW     = (TW0 > WAW + 2) ? TW0 : WAW + 2
) (
  input  logic             clk,
  input  logic             rst_n,
  // fill port (DMA read responses)
  input  logic             wr_en,
  input  logic [RW-1:0]    wr_bank,
  input  logic [WAW-1:0]   wr_addr,
  input  logic [31:0]      wr_data,
  // feed side
  input  logic             run,      // compute phase in progress
  input  logic [TW-1:0]    step,     // compute step t
  input  logic [KW-1:0]    k_len,    // K
  input  logic [CW-1:0]    rows,     // rows taking part (M or N)
  output logic [ARR*8-1:0] feed      // bank i operand at [8*i +: 8]
);

  for (genvar i = 0; i < ARR; i++) begin : g_bank
    logic [TW:0]   k_ext;   // step - i; the top bit is the borrow (step < i)
    logic [TW-1:0] k;
    logic          vld, vld_q;
    logic [1:0]    lane_q;
    logic [31:0]   word;
    logic [7:0]    feed_q;

    assign k_ext = {1'b0, step} - (TW+1)'(i);
    assign k     = k_ext[TW-1:0];
    assign vld   = run && !k_ext[TW] && (k < TW'(k_len)) && (CW'(i) < rows);

    npu_sdp_ram #(.WIDTH(32), .DEPTH(WDEPTH)) u_ram (
      .clk,
      .we    (wr_en && (wr_bank == RW'(i))),
      .waddr (wr_addr),
      .wdata (wr_data),
      .raddr (k[WAW+1:2]),
      .rdata (word)
    );

    always_ff @(posedge clk) begin
      if (!rst_n) begin
        vld_q  <= 1'b0;
        lane_q <= '0;
        feed_q <= '0;
      end else begin
        vld_q  <= vld;
        lane_q <= k[1:0];
        feed_q <= vld_q ? word[8*lane_q +: 8] : 8'd0;
      end
    end

    assign feed[8*i +: 8] = feed_q;
  end

endmodule
