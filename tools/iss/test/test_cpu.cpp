// Unit tests for the decoder, the core, the memory map, the commit log and
// the ELF loader. Expected values are worked out by hand (or taken from
// llvm-mc for encodings), never computed with the code under test.
#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "check.h"
#include "commit_log.h"
#include "cpu.h"
#include "decode.h"
#include "elf.h"
#include "memory.h"
#include "npu.h"
#include "soc.h"

using iss::Op;
using iss::StopReason;

namespace {

// Instruction encoders, checked against llvm-mc in encoder_matches_llvm_mc.
namespace enc {

constexpr uint32_t r(uint32_t f7, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t opc) {
    return f7 << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | rd << 7 | opc;
}
constexpr uint32_t i(int32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t opc) {
    return (static_cast<uint32_t>(imm) & 0xfffu) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | opc;
}
constexpr uint32_t s(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3) {
    const uint32_t u = static_cast<uint32_t>(imm);
    return (u >> 5 & 0x7f) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | (u & 0x1f) << 7 | 0x23;
}
constexpr uint32_t b(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3) {
    const uint32_t u = static_cast<uint32_t>(imm);
    return (u >> 12 & 1) << 31 | (u >> 5 & 0x3f) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | (u >> 1 & 0xf) << 8 |
           (u >> 11 & 1) << 7 | 0x63;
}
constexpr uint32_t j(int32_t imm, uint32_t rd) {
    const uint32_t u = static_cast<uint32_t>(imm);
    return (u >> 20 & 1) << 31 | (u >> 1 & 0x3ff) << 21 | (u >> 11 & 1) << 20 | (u >> 12 & 0xff) << 12 | rd << 7 |
           0x6f;
}

constexpr uint32_t addi(uint32_t rd, uint32_t rs1, int32_t imm) { return i(imm, rs1, 0, rd, 0x13); }
constexpr uint32_t lui(uint32_t rd, uint32_t imm20) { return imm20 << 12 | rd << 7 | 0x37; }
constexpr uint32_t auipc(uint32_t rd, uint32_t imm20) { return imm20 << 12 | rd << 7 | 0x17; }
constexpr uint32_t load(uint32_t f3, uint32_t rd, uint32_t rs1, int32_t imm) { return i(imm, rs1, f3, rd, 0x03); }
constexpr uint32_t store(uint32_t f3, uint32_t rs2, uint32_t rs1, int32_t imm) { return s(imm, rs2, rs1, f3); }
constexpr uint32_t lw(uint32_t rd, uint32_t rs1, int32_t imm) { return load(2, rd, rs1, imm); }
constexpr uint32_t sw(uint32_t rs2, uint32_t rs1, int32_t imm) { return store(2, rs2, rs1, imm); }
constexpr uint32_t jalr(uint32_t rd, uint32_t rs1, int32_t imm) { return i(imm, rs1, 0, rd, 0x67); }
constexpr uint32_t csrr(uint32_t rd, uint32_t csr) { return i(static_cast<int32_t>(csr), 0, 2, rd, 0x73); }
constexpr uint32_t csrrw(uint32_t rd, uint32_t csr, uint32_t rs1) {
    return i(static_cast<int32_t>(csr), rs1, 1, rd, 0x73);
}
constexpr uint32_t kNop = 0x00000013;
constexpr uint32_t kEcall = 0x00000073;
constexpr uint32_t kEbreak = 0x00100073;

}  // namespace enc

// A whole SoC with a program at address 0.
struct Machine {
    iss::Memory mem;
    iss::Npu npu{mem};
    iss::Soc soc;
    iss::Cpu cpu;

    explicit Machine(std::FILE* uart = nullptr) : soc(mem, npu, uart), cpu(mem, soc) {}

    void program(const std::vector<uint32_t>& words) {
        std::vector<uint8_t> bytes;
        for (uint32_t w : words) {
            for (int k = 0; k < 4; k++) {
                bytes.push_back(static_cast<uint8_t>(w >> (8 * k)));
            }
        }
        const auto size = static_cast<uint32_t>(bytes.size());
        CHECK(mem.load(0, bytes.data(), size, size));
        cpu.reset();
    }

    void dmem(uint32_t addr, const std::vector<uint8_t>& bytes) {
        const auto size = static_cast<uint32_t>(bytes.size());
        CHECK(mem.load(addr, bytes.data(), size, size));
    }

    StopReason run(uint64_t max_instret = 1000) { return cpu.run(max_instret, nullptr); }

    // Runs with a commit log and returns its text.
    std::string run_logged(uint64_t max_instret, StopReason* reason = nullptr) {
        std::FILE* f = std::tmpfile();
        StopReason r = StopReason::kMaxInstret;
        {
            iss::CommitLog log(f);
            r = cpu.run(max_instret, &log);
        }
        if (reason != nullptr) {
            *reason = r;
        }
        return read_all(f);
    }

