// Command line front end of the Pulse reference ISS (docs/spec.md section 7.1).
//
// stdout carries exactly the bytes the program writes to UART_TX; everything
// else (diagnostics, the summary line) goes to stderr.
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "commit_log.h"
#include "cpu.h"
#include "elf.h"
#include "memory.h"
#include "npu.h"
#include "soc.h"

namespace {

constexpr int kExitHalt = 2;      // ECALL or EBREAK
constexpr int kExitIllegal = 3;
constexpr int kExitTimeout = 124;  // --max-cycles reached
constexpr int kExitToolError = 125;  // bad command line, ELF or I/O error

struct Options {
    uint64_t max_instret = 100000000;
    std::string commit_log;
    bool quiet = false;
    bool stats = false;
    uint64_t npu_arr = iss::Npu::kDefaultArr;
    uint64_t npu_kmax = iss::Npu::kDefaultKmax;
    std::string elf;
};

void print_usage(std::FILE* out) {
    std::fputs(
        "usage: iss [options] program.elf\n"
        "\n"
        "Reference instruction-set simulator of the Pulse SoC.\n"
        "\n"
        "  --max-cycles N     stop with exit code 124 after N retired instructions\n"
        "                     (default 100000000; the ISS counts instructions, not cycles)\n"
        "  --commit-log FILE  write one line per retired instruction to FILE\n"
        "  --quiet            do not print the summary line on stderr\n"
        "  --stats            print the host run time and simulation speed on stderr\n"
        "  --npu-arr N        NPU array size ARR, 1..255 (default 8)\n"
        "  --npu-kmax N       NPU maximum inner dimension KMAX, 1..65535 (default 1024)\n"
        "  -h, --help         show this help\n"
        "\n"
        "Exit code: 0 on SIM_EXIT = 0, else (value & 0xff) or 1 if that is 0;\n"
        "2 on ECALL/EBREAK, 3 on an illegal instruction, 124 on --max-cycles,\n"
        "125 on a usage, ELF or I/O error.\n",
        out);
}

bool parse_uint(const char* text, uint64_t min, uint64_t max, uint64_t& value) {
    if (*text < '0' || *text > '9') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || v < min || v > max) {
        return false;
    }
    value = v;
    return true;
}

// Returns 0 on success, otherwise the process exit code to use.
int parse_args(int argc, char** argv, Options& opts) {
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(stdout);
            std::exit(0);
        }
        if (arg == "--quiet") {
            opts.quiet = true;
            continue;
        }
        if (arg == "--stats") {
            opts.stats = true;
            continue;
        }
        if (arg == "--max-cycles" || arg == "--commit-log" || arg == "--npu-arr" || arg == "--npu-kmax") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "iss: %s needs a value\n", arg.c_str());
                return kExitToolError;
            }
            const char* value = argv[++i];
            bool ok = true;
            if (arg == "--commit-log") {
                opts.commit_log = value;
            } else if (arg == "--max-cycles") {
                ok = parse_uint(value, 0, UINT64_MAX, opts.max_instret);
            } else if (arg == "--npu-arr") {
                ok = parse_uint(value, 1, 255, opts.npu_arr);
            } else {
                ok = parse_uint(value, 1, 65535, opts.npu_kmax);
            }
            if (!ok) {
                std::fprintf(stderr, "iss: bad value for %s: '%s'\n", arg.c_str(), value);
                return kExitToolError;
            }
            continue;
        }
        if (arg.size() > 1 && arg[0] == '-') {
            std::fprintf(stderr, "iss: unknown option '%s'\n", arg.c_str());
            print_usage(stderr);
            return kExitToolError;
        }
        if (!opts.elf.empty()) {
            std::fprintf(stderr, "iss: more than one program given\n");
            return kExitToolError;
        }
        opts.elf = arg;
    }
    if (opts.elf.empty()) {
        print_usage(stderr);
        return kExitToolError;
    }
    return 0;
}

