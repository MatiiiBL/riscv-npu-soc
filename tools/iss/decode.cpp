#include "decode.h"

#include "bits.h"

namespace iss {
namespace {

constexpr uint32_t kOpLoad = 0x03;
constexpr uint32_t kOpMiscMem = 0x0f;
constexpr uint32_t kOpImm = 0x13;
constexpr uint32_t kOpAuipc = 0x17;
constexpr uint32_t kOpStore = 0x23;
constexpr uint32_t kOpReg = 0x33;
constexpr uint32_t kOpLui = 0x37;
constexpr uint32_t kOpBranch = 0x63;
constexpr uint32_t kOpJalr = 0x67;
constexpr uint32_t kOpJal = 0x6f;
constexpr uint32_t kOpSystem = 0x73;

constexpr uint32_t kEcallWord = 0x00000073;
constexpr uint32_t kEbreakWord = 0x00100073;

uint32_t imm_i(uint32_t raw) { return sext(raw >> 20, 12); }

uint32_t imm_s(uint32_t raw) { return sext((raw >> 25) << 5 | ((raw >> 7) & 0x1f), 12); }

uint32_t imm_b(uint32_t raw) {
    const uint32_t imm = ((raw >> 31) & 1) << 12 | ((raw >> 7) & 1) << 11 |
                         ((raw >> 25) & 0x3f) << 5 | ((raw >> 8) & 0xf) << 1;
    return sext(imm, 13);
}

uint32_t imm_j(uint32_t raw) {
    const uint32_t imm = ((raw >> 31) & 1) << 20 | ((raw >> 12) & 0xff) << 12 |
                         ((raw >> 20) & 1) << 11 | ((raw >> 21) & 0x3ff) << 1;
    return sext(imm, 21);
}

Op decode_branch(uint32_t funct3) {
    switch (funct3) {
    case 0: return Op::kBeq;
    case 1: return Op::kBne;
    case 4: return Op::kBlt;
    case 5: return Op::kBge;
    case 6: return Op::kBltu;
    case 7: return Op::kBgeu;
    default: return Op::kIllegal;
    }
}

Op decode_load(uint32_t funct3) {
    switch (funct3) {
    case 0: return Op::kLb;
    case 1: return Op::kLh;
    case 2: return Op::kLw;
    case 4: return Op::kLbu;
    case 5: return Op::kLhu;
    default: return Op::kIllegal;
    }
}

Op decode_store(uint32_t funct3) {
    switch (funct3) {
    case 0: return Op::kSb;
    case 1: return Op::kSh;
    case 2: return Op::kSw;
    default: return Op::kIllegal;
    }
}

// In RV32 the shift-immediate forms keep funct7 in imm[11:5]; imm[5] (the
// RV64 shamt bit) set is a reserved encoding.
Op decode_op_imm(uint32_t funct3, uint32_t funct7) {
    switch (funct3) {
    case 0: return Op::kAddi;
    case 1: return funct7 == 0x00 ? Op::kSlli : Op::kIllegal;
    case 2: return Op::kSlti;
    case 3: return Op::kSltiu;
    case 4: return Op::kXori;
    case 5: return funct7 == 0x00 ? Op::kSrli : funct7 == 0x20 ? Op::kSrai : Op::kIllegal;
    case 6: return Op::kOri;
    default: return Op::kAndi;  // funct3 == 7
    }
}

Op decode_op_reg(uint32_t funct3, uint32_t funct7) {
    static constexpr Op kBase[8] = {Op::kAdd, Op::kSll, Op::kSlt, Op::kSltu,
                                    Op::kXor, Op::kSrl, Op::kOr,  Op::kAnd};
    static constexpr Op kMext[8] = {Op::kMul, Op::kMulh, Op::kMulhsu, Op::kMulhu,
                                    Op::kDiv, Op::kDivu, Op::kRem,    Op::kRemu};
    switch (funct7) {
    case 0x00: return kBase[funct3];
    case 0x01: return kMext[funct3];
    case 0x20: return funct3 == 0 ? Op::kSub : funct3 == 5 ? Op::kSra : Op::kIllegal;
    default: return Op::kIllegal;
    }
}

Op decode_system(uint32_t raw, uint32_t funct3) {
    if (funct3 == 0) {
        // MRET, WFI, SFENCE.VMA and friends are not implemented: no traps.
        return raw == kEcallWord ? Op::kEcall : raw == kEbreakWord ? Op::kEbreak : Op::kIllegal;
    }
    return funct3 == 4 ? Op::kIllegal : Op::kCsr;
}

bool writes_rd(Op op) {
    switch (op) {
    case Op::kBeq: case Op::kBne: case Op::kBlt: case Op::kBge: case Op::kBltu: case Op::kBgeu:
    case Op::kSb: case Op::kSh: case Op::kSw:
    case Op::kFence: case Op::kEcall: case Op::kEbreak: case Op::kIllegal:
        return false;
    default:
        return true;
    }
}

}  // namespace

Insn decode(uint32_t raw) {
    const uint32_t opcode = raw & 0x7f;
    const uint32_t funct3 = (raw >> 12) & 7;
    const uint32_t funct7 = raw >> 25;

    Insn d;
    d.raw = raw;
    d.rd = static_cast<uint8_t>((raw >> 7) & 0x1f);
    d.rs1 = static_cast<uint8_t>((raw >> 15) & 0x1f);
    d.rs2 = static_cast<uint8_t>((raw >> 20) & 0x1f);

    switch (opcode) {
    case kOpLui: d.op = Op::kLui; d.imm = raw & 0xfffff000u; break;
    case kOpAuipc: d.op = Op::kAuipc; d.imm = raw & 0xfffff000u; break;
    case kOpJal: d.op = Op::kJal; d.imm = imm_j(raw); break;
    case kOpJalr: d.op = funct3 == 0 ? Op::kJalr : Op::kIllegal; d.imm = imm_i(raw); break;
    case kOpBranch: d.op = decode_branch(funct3); d.imm = imm_b(raw); break;
    case kOpLoad: d.op = decode_load(funct3); d.imm = imm_i(raw); break;
    case kOpStore: d.op = decode_store(funct3); d.imm = imm_s(raw); break;
    case kOpImm:
        d.op = decode_op_imm(funct3, funct7);
        d.imm = (funct3 == 1 || funct3 == 5) ? d.rs2 : imm_i(raw);
        break;
    case kOpReg: d.op = decode_op_reg(funct3, funct7); break;
    // FENCE ignores its rs1/rd/fm fields, as the base ISA requires.
    case kOpMiscMem: d.op = funct3 <= 1 ? Op::kFence : Op::kIllegal; break;
    case kOpSystem: d.op = decode_system(raw, funct3); d.imm = raw >> 20; break;
    default: d.op = Op::kIllegal; break;  // includes all 16-bit (compressed) encodings
    }
    if (!writes_rd(d.op)) {
        d.rd = 0;
    }
    return d;
}

}  // namespace iss
