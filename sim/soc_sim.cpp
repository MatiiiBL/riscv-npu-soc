// Verilator harness for soc_top (docs/spec.md 7).
//
//   soc_sim [--max-cycles N] [--commit-log FILE] [--vcd FILE] [--stats] [--quiet] program.elf
//
// Each loop iteration observes one clock cycle of the settled model and then
// clocks it. Within a cycle the retirement port (WB) is handled before the
// MMIO write strobes (MEM), because the WB instruction is the older of the
// two: once the SIM_EXIT store retires, a younger store already in MEM must
// not reach stdout.
#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "Vsoc_top.h"
#include "elf_loader.h"
#include "verilated.h"
#include "verilated_syms.h"
#include "verilated_vcd_c.h"

namespace {

constexpr uint32_t IMEM_BASE = 0x00000000;
constexpr uint32_t IMEM_SIZE = 64 * 1024;
constexpr uint32_t DMEM_BASE = 0x10000000;
constexpr uint32_t DMEM_SIZE = 256 * 1024;

constexpr uint32_t INSN_ECALL = 0x00000073;
constexpr uint32_t INSN_EBREAK = 0x00100073;

constexpr int EXIT_HALT = 2;
constexpr int EXIT_ILLEGAL = 3;
constexpr int EXIT_TIMEOUT = 124;
constexpr int EXIT_USAGE = 125;

constexpr int RESET_CYCLES = 4;

struct Options {
    uint64_t max_cycles = 100000000;
    const char *commit_log = nullptr;
    const char *vcd = nullptr;
    bool stats = false;
    bool quiet = false;
    const char *elf = nullptr;
};

void print_usage(std::FILE *out) {
    std::fputs(
        "usage: soc_sim [options] program.elf\n"
        "\n"
        "Verilator model of the Pulse SoC.\n"
        "\n"
        "  --max-cycles N     stop with exit code 124 after N clock cycles (default 100000000)\n"
        "  --commit-log FILE  write one line per retired instruction to FILE\n"
        "  --vcd FILE         write a waveform of the whole run to FILE\n"
        "  --stats            print the performance counters on stderr\n"
        "  --quiet            do not print the summary line on stderr\n"
        "  -h, --help         show this help\n"
        "\n"
        "Exit code: 0 on SIM_EXIT = 0, else (value & 0xff) or 1 if that is 0;\n"
        "2 on ECALL/EBREAK, 3 on an illegal instruction, 124 on --max-cycles,\n"
        "125 on a usage, ELF or I/O error.\n",
        out);
}

// Decimal only, like the ISS: no sign, no 0x prefix, no trailing characters.
bool parse_uint(const char *text, uint64_t &value) {
    if (*text < '0' || *text > '9') {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0') {
        return false;
    }
    value = v;
    return true;
}

// The same command line as the ISS (tools/iss/main.cpp, spec 7.1), plus
// --vcd. Returns 0 on success, otherwise the process exit code to use.
int parse_args(int argc, char **argv, Options &opt) {
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(stdout);
            std::exit(0);
        }
        if (arg == "--quiet") {
            opt.quiet = true;
            continue;
        }
        if (arg == "--stats") {
            opt.stats = true;
            continue;
        }
        if (arg == "--max-cycles" || arg == "--commit-log" || arg == "--vcd") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "soc_sim: %s needs a value\n", arg.c_str());
                return EXIT_USAGE;
            }
            const char *value = argv[++i];
            if (arg == "--commit-log") {
                opt.commit_log = value;
            } else if (arg == "--vcd") {
                opt.vcd = value;
            } else if (!parse_uint(value, opt.max_cycles)) {
                std::fprintf(stderr, "soc_sim: bad value for %s: '%s'\n", arg.c_str(), value);
                return EXIT_USAGE;
            }
            continue;
        }
        if (arg.size() > 1 && arg[0] == '-') {
            std::fprintf(stderr, "soc_sim: unknown option '%s'\n", arg.c_str());
            print_usage(stderr);
            return EXIT_USAGE;
        }
        if (opt.elf != nullptr) {
            std::fprintf(stderr, "soc_sim: more than one program given\n");
            return EXIT_USAGE;
        }
        opt.elf = argv[i];
    }
    if (opt.elf == nullptr) {
        print_usage(stderr);
        return EXIT_USAGE;
    }
    return 0;
}

