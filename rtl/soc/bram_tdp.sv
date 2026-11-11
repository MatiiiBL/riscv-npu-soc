// True dual-port synchronous RAM with byte enables (data memory), 32-bit words.
//
// Each port reads and writes independently with one cycle of read latency;
// a port's read data is undefined in a cycle it writes. Both ports may name
// the same word in one cycle only if neither writes it: the hardware gives no
// collision guarantee, and the software contract (docs/spec.md 5.1) never
// relies on one. `no_rw_check` tells Yosys exactly that, so it can map the
// array onto true dual-port block RAM instead of emulating Verilog's
// read-old-data semantics across ports with logic.
module bram_tdp #(
  parameter int WORDS     = 65536,
  parameter     INIT_FILE = ""
) (
  input  logic                     clk,

  input  logic                     a_en,
  input  logic                     a_we,
  input  logic [3:0]               a_be,
  input  logic [$clog2(WORDS)-1:0] a_addr,
  input  logic [31:0]              a_wdata,
  output logic [31:0]              a_rdata,

  input  logic                     b_en,
  input  logic                     b_we,
  input  logic [3:0]               b_be,
  input  logic [$clog2(WORDS)-1:0] b_addr,
  input  logic [31:0]              b_wdata,
  output logic [31:0]              b_rdata
);

  (* no_rw_check *)
  logic [31:0] mem [WORDS];

  if (INIT_FILE != "") begin : g_init
    initial $readmemh(INIT_FILE, mem);
  end

  always_ff @(posedge clk) begin
    if (a_en) begin
      for (int i = 0; i < 4; i++) begin
        if (a_we && a_be[i]) mem[a_addr][8*i +: 8] <= a_wdata[8*i +: 8];
      end
      a_rdata <= mem[a_addr];
    end
  end

  always_ff @(posedge clk) begin
    if (b_en) begin
      for (int i = 0; i < 4; i++) begin
        if (b_we && b_be[i]) mem[b_addr][8*i +: 8] <= b_wdata[8*i +: 8];
      end
      b_rdata <= mem[b_addr];
    end
  end

endmodule
