/*
 * The mini printf: every conversion, flag and width it supports, formatted
 * with snprintf and compared with the expected text in the program, then
 * printed through printf so expected.txt checks the UART path as well. Also
 * covers snprintf truncation and return values.
 */
#include "soc.h"

static int failures;

static void check(const char *want, const char *got, int len) {
    int ok = strcmp(got, want) == 0 && len == (int)strlen(want);
    printf("[%s] %s\n", got, ok ? "ok" : "FAIL");
    if (!ok) {
        printf("  want [%s] (%u chars), returned %d\n", want, (uint32_t)strlen(want), len);
        failures++;
    }
}

#define CHECK(want, ...)                                   \
    do {                                                   \
        char buf_[64];                                     \
        int n_ = snprintf(buf_, sizeof buf_, __VA_ARGS__); \
        check(want, buf_, n_);                             \
    } while (0)

int main(void) {
    printf("printf: conversions, flags and widths\n");

    CHECK("0 42 -42", "%d %i %d", 0, 42, -42);
    CHECK("2147483647 -2147483648", "%d %d", (int32_t)2147483647, (int32_t)(-2147483647 - 1));
    CHECK("4294967295 0", "%u %u", 4294967295u, 0u);
    CHECK("deadbeef DEADBEEF 0", "%x %X %x", 0xDEADBEEFu, 0xDEADBEEFu, 0u);
    CHECK("   42|00042|-0042|  -42", "%5d|%05d|%05d|%5d", 42, 42, -42, -42);
    CHECK("12345|7", "%3d|%0d", 12345, 7);
    CHECK("0000abcd|    ab", "%08x|%6x", 0xABCDu, 0xABu);
    CHECK("123 456 789", "%ld %lu %lx", 123L, 456UL, 0x789UL);
    CHECK("A|z|!", "%c|%c|%c", 'A', 'z', '!');
    CHECK("pulse|   pulse|pulse|", "%s|%8s|%2s|%s", "pulse", "pulse", "pulse", "");
    CHECK("0x10000000 0x00000000", "%p %p", (void *)0x10000000u, (void *)0);
    CHECK("100%", "%d%%", 100);
    CHECK("[7|seven|7|0x7]", "[%d|%s|%c|0x%x]", 7, "seven", '7', 7u);

    /* Truncation: the return value is the untruncated length. */
    char small[5];
    int n = snprintf(small, sizeof small, "%s", "abcdefgh");
    check("abcd", small, n == 8 ? 4 : -1);
    char untouched[2] = {'#', '\0'};
    n = snprintf(untouched, 0, "%d", 12345);
    check("#", untouched, n == 5 ? 1 : -1);

    /* printf itself returns the number of characters written. */
    n = printf("printf returned ");
    printf("%d\n", n);
    if (n != 16) {
        failures++;
    }

    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures;
}
