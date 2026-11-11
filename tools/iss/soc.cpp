#include "soc.h"

namespace iss {
namespace {

// MMIO and NPU windows are 4 KiB; the region is selected by addr[31:28] and
// anything past the window reads 0 (no aliasing).
constexpr uint32_t kWindowSize = 0x1000;

bool in_window(uint32_t addr, uint32_t base) { return addr - base < kWindowSize; }

}  // namespace

Soc::Soc(Memory& mem, Npu& npu, std::FILE* uart_out) : mem_(mem), npu_(npu), uart_out_(uart_out) {}

// Registers are 32-bit and the spec only defines word accesses. A sub-word
// load returns the register's word (the core then picks its lane), and a
// sub-word store writes the lane-shifted data with the byte enables ignored.
uint32_t Soc::load_io(uint32_t addr) const {
    const uint32_t word = addr & ~3u;
    if (in_window(word, MMIO_BASE)) {
        switch (word) {
        case UART_STATUS: return 1;  // the simulated UART is always ready
        case LEDS: return leds_;
        default: return 0;  // UART_TX and SIM_EXIT are write-only
        }
    }
    if (in_window(word, NPU_BASE)) {
        return npu_.read(word - NPU_BASE);
    }
    return 0;  // IMEM (fetch only) and unmapped addresses
}

void Soc::store_io(uint32_t addr, uint32_t wdata) {
    const uint32_t word = addr & ~3u;
    if (in_window(word, MMIO_BASE)) {
        switch (word) {
        case UART_TX:
            if (uart_out_ != nullptr) {
                std::fputc(static_cast<int>(wdata & 0xffu), uart_out_);
            }
            break;
        case SIM_EXIT:
            exit_requested_ = true;
            exit_value_ = wdata;
            break;
        case LEDS: leds_ = wdata & 0xffu; break;
        default: break;
        }
    } else if (in_window(word, NPU_BASE)) {
        npu_.write(word - NPU_BASE, wdata);
    }
}

}  // namespace iss
