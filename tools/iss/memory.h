// IMEM and DMEM storage (docs/spec.md section 3).
//
// Both memories are addressed with full SoC byte addresses. DMEM is shared by
// the CPU data port and the NPU DMA port, and both follow the same rule:
// addresses outside DMEM read 0 and drop writes, with no aliasing.
#ifndef PULSE_ISS_MEMORY_H
#define PULSE_ISS_MEMORY_H

#include <cstdint>
#include <vector>

#include "soc_map.h"

namespace iss {

constexpr uint32_t kImemBase = IMEM_BASE;
constexpr uint32_t kImemSize = IMEM_SIZE;
constexpr uint32_t kDmemBase = DMEM_BASE;
constexpr uint32_t kDmemSize = DMEM_SIZE;

class Memory {
public:
    Memory();

    // Copies `filesz` bytes of an ELF segment to physical address `paddr` and
    // zero-fills it up to `memsz`. Returns false (and changes nothing) unless
    // the whole segment lies inside IMEM or inside DMEM.
    bool load(uint32_t paddr, const uint8_t* data, uint32_t filesz, uint32_t memsz);

    // Instruction word at a word-aligned address inside IMEM.
    uint32_t imem_word(uint32_t addr) const { return get32(&imem_[addr - kImemBase]); }

    static bool in_dmem(uint32_t addr) { return addr - kDmemBase < kDmemSize; }

    uint8_t read8(uint32_t addr) const { return in_dmem(addr) ? dmem_[addr - kDmemBase] : 0; }

    // Reads the aligned word containing `addr`.
    uint32_t read32(uint32_t addr) const {
        return in_dmem(addr) ? get32(&dmem_[(addr & ~3u) - kDmemBase]) : 0;
    }

    void write8(uint32_t addr, uint8_t data) {
        if (in_dmem(addr)) {
            dmem_[addr - kDmemBase] = data;
        }
    }

    // Writes the lanes of the aligned word containing `addr` selected by the
    // byte enables `be` (bit i = byte lane i); `data` is already in its lanes.
    void write32(uint32_t addr, uint32_t data, uint32_t be) {
        if (!in_dmem(addr)) {
            return;
        }
        uint8_t* p = &dmem_[(addr & ~3u) - kDmemBase];
        for (unsigned lane = 0; lane < 4; lane++) {
            if (be & (1u << lane)) {
                p[lane] = static_cast<uint8_t>(data >> (8 * lane));
            }
        }
    }

private:
    // Explicit little-endian assembly keeps the model independent of the host
    // byte order; compilers turn it into a single load.
    static uint32_t get32(const uint8_t* p) {
        return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24;
    }

    std::vector<uint8_t> imem_;
    std::vector<uint8_t> dmem_;
};

}  // namespace iss

#endif  // PULSE_ISS_MEMORY_H
