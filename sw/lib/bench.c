#include "bench.h"
#include "soc.h"

const char *fmt_ratio(char *buf, uint32_t num, uint32_t den, unsigned decimals) {
    if (den == 0) {
        snprintf(buf, RATIO_BUF, "inf");
        return buf;
    }
    int n = snprintf(buf, RATIO_BUF, "%u", num / den);
    uint32_t rem = num % den;
    if (decimals > 0 && n < RATIO_BUF - 1) {
        buf[n++] = '.';
    }
    /* Long division, one digit at a time: rem < den, so rem * 10 fits. */
    for (unsigned i = 0; i < decimals && n < RATIO_BUF - 1; i++) {
        rem *= 10;
        buf[n++] = (char)('0' + rem / den);
        rem %= den;
    }
    buf[n] = '\0';
    return buf;
}