int sim_exit_code(uint32_t value) {
    if (value == 0) {
        return 0;
    }
    return (value & 0xffu) != 0 ? static_cast<int>(value & 0xffu) : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Options opts;
    if (const int rc = parse_args(argc, argv, opts)) {
        return rc;
    }

    iss::Memory mem;
    iss::Npu npu(mem, static_cast<unsigned>(opts.npu_arr), static_cast<unsigned>(opts.npu_kmax));
    iss::Soc soc(mem, npu, stdout);

    iss::ElfInfo elf;
    std::string error;
    if (!iss::load_elf_file(opts.elf, mem, elf, error)) {
        std::fprintf(stderr, "iss: %s: %s\n", opts.elf.c_str(), error.c_str());
        return kExitToolError;
    }
    if (elf.entry != iss::kImemBase) {
        std::fprintf(stderr, "iss: warning: ELF entry 0x%08x ignored, execution starts at the reset pc 0x%08x\n",
                     elf.entry, iss::kImemBase);
    }

    std::FILE* log_file = nullptr;
    std::unique_ptr<iss::CommitLog> log;
    if (!opts.commit_log.empty()) {
        log_file = std::fopen(opts.commit_log.c_str(), "wb");
        if (log_file == nullptr) {
            std::fprintf(stderr, "iss: cannot create %s: %s\n", opts.commit_log.c_str(), std::strerror(errno));
            return kExitToolError;
        }
        log = std::make_unique<iss::CommitLog>(log_file);
    }

    iss::Cpu cpu(mem, soc);
    const auto start = std::chrono::steady_clock::now();
    const iss::StopReason reason = cpu.run(opts.max_instret, log.get());
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

    bool io_ok = std::fflush(stdout) == 0;
    if (log) {
        io_ok = log->flush() && io_ok;
        log.reset();
        io_ok = std::fclose(log_file) == 0 && io_ok;
    }

    int code = 0;
    uint32_t summary_exit = 0;
    switch (reason) {
    case iss::StopReason::kExit:
        summary_exit = soc.exit_value();
        code = sim_exit_code(summary_exit);
        break;
    case iss::StopReason::kEcall:
    case iss::StopReason::kEbreak:
        std::fprintf(stderr, "iss: %s at pc 0x%08x\n", reason == iss::StopReason::kEcall ? "ecall" : "ebreak",
                     cpu.pc());
        code = kExitHalt;
        break;
    case iss::StopReason::kIllegal: {
        const uint32_t pc = cpu.pc();
        const char* why = pc - iss::kImemBase >= iss::kImemSize ? " (fetch outside IMEM)"
                          : (pc & 3) != 0                       ? " (fetch from a misaligned pc)"
                                                                : "";
        std::fprintf(stderr, "iss: illegal instruction 0x%08x at pc 0x%08x%s\n", cpu.insn_at(pc), pc, why);
        code = kExitIllegal;
        break;
    }
    case iss::StopReason::kMaxInstret:
        std::fprintf(stderr, "iss: no SIM_EXIT after %" PRIu64 " instructions (--max-cycles)\n", cpu.instret());
        code = kExitTimeout;
        break;
    }
    if (reason != iss::StopReason::kExit) {
        summary_exit = static_cast<uint32_t>(code);
    }

    if (cpu.misaligned_accesses() != 0) {
        std::fprintf(stderr, "iss: warning: %" PRIu64 " misaligned loads/stores (undefined, see docs/spec.md section 2)\n",
                     cpu.misaligned_accesses());
    }
    if (!io_ok) {
        std::fprintf(stderr, "iss: error writing stdout or the commit log\n");
        return kExitToolError;
    }
    if (opts.stats) {
        const double secs = elapsed.count();
        std::fprintf(stderr, "[iss] host_time=%.3fs mips=%.1f\n", secs,
                     secs > 0 ? static_cast<double>(cpu.instret()) / secs / 1e6 : 0.0);
    }
    if (!opts.quiet) {
        std::fprintf(stderr, "[iss] instret=%" PRIu64 " exit=0x%08x\n", cpu.instret(), summary_exit);
    }
    return code;
}
