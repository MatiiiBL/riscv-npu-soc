// Minimal unit-test support: TEST() registers a function, CHECK_EQ()
// compares two integers and reports both values on failure.
#ifndef PULSE_ISS_TEST_CHECK_H
#define PULSE_ISS_TEST_CHECK_H

#include <cstdint>
#include <string>

namespace check {

using TestFn = void (*)();

bool register_test(const char* name, TestFn fn);
void report_eq(bool ok, uint64_t actual, uint64_t expected, const char* expr, const char* file, int line);
void report_str_eq(const std::string& actual, const std::string& expected, const char* expr, const char* file,
                   int line);

}  // namespace check

#define TEST(name)                                                                          \
    static void name();                                                                     \
    [[maybe_unused]] static const bool name##_registered = check::register_test(#name, name); \
    static void name()

// Both sides are widened to 64 bits before comparing, so compare values of
// the same signedness (a uint32_t with 0xffffffffu, an int32_t with -1).
#define CHECK_EQ(actual, expected)                                                 \
    do {                                                                           \
        const uint64_t check_a_ = static_cast<uint64_t>(actual);                   \
        const uint64_t check_e_ = static_cast<uint64_t>(expected);                 \
        check::report_eq(check_a_ == check_e_, check_a_, check_e_, #actual, __FILE__, __LINE__); \
    } while (0)

#define CHECK(cond) CHECK_EQ(static_cast<bool>(cond), true)

#define CHECK_STR_EQ(actual, expected) \
    check::report_str_eq((actual), (expected), #actual, __FILE__, __LINE__)

#endif  // PULSE_ISS_TEST_CHECK_H
