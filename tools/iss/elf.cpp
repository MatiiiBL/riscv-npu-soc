#include "elf.h"

#include <cstdio>
#include <fstream>
#include <iterator>

namespace iss {
namespace {

// The few ELF constants the loader needs; <elf.h> is not available on macOS.
constexpr size_t kEhdrSize = 52;
constexpr size_t kPhdrSize = 32;
constexpr uint8_t kElfClass32 = 1;
constexpr uint8_t kElfData2Lsb = 1;
constexpr uint16_t kEtExec = 2;
constexpr uint16_t kEmRiscv = 243;
constexpr uint32_t kPtLoad = 1;

uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

uint32_t get32(const uint8_t* p) {
    return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24;
}

std::string hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}

}  // namespace

bool load_elf_image(const std::vector<uint8_t>& image, Memory& mem, ElfInfo& info, std::string& error) {
    const uint8_t* e = image.data();
    if (image.size() < kEhdrSize || e[0] != 0x7f || e[1] != 'E' || e[2] != 'L' || e[3] != 'F') {
        error = "not an ELF file";
        return false;
    }
    if (e[4] != kElfClass32 || e[5] != kElfData2Lsb) {
        error = "not a 32-bit little-endian ELF file";
        return false;
    }
    if (get16(e + 16) != kEtExec || get16(e + 18) != kEmRiscv) {
        error = "not a RISC-V executable";
        return false;
    }
    info.entry = get32(e + 24);
    const uint32_t phoff = get32(e + 28);
    const uint16_t phentsize = get16(e + 42);
    const uint16_t phnum = get16(e + 44);
    if (phentsize < kPhdrSize || uint64_t{phoff} + uint64_t{phnum} * phentsize > image.size()) {
        error = "truncated program header table";
        return false;
    }

    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t* ph = e + phoff + size_t{i} * phentsize;
        const uint32_t type = get32(ph);
        const uint32_t offset = get32(ph + 4);
        const uint32_t paddr = get32(ph + 12);
        const uint32_t filesz = get32(ph + 16);
        const uint32_t memsz = get32(ph + 20);
        if (type != kPtLoad || memsz == 0) {
            continue;
        }
        if (filesz > memsz || uint64_t{offset} + filesz > image.size()) {
            error = "segment " + std::to_string(i) + " has a bad size or file offset";
            return false;
        }
        if (!mem.load(paddr, e + offset, filesz, memsz)) {
            error = "segment " + std::to_string(i) + " at " + hex(paddr) + " (" +
                    std::to_string(memsz) + " bytes) does not fit in IMEM or DMEM";
            return false;
        }
    }
    return true;
}

bool load_elf_file(const std::string& path, Memory& mem, ElfInfo& info, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    const std::vector<uint8_t> image((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return load_elf_image(image, mem, info, error);
}

}  // namespace iss
