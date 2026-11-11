#include "cpu.h"

#include "bits.h"

namespace iss {
namespace {

constexpr uint32_t kCsrCycle = 0xc00;
constexpr uint32_t kCsrTime = 0xc01;
constexpr uint32_t kCsrInstret = 0xc02;
constexpr uint32_t kCsrCycleh = 0xc80;
constexpr uint32_t kCsrTimeh = 0xc81;
constexpr uint32_t kCsrInstreth = 0xc82;
constexpr uint32_t kCsrMcycle = 0xb00;
constexpr uint32_t kCsrMinstret = 0xb02;
constexpr uint32_t kCsrMcycleh = 0xb80;
constexpr uint32_t kCsrMinstreth = 0xb82;

constexpr uint32_t kIntMin = 0x80000000u;

uint32_t mulh(uint32_t a, uint32_t b) {
    const int64_t p = int64_t{as_signed(a)} * int64_t{as_signed(b)};
    return static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32);
}

// |signed * unsigned| < 2^63, so the product fits in int64_t.
uint32_t mulhsu(uint32_t a, uint32_t b) {
    const int64_t p = int64_t{as_signed(a)} * int64_t{b};
    return static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32);
}

uint32_t mulhu(uint32_t a, uint32_t b) {
    return static_cast<uint32_t>((uint64_t{a} * uint64_t{b}) >> 32);
}

// RISC-V defines every corner case instead of trapping: x / 0 = -1,
// x % 0 = x, and INT_MIN / -1 overflows to INT_MIN with remainder 0.
uint32_t div(uint32_t a, uint32_t b) {
    if (b == 0) {
        return 0xffffffffu;
    }
    if (a == kIntMin && b == 0xffffffffu) {
        return kIntMin;
    }
    return static_cast<uint32_t>(as_signed(a) / as_signed(b));
}

uint32_t divu(uint32_t a, uint32_t b) { return b == 0 ? 0xffffffffu : a / b; }

uint32_t rem(uint32_t a, uint32_t b) {
    if (b == 0) {
        return a;
    }
    if (a == kIntMin && b == 0xffffffffu) {
        return 0;
    }
    return static_cast<uint32_t>(as_signed(a) % as_signed(b));
}

uint32_t remu(uint32_t a, uint32_t b) { return b == 0 ? a : a % b; }

}  // namespace

Cpu::Cpu(const Memory& mem, Soc& soc) : mem_(mem), soc_(soc) { reset(); }

void Cpu::reset() {
    icache_.resize(kImemSize / 4);
    for (uint32_t i = 0; i < kImemSize / 4; i++) {
        icache_[i] = decode(mem_.imem_word(kImemBase + 4 * i));
    }
    bad_fetch_ = decode(0);
    for (uint32_t& r : x_) {
        r = 0;
    }
    pc_ = kImemBase;
    instret_ = 0;
    misaligned_ = 0;
}

// Counters read their value before the reading instruction retires. The ISS
// has no clock, so cycle and time report instret.
uint32_t Cpu::read_csr(uint32_t csr, uint64_t instret) {
    switch (csr) {
    case kCsrCycle: case kCsrTime: case kCsrInstret: case kCsrMcycle: case kCsrMinstret:
        return static_cast<uint32_t>(instret);
    case kCsrCycleh: case kCsrTimeh: case kCsrInstreth: case kCsrMcycleh: case kCsrMinstreth:
        return static_cast<uint32_t>(instret >> 32);
    default:
        return 0;
    }
}

StopReason Cpu::run(uint64_t max_instret, CommitLog* log) {
    return log != nullptr ? run_loop<true>(max_instret, log) : run_loop<false>(max_instret, log);
}