    static std::string read_all(std::FILE* f) {
        std::string text;
        std::rewind(f);
        for (int c; (c = std::fgetc(f)) != EOF;) {
            text.push_back(static_cast<char>(c));
        }
        std::fclose(f);
        return text;
    }
};

// Executes one instruction with x1 = a, x2 = b and returns x3.
uint32_t exec1(uint32_t insn, uint32_t a, uint32_t b = 0) {
    Machine m;
    m.program({insn});
    m.cpu.set_reg(1, a);
    m.cpu.set_reg(2, b);
    CHECK_EQ(m.run(1), StopReason::kMaxInstret);
    return m.cpu.reg(3);
}

// x3 = x1 <op> x2, for OP (0x33) with the given funct7/funct3.
constexpr uint32_t rr(uint32_t f7, uint32_t f3) { return enc::r(f7, 2, 1, f3, 3, 0x33); }
// x3 = x1 <op> imm, for OP-IMM (0x13).
constexpr uint32_t ri(uint32_t f3, int32_t imm) { return enc::i(imm, 1, f3, 3, 0x13); }

constexpr uint32_t kIntMin = 0x80000000u;
constexpr uint32_t kIntMax = 0x7fffffffu;
constexpr uint32_t kNeg1 = 0xffffffffu;

}  // namespace

TEST(encoder_matches_llvm_mc) {
    CHECK_EQ(enc::lui(10, 0x10000), 0x10000537u);
    CHECK_EQ(enc::auipc(5, 0xfffff), 0xfffff297u);
    CHECK_EQ(enc::j(-2048, 1), 0x801ff0efu);
    CHECK_EQ(enc::jalr(11, 12, -1), 0xfff605e7u);
    CHECK_EQ(enc::b(-4096, 11, 10, 0), 0x80b50063u);
    CHECK_EQ(enc::b(4094, 6, 5, 7), 0x7e62ffe3u);
    CHECK_EQ(enc::load(0, 10, 2, -2048), 0x80010503u);
    CHECK_EQ(enc::load(5, 11, 3, 2047), 0x7ff1d583u);
    CHECK_EQ(enc::sw(11, 10, 0), 0x00b52023u);
    CHECK_EQ(enc::store(1, 5, 6, -1), 0xfe531fa3u);
    CHECK_EQ(enc::r(0x20, 12, 11, 0, 10, 0x33), 0x40c58533u);
    CHECK_EQ(enc::csrr(10, 0xc02), 0xc0202573u);
}

TEST(decode_fields_and_immediates) {
    iss::Insn d = iss::decode(0x10000537);  // lui a0, 0x10000
    CHECK_EQ(d.op, Op::kLui);
    CHECK_EQ(d.rd, 10);
    CHECK_EQ(d.imm, 0x10000000u);

    d = iss::decode(0x801ff0ef);  // jal ra, -2048
    CHECK_EQ(d.op, Op::kJal);
    CHECK_EQ(d.rd, 1);
    CHECK_EQ(d.imm, 0xfffff800u);

    d = iss::decode(0x80b50063);  // beq a0, a1, -4096
    CHECK_EQ(d.op, Op::kBeq);
    CHECK_EQ(d.rd, 0);  // branches write no register
    CHECK_EQ(d.rs1, 10);
    CHECK_EQ(d.rs2, 11);
    CHECK_EQ(d.imm, 0xfffff000u);

    d = iss::decode(0x7e62ffe3);  // bgeu t0, t1, 4094
    CHECK_EQ(d.op, Op::kBgeu);
    CHECK_EQ(d.imm, 4094u);

    d = iss::decode(0xfe531fa3);  // sh t0, -1(t1)
    CHECK_EQ(d.op, Op::kSh);
    CHECK_EQ(d.rd, 0);
    CHECK_EQ(d.rs1, 6);
    CHECK_EQ(d.rs2, 5);
    CHECK_EQ(d.imm, kNeg1);

    d = iss::decode(0x7ff1d583);  // lhu a1, 2047(gp)
    CHECK_EQ(d.op, Op::kLhu);
    CHECK_EQ(d.imm, 2047u);

    d = iss::decode(0x4075d513);  // srai a0, a1, 7
    CHECK_EQ(d.op, Op::kSrai);
    CHECK_EQ(d.imm, 7u);

    CHECK_EQ(iss::decode(0x01f59513).op, Op::kSlli);   // slli a0, a1, 31
    CHECK_EQ(iss::decode(0xfff5b513).op, Op::kSltiu);  // sltiu a0, a1, -1
    CHECK_EQ(iss::decode(0x407352b3).op, Op::kSra);
    CHECK_EQ(iss::decode(0x02c5a533).op, Op::kMulhsu);
    CHECK_EQ(iss::decode(0x0324f433).op, Op::kRemu);

    d = iss::decode(0xb82332f3);  // csrrc t0, minstreth, t1
    CHECK_EQ(d.op, Op::kCsr);
    CHECK_EQ(d.rd, 5);
    CHECK_EQ(d.imm, 0xb82u);
    CHECK_EQ(iss::decode(0x340fd073).op, Op::kCsr);  // csrrwi zero, mscratch, 31

    CHECK_EQ(iss::decode(0x0ff0000f).op, Op::kFence);
    CHECK_EQ(iss::decode(0x0000100f).op, Op::kFence);  // fence.i
    CHECK_EQ(iss::decode(enc::kEcall).op, Op::kEcall);
    CHECK_EQ(iss::decode(enc::kEbreak).op, Op::kEbreak);
}

