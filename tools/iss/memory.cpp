#include "memory.h"

#include <algorithm>
#include <cstring>

namespace iss {

namespace {

// True if [addr, addr + len) lies inside [base, base + size). The unsigned
// subtraction wraps for addr < base, which the first test then rejects.
bool inside(uint32_t addr, uint32_t len, uint32_t base, uint32_t size) {
    const uint32_t off = addr - base;
    return off < size && len <= size - off;
}

}  // namespace

Memory::Memory() : imem_(kImemSize, 0), dmem_(kDmemSize, 0) {}

bool Memory::load(uint32_t paddr, const uint8_t* data, uint32_t filesz, uint32_t memsz) {
    if (filesz > memsz) {
        return false;
    }
    uint8_t* dst = nullptr;
    if (inside(paddr, memsz, kImemBase, kImemSize)) {
        dst = &imem_[paddr - kImemBase];
    } else if (inside(paddr, memsz, kDmemBase, kDmemSize)) {
        dst = &dmem_[paddr - kDmemBase];
    } else {
        return false;
    }
    if (filesz > 0) {
        std::memcpy(dst, data, filesz);
    }
    std::fill(dst + filesz, dst + memsz, uint8_t{0});
    return true;
}

}  // namespace iss
