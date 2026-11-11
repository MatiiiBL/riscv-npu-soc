// Simple dual-port RAM: one write port, one read port with a registered
// output (one cycle of read latency). Written in the template that Yosys and
// vendor tools infer as block RAM, or as LUT RAM when it is small.
//
// No reset and no read-during-write guarantee: callers never read an address
// in the same cycle it is written. no_rw_check tells Yosys so; without it,
// Yosys adds a bypass register and comparator to emulate read-first
// behavior on every block RAM.
module npu_sdp_ram #(
  parameter  int WIDTH = 32,
  parameter  int DEPTH = 256,
  localparam int AW    = (DEPTH > 1) ? $clog2(DEPTH) : 1
) (
  input  logic             clk,
  input  logic             we,
  input  logic [AW-1:0]    waddr,
  input  logic [WIDTH-1:0] wdata,
  input  logic [AW-1:0]    raddr,
  output logic [WIDTH-1:0] rdata
);

  (* no_rw_check *) logic [WIDTH-1:0] mem [0:DEPTH-1];

  always_ff @(posedge clk) begin
    if (we) mem[waddr] <= wdata;
    rdata <= mem[raddr];
  end

endmodule