TEST(decode_reserved_encodings_are_illegal) {
    const uint32_t illegal[] = {
        0x00000000,  // all zeros
        0xffffffff,  // all ones
        0x00004501,  // compressed c.li
        0x0000000b,  // custom-0 opcode
        0x02009093,  // slli x1, x1, 32 (shamt[5] set)
        0x40001093,  // slli with funct7 0x20
        0x6000d093,  // srai with funct7 0x30
        0x80000033,  // add with funct7 0x40
        0x40001033,  // funct7 0x20 with funct3 1
        0x00001067,  // jalr with funct3 1
        0x00003003,  // ld
        0x00006003,  // lwu
        0x00003023,  // sd
        0x00002063,  // branch funct3 2
        0x0000200f,  // MISC-MEM funct3 2
        0x30200073,  // mret
        0x10500073,  // wfi
        0x000000f3,  // ecall with rd != 0
        0x00004073,  // SYSTEM funct3 4
    };
    for (uint32_t raw : illegal) {
        const iss::Insn d = iss::decode(raw);
        CHECK_EQ(d.op, Op::kIllegal);
        CHECK_EQ(d.rd, 0);
    }
}

TEST(alu_register_ops) {
    CHECK_EQ(exec1(rr(0x00, 0), kIntMax, 1), kIntMin);       // add wraps
    CHECK_EQ(exec1(rr(0x20, 0), 0, 1), kNeg1);               // sub
    CHECK_EQ(exec1(rr(0x00, 1), 1, 31), kIntMin);            // sll
    CHECK_EQ(exec1(rr(0x00, 2), kNeg1, 1), 1u);              // slt: -1 < 1
    CHECK_EQ(exec1(rr(0x00, 3), kNeg1, 1), 0u);              // sltu: 0xffffffff > 1
    CHECK_EQ(exec1(rr(0x00, 4), 0xff00ff00u, 0x0ff00ff0u), 0xf0f0f0f0u);  // xor
    CHECK_EQ(exec1(rr(0x00, 5), kIntMin, 31), 1u);           // srl
    CHECK_EQ(exec1(rr(0x20, 5), kIntMin, 31), kNeg1);        // sra
    CHECK_EQ(exec1(rr(0x20, 5), 0x40000000u, 30), 1u);       // sra of a positive value
    CHECK_EQ(exec1(rr(0x00, 6), 0xf0u, 0x0fu), 0xffu);       // or
    CHECK_EQ(exec1(rr(0x00, 7), 0xf0u, 0x3cu), 0x30u);       // and
}

TEST(shift_amount_uses_low_five_bits) {
    CHECK_EQ(exec1(rr(0x00, 1), 1, 0xffffffe1u), 2u);        // sll by 33 -> by 1
    CHECK_EQ(exec1(rr(0x00, 5), kIntMin, 0x20u), kIntMin);   // srl by 32 -> by 0
    CHECK_EQ(exec1(rr(0x20, 5), kIntMin, kNeg1), kNeg1);     // sra by -1 -> by 31
}

TEST(alu_immediate_ops) {
    CHECK_EQ(exec1(ri(0, -2048), 0), 0xfffff800u);           // addi sign-extends
    CHECK_EQ(exec1(ri(0, 2047), 1), 2048u);
    CHECK_EQ(exec1(ri(2, -1), 0xfffffffeu), 1u);             // slti: -2 < -1
    CHECK_EQ(exec1(ri(2, -1), 0), 0u);
    CHECK_EQ(exec1(ri(3, -1), 0xfffffffeu), 1u);             // sltiu: imm is 0xffffffff
    CHECK_EQ(exec1(ri(3, -1), kNeg1), 0u);
    CHECK_EQ(exec1(ri(3, 1), 0), 1u);                        // sltiu rd, rs, 1 == seqz
    CHECK_EQ(exec1(ri(4, -1), 0x12345678u), 0xedcba987u);    // xori -1 == not
    CHECK_EQ(exec1(ri(6, -16), 0x5u), 0xfffffff5u);          // ori
    CHECK_EQ(exec1(ri(7, 0x7f0), 0xffffffffu), 0x7f0u);      // andi
    CHECK_EQ(exec1(ri(1, 0), 0x12345678u), 0x12345678u);     // slli by 0
    CHECK_EQ(exec1(ri(1, 31), 3), kIntMin);                  // slli by 31
    CHECK_EQ(exec1(ri(5, 31), kIntMin), 1u);                 // srli by 31
    CHECK_EQ(exec1(ri(5, 0x400 | 31), kIntMin), kNeg1);      // srai by 31
    CHECK_EQ(exec1(ri(5, 0x400 | 0), kIntMin), kIntMin);     // srai by 0
}