// pc and instret live in locals so the compiler can keep them in registers
// across the calls into the SoC; they are written back on every exit.
template <bool kLog>
StopReason Cpu::run_loop(uint64_t max_instret, CommitLog* log) {
    uint32_t* const x = x_;
    uint32_t pc = pc_;
    uint64_t instret = instret_;
    StopReason reason = StopReason::kMaxInstret;

    while (instret < max_instret) {
        const Insn& d = fetch(pc);
        const uint32_t rs1 = x[d.rs1];
        const uint32_t rs2 = x[d.rs2];
        uint32_t next_pc = pc + 4;
        uint32_t val = 0;
        bool is_store = false;
        uint32_t store_be = 0;
        uint32_t store_data = 0;
        bool halt = false;

        switch (d.op) {
        case Op::kLui: val = d.imm; break;
        case Op::kAuipc: val = pc + d.imm; break;
        case Op::kJal: val = pc + 4; next_pc = pc + d.imm; break;
        case Op::kJalr: val = pc + 4; next_pc = (rs1 + d.imm) & ~1u; break;

        case Op::kBeq: if (rs1 == rs2) next_pc = pc + d.imm; break;
        case Op::kBne: if (rs1 != rs2) next_pc = pc + d.imm; break;
        case Op::kBlt: if (as_signed(rs1) < as_signed(rs2)) next_pc = pc + d.imm; break;
        case Op::kBge: if (as_signed(rs1) >= as_signed(rs2)) next_pc = pc + d.imm; break;
        case Op::kBltu: if (rs1 < rs2) next_pc = pc + d.imm; break;
        case Op::kBgeu: if (rs1 >= rs2) next_pc = pc + d.imm; break;

        case Op::kLb: case Op::kLh: case Op::kLw: case Op::kLbu: case Op::kLhu: {
            const uint32_t addr = rs1 + d.imm;
            const uint32_t data = soc_.load(addr) >> (8 * (addr & 3));
            switch (d.op) {
            case Op::kLb: val = sext(data, 8); break;
            case Op::kLbu: val = data & 0xffu; break;
            case Op::kLh: val = sext(data, 16); misaligned_ += addr & 1; break;
            case Op::kLhu: val = data & 0xffffu; misaligned_ += addr & 1; break;
            default: val = data; misaligned_ += (addr & 3) != 0; break;
            }
            break;
        }

        case Op::kSb: case Op::kSh: case Op::kSw: {
            const uint32_t addr = rs1 + d.imm;
            const unsigned lane = addr & 3;
            uint32_t mask = 0xfu;
            uint32_t data = rs2;
            if (d.op == Op::kSb) {
                mask = 0x1u;
                data &= 0xffu;
            } else if (d.op == Op::kSh) {
                mask = 0x3u;
                data &= 0xffffu;
                misaligned_ += addr & 1;
            } else {
                misaligned_ += lane != 0;
            }
            is_store = true;
            store_be = (mask << lane) & 0xfu;
            store_data = data << (8 * lane);
            val = addr & ~3u;  // only for the log; stores have rd == 0
            soc_.store(addr, store_data, store_be);
            break;
        }

        case Op::kAddi: val = rs1 + d.imm; break;
        case Op::kSlti: val = as_signed(rs1) < as_signed(d.imm); break;
        case Op::kSltiu: val = rs1 < d.imm; break;
        case Op::kXori: val = rs1 ^ d.imm; break;
        case Op::kOri: val = rs1 | d.imm; break;
        case Op::kAndi: val = rs1 & d.imm; break;
        case Op::kSlli: val = rs1 << d.imm; break;
        case Op::kSrli: val = rs1 >> d.imm; break;
        case Op::kSrai: val = sra(rs1, d.imm); break;

        case Op::kAdd: val = rs1 + rs2; break;
        case Op::kSub: val = rs1 - rs2; break;
        case Op::kSll: val = rs1 << (rs2 & 31); break;
        case Op::kSlt: val = as_signed(rs1) < as_signed(rs2); break;
        case Op::kSltu: val = rs1 < rs2; break;
        case Op::kXor: val = rs1 ^ rs2; break;
        case Op::kSrl: val = rs1 >> (rs2 & 31); break;
        case Op::kSra: val = sra(rs1, rs2 & 31); break;
        case Op::kOr: val = rs1 | rs2; break;
        case Op::kAnd: val = rs1 & rs2; break;

        case Op::kMul: val = rs1 * rs2; break;
        case Op::kMulh: val = mulh(rs1, rs2); break;
        case Op::kMulhsu: val = mulhsu(rs1, rs2); break;
        case Op::kMulhu: val = mulhu(rs1, rs2); break;
        case Op::kDiv: val = div(rs1, rs2); break;
        case Op::kDivu: val = divu(rs1, rs2); break;
        case Op::kRem: val = rem(rs1, rs2); break;
        case Op::kRemu: val = remu(rs1, rs2); break;

        case Op::kCsr: val = read_csr(d.imm, instret); break;
        case Op::kFence: break;

        case Op::kEcall: reason = StopReason::kEcall; halt = true; break;
        case Op::kEbreak: reason = StopReason::kEbreak; halt = true; break;
        case Op::kIllegal: reason = StopReason::kIllegal; halt = true; break;
        }

        if (d.rd != 0) {
            x[d.rd] = val;
        }
        instret++;
        if (kLog) {
            if (is_store) {
                log->retire_store(pc, d.raw, val, store_be, store_data);
            } else {
                log->retire(pc, d.raw, d.rd, val);
            }
        }
        if (halt) {
            break;  // pc stays on the halting instruction
        }
        pc = next_pc;
        if (is_store && soc_.exit_requested()) {
            reason = StopReason::kExit;
            break;
        }
    }
    pc_ = pc;
    instret_ = instret;
    return reason;
}

}  // namespace iss
