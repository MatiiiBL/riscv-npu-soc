// Write-back unit: drains the M x N accumulator tile in row-major order, one
// element per cycle, through a fused bias / requantize / ReLU pipeline and
// writes C back over the DMA port.
//
//   S0  issue (m, n); bias RAM read address = n
//   S1  acc[m][n] selected, bias word available
//   S2  v = acc + bias                                      (INT32, wrapping)
//   S3  partial products of v * q  (two 16-bit halves of v, one DSP each)
//   S4  p = v * q + round  (exact, 48 bits)
//   S5  r = p >>> shift    (arithmetic)
//   S6  ReLU, clamp to INT8 or keep INT32, pack, register the DMA write
//
// INT32 output reuses the same pipeline with q = 1 and shift = 0, so r == v.
// INT8 results of one row are packed into words and written with byte
// enables, so bytes of C between and after the outputs are never touched.
// `done` pulses in the cycle the last write is on the bus.
module npu_wb #(
  parameter  int ARR = 8,
  localparam int RW  = (ARR > 1) ? $clog2(ARR) : 1,
  localparam int CW  = $clog2(ARR + 1)
) (
  input  logic                  clk,
  input  logic                  rst_n,
  input  logic                  start,
  // operation parameters, stable while the drain runs
  input  logic [CW-1:0]         m_rows,
  input  logic [CW-1:0]         n_rows,
  input  logic [31:0]           c_addr,
  input  logic [31:0]           c_stride,
  input  logic                  bias_en,
  input  logic                  out_int8,
  input  logic                  relu,
  input  logic [4:0]            shift,
  input  logic signed [15:0]    qmult,
  // accumulators, PE (i, j) at [32*(i*ARR + j) +: 32]
  input  logic [ARR*ARR*32-1:0] acc,
  // bias RAM read port (one cycle latency)
  output logic [RW-1:0]         bias_raddr,
  input  logic [31:0]           bias_rdata,
  // write request (registered)
  output logic                  wr_req,
  output logic [31:0]           wr_addr,
  output logic [3:0]            wr_be,
  output logic [31:0]           wr_data,
  output logic                  done
);

  logic [ARR-1:0][ARR-1:0][31:0] acc_tile;
  assign acc_tile = acc;

  // ---------------------------------------------------------------- S0
  logic          busy_q, issue_q;
  logic [RW-1:0] im_q, in_q;
  logic          row_end, tile_end;
  assign row_end    = (in_q == RW'(n_rows - CW'(1)));
  assign tile_end   = row_end && (im_q == RW'(m_rows - CW'(1)));
  assign bias_raddr = in_q;

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      issue_q <= 1'b0;
      im_q    <= '0;
      in_q    <= '0;
    end else if (start) begin
      issue_q <= 1'b1;
      im_q    <= '0;
      in_q    <= '0;
    end else if (issue_q) begin
      if (tile_end) issue_q <= 1'b0;
      if (row_end) begin
        in_q <= '0;
        im_q <= im_q + RW'(1);
      end else begin
        in_q <= in_q + RW'(1);
      end
    end
  end

  // Per-element tag travelling with the data: valid, column, last in row.
  logic          s1_v, s2_v, s3_v, s4_v, s5_v;
  logic [RW-1:0] s1_n, s2_n, s3_n, s4_n, s5_n;
  logic          s1_eol, s2_eol, s3_eol, s4_eol, s5_eol;

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      {s1_v, s2_v, s3_v, s4_v, s5_v} <= '0;
    end else begin
      {s1_v, s2_v, s3_v, s4_v, s5_v} <= {issue_q, s1_v, s2_v, s3_v, s4_v};
    end
  end

  always_ff @(posedge clk) begin
    {s1_n,   s2_n,   s3_n,   s4_n,   s5_n}   <= {in_q,    s1_n,   s2_n,   s3_n,   s4_n};
    {s1_eol, s2_eol, s3_eol, s4_eol, s5_eol} <= {row_end, s1_eol, s2_eol, s3_eol, s4_eol};
  end

  // ------------------------------------------------------------ S1..S5
  // INT32 output runs through the requantizer with unit gain.
  logic signed [15:0] q;
  logic [4:0]         sh;
  logic [47:0]        rnd;
  assign q   = out_int8 ? qmult : 16'sd1;
  assign sh  = out_int8 ? shift : 5'd0;
  assign rnd = (sh == 5'd0) ? 48'd0 : (48'd1 << (sh - 5'd1));

  logic [31:0]        s1_acc, s2_v32;
  logic signed [32:0] s3_lo;   // unsigned v[15:0] * q
  logic signed [31:0] s3_hi;   // signed   v[31:16] * q
  logic signed [47:0] s4_p, s5_r;

  // v * q = (v[31:16] * q) * 2^16 + v[15:0] * q, exact in 48 bits:
  // |p| <= 2^46 and |round| <= 2^30.
  always_ff @(posedge clk) begin
    s1_acc <= acc_tile[im_q][in_q];
    s2_v32 <= s1_acc + (bias_en ? bias_rdata : 32'd0);
    s3_lo  <= $signed({1'b0, s2_v32[15:0]}) * q;
    s3_hi  <= $signed(s2_v32[31:16]) * q;
    s4_p   <= {s3_hi, 16'h0000} + {{15{s3_lo[32]}}, s3_lo} + rnd;
    s5_r   <= s4_p >>> sh;
  end

  // ---------------------------------------------------------------- S6
  logic       neg, relu_zero, sat_hi, sat_lo;
  logic [7:0] out8;
  assign neg       = s5_r[47];
  assign relu_zero = relu && neg;
  assign sat_hi    = !neg && (|s5_r[46:7]);    // r > 127
  assign sat_lo    = neg && !(&s5_r[46:7]);    // r < -128
  assign out8      = relu_zero ? 8'h00 :
                     sat_hi    ? 8'h7f :
                     sat_lo    ? 8'h80 : s5_r[7:0];

  // INT8 packing: bytes of the current word that are still to be written.
  logic [1:0]  lane;
  logic [31:0] col_off, pack_data, merged_data;
  logic [3:0]  pack_be, merged_be;
  logic [31:0] row_addr_q;
  assign lane        = 2'(s5_n);
  assign col_off     = 32'(s5_n);
  assign merged_data = pack_data | (32'(out8) << (8 * lane));
  assign merged_be   = pack_be | (4'b0001 << lane);

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      wr_req    <= 1'b0;
      pack_data <= '0;
      pack_be   <= '0;
    end else begin
      wr_req <= 1'b0;
      if (s5_v) begin
        if (!out_int8) begin
          wr_req  <= 1'b1;
          wr_addr <= row_addr_q + (col_off << 2);
          wr_be   <= 4'b1111;
          wr_data <= relu_zero ? 32'd0 : s5_r[31:0];
        end else if (lane == 2'd3 || s5_eol) begin
          wr_req    <= 1'b1;
          wr_addr   <= row_addr_q + (col_off & ~32'd3);
          wr_be     <= merged_be;
          wr_data   <= merged_data;
          pack_data <= '0;
          pack_be   <= '0;
        end else begin
          pack_data <= merged_data;
          pack_be   <= merged_be;
        end
      end
    end
  end

  always_ff @(posedge clk) begin
    if (start)               row_addr_q <= c_addr;
    else if (s5_v && s5_eol) row_addr_q <= row_addr_q + c_stride;
  end

  // ------------------------------------------------------------- done
  logic in_flight;
  assign in_flight = issue_q || s1_v || s2_v || s3_v || s4_v || s5_v;
  assign done      = busy_q && !in_flight;

  always_ff @(posedge clk) begin
    if (!rst_n)     busy_q <= 1'b0;
    else if (start) busy_q <= 1'b1;
    else if (done)  busy_q <= 1'b0;
  end

endmodule
