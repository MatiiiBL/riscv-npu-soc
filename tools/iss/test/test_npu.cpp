// Unit tests for the NPU functional model (docs/spec.md section 5.1). Every
// expected value is worked out by hand in the comments.
#include <cstdint>
#include <vector>

#include "check.h"
#include "memory.h"
#include "npu.h"
#include "soc.h"

namespace {

constexpr uint32_t kA = 0x10001000;     // A matrix
constexpr uint32_t kB = 0x10002000;     // B matrix
constexpr uint32_t kC = 0x10003000;     // C matrix
constexpr uint32_t kBias = 0x10004000;  // bias vector

constexpr uint32_t kDone = NPU_STATUS_DONE;
constexpr uint32_t kDoneErr = NPU_STATUS_DONE | NPU_STATUS_ERR;

struct Bench {
    iss::Memory mem;
    iss::Npu npu;

    explicit Bench(unsigned arr = iss::Npu::kDefaultArr, unsigned kmax = iss::Npu::kDefaultKmax)
        : npu(mem, arr, kmax) {}

    void set(uint32_t reg, uint32_t value) { npu.write(reg - NPU_BASE, value); }
    uint32_t get(uint32_t reg) const { return npu.read(reg - NPU_BASE); }

    void put8(uint32_t addr, const std::vector<int>& values) {
        for (size_t i = 0; i < values.size(); i++) {
            mem.write8(addr + static_cast<uint32_t>(i), static_cast<uint8_t>(values[i]));
        }
    }
    void put32(uint32_t addr, const std::vector<uint32_t>& values) {
        for (size_t i = 0; i < values.size(); i++) {
            mem.write32(addr + 4 * static_cast<uint32_t>(i), values[i], 0xf);
        }
    }
    void fill(uint32_t addr, uint32_t bytes, uint8_t value) {
        for (uint32_t i = 0; i < bytes; i++) {
            mem.write8(addr + i, value);
        }
    }
    int32_t c8(uint32_t addr) const { return static_cast<int8_t>(mem.read8(addr)); }
    int32_t c32(uint32_t addr) const { return static_cast<int32_t>(mem.read32(addr)); }

    // M x N x K operation on the fixed buffers with packed rows.
    void shape(uint32_t m, uint32_t n, uint32_t k, uint32_t c_stride) {
        set(NPU_A_ADDR, kA);
        set(NPU_B_ADDR, kB);
        set(NPU_C_ADDR, kC);
        set(NPU_M, m);
        set(NPU_N, n);
        set(NPU_K, k);
        set(NPU_A_STRIDE, (k + 3) & ~3u);
        set(NPU_B_STRIDE, (k + 3) & ~3u);
        set(NPU_C_STRIDE, c_stride);
    }

    uint32_t start(uint32_t flags = 0) {
        set(NPU_CTRL, NPU_CTRL_START | flags);
        return get(NPU_STATUS);
    }

    // C[0][n] = targets[n] for a 1 x N x 1 product: A = [1], every B row =
    // [1], so acc = 1 and bias[n] = targets[n] - 1 (wrapping).
    uint32_t outputs(const std::vector<uint32_t>& targets, uint32_t quant, uint32_t qmult) {
        const auto n = static_cast<uint32_t>(targets.size());
        shape(1, n, 1, 4 * n);
        set(NPU_B_STRIDE, 4);
        put8(kA, {1});
        std::vector<uint32_t> bias;
        for (uint32_t i = 0; i < n; i++) {
            put8(kB + 4 * i, {1});
            bias.push_back(targets[i] - 1);
        }
        put32(kBias, bias);
        set(NPU_BIAS_ADDR, kBias);
        set(NPU_QUANT, quant);
        set(NPU_QMULT, qmult);
        return start();
    }
};

constexpr uint32_t u32(int32_t v) { return static_cast<uint32_t>(v); }

}  // namespace

