// Output-stationary processing element: an INT8 x INT8 multiply feeding an
// INT32 accumulator that stays in place for the whole operation.
//
// Both operands are re-registered and passed on (A to the right, B down), so
// every operand enters the array once and reaches each PE one cycle after its
// upstream neighbour. The product is registered before the accumulate so the
// multiplier and the 32-bit adder sit in separate pipeline stages (the
// multiplier maps to a DSP block with its output register).
module npu_pe (
  input  logic              clk,
  input  logic              rst_n,
  input  logic              clr,    // start a new sum instead of adding to acc
  input  logic signed [7:0] a_in,
  input  logic signed [7:0] b_in,
  output logic signed [7:0] a_out,
  output logic signed [7:0] b_out,
  output logic [31:0]       acc
);

  logic signed [15:0] prod_q;

  // Everything is reset: idle cycles stream zeros through the array and the
  // accumulator adds prod_q unconditionally, so prod_q must start at zero.
  always_ff @(posedge clk) begin
    if (!rst_n) begin
      a_out  <= '0;
      b_out  <= '0;
      prod_q <= '0;
      acc    <= '0;
    end else begin
      a_out  <= a_in;
      b_out  <= b_in;
      prod_q <= a_in * b_in;
      acc    <= (clr ? 32'd0 : acc) + {{16{prod_q[15]}}, prod_q};
    end
  end

endmodule
