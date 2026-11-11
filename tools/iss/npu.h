// Functional model of the NPU (docs/spec.md section 5).
//
// This is the golden reference for rtl/npu: the operation in Npu::run()
// follows the pseudocode of section 5.1 step by step. An operation runs to
// completion inside the START write, so the model never reports BUSY and
// CYCLES always reads 0.
#ifndef PULSE_ISS_NPU_H
#define PULSE_ISS_NPU_H

#include <cstdint>
#include <vector>

#include "memory.h"

namespace iss {

class Npu {
public:
    static constexpr unsigned kDefaultArr = 8;
    static constexpr unsigned kDefaultKmax = 1024;
    static constexpr uint32_t kVersion = 1;

    // `arr` and `kmax` are the npu_top parameters ARR and KMAX; INFO reports
    // them, so they must fit its 8- and 16-bit fields.
    explicit Npu(Memory& mem, unsigned arr = kDefaultArr, unsigned kmax = kDefaultKmax);

    // Register port. `offset` is the byte offset inside the NPU window;
    // undefined offsets read 0 and ignore writes.
    uint32_t read(uint32_t offset) const;
    void write(uint32_t offset, uint32_t value);

    // Accumulator value, for unit tests of ACC_KEEP.
    uint32_t acc(unsigned m, unsigned n) const { return acc_[m * arr_ + n]; }

private:
    bool config_valid() const;
    void run(uint32_t ctrl);
    void write_back();

    Memory& mem_;
    const unsigned arr_;
    const unsigned kmax_;

    uint32_t status_ = 0;
    uint32_t a_addr_ = 0;
    uint32_t b_addr_ = 0;
    uint32_t c_addr_ = 0;
    uint32_t m_ = 0;
    uint32_t n_ = 0;
    uint32_t k_ = 0;
    uint32_t a_stride_ = 0;
    uint32_t b_stride_ = 0;
    uint32_t c_stride_ = 0;
    uint32_t quant_ = 0;
    uint32_t bias_addr_ = 0;
    uint32_t qmult_ = 1;

    // ARR x ARR INT32 accumulators, kept as uint32_t for wrapping arithmetic.
    // They persist between operations so ACC_KEEP can extend a previous sum.
    std::vector<uint32_t> acc_;

    // The bias vector of the current write-back, read before any C write.
    std::vector<uint32_t> bias_;
};

}  // namespace iss

#endif  // PULSE_ISS_NPU_H
