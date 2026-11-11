// ELF32 loader (docs/spec.md section 7.1): every PT_LOAD segment is copied
// to its physical address in IMEM or DMEM; everything else stays zero.
#ifndef PULSE_ISS_ELF_H
#define PULSE_ISS_ELF_H

#include <cstdint>
#include <string>
#include <vector>

#include "memory.h"

namespace iss {

struct ElfInfo {
    uint32_t entry = 0;
};

// Loads a little-endian RV32 executable image. On failure returns false and
// sets `error`; `mem` may then hold some of the segments.
bool load_elf_image(const std::vector<uint8_t>& image, Memory& mem, ElfInfo& info, std::string& error);

// Reads `path` and loads it with load_elf_image().
bool load_elf_file(const std::string& path, Memory& mem, ElfInfo& info, std::string& error);

}  // namespace iss

#endif  // PULSE_ISS_ELF_H