// Returns the 32-bit word array `var` of the instance `scope`, made visible by
// sim/sim.vlt; a model without it is a build error. Looking arrays up by
// name keeps the harness independent of how many DMEM banks the model was
// built with and of Verilator's internal member names.
uint32_t *find_words(const VerilatedContext &ctx, const std::string &scope, const char *var,
                     size_t words) {
    const VerilatedScope *s = ctx.scopeFind(scope.c_str());
    VerilatedVar *v = s != nullptr ? s->varFind(var) : nullptr;
    if (v == nullptr || v->totalSize() != words * sizeof(uint32_t)) {
        std::fprintf(stderr, "soc_sim: the model has no %zu-word array %s.%s\n", words,
                     scope.c_str(), var);
        std::exit(EXIT_USAGE);
    }
    return static_cast<uint32_t *>(v->datap());
}

// Copies the ELF image into the IMEM and DMEM arrays of the model; every
// word not covered by a segment is zero. DMEM word w lives in bank
// w mod banks at row w / banks (rtl/soc/soc_dmem.sv).
bool load_program(const VerilatedContext &ctx, const char *path) {
    std::vector<uint8_t> imem(IMEM_SIZE, 0);
    std::vector<uint8_t> dmem(DMEM_SIZE, 0);
    std::string error;
    if (!load_elf32(path, {{IMEM_BASE, &imem}, {DMEM_BASE, &dmem}}, error)) {
        std::fprintf(stderr, "soc_sim: %s\n", error.c_str());
        return false;
    }

    auto word = [](const std::vector<uint8_t> &m, size_t i) {
        return static_cast<uint32_t>(m[4 * i]) | (static_cast<uint32_t>(m[4 * i + 1]) << 8) |
               (static_cast<uint32_t>(m[4 * i + 2]) << 16) |
               (static_cast<uint32_t>(m[4 * i + 3]) << 24);
    };
    auto bank_scope = [](size_t b) {
        return "TOP.soc_top.u_dmem.g_bank[" + std::to_string(b) + "].u_ram";
    };
    size_t banks = 0;
    while (ctx.scopeFind(bank_scope(banks).c_str()) != nullptr) {
        banks++;
    }
    if (banks == 0 || (DMEM_SIZE / 4) % banks != 0) {
        std::fprintf(stderr, "soc_sim: cannot find the DMEM banks in the model\n");
        return false;
    }
    const size_t rows = DMEM_SIZE / 4 / banks;
    for (size_t b = 0; b < banks; b++) {
        uint32_t *bank = find_words(ctx, bank_scope(b), "mem", rows);
        for (size_t r = 0; r < rows; r++) {
            bank[r] = word(dmem, r * banks + b);
        }
    }

    uint32_t *imem_words = find_words(ctx, "TOP.soc_top.u_imem", "mem", IMEM_SIZE / 4);
    for (size_t i = 0; i < IMEM_SIZE / 4; i++) {
        imem_words[i] = word(imem, i);
    }

    // The ISS starts with every register at zero; do not leave that to the
    // simulator's choice of initial values for the (unreset) register file.
    uint32_t *regs = find_words(ctx, "TOP.soc_top.u_core.u_regfile", "regs", 32);
    std::fill(regs, regs + 32, 0);
    return true;
}

// Maps a SIM_EXIT value to the process exit code (spec 7.1).
int exit_code_for(uint32_t value) {
    if (value == 0) {
        return 0;
    }
    return (value & 0xff) != 0 ? static_cast<int>(value & 0xff) : 1;
}

}  // namespace

