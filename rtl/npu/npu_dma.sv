// Operand loader: streams the inputs of one operation from DMEM into the
// local buffers, one DMA_BYTES-wide aligned block per cycle (the port never
// stalls).
//
// The transfer is up to three segments walked in order, each a set of rows:
//   A     M rows of K bytes, from A_ADDR, row pitch A_STRIDE   (if load_a)
//   B     N rows of K bytes, from B_ADDR, row pitch B_STRIDE   (if load_b)
//   bias  one row of 4N bytes, from BIAS_ADDR                  (if load_bias)
// Rows are only 4-byte aligned, so a row is fetched as the aligned blocks
// covering it: it starts at 32-bit word `off` of its first block and the
// bytes around it are ignored.
//
// Read data comes back one cycle after its request and is split into LANES
// 32-bit words for the buffers (see npu_opbuf). Row word w goes to lane
// w mod LANES at index w / LANES; for block j of a row that starts at word
// `off`, lane l therefore takes bus word (l + off) mod LANES, at index j, or
// j - 1 when l + off wraps past the end of the block. Lanes outside the row
// are not written. `done` pulses in the cycle the last block is written.
module npu_dma #(
  parameter  int ARR       = 8,
  parameter  int KMAX      = 1024,
  parameter  int DMA_BYTES = 4,
  localparam int LANES     = DMA_BYTES / 4,
  localparam int LSH       = $clog2(LANES),
  localparam int LW        = (LANES > 1) ? LSH : 1,
  localparam int RW        = (ARR > 1) ? $clog2(ARR) : 1,
  localparam int CW        = $clog2(ARR + 1),
  localparam int KW        = $clog2(KMAX + 1),
  localparam int WDEPTH    = (KMAX + 3) / 4,
  localparam int LDEPTH    = (WDEPTH + LANES - 1) / LANES,
  localparam int BDEPTH    = (ARR + LANES - 1) / LANES,
  localparam int IW0       = (LDEPTH > BDEPTH) ? LDEPTH : BDEPTH,
  localparam int IW        = (IW0 > 1) ? $clog2(IW0) : 1,
  localparam int LEN       = (KMAX > 4 * ARR) ? KMAX : 4 * ARR,   // longest row, bytes
  localparam int LNW       = $clog2(LEN + DMA_BYTES + 1),         // byte counts
  localparam int BKW       = $clog2(LEN / 4 + 2),                 // block index in a row
  localparam int XW        = BKW + LSH + 1                        // word index in a row
) (
  input  logic                   clk,
  input  logic                   rst_n,
  input  logic                   start,
  // operation parameters, stable while the transfer runs
  input  logic [31:0]            a_addr,
  input  logic [31:0]            b_addr,
  input  logic [31:0]            bias_addr,
  input  logic [31:0]            a_stride,
  input  logic [31:0]            b_stride,
  input  logic [CW-1:0]          m_rows,
  input  logic [CW-1:0]          n_rows,
  input  logic [KW-1:0]          k_len,
  // segments to transfer, sampled with start
  input  logic                   load_a,
  input  logic                   load_b,
  input  logic                   load_bias,
  // read request (registered); the address is DMA_BYTES aligned
  output logic                   rd_req,
  output logic [31:0]            rd_addr,
  input  logic [8*DMA_BYTES-1:0] rd_data,
  // buffer fill, valid in the cycle the read data arrives
  output logic                   fill_a,
  output logic                   fill_b,
  output logic                   fill_bias,
  output logic [RW-1:0]          fill_row,
  output logic [LANES-1:0]       fill_lane_en,
  output logic [LANES*IW-1:0]    fill_lane_idx,
  output logic [LANES*32-1:0]    fill_lane_data,
  output logic                   done
);

  typedef enum logic [1:0] {SEG_A, SEG_B, SEG_BIAS} seg_t;

  localparam logic [31:0] BLK_MASK = 32'(DMA_BYTES - 1);

  // Current request: block blk_q of row row_q of segment seg_q, at rd_addr.
  seg_t           seg_q;
  logic [RW-1:0]  row_q;
  logic [BKW-1:0] blk_q;
  logic [31:0]    row_base_q;  // byte address of the row's first element
  logic [LW-1:0]  off_q;       // 32-bit word of the first block where the row starts
  logic [LNW-1:0] rem_q;       // bytes from the start of this block to the row end
  logic           en_b_q, en_bias_q;

  // Tag of the request issued in the previous cycle, aligned with its data.
  logic           rsp_v_q, rsp_last_q;
  seg_t           rsp_seg_q;
  logic [RW-1:0]  rsp_row_q;
  logic [BKW-1:0] rsp_blk_q;
  logic [LW-1:0]  rsp_off_q;

  // Shape of the current segment.
  logic [RW-1:0] seg_row_last;
  logic [31:0]   seg_stride;
  always_comb begin
    unique case (seg_q)
      SEG_A: begin
        seg_row_last = RW'(m_rows - CW'(1));
        seg_stride   = a_stride;
      end
      SEG_B: begin
        seg_row_last = RW'(n_rows - CW'(1));
        seg_stride   = b_stride;
      end
      default: begin
        seg_row_last = '0;
        seg_stride   = '0;
      end
    endcase
  end

  logic row_end, seg_end, xfer_end;
  assign row_end  = (rem_q <= LNW'(DMA_BYTES));
  assign seg_end  = row_end && (row_q == seg_row_last);
  assign xfer_end = seg_end && ((seg_q == SEG_BIAS) ||
                                (seg_q == SEG_B && !en_bias_q) ||
                                (seg_q == SEG_A && !en_b_q && !en_bias_q));

  // The row the next request starts, when it starts one: the first row of
  // the transfer, the next row of this segment, or the first of the next.
  seg_t           nxt_seg;
  logic [31:0]    nxt_base;
  logic [LNW-1:0] nxt_len;
  always_comb begin
    if (start)        nxt_seg = load_a ? SEG_A : (load_b ? SEG_B : SEG_BIAS);
    else if (seg_end) nxt_seg = (seg_q == SEG_A && en_b_q) ? SEG_B : SEG_BIAS;
    else              nxt_seg = seg_q;

    if (!start && !seg_end) begin
      nxt_base = row_base_q + seg_stride;
    end else begin
      unique case (nxt_seg)
        SEG_A:   nxt_base = a_addr;
        SEG_B:   nxt_base = b_addr;
        default: nxt_base = bias_addr;
      endcase
    end
    nxt_len = (nxt_seg == SEG_BIAS) ? (LNW'(n_rows) << 2) : LNW'(k_len);
  end

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      rd_req <= 1'b0;
    end else if (start) begin
      rd_req <= load_a || load_b || load_bias;
    end else if (rd_req && xfer_end) begin
      rd_req <= 1'b0;
    end
  end

  always_ff @(posedge clk) begin
    if (start || (rd_req && row_end)) begin
      if (start) begin
        en_b_q    <= load_b;
        en_bias_q <= load_bias;
      end
      seg_q      <= nxt_seg;
      row_q      <= (start || seg_end) ? '0 : row_q + RW'(1);
      blk_q      <= '0;
      row_base_q <= nxt_base;
      rd_addr    <= nxt_base & ~BLK_MASK;
      off_q      <= LW'((nxt_base & BLK_MASK) >> 2);
      rem_q      <= LNW'(nxt_base & BLK_MASK) + nxt_len;
    end else if (rd_req) begin
      blk_q   <= blk_q + BKW'(1);
      rd_addr <= rd_addr + 32'(DMA_BYTES);
      rem_q   <= rem_q - LNW'(DMA_BYTES);
    end
  end

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      rsp_v_q    <= 1'b0;
      rsp_last_q <= 1'b0;
    end else begin
      rsp_v_q    <= rd_req;
      // An empty transfer (every operand kept, no bias) is done at once.
      rsp_last_q <= (rd_req && xfer_end) || (start && !(load_a || load_b || load_bias));
    end
  end

  always_ff @(posedge clk) begin
    rsp_seg_q <= seg_q;
    rsp_row_q <= row_q;
    rsp_blk_q <= blk_q;
    rsp_off_q <= off_q;
  end

  // ------------------------------------------------------------ fill side
  // Words in the row being filled; ceil(K / 4) is computed at a width that
  // holds both K + 3 and the word index (either can be the wider one).
  localparam int KWW = (XW > KW + 2) ? XW : KW + 2;
  logic [XW-1:0] rsp_words;
  assign rsp_words = (rsp_seg_q == SEG_BIAS) ? XW'(n_rows)
                                             : XW'((KWW'(k_len) + KWW'(3)) >> 2);

  for (genvar l = 0; l < LANES; l++) begin : g_lane
    logic [LW:0]    pos;    // bus word holding this lane's word, before the wrap
    logic           wrap;
    logic [LW-1:0]  src;
    logic [BKW-1:0] idx;
    logic [XW-1:0]  word;   // word index within the row

    assign pos  = (LW+1)'(l) + (LW+1)'(rsp_off_q);
    assign wrap = (pos >= (LW+1)'(LANES));
    assign src  = LW'(pos & (LW+1)'(LANES - 1));
    assign idx  = rsp_blk_q - BKW'(wrap);
    assign word = (XW'(idx) << LSH) | XW'(l);

    assign fill_lane_en[l]            = rsp_v_q && !(rsp_blk_q == '0 && wrap) && (word < rsp_words);
    assign fill_lane_idx[IW*l +: IW]  = IW'(idx);
    assign fill_lane_data[32*l +: 32] = rd_data[32*src +: 32];
  end

  assign fill_a    = rsp_v_q && (rsp_seg_q == SEG_A);
  assign fill_b    = rsp_v_q && (rsp_seg_q == SEG_B);
  assign fill_bias = rsp_v_q && (rsp_seg_q == SEG_BIAS);
  assign fill_row  = rsp_row_q;
  assign done      = rsp_last_q;

endmodule
