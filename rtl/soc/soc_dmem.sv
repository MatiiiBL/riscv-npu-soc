// Data memory (docs/spec.md 3 and 4.5): WORDS 32-bit words split into BANKS
// word-interleaved banks. Word w lives in bank (w mod BANKS) at row
// (w / BANKS), and every bank is a true dual-port block RAM with byte
// enables (bram_tdp).
//
//  * Port A (CPU) is one word wide. Only the bank holding the word is
//    enabled; the banks answer a cycle later, so the bank number is
//    registered with the request and selects the read data in that cycle.
//  * Port B (NPU DMA) is BANKS words wide. An aligned block of BANKS words
//    is the same row of every bank, so all banks are accessed in parallel
//    and word i of the block is bank i (bits 32i +: 32, little-endian).
//
// The banks together hold WORDS words for any BANKS, so the amount of
// block RAM does not depend on the DMA width.
//
// INIT_FILE is a path prefix: bank b loads <INIT_FILE><b>.hex, one word per
// line in row order, as written by tools/elf2hex.py --dmem-banks BANKS.
module soc_dmem #(
  parameter int WORDS     = 65536,
  parameter int BANKS     = 4,       // power of two, 1 to 8
  parameter     INIT_FILE = ""
) (
  input  logic                           clk,

  input  logic                           a_en,
  input  logic                           a_we,
  input  logic [3:0]                     a_be,
  input  logic [$clog2(WORDS)-1:0]       a_addr,    // word address
  input  logic [31:0]                    a_wdata,
  output logic [31:0]                    a_rdata,

  input  logic                           b_en,
  input  logic                           b_we,
  input  logic [4*BANKS-1:0]             b_be,      // lane i = byte i of the block
  input  logic [$clog2(WORDS/BANKS)-1:0] b_addr,    // block (row) address
  input  logic [32*BANKS-1:0]            b_wdata,
  output logic [32*BANKS-1:0]            b_rdata
);

  localparam int AW = $clog2(WORDS);
  localparam int BW = $clog2(BANKS);
  localparam int SW = (BW > 0) ? BW : 1;    // width of the bank-number signals

  if (BANKS < 1 || BANKS > 8 || (BANKS & (BANKS - 1)) != 0 || WORDS % BANKS != 0) begin : g_bad
    $error("soc_dmem: BANKS must be a power of two from 1 to 8 that divides WORDS");
  end

  logic [SW-1:0] a_bank, a_bank_q;
  logic [31:0]   bank_a_rdata [BANKS];

  if (BW > 0) begin : g_bank_sel
    assign a_bank = a_addr[BW-1:0];
  end else begin : g_one_bank
    assign a_bank = '0;
  end

  always_ff @(posedge clk) begin
    if (a_en) begin
      a_bank_q <= a_bank;
    end
  end

  for (genvar b = 0; b < BANKS; b++) begin : g_bank
    localparam logic [7:0] DIGIT = 8'h30 + 8'(b);

    bram_tdp #(
      .WORDS     (WORDS / BANKS),
      .INIT_FILE (INIT_FILE == "" ? "" : {INIT_FILE, DIGIT, ".hex"})
    ) u_ram (
      .clk     (clk),
      .a_en    (a_en && a_bank == SW'(b)),
      .a_we    (a_we),
      .a_be    (a_be),
      .a_addr  (a_addr[AW-1:BW]),
      .a_wdata (a_wdata),
      .a_rdata (bank_a_rdata[b]),
      .b_en    (b_en),
      .b_we    (b_we),
      .b_be    (b_be[4*b +: 4]),
      .b_addr  (b_addr),
      .b_wdata (b_wdata[32*b +: 32]),
      .b_rdata (b_rdata[32*b +: 32])
    );
  end

  always_comb begin
    a_rdata = '0;
    for (int b = 0; b < BANKS; b++) begin
      if (a_bank_q == SW'(b)) a_rdata = bank_a_rdata[b];
    end
  end

endmodule