TEST(lui_and_auipc) {
    Machine m;
    m.program({enc::kNop, enc::kNop, enc::lui(3, 0xfffff), enc::auipc(4, 0x80000), enc::auipc(5, 0)});
    m.run(5);
    CHECK_EQ(m.cpu.reg(3), 0xfffff000u);
    CHECK_EQ(m.cpu.reg(4), 0x8000000cu);  // pc 0x0c + 0x80000000
    CHECK_EQ(m.cpu.reg(5), 0x10u);
}

TEST(multiply_corner_cases) {
    const uint32_t mul = rr(1, 0), mulh = rr(1, 1), mulhsu = rr(1, 2), mulhu = rr(1, 3);
    CHECK_EQ(exec1(mul, kIntMin, kNeg1), kIntMin);
    CHECK_EQ(exec1(mul, 0x10001u, 0x10001u), 0x00020001u);    // low word of 0x1_0002_0001
    CHECK_EQ(exec1(mul, kNeg1, kNeg1), 1u);
    CHECK_EQ(exec1(mulh, kIntMin, kIntMin), 0x40000000u);     // 2^62
    CHECK_EQ(exec1(mulh, kIntMax, kIntMax), 0x3fffffffu);     // 2^62 - 2^32 + 1
    CHECK_EQ(exec1(mulh, kIntMin, kIntMax), 0xc0000000u);     // -2^62 + 2^31
    CHECK_EQ(exec1(mulh, kNeg1, kNeg1), 0u);                  // +1
    CHECK_EQ(exec1(mulh, kNeg1, 1), kNeg1);                   // -1
    CHECK_EQ(exec1(mulh, 2, kIntMin), kNeg1);                 // -2^32
    CHECK_EQ(exec1(mulh, 0, kIntMin), 0u);
    CHECK_EQ(exec1(mulhsu, kNeg1, kNeg1), kNeg1);             // -1 * (2^32 - 1)
    CHECK_EQ(exec1(mulhsu, kIntMin, kNeg1), kIntMin);         // -2^63 + 2^31
    CHECK_EQ(exec1(mulhsu, 2, kIntMin), 1u);                  // rs2 unsigned: +2^32
    CHECK_EQ(exec1(mulhsu, kIntMax, kNeg1), 0x7ffffffeu);
    CHECK_EQ(exec1(mulhsu, 1, kNeg1), 0u);
    CHECK_EQ(exec1(mulhu, kNeg1, kNeg1), 0xfffffffeu);
    CHECK_EQ(exec1(mulhu, kIntMin, 2), 1u);
    CHECK_EQ(exec1(mulhu, kIntMin, kIntMin), 0x40000000u);
}

TEST(divide_corner_cases) {
    const uint32_t div = rr(1, 4), divu = rr(1, 5), rem = rr(1, 6), remu = rr(1, 7);
    // Division by zero.
    CHECK_EQ(exec1(div, 7, 0), kNeg1);
    CHECK_EQ(exec1(div, kIntMin, 0), kNeg1);
    CHECK_EQ(exec1(divu, 7, 0), kNeg1);
    CHECK_EQ(exec1(rem, 7, 0), 7u);
    CHECK_EQ(exec1(rem, kIntMin, 0), kIntMin);
    CHECK_EQ(exec1(remu, 0xfffffff9u, 0), 0xfffffff9u);
    // Signed overflow.
    CHECK_EQ(exec1(div, kIntMin, kNeg1), kIntMin);
    CHECK_EQ(exec1(rem, kIntMin, kNeg1), 0u);
    CHECK_EQ(exec1(divu, kIntMin, kNeg1), 0u);
    CHECK_EQ(exec1(remu, kIntMin, kNeg1), kIntMin);
    // Truncation toward zero; the remainder takes the dividend's sign.
    CHECK_EQ(exec1(div, 0xfffffff9u, 2), 0xfffffffdu);  // -7 / 2 = -3
    CHECK_EQ(exec1(rem, 0xfffffff9u, 2), kNeg1);        // -7 % 2 = -1
    CHECK_EQ(exec1(div, 7, 0xfffffffeu), 0xfffffffdu);  // 7 / -2 = -3
    CHECK_EQ(exec1(rem, 7, 0xfffffffeu), 1u);           // 7 % -2 = 1
    CHECK_EQ(exec1(div, kIntMin, 1), kIntMin);
    CHECK_EQ(exec1(divu, 0xfffffff9u, 2), 0x7ffffffcu);
    CHECK_EQ(exec1(remu, 0xfffffff9u, 2), 1u);
    CHECK_EQ(exec1(divu, kNeg1, 1), kNeg1);
    CHECK_EQ(exec1(rem, 0xfffffff9u, 0xfffffff9u), 0u);
}

