// Integer ALU for RV32I register-register and register-immediate operations,
// address generation and the LUI/AUIPC adds. Branch comparisons are done by a
// separate comparator in the core so they do not wait on the ALU result mux.
module rv32_alu (
  input  rv32_pkg::alu_op_e op,
  input  logic [31:0]       a,
  input  logic [31:0]       b,
  output logic [31:0]       y
);

  always_comb begin
    case (op)
      rv32_pkg::ALU_SUB:  y = a - b;
      rv32_pkg::ALU_SLL:  y = a << b[4:0];
      rv32_pkg::ALU_SLT:  y = {31'b0, $signed(a) < $signed(b)};
      rv32_pkg::ALU_SLTU: y = {31'b0, a < b};
      rv32_pkg::ALU_XOR:  y = a ^ b;
      rv32_pkg::ALU_SRL:  y = a >> b[4:0];
      rv32_pkg::ALU_SRA:  y = $unsigned($signed(a) >>> b[4:0]);
      rv32_pkg::ALU_OR:   y = a | b;
      rv32_pkg::ALU_AND:  y = a & b;
      default:            y = a + b;
    endcase
  end

endmodule
