// Everything behind the core's data port: the address decoder, DMEM, the MMIO
// peripherals (UART, SIM_EXIT, LEDS) and the NPU register window
// (docs/spec.md section 3).
#ifndef PULSE_ISS_SOC_H
#define PULSE_ISS_SOC_H

#include <cstdint>
#include <cstdio>

#include "memory.h"
#include "npu.h"

namespace iss {

class Soc {
public:
    // UART_TX bytes go to `uart_out`; nullptr discards them.
    Soc(Memory& mem, Npu& npu, std::FILE* uart_out);

    // Returns the aligned word containing `addr`, like the dmem_rdata bus;
    // the core extracts sub-word data itself.
    uint32_t load(uint32_t addr) {
        return Memory::in_dmem(addr) ? mem_.read32(addr) : load_io(addr);
    }

    // `wdata` is already shifted into its byte lanes and `be` selects them.
    void store(uint32_t addr, uint32_t wdata, uint32_t be) {
        if (Memory::in_dmem(addr)) {
            mem_.write32(addr, wdata, be);
        } else {
            store_io(addr, wdata);
        }
    }

    bool exit_requested() const { return exit_requested_; }
    uint32_t exit_value() const { return exit_value_; }
    uint32_t leds() const { return leds_; }

private:
    uint32_t load_io(uint32_t addr) const;
    void store_io(uint32_t addr, uint32_t wdata);

    Memory& mem_;
    Npu& npu_;
    std::FILE* uart_out_;
    bool exit_requested_ = false;
    uint32_t exit_value_ = 0;
    uint32_t leds_ = 0;
};

}  // namespace iss

#endif  // PULSE_ISS_SOC_H
