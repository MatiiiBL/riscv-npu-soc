#include "elf_loader.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace {

constexpr size_t EHDR_SIZE = 52;
constexpr size_t PHDR_SIZE = 32;
constexpr uint8_t ELFCLASS32 = 1;
constexpr uint8_t ELFDATA2LSB = 1;
constexpr uint16_t ET_EXEC = 2;
constexpr uint16_t EM_RISCV = 243;
constexpr uint32_t PT_LOAD = 1;

uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t rd32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// True if [addr, addr + len) lies inside the region. The unsigned
// subtraction wraps for addr < base, which the first test then rejects.
bool inside(uint32_t addr, uint32_t len, const ElfRegion &r) {
    const uint32_t off = addr - r.base;
    const uint32_t size = static_cast<uint32_t>(r.bytes->size());
    return off < size && len <= size - off;
}

std::string hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}

}  // namespace

bool load_elf32(const std::string &path, const std::vector<ElfRegion> &regions,
                std::string &error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    const uint8_t *e = file.data();

    if (file.size() < EHDR_SIZE || e[0] != 0x7f || e[1] != 'E' || e[2] != 'L' || e[3] != 'F') {
        error = path + ": not an ELF file";
        return false;
    }
    if (e[4] != ELFCLASS32 || e[5] != ELFDATA2LSB) {
        error = path + ": not a 32-bit little-endian ELF file";
        return false;
    }
    if (rd16(e + 16) != ET_EXEC || rd16(e + 18) != EM_RISCV) {
        error = path + ": not a RISC-V executable";
        return false;
    }
    const uint32_t phoff = rd32(e + 28);
    const uint16_t phentsize = rd16(e + 42);
    const uint16_t phnum = rd16(e + 44);
    if (phentsize < PHDR_SIZE || uint64_t{phoff} + uint64_t{phnum} * phentsize > file.size()) {
        error = path + ": truncated program header table";
        return false;
    }

    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *ph = e + phoff + size_t{i} * phentsize;
        const uint32_t offset = rd32(ph + 4);
        const uint32_t paddr = rd32(ph + 12);
        const uint32_t filesz = rd32(ph + 16);
        const uint32_t memsz = rd32(ph + 20);
        if (rd32(ph) != PT_LOAD || memsz == 0) {
            continue;
        }
        if (filesz > memsz || uint64_t{offset} + filesz > file.size()) {
            error = path + ": segment " + std::to_string(i) + " has a bad size or file offset";
            return false;
        }
        const auto region = std::find_if(regions.begin(), regions.end(),
                                         [&](const ElfRegion &r) { return inside(paddr, memsz, r); });
        if (region == regions.end()) {
            error = path + ": segment " + std::to_string(i) + " at " + hex(paddr) + " (" +
                    std::to_string(memsz) + " bytes) does not fit in IMEM or DMEM";
            return false;
        }
        const auto dst = region->bytes->begin() + (paddr - region->base);
        std::copy(file.begin() + offset, file.begin() + offset + filesz, dst);
        std::fill(dst + filesz, dst + memsz, uint8_t{0});
    }
    return true;
}