TEST(branches_signed_and_unsigned) {
    struct Case {
        uint32_t funct3, a, b;
        bool taken;
    };
    const Case cases[] = {
        {0, 5, 5, true},        {0, 5, 6, false},       // beq
        {1, 5, 6, true},        {1, 5, 5, false},       // bne
        {4, kNeg1, 1, true},    {4, 1, kNeg1, false},   // blt
        {4, 3, 3, false},
        {5, 1, kNeg1, true},    {5, 3, 3, true},        // bge
        {5, kIntMin, 0, false},
        {6, 1, kNeg1, true},    {6, kNeg1, 1, false},   // bltu
        {7, kNeg1, 1, true},    {7, 0, 0, true},        // bgeu
        {7, 0, 1, false},
    };
    for (const Case& c : cases) {
        Machine m;
        // Taken: skip the addi at 0x4 and land at 0x8.
        m.program({enc::b(8, 2, 1, c.funct3), enc::addi(3, 0, 1), enc::addi(4, 0, 1)});
        m.cpu.set_reg(1, c.a);
        m.cpu.set_reg(2, c.b);
        m.run(2);
        CHECK_EQ(m.cpu.reg(3), c.taken ? 0u : 1u);
        CHECK_EQ(m.cpu.pc(), c.taken ? 0xcu : 0x8u);
    }
}

TEST(jumps_and_links) {
    Machine m;
    m.program({
        enc::j(12, 1),          // 00: jal ra, 0x0c
        enc::kNop,              // 04
        enc::kNop,              // 08
        enc::addi(5, 0, 0x21),  // 0c
        enc::jalr(5, 5, -1),    // 10: rd == rs1: target from the old x5 = 0x20
        enc::kNop,              // 14
        enc::kNop,              // 18
        enc::kNop,              // 1c
        enc::jalr(6, 5, 13),    // 20: 0x14 + 13 = 0x21, bit 0 cleared -> 0x20
    });
    m.run(3);
    CHECK_EQ(m.cpu.reg(1), 4u);
    CHECK_EQ(m.cpu.reg(5), 0x14u);
    CHECK_EQ(m.cpu.pc(), 0x20u);
    m.run(4);
    CHECK_EQ(m.cpu.reg(6), 0x24u);
    CHECK_EQ(m.cpu.pc(), 0x20u);
}

TEST(loads_extend_and_select_lanes) {
    Machine m;
    m.dmem(0x10000100, {0x80, 0x01, 0xff, 0x7f});
    m.program({
        enc::lui(1, 0x10000),
        enc::load(0, 3, 1, 0x100),   // lb   0x80
        enc::load(4, 4, 1, 0x100),   // lbu  0x80
        enc::load(0, 5, 1, 0x103),   // lb   0x7f
        enc::load(1, 6, 1, 0x102),   // lh   0x7fff
        enc::load(1, 7, 1, 0x100),   // lh   0x0180
        enc::load(5, 8, 1, 0x102),   // lhu  0x7fff
        enc::load(1, 9, 1, 0x101),   // lh at offset 1: misaligned
        enc::load(2, 10, 1, 0x100),  // lw
        enc::load(0, 11, 1, 0x102),  // lb   0xff
        enc::load(4, 12, 1, 0x102),  // lbu  0xff
    });
    m.run(11);
    CHECK_EQ(m.cpu.reg(3), 0xffffff80u);
    CHECK_EQ(m.cpu.reg(4), 0x80u);
    CHECK_EQ(m.cpu.reg(5), 0x7fu);
    CHECK_EQ(m.cpu.reg(6), 0x7fffu);
    CHECK_EQ(m.cpu.reg(7), 0x0180u);
    CHECK_EQ(m.cpu.reg(8), 0x7fffu);
    CHECK_EQ(m.cpu.reg(10), 0x7fff0180u);
    CHECK_EQ(m.cpu.reg(11), kNeg1);
    CHECK_EQ(m.cpu.reg(12), 0xffu);
    CHECK_EQ(m.cpu.misaligned_accesses(), 1u);
}

TEST(stores_write_only_their_lanes) {
    Machine m;
    m.dmem(0x10000000, {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88});
    m.program({
        enc::lui(1, 0x10000),
        enc::addi(2, 0, -1),
        enc::store(0, 2, 1, 1),  // sb 0xff at lane 1
        enc::store(1, 2, 1, 6),  // sh 0xffff at lanes 2-3 of word 1
        enc::lw(3, 1, 0),
        enc::lw(4, 1, 4),
    });
    m.run(6);
    CHECK_EQ(m.cpu.reg(3), 0x4433ff11u);
    CHECK_EQ(m.cpu.reg(4), 0xffff6655u);
    CHECK_EQ(m.mem.read8(0x10000000), 0x11u);
    CHECK_EQ(m.mem.read8(0x10000002), 0x33u);
}

