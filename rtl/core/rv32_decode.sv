// Instruction decoder (ID stage), RV32IM + Zicsr.
//
// Purely combinational. Every encoding outside the implemented ISA decodes as
// a halting instruction with no other effect: the core has no traps, so
// ECALL, EBREAK and illegal instructions simply stop the machine when they
// reach WB (docs/spec.md section 2).
module rv32_decode (
  input  logic [31:0]        insn,

  output logic [4:0]         rs1,
  output logic [4:0]         rs2,
  output logic [4:0]         rd,
  output logic               uses_rs1,  // operand is read: drives hazard detection
  output logic               uses_rs2,
  output logic               rd_we,     // writes a register other than x0
  output logic [31:0]        imm,       // CSR instructions carry the CSR number in imm[11:0]

  output rv32_pkg::alu_op_e  alu_op,
  output rv32_pkg::opa_sel_e opa_sel,
  output logic               opb_imm,   // ALU operand B is imm (else rs2)
  output rv32_pkg::res_sel_e res_sel,

  output logic               is_branch,
  output logic               is_jal,
  output logic               is_jalr,
  output logic               is_load,
  output logic               is_store,
  output logic               is_div,    // DIV/DIVU/REM/REMU: multi-cycle, stalls EX
  output logic               is_halt    // ECALL, EBREAK or illegal
);

  logic [6:0]  opcode;
  logic [2:0]  funct3;
  logic [6:0]  funct7;
  logic [31:0] imm_i, imm_s, imm_b, imm_u, imm_j;
  logic        legal;
  logic        writes_rd;

  assign opcode = insn[6:0];
  assign funct3 = insn[14:12];
  assign funct7 = insn[31:25];
  assign rd     = insn[11:7];
  assign rs1    = insn[19:15];
  assign rs2    = insn[24:20];

  assign imm_i = {{20{insn[31]}}, insn[31:20]};
  assign imm_s = {{20{insn[31]}}, insn[31:25], insn[11:7]};
  assign imm_b = {{19{insn[31]}}, insn[31], insn[7], insn[30:25], insn[11:8], 1'b0};
  assign imm_u = {insn[31:12], 12'b0};
  assign imm_j = {{11{insn[31]}}, insn[31], insn[19:12], insn[20], insn[30:21], 1'b0};

  always_comb begin
    legal      = 1'b0;
    writes_rd  = 1'b0;
    uses_rs1   = 1'b0;
    uses_rs2   = 1'b0;
    imm        = imm_i;
    alu_op     = rv32_pkg::ALU_ADD;
    opa_sel    = rv32_pkg::OPA_RS1;
    opb_imm    = 1'b1;
    res_sel    = rv32_pkg::RES_ALU;
    is_branch  = 1'b0;
    is_jal     = 1'b0;
    is_jalr    = 1'b0;
    is_load    = 1'b0;
    is_store   = 1'b0;
    is_div     = 1'b0;
    is_halt    = 1'b0;

    case (opcode)
      rv32_pkg::OP_LUI: begin
        legal     = 1'b1;
        writes_rd = 1'b1;
        imm       = imm_u;
        opa_sel   = rv32_pkg::OPA_ZERO;
      end

      rv32_pkg::OP_AUIPC: begin
        legal     = 1'b1;
        writes_rd = 1'b1;
        imm       = imm_u;
        opa_sel   = rv32_pkg::OPA_PC;
      end

      rv32_pkg::OP_JAL: begin
        legal     = 1'b1;
        writes_rd = 1'b1;
        imm       = imm_j;
        is_jal    = 1'b1;
        res_sel   = rv32_pkg::RES_LINK;
      end

      // The ALU computes rs1 + imm, the jump target; rd gets the link value.
      rv32_pkg::OP_JALR: begin
        legal     = (funct3 == 3'b000);
        writes_rd = 1'b1;
        uses_rs1  = 1'b1;
        is_jalr   = 1'b1;
        res_sel   = rv32_pkg::RES_LINK;
      end

      rv32_pkg::OP_BRANCH: begin
        legal     = (funct3 != 3'b010) && (funct3 != 3'b011);
        uses_rs1  = 1'b1;
        uses_rs2  = 1'b1;
        imm       = imm_b;
        is_branch = 1'b1;
      end

      rv32_pkg::OP_LOAD: begin
        legal     = (funct3 != 3'b011) && (funct3 != 3'b110) && (funct3 != 3'b111);
        writes_rd = 1'b1;
        uses_rs1  = 1'b1;
        is_load   = 1'b1;
      end

      rv32_pkg::OP_STORE: begin
        legal     = (funct3[2] == 1'b0) && (funct3[1:0] != 2'b11);
        uses_rs1  = 1'b1;
        uses_rs2  = 1'b1;
        imm       = imm_s;
        is_store  = 1'b1;
      end

      rv32_pkg::OP_OP_IMM: begin
        legal     = 1'b1;
        writes_rd = 1'b1;
        uses_rs1  = 1'b1;
        case (funct3)
          3'b000: alu_op = rv32_pkg::ALU_ADD;
          3'b010: alu_op = rv32_pkg::ALU_SLT;
          3'b011: alu_op = rv32_pkg::ALU_SLTU;
          3'b100: alu_op = rv32_pkg::ALU_XOR;
          3'b110: alu_op = rv32_pkg::ALU_OR;
          3'b111: alu_op = rv32_pkg::ALU_AND;
          3'b001: begin
            alu_op = rv32_pkg::ALU_SLL;
            legal  = (funct7 == 7'b0000000);
          end
          default: begin  // 3'b101
            alu_op = funct7[5] ? rv32_pkg::ALU_SRA : rv32_pkg::ALU_SRL;
            legal  = (funct7 == 7'b0000000) || (funct7 == 7'b0100000);
          end
        endcase
      end

      rv32_pkg::OP_OP: begin
        writes_rd = 1'b1;
        uses_rs1  = 1'b1;
        uses_rs2  = 1'b1;
        opb_imm   = 1'b0;
        if (funct7 == 7'b0000001) begin
          legal   = 1'b1;
          is_div  = funct3[2];
          res_sel = funct3[2] ? rv32_pkg::RES_DIV : rv32_pkg::RES_MUL;
        end else if (funct7 == 7'b0000000) begin
          legal = 1'b1;
          case (funct3)
            3'b000:  alu_op = rv32_pkg::ALU_ADD;
            3'b001:  alu_op = rv32_pkg::ALU_SLL;
            3'b010:  alu_op = rv32_pkg::ALU_SLT;
            3'b011:  alu_op = rv32_pkg::ALU_SLTU;
            3'b100:  alu_op = rv32_pkg::ALU_XOR;
            3'b101:  alu_op = rv32_pkg::ALU_SRL;
            3'b110:  alu_op = rv32_pkg::ALU_OR;
            default: alu_op = rv32_pkg::ALU_AND;
          endcase
        end else if (funct7 == 7'b0100000) begin
          legal  = (funct3 == 3'b000) || (funct3 == 3'b101);
          alu_op = (funct3 == 3'b000) ? rv32_pkg::ALU_SUB : rv32_pkg::ALU_SRA;
        end
      end

      // FENCE and FENCE.I are no-ops: there are no caches or buffers to order.
      rv32_pkg::OP_MISC_MEM: begin
        legal = (funct3 == 3'b000) || (funct3 == 3'b001);
      end

      rv32_pkg::OP_SYSTEM: begin
        if (funct3 == 3'b000) begin
          legal   = (insn == rv32_pkg::INSN_ECALL) || (insn == rv32_pkg::INSN_EBREAK);
          is_halt = 1'b1;
        end else if (funct3 != 3'b100) begin
          // Zicsr. Every CSR is read-only and writes are ignored, so rs1/uimm
          // is never needed and the instruction must not interlock on it.
          legal     = 1'b1;
          writes_rd = 1'b1;
          res_sel   = rv32_pkg::RES_CSR;
        end
      end

      default: ;
    endcase

    if (!legal) begin
      is_halt = 1'b1;
    end
    // A halting instruction has no architectural effect besides stopping.
    if (is_halt) begin
      writes_rd = 1'b0;
      uses_rs1  = 1'b0;
      uses_rs2  = 1'b0;
      is_branch = 1'b0;
      is_jal    = 1'b0;
      is_jalr   = 1'b0;
      is_load   = 1'b0;
      is_store  = 1'b0;
      is_div    = 1'b0;
    end
  end

  assign rd_we = writes_rd && (rd != 5'd0);

endmodule
