// Two's-complement helpers shared by the core and the NPU model.
//
// C++17 leaves two things implementation-defined that the model relies on:
// converting an out-of-range value to a signed type, and `>>` of a negative
// value. GCC and Clang have always wrapped and shifted arithmetically, and
// C++20 requires exactly that, so the model uses the plain operators.
#ifndef PULSE_ISS_BITS_H
#define PULSE_ISS_BITS_H

#include <cstdint>

namespace iss {

inline int32_t as_signed(uint32_t v) { return static_cast<int32_t>(v); }

// Sign-extends the low `bits` bits of `v` (1 <= bits <= 32).
inline uint32_t sext(uint32_t v, unsigned bits) {
    const uint32_t sign = 1u << (bits - 1);
    const uint32_t mask = (sign << 1) - 1;  // all ones when bits == 32
    return ((v & mask) ^ sign) - sign;
}

inline uint32_t sra(uint32_t v, unsigned shamt) {
    return static_cast<uint32_t>(as_signed(v) >> shamt);
}

}  // namespace iss

#endif  // PULSE_ISS_BITS_H
