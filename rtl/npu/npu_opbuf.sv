// Operand buffer and skewing feeder for one edge of the systolic array.
//
// Storage. A row is kept as 32-bit little-endian words; word w holds
// elements 4w..4w+3. Rows are grouped by four (group g = rows 4g..4g+3) and
// each group has LANES RAMs: lane RAM l holds the words w with
// w mod LANES == l of the group's rows, at address {row mod 4, w / LANES}.
// A DMA block carries LANES consecutive words of one row, which fall in
// different lane RAMs wherever the row starts within the block, so a whole
// block is written in one cycle.
//
// Feeding. Row i presents element k = t - 2 - i at compute step t: the
// systolic skew. It consumes one word every four steps, and the four rows of
// a group need their next word in four different steps. So at step t group
// g reads word floor(t/4) - g of row 4g + (t mod 4), and that row keeps the
// word in a register while its four bytes go out: one read port per lane RAM
// serves four rows. At KMAX = 1024 a lane RAM holds 4 x 256 / LANES words,
// two ECP5 block RAMs for LANES = 1 and one otherwise.
//
// Pipeline for row i, element k (word w = k / 4):
//   step 4w + i       group read address {i mod 4, w / LANES}
//   step 4w + i + 1   lane RAM data -> word register of row i
//   step k + i + 2    byte k mod 4 selected -> feed register
//   step k + i + 3    feed[i] = X[i][k], or 0 when k is outside 0..K-1 or
//                     i >= rows
// Zero-gating the edges means idle PEs multiply by zero, so the array needs
// no valid bits and rows/columns beyond M/N keep their accumulators.
module npu_opbuf #(
  parameter  int ARR    = 8,
  parameter  int KMAX   = 1024,
  parameter  int LANES  = 1,   // 32-bit words per DMA block: 1, 2 or 4
  localparam int RW     = (ARR > 1) ? $clog2(ARR) : 1,
  localparam int CW     = $clog2(ARR + 1),
  localparam int KW     = $clog2(KMAX + 1),
  localparam int LSH    = $clog2(LANES),
  localparam int LW     = (LANES > 1) ? LSH : 1,
  localparam int WDEPTH = (KMAX + 3) / 4,
  localparam int LDEPTH = (WDEPTH + LANES - 1) / LANES,
  // The lane index is shared with the bias vector, hence BDEPTH (same as npu_dma).
  localparam int BDEPTH = (ARR + LANES - 1) / LANES,
  localparam int IW0    = (LDEPTH > BDEPTH) ? LDEPTH : BDEPTH,
  localparam int IW     = (IW0 > 1) ? $clog2(IW0) : 1,
  localparam int TW0    = $clog2(KMAX + 2 * ARR + 2),
  localparam int TW     = (TW0 > IW + LSH + 2) ? TW0 : IW + LSH + 2
) (
  input  logic                 clk,
  input  logic                 rst_n,
  // fill port: one DMA block, already split into lanes by npu_dma
  input  logic                 fill_en,
  input  logic [RW-1:0]        fill_row,
  input  logic [LANES-1:0]     fill_lane_en,
  input  logic [LANES*IW-1:0]  fill_lane_idx,   // word / LANES within the row
  input  logic [LANES*32-1:0]  fill_lane_data,
  // feed side
  input  logic                 run,      // compute phase in progress
  input  logic [TW-1:0]        step,     // compute step t
  input  logic [KW-1:0]        k_len,    // K
  input  logic [CW-1:0]        rows,     // rows taking part (M or N)
  output logic [ARR*8-1:0]     feed      // row i operand at [8*i +: 8]
);

  localparam int NGROUP = (ARR + 3) / 4;

  logic [1:0]     fill_slot;    // row within its group
  logic [RW-1:0]  fill_group;
  assign fill_slot  = 2'(fill_row);
  assign fill_group = fill_row >> 2;

  for (genvar g = 0; g < NGROUP; g++) begin : g_group
    localparam int NROWS = (ARR - 4 * g < 4) ? ARR - 4 * g : 4;

    // Word fetched this step, for row 4g + step[1:0].
    logic [TW-1:0]           fetch_word;
    logic [IW-1:0]           fetch_idx;
    logic [LW-1:0]           fetch_lane_q;   // lane of the word read last step
    logic [1:0]              fetch_slot_q;
    logic [LANES*32-1:0]     lane_rdata;
    logic [31:0]             fetched;

    assign fetch_word = (step >> 2) - TW'(g);
    assign fetch_idx  = IW'(fetch_word >> LSH);

    for (genvar l = 0; l < LANES; l++) begin : g_lane
      npu_sdp_ram #(.WIDTH(32), .DEPTH(4 << IW)) u_ram (
        .clk,
        .we    (fill_en && fill_group == RW'(g) && fill_lane_en[l]),
        .waddr ({fill_slot, fill_lane_idx[IW*l +: IW]}),
        .wdata (fill_lane_data[32*l +: 32]),
        .raddr ({step[1:0], fetch_idx}),
        .rdata (lane_rdata[32*l +: 32])
      );
    end

    always_ff @(posedge clk) begin
      fetch_slot_q <= step[1:0];
      fetch_lane_q <= LW'(fetch_word & TW'(LANES - 1));
    end
    assign fetched = lane_rdata[32*fetch_lane_q +: 32];

    for (genvar r = 0; r < NROWS; r++) begin : g_row
      localparam int I = 4 * g + r;

      logic [31:0]   word_q;
      logic [TW:0]   k_ext;   // step - 2 - i; the top bit is the borrow
      logic [TW-1:0] k;
      logic          vld;
      logic [7:0]    feed_q;

      assign k_ext = {1'b0, step} - (TW+1)'(I + 2);
      assign k     = k_ext[TW-1:0];
      assign vld   = run && !k_ext[TW] && (k < TW'(k_len)) && (CW'(I) < rows);

      always_ff @(posedge clk) begin
        if (fetch_slot_q == 2'(r)) word_q <= fetched;
      end

      always_ff @(posedge clk) begin
        if (!rst_n) feed_q <= '0;
        else        feed_q <= vld ? word_q[8*k[1:0] +: 8] : 8'd0;
      end

      assign feed[8*I +: 8] = feed_q;
    end
  end

endmodule