TEST(commit_log_matches_spec_format) {
    Machine m;
    m.program({
        0x01700593,  // 00: li a1, 0x17
        enc::kNop,   // 04
        enc::kNop,   // 08
        enc::kNop,   // 0c
        0x10000537,  // 10: lui a0, 0x10000      (spec example line 1)
        enc::kNop,   // 14
        0x00b52023,  // 18: sw a1, 0(a0)         (spec example line 2)
        0x00b51123,  // 1c: sh a1, 2(a0)
        0x00b501a3,  // 20: sb a1, 3(a0)
        0x00052603,  // 24: lw a2, 0(a0)
        0x200002b7,  // 28: lui t0, 0x20000
        0x0002a823,  // 2c: sw zero, 16(t0)      SIM_EXIT
        enc::kNop,   // 30: never reached
    });
    StopReason reason = StopReason::kMaxInstret;
    const std::string log = m.run_logged(100, &reason);
    CHECK_EQ(reason, StopReason::kExit);
    CHECK_STR_EQ(log,
                 "00000000 01700593 11 00000017\n"
                 "00000004 00000013 00 00000000\n"
                 "00000008 00000013 00 00000000\n"
                 "0000000c 00000013 00 00000000\n"
                 "00000010 10000537 10 10000000\n"
                 "00000014 00000013 00 00000000\n"
                 "00000018 00b52023 00 00000000 M 10000000 f 00000017\n"
                 "0000001c 00b51123 00 00000000 M 10000000 c 00170000\n"
                 "00000020 00b501a3 00 00000000 M 10000000 8 17000000\n"
                 "00000024 00052603 12 17170017 M 10000000 0 00000000\n"
                 "00000028 200002b7 05 20000000\n"
                 "0000002c 0002a823 00 00000000 M 20000010 f 00000000\n");
    CHECK_EQ(m.cpu.instret(), 12u);
}

TEST(commit_log_records_load_addresses) {
    Machine m;
    m.dmem(0x1003fffc, {0x11, 0x22, 0x33, 0x84});  // last word of DMEM
    m.program({
        0x200002b7,  // 00: lui t0, 0x20000
        0x0042a503,  // 04: lw a0, 4(t0)    UART_STATUS
        0x00002003,  // 08: lw zero, 0(zero) IMEM reads 0, rd x0
        0x10040337,  // 0c: lui t1, 0x10040 just past DMEM
        0x00334583,  // 10: lbu a1, 3(t1)   unbacked, reads 0
        0xffe31603,  // 14: lh a2, -2(t1)   lane 2 of the last DMEM word
        0x0202a683,  // 18: lw a3, 32(t0)   LEDS
    });
    const std::string log = m.run_logged(7);
    CHECK_STR_EQ(log,
                 "00000000 200002b7 05 20000000\n"
                 "00000004 0042a503 10 00000001 M 20000004 0 00000000\n"
                 "00000008 00002003 00 00000000 M 00000000 0 00000000\n"
                 "0000000c 10040337 06 10040000\n"
                 "00000010 00334583 11 00000000 M 10040000 0 00000000\n"
                 "00000014 ffe31603 12 ffff8433 M 1003fffc 0 00000000\n"
                 "00000018 0202a683 13 00000000 M 20000020 0 00000000\n");
}

TEST(writes_to_x0_are_dropped_and_logged_as_none) {
    Machine m;
    m.program({enc::addi(0, 0, 5), enc::addi(1, 0, 0), enc::j(4, 0), enc::lui(0, 1)});
    const std::string log = m.run_logged(4);
    CHECK_EQ(m.cpu.reg(0), 0u);
    CHECK_EQ(m.cpu.reg(1), 0u);
    CHECK_STR_EQ(log,
                 "00000000 00500013 00 00000000\n"
                 "00000004 00000093 01 00000000\n"
                 "00000008 0040006f 00 00000000\n"
                 "0000000c 00001037 00 00000000\n");
}

TEST(csr_counters_read_instret_before_retirement) {
    Machine m;
    m.program({
        enc::kNop,                    // 0
        enc::kNop,                    // 1
        enc::csrr(1, 0xc02),          // 2: instret
        enc::csrr(2, 0xc82),          // 3: instreth
        enc::csrr(3, 0xc00),          // 4: cycle (= instret on the ISS)
        enc::csrrw(4, 0xb02, 1),      // 5: minstret, the write is ignored
        enc::csrr(5, 0xc02),          // 6: instret
        enc::csrr(6, 0x340),          // 7: mscratch reads 0
        enc::csrrw(7, 0x340, 1),      // 8: still 0 after a write
        enc::csrr(8, 0xf14),          // 9: mhartid reads 0
        enc::csrr(9, 0xc01),          // 10: time
        enc::csrr(0, 0xc02),          // 11: rd = x0
    });
    m.cpu.set_reg(6, 0x1234);
    m.run(12);
    CHECK_EQ(m.cpu.reg(1), 2u);
    CHECK_EQ(m.cpu.reg(2), 0u);
    CHECK_EQ(m.cpu.reg(3), 4u);
    CHECK_EQ(m.cpu.reg(4), 5u);
    CHECK_EQ(m.cpu.reg(5), 6u);
    CHECK_EQ(m.cpu.reg(6), 0u);
    CHECK_EQ(m.cpu.reg(7), 0u);
    CHECK_EQ(m.cpu.reg(8), 0u);
    CHECK_EQ(m.cpu.reg(9), 10u);
    CHECK_EQ(m.cpu.instret(), 12u);
}

TEST(fence_is_a_nop) {
    Machine m;
    m.program({0x0ff0000f, 0x0000100f, enc::addi(1, 0, 1)});
    const std::string log = m.run_logged(3);
    CHECK_EQ(m.cpu.reg(1), 1u);
    CHECK_STR_EQ(log,
                 "00000000 0ff0000f 00 00000000\n"
                 "00000004 0000100f 00 00000000\n"
                 "00000008 00100093 01 00000001\n");
}

