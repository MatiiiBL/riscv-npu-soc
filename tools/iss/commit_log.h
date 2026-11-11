// Commit log writer (docs/spec.md section 7.2). One line per retired
// instruction, byte-identical to the log the RTL produces from its
// retirement port:
//
//   <pc> <insn> <rd> <rd_wdata>[ M <mem_addr> <wmask> <wdata>]
#ifndef PULSE_ISS_COMMIT_LOG_H
#define PULSE_ISS_COMMIT_LOG_H

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace iss {

class CommitLog {
public:
    // Writes to `out`, which stays owned by the caller.
    explicit CommitLog(std::FILE* out) : out_(out) {}
    ~CommitLog() { flush(); }
    CommitLog(const CommitLog&) = delete;
    CommitLog& operator=(const CommitLog&) = delete;

    // An instruction that is not a store. `rd` is 0 when no register is
    // written, and then `rd_wdata` is ignored.
    void retire(uint32_t pc, uint32_t insn, unsigned rd, uint32_t rd_wdata) {
        char* p = reserve();
        p = put_common(p, pc, insn, rd, rd_wdata);
        *p++ = '\n';
        len_ = static_cast<size_t>(p - buf_);
    }

    // A store: `addr` is the word-aligned address, `wdata` in its lanes.
    void retire_store(uint32_t pc, uint32_t insn, uint32_t addr, uint32_t wmask, uint32_t wdata) {
        char* p = reserve();
        p = put_common(p, pc, insn, 0, 0);
        *p++ = ' ';
        *p++ = 'M';
        *p++ = ' ';
        p = put_hex(p, addr, 8);
        *p++ = ' ';
        p = put_hex(p, wmask, 1);
        *p++ = ' ';
        p = put_hex(p, wdata, 8);
        *p++ = '\n';
        len_ = static_cast<size_t>(p - buf_);
    }

    // Returns false if the underlying stream reported a write error.
    bool flush();

private:
    // Hand-rolled formatting: the log is written once per instruction and
    // printf would dominate the simulation time.
    static constexpr size_t kBufSize = 1 << 16;
    static constexpr size_t kMaxLine = 64;

    char* reserve() {
        if (len_ > kBufSize - kMaxLine) {
            flush();
        }
        return buf_ + len_;
    }

    static char* put_hex(char* p, uint32_t v, int digits) {
        static constexpr char kDigits[] = "0123456789abcdef";
        for (int i = digits - 1; i >= 0; i--) {
            p[i] = kDigits[v & 0xf];
            v >>= 4;
        }
        return p + digits;
    }

    static char* put_common(char* p, uint32_t pc, uint32_t insn, unsigned rd, uint32_t rd_wdata) {
        p = put_hex(p, pc, 8);
        *p++ = ' ';
        p = put_hex(p, insn, 8);
        *p++ = ' ';
        *p++ = static_cast<char>('0' + rd / 10);
        *p++ = static_cast<char>('0' + rd % 10);
        *p++ = ' ';
        return put_hex(p, rd != 0 ? rd_wdata : 0, 8);
    }

    std::FILE* out_;
    bool ok_ = true;
    size_t len_ = 0;
    char buf_[kBufSize];
};

}  // namespace iss

#endif  // PULSE_ISS_COMMIT_LOG_H