TEST(npu_reset_values_and_info) {
    Bench t;
    CHECK_EQ(t.get(NPU_STATUS), 0u);
    CHECK_EQ(t.get(NPU_QMULT), 1u);
    CHECK_EQ(t.get(NPU_M), 0u);
    CHECK_EQ(t.get(NPU_QUANT), 0u);
    CHECK_EQ(t.get(NPU_BIAS_ADDR), 0u);
    CHECK_EQ(t.get(NPU_INFO), 0x01040008u);  // version 1, KMAX 1024, ARR 8
    CHECK_EQ(t.get(NPU_CYCLES), 0u);
    CHECK_EQ(t.get(NPU_CTRL), 0u);           // write-only
    CHECK_EQ(t.get(NPU_BASE + 0x40), 0u);

    Bench small(4, 16);
    CHECK_EQ(small.get(NPU_INFO), 0x01001004u);
}

TEST(npu_registers_read_back_full_width) {
    Bench t;
    t.set(NPU_M, 0x12345678);
    t.set(NPU_QUANT, 0xffffffff);
    t.set(NPU_QMULT, 0xabcd8000);
    t.set(NPU_INFO, 0);    // read-only
    t.set(NPU_STATUS, 7);  // read-only
    t.set(NPU_CTRL, NPU_CTRL_ACC_KEEP);  // no START: nothing happens
    CHECK_EQ(t.get(NPU_M), 0x12345678u);
    CHECK_EQ(t.get(NPU_QUANT), 0xffffffffu);
    CHECK_EQ(t.get(NPU_QMULT), 0xabcd8000u);
    CHECK_EQ(t.get(NPU_INFO), 0x01040008u);
    CHECK_EQ(t.get(NPU_STATUS), 0u);
}

TEST(npu_int32_matmul) {
    Bench t;
    // A = [ 1  2  3]   B = [ 4  5  6]   C = A * B^T = [ 32 -18]
    //     [-1 -2 -3]       [-7  8 -9]                 [-32  18]
    t.put8(kA, {1, 2, 3, 0, -1, -2, -3, 0});
    t.put8(kB, {4, 5, 6, 0, -7, 8, -9, 0});
    t.fill(kC, 64, 0xee);
    t.shape(2, 2, 3, 16);
    CHECK_EQ(t.start(), kDone);
    CHECK_EQ(t.c32(kC + 0), 32);
    CHECK_EQ(t.c32(kC + 4), -18);
    CHECK_EQ(t.c32(kC + 16), -32);
    CHECK_EQ(t.c32(kC + 20), 18);
    CHECK_EQ(t.mem.read32(kC + 8), 0xeeeeeeeeu);   // past N
    CHECK_EQ(t.mem.read32(kC + 12), 0xeeeeeeeeu);  // row padding
    CHECK_EQ(t.mem.read32(kC + 32), 0xeeeeeeeeu);  // past M
    CHECK_EQ(t.get(NPU_CYCLES), 0u);
}

TEST(npu_int32_bias_and_relu) {
    Bench t;
    t.put8(kA, {1, 2, 3, 0, -1, -2, -3, 0});
    t.put8(kB, {4, 5, 6, 0, -7, 8, -9, 0});
    t.put32(kBias, {100, u32(-200)});
    t.shape(2, 2, 3, 8);
    t.set(NPU_BIAS_ADDR, kBias);
    CHECK_EQ(t.start(), kDone);
    CHECK_EQ(t.c32(kC + 0), 132);
    CHECK_EQ(t.c32(kC + 4), -218);
    CHECK_EQ(t.c32(kC + 8), 68);
    CHECK_EQ(t.c32(kC + 12), -182);

    t.set(NPU_QUANT, NPU_QUANT_RELU);
    CHECK_EQ(t.start(), kDone);
    CHECK_EQ(t.c32(kC + 0), 132);
    CHECK_EQ(t.c32(kC + 4), 0);
    CHECK_EQ(t.c32(kC + 8), 68);
    CHECK_EQ(t.c32(kC + 12), 0);
}

TEST(npu_int32_wraps) {
    Bench t;
    // acc = 1, bias = INT_MAX: the sum wraps to INT_MIN.
    CHECK_EQ(t.outputs({0x80000000u}, 0, 1), kDone);
    CHECK_EQ(t.mem.read32(kC), 0x80000000u);
}

