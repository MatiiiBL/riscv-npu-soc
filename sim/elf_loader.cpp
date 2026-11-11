#include "elf_loader.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>

namespace {

constexpr uint8_t ELFCLASS32 = 1;
constexpr uint8_t ELFDATA2LSB = 1;
constexpr uint16_t EM_RISCV = 243;
constexpr uint32_t PT_LOAD = 1;
constexpr size_t EHDR_SIZE = 52;
constexpr size_t PHDR_SIZE = 32;

uint16_t rd16(const std::vector<uint8_t> &b, size_t off) {
    return static_cast<uint16_t>(b[off] | (b[off + 1] << 8));
}

uint32_t rd32(const std::vector<uint8_t> &b, size_t off) {
    return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
           (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
}

}  // namespace

bool load_elf32(const std::string &path, std::vector<ElfSegment> &segments,
                std::string &error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());

    if (file.size() < EHDR_SIZE || file[0] != 0x7f || file[1] != 'E' || file[2] != 'L' ||
        file[3] != 'F') {
        error = path + ": not an ELF file";
        return false;
    }
    if (file[4] != ELFCLASS32 || file[5] != ELFDATA2LSB) {
        error = path + ": not a 32-bit little-endian ELF";
        return false;
    }
    if (rd16(file, 18) != EM_RISCV) {
        error = path + ": not a RISC-V ELF";
        return false;
    }

    const uint32_t phoff = rd32(file, 28);
    const uint16_t phentsize = rd16(file, 42);
    const uint16_t phnum = rd16(file, 44);
    if (phnum != 0 && phentsize < PHDR_SIZE) {
        error = path + ": bad program header size";
        return false;
    }

    for (uint16_t i = 0; i < phnum; i++) {
        const uint64_t ph = static_cast<uint64_t>(phoff) + static_cast<uint64_t>(i) * phentsize;
        if (ph + PHDR_SIZE > file.size()) {
            error = path + ": program header table is truncated";
            return false;
        }
        if (rd32(file, ph + 0) != PT_LOAD) {
            continue;
        }
        const uint32_t offset = rd32(file, ph + 4);
        const uint32_t paddr = rd32(file, ph + 12);
        const uint32_t filesz = rd32(file, ph + 16);
        const uint32_t memsz = rd32(file, ph + 20);
        if (memsz == 0) {
            continue;
        }
        if (filesz > memsz || static_cast<uint64_t>(offset) + filesz > file.size()) {
            error = path + ": PT_LOAD segment lies outside the file";
            return false;
        }
        ElfSegment seg;
        seg.paddr = paddr;
        seg.bytes.assign(memsz, 0);
        std::copy(file.begin() + offset, file.begin() + offset + filesz, seg.bytes.begin());
        segments.push_back(std::move(seg));
    }
    return true;
}
