// Runs every TEST() and exits nonzero if any check failed.
#include <cinttypes>
#include <cstdio>
#include <vector>

#include "check.h"

namespace check {
namespace {

struct TestCase {
    const char* name;
    TestFn fn;
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

int g_checks = 0;
int g_failures = 0;
const char* g_current = "";

}  // namespace

bool register_test(const char* name, TestFn fn) {
    registry().push_back({name, fn});
    return true;
}

void report_eq(bool ok, uint64_t actual, uint64_t expected, const char* expr, const char* file, int line) {
    g_checks++;
    if (!ok) {
        g_failures++;
        std::fprintf(stderr, "%s:%d: [%s] %s = 0x%" PRIx64 ", expected 0x%" PRIx64 "\n", file, line, g_current,
                     expr, actual, expected);
    }
}

void report_str_eq(const std::string& actual, const std::string& expected, const char* expr, const char* file,
                   int line) {
    g_checks++;
    if (actual != expected) {
        g_failures++;
        std::fprintf(stderr, "%s:%d: [%s] %s =\n%s\nexpected:\n%s\n", file, line, g_current, expr, actual.c_str(),
                     expected.c_str());
    }
}

}  // namespace check

int main() {
    for (const auto& t : check::registry()) {
        check::g_current = t.name;
        t.fn();
    }
    std::printf("iss unit tests: %zu tests, %d checks, %d failed\n", check::registry().size(), check::g_checks,
                check::g_failures);
    return check::g_failures == 0 ? 0 : 1;
}
