// Operand loader: streams the inputs of one operation from DMEM into the
// local buffers, one 32-bit read request per cycle (the port never stalls).
//
// The transfer is three segments walked in order, each a set of rows of
// whole words:
//   A     M rows of ceil(K/4) words, from A_ADDR, row pitch A_STRIDE
//   B     N rows of ceil(K/4) words, from B_ADDR, row pitch B_STRIDE
//   bias  N rows of one word, from BIAS_ADDR, pitch 4 (only if load_bias)
// Every request carries a tag (segment, row, word) that is delayed by one
// cycle to meet its read data, which is then written into the selected
// buffer bank. `done` pulses in the cycle the last word is written.
module npu_dma #(
  parameter  int ARR    = 8,
  parameter  int KMAX   = 1024,
  localparam int RW     = (ARR > 1) ? $clog2(ARR) : 1,
  localparam int CW     = $clog2(ARR + 1),
  localparam int KW     = $clog2(KMAX + 1),
  localparam int WDEPTH = (KMAX + 3) / 4,
  localparam int WAW    = (WDEPTH > 1) ? $clog2(WDEPTH) : 1
) (
  input  logic           clk,
  input  logic           rst_n,
  input  logic           start,
  // operation parameters, stable while the transfer runs
  input  logic [31:0]    a_addr,
  input  logic [31:0]    b_addr,
  input  logic [31:0]    bias_addr,
  input  logic [31:0]    a_stride,
  input  logic [31:0]    b_stride,
  input  logic [CW-1:0]  m_rows,
  input  logic [CW-1:0]  n_rows,
  input  logic [KW-1:0]  k_len,
  input  logic           load_bias,
  // read request (registered)
  output logic           rd_req,
  output logic [31:0]    rd_addr,
  // buffer fill, valid in the cycle the read data arrives
  output logic           fill_a,
  output logic           fill_b,
  output logic           fill_bias,
  output logic [RW-1:0]  fill_row,
  output logic [WAW-1:0] fill_word,
  output logic           done
);

  typedef enum logic [1:0] {SEG_A, SEG_B, SEG_BIAS} seg_t;

  // Current request.
  seg_t           seg_q;
  logic [RW-1:0]  row_q;
  logic [WAW-1:0] word_q;
  logic [31:0]    row_base_q;  // address of word 0 of the current row

  // Tag of the request issued in the previous cycle, aligned with its data.
  logic           rsp_v_q, rsp_last_q;
  seg_t           rsp_seg_q;
  logic [RW-1:0]  rsp_row_q;
  logic [WAW-1:0] rsp_word_q;

  // Last word index of an operand row: ceil(K/4) - 1. K <= KMAX was checked
  // before start, so it fits the buffer address width.
  logic [KW-1:0]  k_m1;
  logic [WAW-1:0] op_word_last;
  assign k_m1         = k_len - KW'(1);
  assign op_word_last = WAW'(k_m1 >> 2);

  // Shape of the current segment.
  logic [RW-1:0]  seg_row_last;
  logic [WAW-1:0] seg_word_last;
  logic [31:0]    seg_stride;
  always_comb begin
    unique case (seg_q)
      SEG_A: begin
        seg_row_last  = RW'(m_rows - CW'(1));
        seg_word_last = op_word_last;
        seg_stride    = a_stride;
      end
      SEG_B: begin
        seg_row_last  = RW'(n_rows - CW'(1));
        seg_word_last = op_word_last;
        seg_stride    = b_stride;
      end
      default: begin
        seg_row_last  = RW'(n_rows - CW'(1));
        seg_word_last = '0;
        seg_stride    = 32'd4;
      end
    endcase
  end

  logic row_end, seg_end, xfer_end;
  assign row_end  = (word_q == seg_word_last);
  assign seg_end  = row_end && (row_q == seg_row_last);
  assign xfer_end = seg_end && ((seg_q == SEG_BIAS) || (seg_q == SEG_B && !load_bias));

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      rd_req     <= 1'b0;
      rd_addr    <= '0;
      seg_q      <= SEG_A;
      row_q      <= '0;
      word_q     <= '0;
      row_base_q <= '0;
    end else if (start) begin
      rd_req     <= 1'b1;
      rd_addr    <= a_addr;
      seg_q      <= SEG_A;
      row_q      <= '0;
      word_q     <= '0;
      row_base_q <= a_addr;
    end else if (rd_req) begin
      if (xfer_end) begin
        rd_req <= 1'b0;
      end else if (!row_end) begin
        word_q  <= word_q + WAW'(1);
        rd_addr <= rd_addr + 32'd4;
      end else if (!seg_end) begin
        word_q     <= '0;
        row_q      <= row_q + RW'(1);
        row_base_q <= row_base_q + seg_stride;
        rd_addr    <= row_base_q + seg_stride;
      end else begin
        word_q     <= '0;
        row_q      <= '0;
        seg_q      <= (seg_q == SEG_A) ? SEG_B : SEG_BIAS;
        row_base_q <= (seg_q == SEG_A) ? b_addr : bias_addr;
        rd_addr    <= (seg_q == SEG_A) ? b_addr : bias_addr;
      end
    end
  end

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      rsp_v_q    <= 1'b0;
      rsp_last_q <= 1'b0;
      rsp_seg_q  <= SEG_A;
      rsp_row_q  <= '0;
      rsp_word_q <= '0;
    end else begin
      rsp_v_q    <= rd_req;
      rsp_last_q <= rd_req && xfer_end;
      rsp_seg_q  <= seg_q;
      rsp_row_q  <= row_q;
      rsp_word_q <= word_q;
    end
  end

  assign fill_a    = rsp_v_q && (rsp_seg_q == SEG_A);
  assign fill_b    = rsp_v_q && (rsp_seg_q == SEG_B);
  assign fill_bias = rsp_v_q && (rsp_seg_q == SEG_BIAS);
  assign fill_row  = rsp_row_q;
  assign fill_word = rsp_word_q;
  assign done      = rsp_last_q;

endmodule