TEST(halting_instructions) {
    struct Case {
        uint32_t insn;
        StopReason reason;
    };
    const Case cases[] = {
        {enc::kEcall, StopReason::kEcall},
        {enc::kEbreak, StopReason::kEbreak},
        {0x00000000, StopReason::kIllegal},
        {0x30200073, StopReason::kIllegal},  // mret
    };
    for (const Case& c : cases) {
        Machine m;
        m.program({enc::addi(1, 0, 1), c.insn, enc::addi(2, 0, 1)});
        StopReason reason = StopReason::kMaxInstret;
        const std::string log = m.run_logged(100, &reason);
        CHECK_EQ(reason, c.reason);
        CHECK_EQ(m.cpu.instret(), 2u);  // the halting instruction retires
        CHECK_EQ(m.cpu.pc(), 4u);       // and pc stays on it
        CHECK_EQ(m.cpu.reg(2), 0u);
        char line[64];
        std::snprintf(line, sizeof line, "00000004 %08x 00 00000000\n", c.insn);
        CHECK_STR_EQ(log.substr(log.find('\n') + 1), std::string(line));
    }
}

TEST(fetch_outside_imem_is_illegal) {
    Machine m;
    m.program({enc::lui(1, 0x10), enc::jalr(0, 1, 0)});  // jump to 0x10000
    CHECK_EQ(m.run(), StopReason::kIllegal);
    CHECK_EQ(m.cpu.pc(), 0x10000u);
    CHECK_EQ(m.cpu.insn_at(0x10000), 0u);

    Machine misaligned;
    misaligned.program({enc::addi(1, 0, 6), enc::jalr(0, 1, 0)});  // jump to 0x6
    CHECK_EQ(misaligned.run(), StopReason::kIllegal);
    CHECK_EQ(misaligned.cpu.pc(), 6u);
}

TEST(sim_exit_stops_after_the_store) {
    Machine m;
    m.program({enc::lui(1, 0x20000), enc::addi(2, 0, 0x55), enc::sw(2, 1, 0x10), enc::addi(3, 0, 1)});
    CHECK_EQ(m.run(), StopReason::kExit);
    CHECK_EQ(m.soc.exit_value(), 0x55u);
    CHECK_EQ(m.cpu.instret(), 3u);
    CHECK_EQ(m.cpu.reg(3), 0u);
}

TEST(max_instret_limit) {
    Machine m;
    m.program({enc::addi(1, 1, 1), enc::j(-4, 0)});
    CHECK_EQ(m.run(1001), StopReason::kMaxInstret);
    CHECK_EQ(m.cpu.instret(), 1001u);
    CHECK_EQ(m.cpu.reg(1), 501u);
    CHECK_EQ(m.run(1001), StopReason::kMaxInstret);  // the limit is a total
    CHECK_EQ(m.cpu.instret(), 1001u);
}

TEST(memory_map_unbacked_addresses) {
    Machine m;
    m.dmem(0x10000000, {0x78, 0x56, 0x34, 0x12});
    m.program({
        enc::lw(3, 0, 0),             // 00: load from IMEM reads 0
        enc::sw(1, 0, 0x24),          // 04: store to IMEM is dropped
        enc::lui(4, 0x10040),         // 08: x4 = 0x1004_0000, just past DMEM
        enc::sw(1, 4, 0),             // 0c: dropped, does not alias DMEM base
        enc::lw(5, 4, 0),             // 10: reads 0
        enc::lui(6, 0x10000),         // 14
        enc::lw(7, 6, 0),             // 18: DMEM base unchanged
        enc::lui(8, 0x40000),         // 1c: unmapped region 4
        enc::lw(9, 8, 0),             // 20
        enc::addi(10, 0, 7),          // 24: target of the dropped store
    });
    m.cpu.set_reg(1, 0xdeadbeef);
    m.cpu.set_reg(9, 0x1111);
    m.run(10);
    CHECK_EQ(m.cpu.reg(3), 0u);
    CHECK_EQ(m.cpu.reg(5), 0u);
    CHECK_EQ(m.cpu.reg(7), 0x12345678u);
    CHECK_EQ(m.cpu.reg(9), 0u);
    CHECK_EQ(m.cpu.reg(10), 7u);
    CHECK_EQ(m.mem.imem_word(0x24), enc::addi(10, 0, 7));
}

