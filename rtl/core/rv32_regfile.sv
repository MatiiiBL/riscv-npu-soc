// 32 x 32 register file: two asynchronous read ports (ID), one write port (WB).
//
// The storage has no reset, so it maps to distributed (LUT) RAM on the FPGA;
// x0 is never written and reads of it are forced to zero instead. A write and
// a read of the same register in one cycle return the new value, so WB needs
// no separate forwarding path into ID.
module rv32_regfile (
  input  logic        clk,
  input  logic [4:0]  raddr1,
  input  logic [4:0]  raddr2,
  output logic [31:0] rdata1,
  output logic [31:0] rdata2,
  input  logic        we,
  input  logic [4:0]  waddr,
  input  logic [31:0] wdata
);

  logic [31:0] regs [32];

  always_ff @(posedge clk) begin
    if (we && waddr != 5'd0) begin
      regs[waddr] <= wdata;
    end
  end

  always_comb begin
    if (raddr1 == 5'd0)                 rdata1 = '0;
    else if (we && waddr == raddr1)     rdata1 = wdata;
    else                                rdata1 = regs[raddr1];

    if (raddr2 == 5'd0)                 rdata2 = '0;
    else if (we && waddr == raddr2)     rdata2 = wdata;
    else                                rdata2 = regs[raddr2];
  end

endmodule