TEST(npu_strides_skip_row_padding) {
    Bench t;
    // K = 5 with A rows 8 bytes apart and B rows 12 bytes apart. The bytes
    // past K in each row hold 0x7f and must not contribute.
    t.fill(kA, 64, 0x7f);
    t.fill(kB, 64, 0x7f);
    t.put8(kA, {1, 1, 1, 1, 1});
    t.put8(kA + 8, {1, 2, 3, 4, 5});
    t.put8(kB, {1, 0, 0, 0, -1});
    t.put8(kB + 12, {2, 2, 2, 2, 2});
    t.shape(2, 2, 5, 8);
    t.set(NPU_A_STRIDE, 8);
    t.set(NPU_B_STRIDE, 12);
    CHECK_EQ(t.start(), kDone);
    CHECK_EQ(t.c32(kC + 0), 0);    // 1 - 1
    CHECK_EQ(t.c32(kC + 4), 10);   // 5 * 2
    CHECK_EQ(t.c32(kC + 8), -4);   // 1 - 5
    CHECK_EQ(t.c32(kC + 12), 30);  // (1+2+3+4+5) * 2
}

TEST(npu_int8_rounds_half_up) {
    Bench t;
    // SHIFT = 1: (v + 1) >> 1, an arithmetic shift, so negative halves round
    // toward +inf: -1.5 -> -1, -1.0 -> -1, -0.5 -> 0.
    CHECK_EQ(t.outputs({u32(-3), u32(-2), u32(-1), 0, 1, 2, 3, 5}, NPU_QUANT_OUT_INT8 | 1, 1), kDone);
    const int32_t shift1[] = {-1, -1, 0, 0, 1, 1, 2, 3};
    for (uint32_t n = 0; n < 8; n++) {
        CHECK_EQ(t.c8(kC + n), shift1[n]);
    }
    // SHIFT = 2: -6/4 = -1.5 -> -1, -5/4 -> -1, -3/4 -> -1, -2/4 -> 0,
    // 2/4 -> 1, 3/4 -> 1, 5/4 -> 1, 6/4 -> 2.
    CHECK_EQ(t.outputs({u32(-6), u32(-5), u32(-3), u32(-2), 2, 3, 5, 6}, NPU_QUANT_OUT_INT8 | 2, 1), kDone);
    const int32_t shift2[] = {-1, -1, -1, 0, 1, 1, 1, 2};
    for (uint32_t n = 0; n < 8; n++) {
        CHECK_EQ(t.c8(kC + n), shift2[n]);
    }
}

TEST(npu_int8_qmult_and_saturation) {
    Bench t;
    // SHIFT = 0: p = v * QMULT, no rounding.
    CHECK_EQ(t.outputs({42, 43, u32(-42), u32(-43)}, NPU_QUANT_OUT_INT8, 3), kDone);
    CHECK_EQ(t.c8(kC + 0), 126);
    CHECK_EQ(t.c8(kC + 1), 127);   // 129 saturates
    CHECK_EQ(t.c8(kC + 2), -126);
    CHECK_EQ(t.c8(kC + 3), -128);  // -129 saturates
    // Only QMULT[15:0] counts, as a signed value: 0xfffe = -2, 0x8000 = -32768.
    CHECK_EQ(t.outputs({10, 100, 1, u32(-1)}, NPU_QUANT_OUT_INT8, 0x1234fffe), kDone);
    CHECK_EQ(t.c8(kC + 0), -20);
    CHECK_EQ(t.c8(kC + 1), -128);
    CHECK_EQ(t.c8(kC + 2), -2);
    CHECK_EQ(t.c8(kC + 3), 2);
    CHECK_EQ(t.outputs({1, u32(-1)}, NPU_QUANT_OUT_INT8, 0x8000), kDone);
    CHECK_EQ(t.c8(kC + 0), -128);
    CHECK_EQ(t.c8(kC + 1), 127);
    // QMULT = 0 zeroes everything.
    CHECK_EQ(t.outputs({0x7fffffff, 0x80000000u}, NPU_QUANT_OUT_INT8, 0), kDone);
    CHECK_EQ(t.c8(kC + 0), 0);
    CHECK_EQ(t.c8(kC + 1), 0);
}

