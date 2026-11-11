// RV32IM + Zicsr core model (docs/spec.md sections 2 and 7).
//
// Instructions retire one at a time in program order. IMEM cannot be written
// from the data port, so the whole IMEM is decoded once at reset and the
// execute loop never decodes again.
#ifndef PULSE_ISS_CPU_H
#define PULSE_ISS_CPU_H

#include <cstdint>
#include <vector>

#include "commit_log.h"
#include "decode.h"
#include "memory.h"
#include "soc.h"

namespace iss {

enum class StopReason {
    kExit,        // the store to SIM_EXIT retired
    kEcall,
    kEbreak,
    kIllegal,     // illegal instruction, or a fetch outside IMEM / misaligned
    kMaxInstret,  // the instruction limit was reached first
};

class Cpu {
public:
    Cpu(const Memory& mem, Soc& soc);

    // Reset state: pc = 0, all registers and counters 0, IMEM re-decoded.
    void reset();

    // Runs until the program stops or `max_instret` instructions have
    // retired in total. Every retired instruction is appended to `log`
    // unless it is nullptr. A halting instruction counts as retired and
    // leaves pc() pointing at it.
    StopReason run(uint64_t max_instret, CommitLog* log);

    uint32_t pc() const { return pc_; }
    uint32_t insn_at(uint32_t pc) const { return fetch(pc).raw; }
    uint64_t instret() const { return instret_; }
    uint32_t reg(unsigned i) const { return x_[i]; }
    void set_reg(unsigned i, uint32_t v) {
        if (i != 0) {
            x_[i] = v;
        }
    }

    // Loads and stores whose address is not a multiple of their size. The
    // spec leaves them undefined; the model keeps the containing word's
    // lanes and drops the rest, and main.cpp warns when this is nonzero.
    uint64_t misaligned_accesses() const { return misaligned_; }

private:
    template <bool kLog>
    StopReason run_loop(uint64_t max_instret, CommitLog* log);

    const Insn& fetch(uint32_t pc) const {
        return (pc < kImemBase + kImemSize && (pc & 3) == 0) ? icache_[(pc - kImemBase) >> 2]
                                                              : bad_fetch_;
    }

    static uint32_t read_csr(uint32_t csr, uint64_t instret);

    const Memory& mem_;
    Soc& soc_;
    std::vector<Insn> icache_;
    Insn bad_fetch_;  // what a fetch outside IMEM returns: an illegal word 0
    uint32_t x_[32] = {};
    uint32_t pc_ = 0;
    uint64_t instret_ = 0;
    uint64_t misaligned_ = 0;
};

}  // namespace iss

#endif  // PULSE_ISS_CPU_H
