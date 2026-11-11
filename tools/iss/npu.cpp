#include "npu.h"

#include <algorithm>

#include "bits.h"

namespace iss {
namespace {

constexpr uint32_t reg_offset(uint32_t addr) { return addr - NPU_BASE; }

constexpr uint32_t kCtrl = reg_offset(NPU_CTRL);
constexpr uint32_t kStatus = reg_offset(NPU_STATUS);
constexpr uint32_t kAAddr = reg_offset(NPU_A_ADDR);
constexpr uint32_t kBAddr = reg_offset(NPU_B_ADDR);
constexpr uint32_t kCAddr = reg_offset(NPU_C_ADDR);
constexpr uint32_t kM = reg_offset(NPU_M);
constexpr uint32_t kN = reg_offset(NPU_N);
constexpr uint32_t kK = reg_offset(NPU_K);
constexpr uint32_t kAStride = reg_offset(NPU_A_STRIDE);
constexpr uint32_t kBStride = reg_offset(NPU_B_STRIDE);
constexpr uint32_t kCStride = reg_offset(NPU_C_STRIDE);
constexpr uint32_t kQuant = reg_offset(NPU_QUANT);
constexpr uint32_t kBiasAddr = reg_offset(NPU_BIAS_ADDR);
constexpr uint32_t kCycles = reg_offset(NPU_CYCLES);
constexpr uint32_t kInfo = reg_offset(NPU_INFO);
constexpr uint32_t kQmult = reg_offset(NPU_QMULT);

int32_t int8_at(const Memory& mem, uint32_t addr) {
    return as_signed(sext(mem.read8(addr), 8));
}

}  // namespace

Npu::Npu(Memory& mem, unsigned arr, unsigned kmax)
    : mem_(mem), arr_(arr), kmax_(kmax), acc_(size_t{arr} * arr, 0) {}

uint32_t Npu::read(uint32_t offset) const {
    switch (offset) {
    case kStatus: return status_;
    case kAAddr: return a_addr_;
    case kBAddr: return b_addr_;
    case kCAddr: return c_addr_;
    case kM: return m_;
    case kN: return n_;
    case kK: return k_;
    case kAStride: return a_stride_;
    case kBStride: return b_stride_;
    case kCStride: return c_stride_;
    case kQuant: return quant_;
    case kBiasAddr: return bias_addr_;
    case kCycles: return 0;
    case kInfo: return arr_ | kmax_ << 8 | kVersion << 24;
    case kQmult: return qmult_;
    default: return 0;  // CTRL is write-only
    }
}

// The spec ignores register writes while BUSY; this model is never busy.
// CTRL bits other than START, ACC_KEEP and NO_WB only affect hardware timing
// (operand-reuse hints), so the model ignores them and always reads memory.
void Npu::write(uint32_t offset, uint32_t value) {
    switch (offset) {
    case kCtrl:
        if (value & NPU_CTRL_START) {
            run(value);
        }
        break;
    case kAAddr: a_addr_ = value; break;
    case kBAddr: b_addr_ = value; break;
    case kCAddr: c_addr_ = value; break;
    case kM: m_ = value; break;
    case kN: n_ = value; break;
    case kK: k_ = value; break;
    case kAStride: a_stride_ = value; break;
    case kBStride: b_stride_ = value; break;
    case kCStride: c_stride_ = value; break;
    case kQuant: quant_ = value; break;
    case kBiasAddr: bias_addr_ = value; break;
    case kQmult: qmult_ = value; break;
    default: break;  // STATUS, CYCLES and INFO are read-only
    }
}

// M, N and K are checked as full 32-bit values: a write of 0x101 to M is an
// error, not M = 1.
bool Npu::config_valid() const {
    const bool dims_ok = m_ >= 1 && m_ <= arr_ && n_ >= 1 && n_ <= arr_ && k_ >= 1 && k_ <= kmax_;
    const uint32_t word_fields =
        a_addr_ | b_addr_ | c_addr_ | bias_addr_ | a_stride_ | b_stride_ | c_stride_;
    return dims_ok && (word_fields & 3u) == 0;
}

void Npu::run(uint32_t ctrl) {
    if (!config_valid()) {
        // Nothing is read or written and the accumulators keep their values.
        status_ = NPU_STATUS_DONE | NPU_STATUS_ERR;
        return;
    }
    if (!(ctrl & NPU_CTRL_ACC_KEEP)) {
        std::fill(acc_.begin(), acc_.end(), 0u);
    }
    // All address arithmetic wraps at 32 bits, like the DMA address adders.
    for (uint32_t m = 0; m < m_; m++) {
        const uint32_t a_row = a_addr_ + m * a_stride_;
        for (uint32_t n = 0; n < n_; n++) {
            const uint32_t b_row = b_addr_ + n * b_stride_;
            uint32_t sum = 0;
            for (uint32_t k = 0; k < k_; k++) {
                sum += static_cast<uint32_t>(int8_at(mem_, a_row + k) * int8_at(mem_, b_row + k));
            }
            acc_[m * arr_ + n] += sum;
        }
    }
    if (!(ctrl & NPU_CTRL_NO_WB)) {
        write_back();
    }
    status_ = NPU_STATUS_DONE;
}

void Npu::write_back() {
    const unsigned shift = NPU_QUANT_SHIFT(quant_);
    const bool out_int8 = (quant_ & NPU_QUANT_OUT_INT8) != 0;
    const bool relu = (quant_ & NPU_QUANT_RELU) != 0;
    const int64_t qmult = as_signed(sext(qmult_, 16));

    for (uint32_t m = 0; m < m_; m++) {
        const uint32_t c_row = c_addr_ + m * c_stride_;
        for (uint32_t n = 0; n < n_; n++) {
            uint32_t v = acc_[m * arr_ + n];
            if (bias_addr_ != 0) {
                v += mem_.read32(bias_addr_ + 4 * n);
            }
            if (out_int8) {
                // |v * qmult| < 2^46, so the product and the rounding term
                // fit easily in 64 bits.
                int64_t p = int64_t{as_signed(v)} * qmult;
                if (shift > 0) {
                    p = (p + (int64_t{1} << (shift - 1))) >> shift;  // arithmetic
                }
                if (relu && p < 0) {
                    p = 0;
                }
                p = std::min<int64_t>(std::max<int64_t>(p, -128), 127);
                mem_.write8(c_row + n, static_cast<uint8_t>(p));
            } else {
                if (relu && as_signed(v) < 0) {
                    v = 0;
                }
                mem_.write32(c_row + 4 * n, v, 0xfu);
            }
        }
    }
}

}  // namespace iss