TEST(npu_int8_shift31_uses_48_bit_product) {
    Bench t;
    // QMULT = 1, SHIFT = 31: (v + 2^30) >> 31.
    CHECK_EQ(t.outputs({0x80000000u, 0x40000000u, 0x3fffffffu, 0xc0000000u, 0xbfffffffu}, NPU_QUANT_OUT_INT8 | 31, 1),
             kDone);
    CHECK_EQ(t.c8(kC + 0), -1);  // -2^31 / 2^31 = -1.0
    CHECK_EQ(t.c8(kC + 1), 1);   // 2^30 / 2^31 = 0.5 -> 1
    CHECK_EQ(t.c8(kC + 2), 0);   // just under 0.5 -> 0
    CHECK_EQ(t.c8(kC + 3), 0);   // -0.5 -> 0
    CHECK_EQ(t.c8(kC + 4), -1);  // just under -0.5 -> -1
    // Products wider than 32 bits: INT_MAX * 32767 / 2^31 = 32767.49... and
    // INT_MIN * -32768 = 2^46 -> 32768.5 -> 32768, both saturate to 127;
    // INT_MIN * 32767 / 2^31 = -32767 -> -128.
    CHECK_EQ(t.outputs({0x7fffffff}, NPU_QUANT_OUT_INT8 | 31, 0x7fff), kDone);
    CHECK_EQ(t.c8(kC), 127);
    CHECK_EQ(t.outputs({0x80000000u}, NPU_QUANT_OUT_INT8 | 31, 0x8000), kDone);
    CHECK_EQ(t.c8(kC), 127);
    CHECK_EQ(t.outputs({0x80000000u}, NPU_QUANT_OUT_INT8 | 31, 0x7fff), kDone);
    CHECK_EQ(t.c8(kC), -128);
    // A 48-bit product that lands back in range: 0x40000000 * 255 / 2^31 =
    // 127.5 -> 128 -> 127, and 0x3f000000 * 255 / 2^31 = 125.5 -> 126.
    CHECK_EQ(t.outputs({0x40000000u, 0x3f000000u}, NPU_QUANT_OUT_INT8 | 31, 255), kDone);
    CHECK_EQ(t.c8(kC + 0), 127);
    CHECK_EQ(t.c8(kC + 1), 126);
}

TEST(npu_int8_relu_after_rounding) {
    Bench t;
    // ReLU clamps after the shift: -3 -> -1 -> 0, -1 -> 0 -> 0, 3 -> 2.
    CHECK_EQ(t.outputs({u32(-3), u32(-1), 3, u32(-100000)}, NPU_QUANT_OUT_INT8 | NPU_QUANT_RELU | 1, 1), kDone);
    CHECK_EQ(t.c8(kC + 0), 0);
    CHECK_EQ(t.c8(kC + 1), 0);
    CHECK_EQ(t.c8(kC + 2), 2);
    CHECK_EQ(t.c8(kC + 3), 0);
}

TEST(npu_int8_writes_only_its_bytes) {
    Bench t;
    // 2 x 3 INT8 output with C rows 8 bytes apart: exactly bytes 0..2 and
    // 8..10 change, so partial words use byte enables.
    t.put8(kA, {1, 0, 0, 0, 2, 0, 0, 0});
    t.put8(kB, {1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0});
    t.fill(kC, 32, 0xaa);
    t.shape(2, 3, 1, 8);
    t.set(NPU_QUANT, NPU_QUANT_OUT_INT8);
    CHECK_EQ(t.start(), kDone);
    CHECK_EQ(t.mem.read32(kC + 0), 0xaa030201u);
    CHECK_EQ(t.mem.read32(kC + 4), 0xaaaaaaaau);
    CHECK_EQ(t.mem.read32(kC + 8), 0xaa060402u);
    CHECK_EQ(t.mem.read32(kC + 12), 0xaaaaaaaau);
    CHECK_EQ(t.mem.read32(kC + 16), 0xaaaaaaaau);
}

