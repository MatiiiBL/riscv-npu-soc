// Pulse RV32IM_Zicsr core: in-order, five stages (IF ID EX MEM WB).
//
// See docs/core.md for the full description. In short:
//
//  * IF presents pc_f to the synchronous IMEM and looks it up in the branch
//    predictor in the same cycle; the BRAM output register is the IF/ID
//    instruction register, so ID decodes imem_rdata directly. Stalling ID
//    therefore means dropping imem_req, which makes the BRAM hold its output.
//  * Branches and jumps resolve in EX. A wrong prediction redirects IF and
//    kills the two younger instructions in IF and ID (2 bubbles).
//  * Operands are forwarded into EX from EX/MEM and MEM/WB; the register file
//    is write-before-read, which covers the WB -> ID distance.
//  * A load followed by an instruction that reads its result stalls ID once;
//    the load data then reaches EX through the MEM/WB forward.
//  * DIV/REM hold EX for 34 cycles while the iterative divider runs; MUL is a
//    single-cycle EX operation.
//  * ECALL, EBREAK and illegal instructions flush everything younger when
//    they reach EX, retire normally from WB and then raise `halted`.
//
// Stage-suffix convention: _f IF, _d ID, _x EX, _m MEM, _w WB.
module rv32_core #(
  parameter int BP_MODE     = 1,   // 0: static not-taken, 1: BTB + 2-bit BHT
  parameter int BTB_ENTRIES = 64
) (
  input  logic        clk,
  input  logic        rst_n,

  // Instruction port (docs/spec.md 4.1)
  output logic        imem_req,
  output logic [31:0] imem_addr,
  input  logic [31:0] imem_rdata,

  // Data port (docs/spec.md 4.2)
  output logic        dmem_req,
  output logic        dmem_we,
  output logic [31:0] dmem_addr,
  output logic [3:0]  dmem_be,
  output logic [31:0] dmem_wdata,
  input  logic [31:0] dmem_rdata,

  // Retirement port (docs/spec.md 4.3)
  output logic        rvfi_valid,
  output logic [31:0] rvfi_pc,
  output logic [31:0] rvfi_insn,
  output logic [4:0]  rvfi_rd,
  output logic [31:0] rvfi_rd_wdata,
  output logic [31:0] rvfi_mem_addr,
  output logic [3:0]  rvfi_mem_wmask,
  output logic [31:0] rvfi_mem_wdata,
  output logic        halted,

  // Performance counters (docs/spec.md 6)
  output logic [63:0] perf_cycles,
  output logic [63:0] perf_instret,
  output logic [31:0] perf_load_use,     // bubbles inserted by load-use stalls
  output logic [31:0] perf_mispredict,   // EX redirects (wrong direction or target)
  output logic [31:0] perf_div_stall,    // cycles EX was held by the divider
  output logic [31:0] perf_ctrl          // branches and jumps resolved
);

  localparam logic [31:0] RESET_PC = 32'h0000_0000;

  // Pipeline control, produced further down.
  logic        stall_d;        // hold IF and ID
  logic        lu_hazard;      // load in EX feeds the instruction in ID
  logic        div_stall;      // divide in EX not finished: hold IF, ID, EX
  logic        redirect_x;     // EX found a misprediction
  logic [31:0] redirect_pc_x;
  logic        halt_x;         // halting instruction in EX
  logic        halt_q;         // a halting instruction has passed EX: stop fetching

  // ======================================================================
  // IF
  // ======================================================================
  logic [31:0] pc_f, npc_f;
  logic        pred_taken_f;
  logic        bp_hit_f;
  logic [1:0]  bp_ctr_f;
  logic [31:0] bp_tgt_f;

  // Update interface, driven from EX.
  logic        bp_upd_valid, bp_upd_cond, bp_upd_taken, bp_upd_hit;
  logic [31:0] bp_upd_pc, bp_upd_target, bp_upd_old_target;
  logic [1:0]  bp_upd_ctr;

  rv32_bpred #(
    .BP_MODE     (BP_MODE),
    .BTB_ENTRIES (BTB_ENTRIES)
  ) u_bpred (
    .clk            (clk),
    .rst_n          (rst_n),
    .if_pc          (pc_f),
    .pred_hit       (bp_hit_f),
    .pred_ctr       (bp_ctr_f),
    .pred_target    (bp_tgt_f),
    .upd_valid      (bp_upd_valid),
    .upd_pc         (bp_upd_pc),
    .upd_cond       (bp_upd_cond),
    .upd_taken      (bp_upd_taken),
    .upd_target     (bp_upd_target),
    .upd_hit        (bp_upd_hit),
    .upd_ctr        (bp_upd_ctr),
    .upd_old_target (bp_upd_old_target)
  );

  // The fetch decision itself travels down the pipe, so EX checks exactly
  // what IF did rather than re-deriving it from the counter.
  assign pred_taken_f = bp_hit_f && bp_ctr_f[1];
  assign npc_f        = pred_taken_f ? bp_tgt_f : pc_f + 32'd4;
  assign imem_req  = !stall_d && !halt_q;
  assign imem_addr = pc_f;

  // IF/ID. The instruction word itself is imem_rdata.
  logic        valid_d;
  logic [31:0] pc_d;
  logic        pred_taken_d;
  logic        bp_hit_d;
  logic [1:0]  bp_ctr_d;
  logic [31:0] bp_tgt_d;

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      pc_f    <= RESET_PC;
      valid_d <= 1'b0;
    end else if (redirect_x) begin
      pc_f    <= redirect_pc_x;
      valid_d <= 1'b0;
    end else if (halt_x || halt_q) begin
      valid_d <= 1'b0;
    end else if (!stall_d) begin
      pc_f    <= npc_f;
      valid_d <= 1'b1;
    end
  end

  always_ff @(posedge clk) begin
    if (!stall_d) begin
      pc_d         <= pc_f;
      pred_taken_d <= pred_taken_f;
      bp_hit_d     <= bp_hit_f;
      bp_ctr_d     <= bp_ctr_f;
      bp_tgt_d     <= bp_tgt_f;
    end
  end

  // ======================================================================
  // ID
  // ======================================================================
  logic [31:0]        insn_d;
  logic [4:0]         rs1_d, rs2_d, rd_d;
  logic               uses_rs1_d, uses_rs2_d, rd_we_d;
  logic [31:0]        imm_d;
  rv32_pkg::alu_op_e  alu_op_d;
  rv32_pkg::opa_sel_e opa_sel_d;
  logic               opb_imm_d;
  rv32_pkg::res_sel_e res_sel_d;
  logic               is_branch_d, is_jal_d, is_jalr_d, is_load_d, is_store_d;
  logic               is_div_d, is_halt_d;
  logic [31:0]        rs1_val_d, rs2_val_d;
  logic [31:0]        tgt_d;

  assign insn_d = imem_rdata;

  rv32_decode u_decode (
    .insn      (insn_d),
    .rs1       (rs1_d),
    .rs2       (rs2_d),
    .rd        (rd_d),
    .uses_rs1  (uses_rs1_d),
    .uses_rs2  (uses_rs2_d),
    .rd_we     (rd_we_d),
    .imm       (imm_d),
    .alu_op    (alu_op_d),
    .opa_sel   (opa_sel_d),
    .opb_imm   (opb_imm_d),
    .res_sel   (res_sel_d),
    .is_branch (is_branch_d),
    .is_jal    (is_jal_d),
    .is_jalr   (is_jalr_d),
    .is_load   (is_load_d),
    .is_store  (is_store_d),
    .is_div    (is_div_d),
    .is_halt   (is_halt_d)
  );

  // WB write port, driven from the WB stage.
  logic        rf_we_w;
  logic [4:0]  rd_w;
  logic [31:0] wb_data_w;

  rv32_regfile u_regfile (
    .clk    (clk),
    .raddr1 (rs1_d),
    .raddr2 (rs2_d),
    .rdata1 (rs1_val_d),
    .rdata2 (rs2_val_d),
    .we     (rf_we_w),
    .waddr  (rd_w),
    .wdata  (wb_data_w)
  );

  // Branch and JAL targets do not depend on registers, so they are computed
  // here, as is the check of the predicted target, keeping both off the EX
  // critical path.
  assign tgt_d = pc_d + imm_d;

  // ID/EX
  logic               valid_x;
  logic [31:0]        pc_x, insn_x, imm_x, tgt_x, rs1_val_x, rs2_val_x;
  logic [4:0]         rs1_x, rs2_x, rd_x;
  logic               rd_we_x;
  rv32_pkg::alu_op_e  alu_op_x;
  rv32_pkg::opa_sel_e opa_sel_x;
  logic               opb_imm_x;
  rv32_pkg::res_sel_e res_sel_x;
  logic               is_branch_x, is_jal_x, is_jalr_x, is_load_x, is_store_x;
  logic               is_div_x, is_halt_x;
  logic               pred_taken_x;
  logic               bp_hit_x;
  logic [1:0]         bp_ctr_x;
  logic [31:0]        bp_tgt_x;
  logic               tgt_ok_x;      // predicted target equals tgt_x

  // Load-use: the only data hazard forwarding cannot cover. A divide sitting
  // in EX is never a load, so this and div_stall are mutually exclusive.
  assign lu_hazard = valid_x && is_load_x && rd_we_x && valid_d &&
                     ((uses_rs1_d && rs1_d == rd_x) || (uses_rs2_d && rs2_d == rd_x));
  assign stall_d   = lu_hazard || div_stall;

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      valid_x <= 1'b0;
    end else if (!div_stall) begin
      valid_x <= valid_d && !lu_hazard && !redirect_x && !halt_x;
    end
  end

  always_ff @(posedge clk) begin
    if (!div_stall) begin
      pc_x         <= pc_d;
      insn_x       <= insn_d;
      imm_x        <= imm_d;
      tgt_x        <= tgt_d;
      rs1_x        <= rs1_d;
      rs2_x        <= rs2_d;
      rs1_val_x    <= rs1_val_d;
      rs2_val_x    <= rs2_val_d;
      rd_x         <= rd_d;
      rd_we_x      <= rd_we_d;
      alu_op_x     <= alu_op_d;
      opa_sel_x    <= opa_sel_d;
      opb_imm_x    <= opb_imm_d;
      res_sel_x    <= res_sel_d;
      is_branch_x  <= is_branch_d;
      is_jal_x     <= is_jal_d;
      is_jalr_x    <= is_jalr_d;
      is_load_x    <= is_load_d;
      is_store_x   <= is_store_d;
      is_div_x     <= is_div_d;
      is_halt_x    <= is_halt_d;
      pred_taken_x <= pred_taken_d;
      bp_hit_x     <= bp_hit_d;
      bp_ctr_x     <= bp_ctr_d;
      bp_tgt_x     <= bp_tgt_d;
      tgt_ok_x     <= (bp_tgt_d == tgt_d);
    end
  end

  // ======================================================================
  // EX
  // ======================================================================
  // EX/MEM and MEM/WB state used for forwarding, declared here.
  logic        valid_m, rd_we_m;
  logic [4:0]  rd_m;
  logic [31:0] res_m;
  logic        valid_w, rd_we_w;

  logic [31:0] fwd1_x, fwd2_x, op_a_x, op_b_x, alu_y_x;
  logic [31:0] link_x, jalr_tgt_x, act_tgt_x, csr_y_x, mul_y_x, div_y_x, res_x;
  logic        div_done_x;
  logic        cmp_eq_x, cmp_lt_x, cmp_ltu_x, cond_x;
  logic        taken_x, tgt_match_x;

  // The MEM-stage result is never a load's data here: lu_hazard guarantees
  // a consumer of a load is still in ID while the load is in MEM.
  always_comb begin
    if (valid_m && rd_we_m && rd_m == rs1_x)      fwd1_x = res_m;
    else if (valid_w && rd_we_w && rd_w == rs1_x) fwd1_x = wb_data_w;
    else                                          fwd1_x = rs1_val_x;

    if (valid_m && rd_we_m && rd_m == rs2_x)      fwd2_x = res_m;
    else if (valid_w && rd_we_w && rd_w == rs2_x) fwd2_x = wb_data_w;
    else                                          fwd2_x = rs2_val_x;
  end

  always_comb begin
    case (opa_sel_x)
      rv32_pkg::OPA_PC:   op_a_x = pc_x;
      rv32_pkg::OPA_ZERO: op_a_x = '0;
      default:            op_a_x = fwd1_x;
    endcase
  end
  assign op_b_x = opb_imm_x ? imm_x : fwd2_x;

  rv32_alu u_alu (
    .op (alu_op_x),
    .a  (op_a_x),
    .b  (op_b_x),
    .y  (alu_y_x)
  );

  rv32_muldiv u_muldiv (
    .clk      (clk),
    .rst_n    (rst_n),
    .op       (insn_x[13:12]),
    .a        (fwd1_x),
    .b        (fwd2_x),
    .mul_y    (mul_y_x),
    .div_req  (valid_x && is_div_x),
    .div_done (div_done_x),
    .div_y    (div_y_x)
  );

  assign div_stall = valid_x && is_div_x && !div_done_x;

  // Branch resolution.
  assign cmp_eq_x  = (fwd1_x == fwd2_x);
  assign cmp_lt_x  = ($signed(fwd1_x) < $signed(fwd2_x));
  assign cmp_ltu_x = (fwd1_x < fwd2_x);

  always_comb begin
    case (insn_x[14:12])
      rv32_pkg::F3_BEQ:  cond_x = cmp_eq_x;
      rv32_pkg::F3_BNE:  cond_x = !cmp_eq_x;
      rv32_pkg::F3_BLT:  cond_x = cmp_lt_x;
      rv32_pkg::F3_BGE:  cond_x = !cmp_lt_x;
      rv32_pkg::F3_BLTU: cond_x = cmp_ltu_x;
      rv32_pkg::F3_BGEU: cond_x = !cmp_ltu_x;
      default:           cond_x = 1'b0;  // reserved encodings decode as illegal
    endcase
  end

  assign link_x       = pc_x + 32'd4;
  assign jalr_tgt_x   = {alu_y_x[31:1], 1'b0};
  assign act_tgt_x    = is_jalr_x ? jalr_tgt_x : tgt_x;
  assign taken_x      = is_jal_x || is_jalr_x || (is_branch_x && cond_x);
  assign tgt_match_x  = is_jalr_x ? (bp_tgt_x == jalr_tgt_x) : tgt_ok_x;

  // Comparing against the predicted next PC for every instruction (not only
  // branches) keeps recovery correct whatever the predictor returned.
  assign redirect_x    = valid_x && ((taken_x != pred_taken_x) || (taken_x && !tgt_match_x));
  assign redirect_pc_x = taken_x ? act_tgt_x : link_x;
  assign halt_x        = valid_x && is_halt_x;

  assign bp_upd_valid      = valid_x && (is_branch_x || is_jal_x || is_jalr_x);
  assign bp_upd_pc         = pc_x;
  assign bp_upd_cond       = is_branch_x;
  assign bp_upd_taken      = taken_x;
  assign bp_upd_target     = act_tgt_x;
  assign bp_upd_hit        = bp_hit_x;
  assign bp_upd_ctr        = bp_ctr_x;
  assign bp_upd_old_target = bp_tgt_x;

  // Counters. A CSR read in EX must see instret as of its own position in
  // program order, so the valid instructions ahead of it in MEM and WB, which
  // retire before it but have not been counted yet, are added in.
  logic [63:0] cycle_q, instret_q, instret_x;
  logic [11:0] csr_addr_x;

  assign instret_x  = instret_q + {63'b0, valid_m} + {63'b0, valid_w};
  assign csr_addr_x = imm_x[11:0];

  always_comb begin
    case (csr_addr_x)
      rv32_pkg::CSR_CYCLE, rv32_pkg::CSR_MCYCLE, rv32_pkg::CSR_TIME:
        csr_y_x = cycle_q[31:0];
      rv32_pkg::CSR_CYCLEH, rv32_pkg::CSR_MCYCLEH, rv32_pkg::CSR_TIMEH:
        csr_y_x = cycle_q[63:32];
      rv32_pkg::CSR_INSTRET, rv32_pkg::CSR_MINSTRET:
        csr_y_x = instret_x[31:0];
      rv32_pkg::CSR_INSTRETH, rv32_pkg::CSR_MINSTRETH:
        csr_y_x = instret_x[63:32];
      default:
        csr_y_x = '0;
    endcase
  end

  always_comb begin
    case (res_sel_x)
      rv32_pkg::RES_LINK: res_x = link_x;
      rv32_pkg::RES_CSR:  res_x = csr_y_x;
      rv32_pkg::RES_MUL:  res_x = mul_y_x;
      rv32_pkg::RES_DIV:  res_x = div_y_x;
      default:            res_x = alu_y_x;
    endcase
  end

  // EX/MEM
  logic [31:0] pc_m, insn_m, store_data_m;
  logic        is_load_m, is_store_m, is_halt_m;
  logic [2:0]  funct3_m;

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      valid_m <= 1'b0;
    end else begin
      valid_m <= valid_x && !div_stall;
    end
  end

  always_ff @(posedge clk) begin
    pc_m         <= pc_x;
    insn_m       <= insn_x;
    rd_m         <= rd_x;
    rd_we_m      <= rd_we_x;
    res_m        <= res_x;
    store_data_m <= fwd2_x;
    is_load_m    <= is_load_x;
    is_store_m   <= is_store_x;
    is_halt_m    <= is_halt_x;
    funct3_m     <= insn_x[14:12];
  end

  // ======================================================================
  // MEM
  // ======================================================================
  logic [1:0]  byte_off_m;
  logic [3:0]  be_m;
  logic [31:0] wdata_m;

  assign byte_off_m = res_m[1:0];

  // Store data goes out already in its byte lanes with the other lanes
  // zero, which is also the form the retirement port reports.
  always_comb begin
    case (funct3_m[1:0])
      rv32_pkg::SIZE_B: begin
        be_m    = 4'b0001 << byte_off_m;
        wdata_m = {24'b0, store_data_m[7:0]} << {byte_off_m, 3'b000};
      end
      rv32_pkg::SIZE_H: begin
        be_m    = 4'b0011 << byte_off_m;
        wdata_m = {16'b0, store_data_m[15:0]} << {byte_off_m, 3'b000};
      end
      default: begin
        be_m    = 4'b1111;
        wdata_m = store_data_m;
      end
    endcase
  end

  assign dmem_req   = valid_m && (is_load_m || is_store_m);
  assign dmem_we    = is_store_m;
  assign dmem_addr  = res_m;
  assign dmem_be    = be_m;
  assign dmem_wdata = wdata_m;

  // MEM/WB
  logic [31:0] pc_w, insn_w, res_w, mem_addr_w, mem_wdata_w;
  logic [3:0]  mem_wmask_w;
  logic        is_load_w, is_halt_w;
  logic [2:0]  funct3_w;
  logic [1:0]  byte_off_w;

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      valid_w <= 1'b0;
    end else begin
      valid_w <= valid_m;
    end
  end

  always_ff @(posedge clk) begin
    pc_w        <= pc_m;
    insn_w      <= insn_m;
    rd_w        <= rd_m;
    rd_we_w     <= rd_we_m;
    res_w       <= res_m;
    is_load_w   <= is_load_m;
    is_halt_w   <= is_halt_m;
    funct3_w    <= funct3_m;
    byte_off_w  <= byte_off_m;
    mem_addr_w  <= (is_load_m || is_store_m) ? {res_m[31:2], 2'b00} : '0;
    mem_wmask_w <= is_store_m ? be_m : 4'b0000;
    mem_wdata_w <= is_store_m ? wdata_m : '0;
  end

  // ======================================================================
  // WB
  // ======================================================================
  logic [7:0]  load_byte_w;
  logic [15:0] load_half_w;
  logic [31:0] load_y_w;

  always_comb begin
    case (byte_off_w)
      2'd0:    load_byte_w = dmem_rdata[7:0];
      2'd1:    load_byte_w = dmem_rdata[15:8];
      2'd2:    load_byte_w = dmem_rdata[23:16];
      default: load_byte_w = dmem_rdata[31:24];
    endcase
    load_half_w = byte_off_w[1] ? dmem_rdata[31:16] : dmem_rdata[15:0];

    case (funct3_w)
      3'b000:  load_y_w = {{24{load_byte_w[7]}}, load_byte_w};    // LB
      3'b001:  load_y_w = {{16{load_half_w[15]}}, load_half_w};   // LH
      3'b100:  load_y_w = {24'b0, load_byte_w};                   // LBU
      3'b101:  load_y_w = {16'b0, load_half_w};                   // LHU
      default: load_y_w = dmem_rdata;                             // LW
    endcase
  end

  assign wb_data_w = is_load_w ? load_y_w : res_w;
  assign rf_we_w   = valid_w && rd_we_w;

  assign rvfi_valid     = valid_w;
  assign rvfi_pc        = pc_w;
  assign rvfi_insn      = insn_w;
  assign rvfi_rd        = rd_we_w ? rd_w : 5'd0;
  assign rvfi_rd_wdata  = rd_we_w ? wb_data_w : '0;
  assign rvfi_mem_addr  = mem_addr_w;
  assign rvfi_mem_wmask = mem_wmask_w;
  assign rvfi_mem_wdata = mem_wdata_w;

  // ======================================================================
  // Halt and counters
  // ======================================================================
  logic [31:0] load_use_q, mispredict_q, div_stall_q, ctrl_q;

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      halt_q       <= 1'b0;
      halted       <= 1'b0;
      cycle_q      <= '0;
      instret_q    <= '0;
      load_use_q   <= '0;
      mispredict_q <= '0;
      div_stall_q  <= '0;
      ctrl_q       <= '0;
    end else begin
      if (halt_x)                halt_q <= 1'b1;
      if (valid_w && is_halt_w)  halted <= 1'b1;
      cycle_q      <= cycle_q + 64'd1;
      instret_q    <= instret_q + {63'b0, valid_w};
      load_use_q   <= load_use_q + {31'b0, lu_hazard};
      mispredict_q <= mispredict_q + {31'b0, redirect_x};
      div_stall_q  <= div_stall_q + {31'b0, div_stall};
      ctrl_q       <= ctrl_q + {31'b0, bp_upd_valid};
    end
  end

  assign perf_cycles     = cycle_q;
  assign perf_instret    = instret_q;
  assign perf_load_use   = load_use_q;
  assign perf_mispredict = mispredict_q;
  assign perf_div_stall  = div_stall_q;
  assign perf_ctrl       = ctrl_q;

endmodule
