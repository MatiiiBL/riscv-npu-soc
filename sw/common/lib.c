/* Minimal freestanding libc subset: enough for tests and benchmarks. */
#include <stdarg.h>
#include "soc.h"

int putchar(int c) {
    uart_putc((char)c);
    return c;
}

int puts(const char *s) {
    while (*s) {
        putchar(*s++);
    }
    putchar('\n');
    return 0;
}

static void print_uint(uint32_t v, unsigned base, int width, char pad, int upper) {
    char buf[12];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        buf[n++] = digits[v % base];
        v /= base;
    } while (v != 0);
    while (n < width--) {
        putchar(pad);
    }
    while (n > 0) {
        putchar(buf[--n]);
    }
}

/* Supports %d %i %u %x %X %c %s %p %% with optional '0' flag and width. */
int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            putchar(*fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        switch (*fmt) {
        case 'd':
        case 'i': {
            int32_t v = va_arg(ap, int32_t);
            if (v < 0) {
                putchar('-');
                print_uint(0u - (uint32_t)v, 10, width > 0 ? width - 1 : 0, pad, 0);
            } else {
                print_uint((uint32_t)v, 10, width, pad, 0);
            }
            break;
        }
        case 'u':
            print_uint(va_arg(ap, uint32_t), 10, width, pad, 0);
            break;
        case 'x':
        case 'X':
            print_uint(va_arg(ap, uint32_t), 16, width, pad, *fmt == 'X');
            break;
        case 'p':
            putchar('0');
            putchar('x');
            print_uint((uint32_t)(uintptr_t)va_arg(ap, void *), 16, 8, '0', 0);
            break;
        case 'c':
            putchar(va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            int len = (int)strlen(s);
            while (len < width--) {
                putchar(' ');
            }
            while (*s) {
                putchar(*s++);
            }
            break;
        }
        case '%':
            putchar('%');
            break;
        case '\0':
            va_end(ap);
            return 0;
        default:
            putchar('%');
            putchar(*fmt);
            break;
        }
    }
    va_end(ap);
    return 0;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = dst;
    while (n--) {
        *d++ = (uint8_t)c;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a;
    const uint8_t *y = b;
    for (; n; n--, x++, y++) {
        if (*x != *y) {
            return *x - *y;
        }
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}
