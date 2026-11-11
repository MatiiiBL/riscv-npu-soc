// Minimal ELF32 little-endian RISC-V loader for the simulator harness.
//
// It accepts and rejects exactly the files the ISS does (tools/iss/elf.cpp),
// so both simulators fail the same programs with exit code 125 (spec 7.1).
#ifndef PULSE_SIM_ELF_LOADER_H
#define PULSE_SIM_ELF_LOADER_H

#include <cstdint>
#include <string>
#include <vector>

// A memory a segment may be loaded into: bytes [base, base + bytes.size()).
struct ElfRegion {
    uint32_t base;
    std::vector<uint8_t> *bytes;
};

// Copies every non-empty PT_LOAD segment of the executable at `path` into
// the region that contains it entirely, zero-filling it from p_filesz up to
// p_memsz. Returns false with a message in `error` if the file is not a
// 32-bit little-endian RISC-V executable, its program header table or a
// segment lies outside the file, or a segment does not fit in one region.
// A segment is checked against the regions before anything is written, and
// nothing is allocated from sizes in the file.
bool load_elf32(const std::string &path, const std::vector<ElfRegion> &regions,
                std::string &error);

#endif
