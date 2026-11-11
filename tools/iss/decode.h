// RV32IM + Zicsr instruction decoder (docs/spec.md section 2).
#ifndef PULSE_ISS_DECODE_H
#define PULSE_ISS_DECODE_H

#include <cstdint>

namespace iss {

enum class Op : uint8_t {
    kLui, kAuipc, kJal, kJalr,
    kBeq, kBne, kBlt, kBge, kBltu, kBgeu,
    kLb, kLh, kLw, kLbu, kLhu,
    kSb, kSh, kSw,
    kAddi, kSlti, kSltiu, kXori, kOri, kAndi, kSlli, kSrli, kSrai,
    kAdd, kSub, kSll, kSlt, kSltu, kXor, kSrl, kSra, kOr, kAnd,
    kMul, kMulh, kMulhsu, kMulhu, kDiv, kDivu, kRem, kRemu,
    // Every CSR is read-only or reads as zero and writes are ignored, so all
    // six Zicsr instructions reduce to "rd = csr".
    kCsr,
    kFence,  // FENCE and FENCE.I, both no-ops
    kEcall, kEbreak,
    kIllegal,
};

struct Insn {
    uint32_t raw = 0;
    uint32_t imm = 0;  // sign-extended immediate; shamt for shifts, CSR number for kCsr
    Op op = Op::kIllegal;
    uint8_t rd = 0;    // 0 for instructions that write no register
    uint8_t rs1 = 0;
    uint8_t rs2 = 0;
};

// Decodes one 32-bit instruction. Reserved encodings (unknown opcode,
// funct3/funct7 combination or SYSTEM instruction) decode to Op::kIllegal.
Insn decode(uint32_t raw);

}  // namespace iss

#endif  // PULSE_ISS_DECODE_H
