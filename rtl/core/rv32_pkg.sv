// Encodings shared by the RV32IM core modules.
//
// Referenced with explicit `rv32_pkg::` scoping rather than `import`, because
// Yosys does not accept package imports inside a module.
package rv32_pkg;

  // Major opcodes, insn[6:0].
  localparam logic [6:0] OP_LOAD     = 7'b0000011;
  localparam logic [6:0] OP_MISC_MEM = 7'b0001111;
  localparam logic [6:0] OP_OP_IMM   = 7'b0010011;
  localparam logic [6:0] OP_AUIPC    = 7'b0010111;
  localparam logic [6:0] OP_STORE    = 7'b0100011;
  localparam logic [6:0] OP_OP       = 7'b0110011;
  localparam logic [6:0] OP_LUI      = 7'b0110111;
  localparam logic [6:0] OP_BRANCH   = 7'b1100011;
  localparam logic [6:0] OP_JALR     = 7'b1100111;
  localparam logic [6:0] OP_JAL      = 7'b1101111;
  localparam logic [6:0] OP_SYSTEM   = 7'b1110011;

  // The only two SYSTEM encodings with funct3 = 0 that the core accepts.
  localparam logic [31:0] INSN_ECALL  = 32'h0000_0073;
  localparam logic [31:0] INSN_EBREAK = 32'h0010_0073;

  // Branch funct3.
  localparam logic [2:0] F3_BEQ  = 3'b000;
  localparam logic [2:0] F3_BNE  = 3'b001;
  localparam logic [2:0] F3_BLT  = 3'b100;
  localparam logic [2:0] F3_BGE  = 3'b101;
  localparam logic [2:0] F3_BLTU = 3'b110;
  localparam logic [2:0] F3_BGEU = 3'b111;

  // Load/store funct3: [1:0] is the size, [2] selects zero extension.
  localparam logic [1:0] SIZE_B = 2'b00;
  localparam logic [1:0] SIZE_H = 2'b01;
  localparam logic [1:0] SIZE_W = 2'b10;

  // Counter CSRs (all read-only). time/timeh alias cycle/cycleh.
  localparam logic [11:0] CSR_CYCLE     = 12'hC00;
  localparam logic [11:0] CSR_TIME      = 12'hC01;
  localparam logic [11:0] CSR_INSTRET   = 12'hC02;
  localparam logic [11:0] CSR_CYCLEH    = 12'hC80;
  localparam logic [11:0] CSR_TIMEH     = 12'hC81;
  localparam logic [11:0] CSR_INSTRETH  = 12'hC82;
  localparam logic [11:0] CSR_MCYCLE    = 12'hB00;
  localparam logic [11:0] CSR_MINSTRET  = 12'hB02;
  localparam logic [11:0] CSR_MCYCLEH   = 12'hB80;
  localparam logic [11:0] CSR_MINSTRETH = 12'hB82;

  typedef enum logic [3:0] {
    ALU_ADD,
    ALU_SUB,
    ALU_SLL,
    ALU_SLT,
    ALU_SLTU,
    ALU_XOR,
    ALU_SRL,
    ALU_SRA,
    ALU_OR,
    ALU_AND
  } alu_op_e;

  // ALU operand A. LUI uses zero + imm so the ALU adder produces the result.
  typedef enum logic [1:0] {
    OPA_RS1,
    OPA_PC,
    OPA_ZERO
  } opa_sel_e;

  // Value written to rd, chosen in EX. Load data is merged in later, in WB.
  typedef enum logic [2:0] {
    RES_ALU,
    RES_LINK,
    RES_CSR,
    RES_MUL,
    RES_DIV
  } res_sel_e;

endpackage