TEST(npu_acc_keep_and_no_wb) {
    Bench t;
    t.fill(kC, 64, 0x55);
    // First half of K: A = [[1, 2], [3, 4]], B = [[1, 1], [1, -1]].
    t.put8(kA, {1, 2, 0, 0, 3, 4, 0, 0});
    t.put8(kB, {1, 1, 0, 0, 1, -1, 0, 0});
    t.shape(2, 2, 2, 8);
    CHECK_EQ(t.start(NPU_CTRL_NO_WB), kDone);
    CHECK_EQ(t.mem.read32(kC), 0x55555555u);  // NO_WB writes nothing
    CHECK_EQ(t.npu.acc(0, 0), 3u);
    CHECK_EQ(t.npu.acc(0, 1), u32(-1));
    CHECK_EQ(t.npu.acc(1, 0), 7u);
    CHECK_EQ(t.npu.acc(1, 1), u32(-1));

    // Second half: A = [[1], [1]], B = [[10], [20]], added to the first.
    t.put8(kA, {1, 0, 0, 0, 1, 0, 0, 0});
    t.put8(kB, {10, 0, 0, 0, 20, 0, 0, 0});
    t.shape(2, 2, 1, 8);
    CHECK_EQ(t.start(NPU_CTRL_ACC_KEEP), kDone);
    CHECK_EQ(t.c32(kC + 0), 13);
    CHECK_EQ(t.c32(kC + 4), 19);
    CHECK_EQ(t.c32(kC + 8), 17);
    CHECK_EQ(t.c32(kC + 12), 19);

    // Accumulators outside M x N persist across a smaller ACC_KEEP op...
    t.shape(1, 1, 1, 8);
    CHECK_EQ(t.start(NPU_CTRL_ACC_KEEP | NPU_CTRL_NO_WB), kDone);
    CHECK_EQ(t.npu.acc(0, 0), 23u);
    CHECK_EQ(t.npu.acc(1, 1), 19u);
    // ...and a run without ACC_KEEP clears all of them.
    CHECK_EQ(t.start(NPU_CTRL_NO_WB), kDone);
    CHECK_EQ(t.npu.acc(0, 0), 10u);
    CHECK_EQ(t.npu.acc(1, 1), 0u);
}

TEST(npu_full_kmax_and_wraparound) {
    Bench t;
    // K = 1024 with every product (-128)(-128) = 2^14: acc = 2^24.
    t.fill(kA, 1024, 0x80);
    t.fill(kB, 1024, 0x80);
    t.shape(1, 1, 1024, 4);
    CHECK_EQ(t.start(), kDone);
    CHECK_EQ(t.mem.read32(kC), 0x01000000u);
    // 127 more ACC_KEEP runs reach 2^31, which wraps to INT_MIN.
    for (int i = 0; i < 127; i++) {
        CHECK_EQ(t.start(NPU_CTRL_ACC_KEEP | NPU_CTRL_NO_WB), kDone);
    }
    CHECK_EQ(t.start(NPU_CTRL_ACC_KEEP), kDone);
    CHECK_EQ(t.mem.read32(kC), 0x81000000u);
}