TEST(memory_map_mmio_registers) {
    Machine m;
    iss::Soc& soc = m.soc;
    CHECK_EQ(soc.load(UART_STATUS), 1u);
    CHECK_EQ(soc.load(UART_TX), 0u);
    CHECK_EQ(soc.load(SIM_EXIT), 0u);
    CHECK_EQ(soc.load(MMIO_BASE + 0x8), 0u);
    soc.store(LEDS, 0x12345678u, 0xf);
    CHECK_EQ(soc.load(LEDS), 0x78u);
    CHECK_EQ(soc.leds(), 0x78u);
    CHECK_EQ(soc.load(LEDS + 0x1000), 0u);         // past the 4 KiB window
    CHECK_EQ(soc.load(UART_STATUS + 0x1000), 0u);
    soc.store(SIM_EXIT + 0x1000, 1, 0xf);          // no alias of SIM_EXIT
    CHECK(!soc.exit_requested());
    CHECK_EQ(soc.load(NPU_INFO), 0x01040008u);
    CHECK_EQ(soc.load(NPU_INFO + 0x1000), 0u);
    CHECK_EQ(soc.load(NPU_BASE + 0x40), 0u);
    CHECK_EQ(soc.load(0x10040000), 0u);
    CHECK_EQ(soc.load(0xfffffffc), 0u);
}

TEST(uart_bytes_reach_the_output_stream) {
    std::FILE* out = std::tmpfile();
    {
        Machine m(out);
        m.program({
            enc::lui(1, 0x20000),
            enc::addi(2, 0, 0x248),    // low byte 0x48 'H'
            enc::sw(2, 1, 0),
            enc::addi(2, 0, 0x69),     // 'i'
            enc::sw(2, 1, 0),
            enc::sw(0, 1, 0),          // a 0 byte is transmitted too
            enc::lw(3, 1, 4),          // UART_STATUS
        });
        m.run(7);
        CHECK_EQ(m.cpu.reg(3), 1u);
    }
    CHECK_STR_EQ(Machine::read_all(out), std::string("Hi\0", 3));
}

namespace {

void put16(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    v[at] = static_cast<uint8_t>(x);
    v[at + 1] = static_cast<uint8_t>(x >> 8);
}

void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    put16(v, at, x);
    put16(v, at + 2, x >> 16);
}

// A minimal ELF32 executable with one segment per entry of `segs`
// ({paddr, filesz, memsz}); segment i's file bytes are all 0xa0 + i.
std::vector<uint8_t> make_elf(const std::vector<std::vector<uint32_t>>& segs, uint16_t machine = 243) {
    const size_t phoff = 52;
    size_t data = phoff + 32 * segs.size();
    std::vector<uint8_t> elf(data, 0);
    const uint8_t ident[] = {0x7f, 'E', 'L', 'F', 1, 1, 1};
    std::copy(std::begin(ident), std::end(ident), elf.begin());
    put16(elf, 16, 2);  // ET_EXEC
    put16(elf, 18, machine);
    put32(elf, 24, 0);  // entry
    put32(elf, 28, static_cast<uint32_t>(phoff));
    put16(elf, 42, 32);
    put16(elf, 44, static_cast<uint16_t>(segs.size()));
    for (size_t i = 0; i < segs.size(); i++) {
        const size_t ph = phoff + 32 * i;
        put32(elf, ph, 1);  // PT_LOAD
        put32(elf, ph + 4, static_cast<uint32_t>(data));
        put32(elf, ph + 8, segs[i][0]);
        put32(elf, ph + 12, segs[i][0]);
        put32(elf, ph + 16, segs[i][1]);
        put32(elf, ph + 20, segs[i][2]);
        elf.resize(data + segs[i][1], static_cast<uint8_t>(0xa0 + i));
        data += segs[i][1];
    }
    return elf;
}

}  // namespace

TEST(elf_loader_places_segments_by_physical_address) {
    iss::Memory mem;
    mem.write32(0x10000104, 0xffffffffu, 0xf);  // must be zero-filled by memsz
    iss::ElfInfo info;
    std::string error;
    CHECK(iss::load_elf_image(make_elf({{0x0, 8, 8}, {0x10000100, 4, 12}}), mem, info, error));
    CHECK_STR_EQ(error, "");
    CHECK_EQ(mem.imem_word(0x4), 0xa0a0a0a0u);
    CHECK_EQ(mem.read32(0x10000100), 0xa1a1a1a1u);
    CHECK_EQ(mem.read32(0x10000104), 0u);
    CHECK_EQ(mem.read32(0x10000108), 0u);
}

TEST(elf_loader_rejects_bad_files) {
    iss::Memory mem;
    iss::ElfInfo info;
    std::string error;
    CHECK(!iss::load_elf_image(std::vector<uint8_t>(100, 0), mem, info, error));
    CHECK(!iss::load_elf_image(make_elf({{0x0, 4, 4}}, 62), mem, info, error));  // x86-64
    CHECK(!iss::load_elf_image(make_elf({{0xfffc, 8, 8}}), mem, info, error));   // crosses IMEM end
    CHECK(!iss::load_elf_image(make_elf({{0x20000000, 4, 4}}), mem, info, error));
    CHECK(!iss::load_elf_image(make_elf({{0x1003fffc, 4, 8}}), mem, info, error));  // bss past DMEM
    CHECK(!iss::load_elf_image(make_elf({{0x0, 8, 4}}), mem, info, error));         // filesz > memsz
    std::vector<uint8_t> truncated = make_elf({{0x0, 8, 8}});
    truncated.resize(truncated.size() - 1);
    CHECK(!iss::load_elf_image(truncated, mem, info, error));
}