int main(int argc, char **argv) {
    Options opt;
    if (const int rc = parse_args(argc, argv, opt)) {
        return rc;
    }

    auto ctx = std::make_unique<VerilatedContext>();
    auto top = std::make_unique<Vsoc_top>(ctx.get());
    if (!load_program(*ctx, opt.elf)) {
        return EXIT_USAGE;
    }

    FILE *log = nullptr;
    if (opt.commit_log != nullptr) {
        log = std::fopen(opt.commit_log, "w");
        if (log == nullptr) {
            std::fprintf(stderr, "soc_sim: cannot write %s: %s\n", opt.commit_log,
                         std::strerror(errno));
            return EXIT_USAGE;
        }
        std::setvbuf(log, nullptr, _IOFBF, 1 << 20);
    }
    std::setvbuf(stdout, nullptr, _IOFBF, 1 << 16);

    std::unique_ptr<VerilatedVcdC> vcd;
    if (opt.vcd != nullptr) {
        ctx->traceEverOn(true);
        vcd = std::make_unique<VerilatedVcdC>();
        top->trace(vcd.get(), 99);
        vcd->open(opt.vcd);
    }
    // Waveform time in ps (the VCD timescale): a nominal 10 ns clock, so
    // cycle n of the run starts at n * 10 ns.
    constexpr uint64_t HALF_PERIOD_PS = 5000;
    uint64_t sim_time = 0;
    auto tick = [&]() {
        top->clk = 0;
        top->eval();
        if (vcd) vcd->dump(sim_time);
        sim_time += HALF_PERIOD_PS;
        top->clk = 1;
        top->eval();
        if (vcd) vcd->dump(sim_time);
        sim_time += HALF_PERIOD_PS;
    };

    top->clk = 1;
    top->rst_n = 0;
    top->eval();
    for (int i = 0; i < RESET_CYCLES; i++) {
        tick();
    }
    top->rst_n = 1;

    uint64_t cycle = 0;
    bool exit_pending = false;
    uint32_t exit_value = 0;
    uint32_t last_pc = 0;
    uint32_t last_insn = 0;
    int code = -1;
    uint32_t reported = 0;   // exit= field of the summary line

    for (;;) {
        if (top->halted) {
            code = (last_insn == INSN_ECALL || last_insn == INSN_EBREAK) ? EXIT_HALT : EXIT_ILLEGAL;
            if (code == EXIT_HALT) {
                std::fprintf(stderr, "[rtl] halted by %s at pc 0x%08x\n",
                             last_insn == INSN_ECALL ? "ecall" : "ebreak", last_pc);
            } else {
                const char *why = last_pc - IMEM_BASE >= IMEM_SIZE ? " (fetch outside IMEM)"
                                  : (last_pc & 3) != 0             ? " (misaligned fetch)"
                                                                   : "";
                std::fprintf(stderr, "[rtl] illegal instruction 0x%08x at pc 0x%08x%s\n", last_insn,
                             last_pc, why);
            }
            reported = static_cast<uint32_t>(code);
            break;
        }
        if (cycle >= opt.max_cycles) {
            code = EXIT_TIMEOUT;
            reported = EXIT_TIMEOUT;
            std::fprintf(stderr, "[rtl] no exit after %" PRIu64 " cycles\n", cycle);
            break;
        }

        bool done = false;
        if (top->rvfi_valid) {
            last_pc = top->rvfi_pc;
            last_insn = top->rvfi_insn;
            if (log != nullptr) {
                std::fprintf(log, "%08x %08x %02u %08x", top->rvfi_pc, top->rvfi_insn,
                             static_cast<unsigned>(top->rvfi_rd), top->rvfi_rd_wdata);
                if (top->rvfi_mem_wmask != 0) {
                    std::fprintf(log, " M %08x %x %08x", top->rvfi_mem_addr,
                                 static_cast<unsigned>(top->rvfi_mem_wmask), top->rvfi_mem_wdata);
                }
                std::fputc('\n', log);
            }
            done = exit_pending;   // the SIM_EXIT store itself just retired
        }
        if (!done) {
            if (top->sim_uart_valid) {
                std::fputc(top->sim_uart_data, stdout);
            }
            if (top->sim_exit_valid) {
                exit_pending = true;
                exit_value = top->sim_exit_code;
            }
        }

        tick();
        cycle++;
        if (done) {
            code = exit_code_for(exit_value);
            reported = exit_value;
            break;
        }
    }

    std::fflush(stdout);
    if (log != nullptr) {
        std::fclose(log);
    }
    if (vcd) {
        vcd->close();
    }
    top->final();

    const uint64_t cycles = top->perf_cycles;
    const uint64_t instret = top->perf_instret;
    const double cpi = instret != 0 ? static_cast<double>(cycles) / static_cast<double>(instret) : 0.0;
    if (!opt.quiet) {
        std::fprintf(stderr, "[rtl] cycles=%" PRIu64 " instret=%" PRIu64 " cpi=%.3f exit=0x%08x\n",
                     cycles, instret, cpi, reported);
    }
    if (opt.stats) {
        const uint32_t ctrl = top->perf_ctrl;
        const uint32_t mispredict = top->perf_mispredict;
        std::fprintf(stderr, "[rtl] stats: cycles           %" PRIu64 "\n", cycles);
        std::fprintf(stderr, "[rtl] stats: instret          %" PRIu64 "  (cpi %.3f)\n", instret,
                     cpi);
        std::fprintf(stderr, "[rtl] stats: load-use stalls  %u\n", top->perf_load_use);
        std::fprintf(stderr, "[rtl] stats: branches+jumps   %u\n", ctrl);
        std::fprintf(stderr, "[rtl] stats: mispredicts      %u  (%.1f%% of branches+jumps)\n",
                     mispredict, ctrl != 0 ? 100.0 * mispredict / ctrl : 0.0);
        std::fprintf(stderr, "[rtl] stats: divider stalls   %u cycles\n", top->perf_div_stall);
    }
    return code;
}