TEST(npu_errors_touch_nothing) {
    struct Case {
        uint32_t reg, value;
    };
    const Case cases[] = {
        {NPU_M, 0},          {NPU_M, 9},           {NPU_M, 0x101},        {NPU_N, 0},
        {NPU_N, 9},          {NPU_K, 0},           {NPU_K, 1025},         {NPU_K, 0x10001},
        {NPU_A_ADDR, kA + 1}, {NPU_B_ADDR, kB + 2}, {NPU_C_ADDR, kC + 3}, {NPU_BIAS_ADDR, kBias + 2},
        {NPU_A_STRIDE, 5},   {NPU_B_STRIDE, 6},    {NPU_C_STRIDE, 7},
    };
    for (const Case& c : cases) {
        Bench t;
        t.put8(kA, {2, 0, 0, 0, 3, 0, 0, 0});
        t.put8(kB, {5, 0, 0, 0, 7, 0, 0, 0});
        t.shape(2, 2, 1, 8);
        CHECK_EQ(t.start(), kDone);
        t.fill(kC, 32, 0x99);
        t.set(c.reg, c.value);
        CHECK_EQ(t.start(), kDoneErr);
        CHECK_EQ(t.mem.read32(kC), 0x99999999u);  // nothing written
        CHECK_EQ(t.npu.acc(0, 0), 10u);           // accumulators untouched
        CHECK_EQ(t.npu.acc(1, 1), 21u);
        CHECK_EQ(t.get(c.reg), c.value);
        t.shape(2, 2, 1, 8);
        t.set(NPU_BIAS_ADDR, 0);
        CHECK_EQ(t.start(), kDone);               // the next START clears ERR
    }
}

TEST(npu_parameters_limit_m_n_k) {
    Bench t(4, 16);
    t.shape(4, 4, 16, 16);
    CHECK_EQ(t.start(), kDone);
    t.shape(5, 4, 16, 16);
    CHECK_EQ(t.start(), kDoneErr);
    t.shape(4, 5, 16, 16);
    CHECK_EQ(t.start(), kDoneErr);
    t.shape(4, 4, 17, 20);
    CHECK_EQ(t.start(), kDoneErr);
}

TEST(npu_dma_sees_only_dmem) {
    iss::Memory mem;
    iss::Npu npu(mem);
    iss::Soc soc(mem, npu, nullptr);
    mem.write32(0x10000000, 0x01010101u, 0xf);
    // A in IMEM reads 0; C at SIM_EXIT must not reach the MMIO register.
    soc.store(NPU_A_ADDR, 0x00000000, 0xf);
    soc.store(NPU_B_ADDR, 0x10000000, 0xf);
    soc.store(NPU_C_ADDR, SIM_EXIT, 0xf);
    soc.store(NPU_M, 1, 0xf);
    soc.store(NPU_N, 1, 0xf);
    soc.store(NPU_K, 4, 0xf);
    soc.store(NPU_CTRL, NPU_CTRL_START, 0xf);
    CHECK_EQ(soc.load(NPU_STATUS), kDone);  // DONE right after START, never BUSY
    CHECK(!soc.exit_requested());
    CHECK_EQ(npu.acc(0, 0), 0u);
    // A in DMEM: 4 * (1 * 1) = 4, C into DMEM through the same path.
    soc.store(NPU_A_ADDR, 0x10000000, 0xf);
    soc.store(NPU_C_ADDR, 0x10000010, 0xf);
    soc.store(NPU_CTRL, NPU_CTRL_START, 0xf);
    CHECK_EQ(soc.load(0x10000010), 4u);
}

TEST(npu_ctrl_reuse_hints_change_nothing) {
    // CTRL bits 3 (B_KEEP) and 4 (A_KEEP) only let the hardware skip
    // reloading an operand; the model always reads memory, and the result
    // must equal a plain START.
    constexpr uint32_t kReuseHints = (1u << 3) | (1u << 4);
    Bench t;
    t.put8(kA, {1, 2, 3, 0, -1, -2, -3, 0});
    t.put8(kB, {4, 5, 6, 0, -7, 8, -9, 0});
    t.shape(2, 2, 3, 8);
    CHECK_EQ(t.start(), kDone);
    t.put8(kA, {2, 0, 0, 0});  // A changed in memory: it must be read again
    CHECK_EQ(t.start(kReuseHints), kDone);
    CHECK_EQ(t.c32(kC + 0), 8);     // 2*4
    CHECK_EQ(t.c32(kC + 4), -14);   // 2*-7
    CHECK_EQ(t.c32(kC + 8), -32);
    CHECK_EQ(t.c32(kC + 12), 18);
    CHECK_EQ(t.start(kReuseHints | NPU_CTRL_ACC_KEEP), kDone);
    CHECK_EQ(t.c32(kC + 0), 16);
}
