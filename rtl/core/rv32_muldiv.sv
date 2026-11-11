// M extension execution unit.
//
// MUL/MULH/MULHSU/MULHU: the operands are taken in EX and registered, and
// the 33 x 33 signed multiply runs in MEM from those registers, so mul_y
// belongs to the instruction now in MEM. It is written as 64 x 64 on
// sign-extended operands, which Verilator evaluates natively and Yosys
// narrows back to 4 ECP5 MULT18X18D blocks. Keeping the multiplier out of
// EX keeps it off the forwarding paths; the core interlocks a dependent
// instruction for one cycle instead (docs/core.md section 3).
//
// DIV/DIVU/REM/REMU: a radix-2 restoring divider on operand magnitudes, one
// quotient bit per cycle. The core holds the instruction in EX (div_req high)
// until div_done:
//
//   cycle 0       IDLE  capture |a|, |b| and the result signs
//   cycles 1..32  BUSY  one shift/subtract step each
//   cycle 33      DONE  sign-corrected result on div_y, EX advances
//
// so a divide occupies EX for 34 cycles (33 stall cycles). Divide by zero
// falls out of the algorithm (quotient all ones, remainder = dividend) except
// for the quotient sign, which is suppressed; INT_MIN / -1 produces INT_MIN
// with remainder 0 without special casing.
module rv32_muldiv (
  input  logic        clk,
  input  logic        rst_n,
  input  logic [1:0]  op,        // funct3[1:0]; funct3[2] (mul vs div) is decoded by the core
  input  logic [31:0] a,         // rs1, after forwarding
  input  logic [31:0] b,         // rs2, after forwarding
  output logic [31:0] mul_y,     // product of the operands of the last cycle (MEM)
  input  logic        div_req,   // a DIV/REM instruction is in EX
  output logic        div_done,  // div_y is valid; the instruction may leave EX
  output logic [31:0] div_y
);

  // ---------------------------------------------------------------- multiply
  logic        mul_a_signed, mul_b_signed;
  logic [32:0] mul_a_q, mul_b_q;   // operands as 33-bit two's complement
  logic        mul_hi_q;           // MULH/MULHSU/MULHU: take the high word
  logic [63:0] mul_a, mul_b, product;

  // op: 00 MUL, 01 MULH (s x s), 10 MULHSU (s x u), 11 MULHU (u x u).
  // MUL takes the low half, where operand signedness makes no difference.
  assign mul_a_signed = (op != 2'b11);
  assign mul_b_signed = !op[1];

  // Captured every cycle; only the cycle after a multiply leaves EX matters.
  always_ff @(posedge clk) begin
    mul_a_q  <= {mul_a_signed & a[31], a};
    mul_b_q  <= {mul_b_signed & b[31], b};
    mul_hi_q <= (op != 2'b00);
  end

  assign mul_a   = {{31{mul_a_q[32]}}, mul_a_q};
  assign mul_b   = {{31{mul_b_q[32]}}, mul_b_q};
  assign product = mul_a * mul_b;
  assign mul_y   = mul_hi_q ? product[63:32] : product[31:0];

  // ------------------------------------------------------------------ divide
  typedef enum logic [1:0] {
    DIV_IDLE,
    DIV_BUSY,
    DIV_DONE
  } div_state_e;

  div_state_e  state;
  logic [4:0]  count;
  logic [31:0] quo;        // dividend bits shift out the top, quotient bits in
  logic [31:0] rem;        // partial remainder
  logic [31:0] divisor;
  logic        neg_quo, neg_rem, want_rem;

  logic        div_signed, a_neg, b_neg;
  logic [32:0] shifted;
  logic [32:0] trial;

  // op: 00 DIV, 01 DIVU, 10 REM, 11 REMU.
  assign div_signed = !op[0];
  assign a_neg      = div_signed & a[31];
  assign b_neg      = div_signed & b[31];

  // rem < divisor holds between steps, so shifted < 2 * divisor and the
  // 33-bit difference has bit 32 set exactly when the subtraction borrows.
  // (With a zero divisor, shifted never reaches bit 32 within 32 steps.)
  assign shifted = {rem, quo[31]};
  assign trial   = shifted - {1'b0, divisor};

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      state <= DIV_IDLE;
    end else begin
      case (state)
        DIV_IDLE: if (div_req) state <= DIV_BUSY;
        DIV_BUSY: if (count == 5'd0) state <= DIV_DONE;
        default:  state <= DIV_IDLE;
      endcase
    end
  end

  always_ff @(posedge clk) begin
    if (state == DIV_IDLE) begin
      quo      <= a_neg ? -a : a;
      divisor  <= b_neg ? -b : b;
      rem      <= '0;
      count    <= 5'd31;
      neg_quo  <= (a_neg ^ b_neg) && (b != '0);
      neg_rem  <= a_neg;
      want_rem <= op[1];
    end else if (state == DIV_BUSY) begin
      count <= count - 5'd1;
      if (!trial[32]) begin
        rem <= trial[31:0];
        quo <= {quo[30:0], 1'b1};
      end else begin
        rem <= shifted[31:0];
        quo <= {quo[30:0], 1'b0};
      end
    end
  end

  assign div_done = (state == DIV_DONE);
  assign div_y    = want_rem ? (neg_rem ? -rem : rem)
                             : (neg_quo ? -quo : quo);

endmodule
