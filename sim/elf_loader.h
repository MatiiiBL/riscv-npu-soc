// Minimal ELF32 little-endian RISC-V loader: returns the PT_LOAD segments.
#ifndef PULSE_SIM_ELF_LOADER_H
#define PULSE_SIM_ELF_LOADER_H

#include <cstdint>
#include <string>
#include <vector>

struct ElfSegment {
    uint32_t paddr;              // physical load address
    std::vector<uint8_t> bytes;  // p_memsz bytes: file contents, then zero fill
};

// Parses `path` and appends its non-empty PT_LOAD segments to `segments`.
// Returns false with a message in `error` if the file is not a 32-bit
// little-endian RISC-V ELF or a segment lies outside the file.
bool load_elf32(const std::string &path, std::vector<ElfSegment> &segments,
                std::string &error);

#endif
