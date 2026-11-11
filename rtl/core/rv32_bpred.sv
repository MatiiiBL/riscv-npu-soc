// Branch predictor: direct-mapped BTB with a 2-bit saturating counter per
// entry (BP_MODE = 1), or nothing at all (BP_MODE = 0, static not-taken).
//
// Lookup happens in IF on the PC being fetched, so a hit that predicts taken
// redirects the very next fetch and a correctly predicted taken branch costs
// no bubble. The lookup result (hit, counter, target) travels down the pipe
// with the instruction; EX compares it with the resolved outcome and writes
// the entry back:
//
//   conditional branch, hit      counter +/- 1 (saturating), target refreshed if taken
//   conditional branch, miss     allocate with counter = 2 (weakly taken) if taken,
//                                otherwise leave the BTB alone
//   JAL / JALR                   allocate or refresh with counter = 3
//
// Tags are the full upper PC bits, so an entry never matches a different
// instruction; two branches whose PCs share index bits evict each other.
// JALR is predicted to go where it went last time. Updating from the
// pipeline's copy of the counter, rather than re-reading the table, means two
// in-flight instances of one branch can both start from the same old value;
// that only delays training by a step.
module rv32_bpred #(
  parameter int BP_MODE     = 1,
  parameter int BTB_ENTRIES = 64   // power of two
) (
  input  logic        clk,
  input  logic        rst_n,

  // Lookup (IF)
  input  logic [31:0] if_pc,
  output logic        pred_hit,
  output logic [1:0]  pred_ctr,
  output logic [31:0] pred_target,

  // Update (EX), at most one resolved control-flow instruction per cycle
  input  logic        upd_valid,
  input  logic [31:0] upd_pc,
  input  logic        upd_cond,        // conditional branch (else JAL/JALR)
  input  logic        upd_taken,
  input  logic [31:0] upd_target,      // resolved target, meaningful when taken
  input  logic        upd_hit,         // lookup result carried from IF
  input  logic [1:0]  upd_ctr,
  input  logic [31:0] upd_old_target
);

  if (BP_MODE == 1) begin : g_btb
    localparam int IDX_W = $clog2(BTB_ENTRIES);
    localparam int TAG_W = 30 - IDX_W;

    // The arrays are read asynchronously from if_pc, so on the FPGA they go
    // in LUT RAM. A block RAM would need a registered read address, which
    // Yosys gets by moving pc_f into the RAM: the whole next-PC selection,
    // including the late stall and redirect, would then end at the RAM's
    // address pins instead of at pc_f.
    logic [BTB_ENTRIES-1:0]                        valid;
    (* ram_style = "distributed" *) logic [TAG_W-1:0] tag_mem [BTB_ENTRIES];
    (* ram_style = "distributed" *) logic [29:0]      tgt_mem [BTB_ENTRIES];
    logic [1:0]                                    ctr_mem [BTB_ENTRIES];

    logic [IDX_W-1:0] rd_idx, wr_idx;
    logic             wr_en;
    logic [29:0]      wr_tgt;
    logic [1:0]       wr_ctr;

    assign rd_idx      = if_pc[IDX_W+1:2];
    assign pred_hit    = valid[rd_idx] && (tag_mem[rd_idx] == if_pc[31:IDX_W+2]);
    assign pred_ctr    = ctr_mem[rd_idx];
    assign pred_target = {tgt_mem[rd_idx], 2'b00};

    assign wr_idx = upd_pc[IDX_W+1:2];

    always_comb begin
      wr_en  = upd_valid;
      wr_tgt = upd_target[31:2];
      wr_ctr = 2'b11;
      if (upd_cond) begin
        if (upd_hit) begin
          if (upd_taken) begin
            wr_ctr = (upd_ctr == 2'b11) ? 2'b11 : upd_ctr + 2'b01;
          end else begin
            wr_ctr = (upd_ctr == 2'b00) ? 2'b00 : upd_ctr - 2'b01;
            wr_tgt = upd_old_target[31:2];
          end
        end else begin
          wr_en  = upd_valid && upd_taken;
          wr_ctr = 2'b10;
        end
      end
    end

    always_ff @(posedge clk) begin
      if (!rst_n) begin
        valid <= '0;
      end else if (wr_en) begin
        valid[wr_idx] <= 1'b1;
      end
    end

    always_ff @(posedge clk) begin
      if (wr_en) begin
        tag_mem[wr_idx] <= upd_pc[31:IDX_W+2];
        tgt_mem[wr_idx] <= wr_tgt;
        ctr_mem[wr_idx] <= wr_ctr;
      end
    end

    logic unused;
    assign unused = ^{if_pc[1:0], upd_pc[1:0], upd_target[1:0], upd_old_target[1:0]};

  end else begin : g_static
    assign pred_hit    = 1'b0;
    assign pred_ctr    = 2'b00;
    assign pred_target = '0;

    logic unused;
    assign unused = ^{clk, rst_n, if_pc, upd_valid, upd_pc, upd_cond, upd_taken,
                      upd_target, upd_hit, upd_ctr, upd_old_target};
  end

endmodule
