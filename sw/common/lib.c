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

/*
 * Formatter output: the UART when buf is NULL, otherwise a bounded string
 * buffer. len counts every character produced, even those that did not fit,
 * which is the return value snprintf needs.
 */
typedef struct {
    char *buf;
    size_t size;
    size_t len;
} sink_t;

static void emit(sink_t *s, char c) {
    if (s->buf == NULL) {
        putchar(c);
    } else if (s->len + 1 < s->size) {
        s->buf[s->len] = c;
    }
    s->len++;
}

/* Prints v in `base`, preceded by '-' if negative, right-aligned in `width`
 * columns. Space padding goes before the sign, zero padding after it. */
static void emit_number(sink_t *s, uint32_t v, unsigned base, int negative, int width, char pad,
                        int upper) {
    char buf[12];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        buf[n++] = digits[v % base];
        v /= base;
    } while (v != 0);
    int fill = width - n - (negative ? 1 : 0);
    if (negative && pad == '0') {
        emit(s, '-');
    }
    for (; fill > 0; fill--) {
        emit(s, pad);
    }
    if (negative && pad != '0') {
        emit(s, '-');
    }
    while (n > 0) {
        emit(s, buf[--n]);
    }
}

/*
 * Supports %d %i %u %x %X %c %s %p %% with an optional '0' flag, a width and
 * an 'l' length modifier (long is 32 bits on ilp32, so it changes nothing).
 */
static void format(sink_t *s, const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit(s, *fmt);
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
        if (*fmt == 'l') {
            fmt++;
        }
        switch (*fmt) {
        case 'd':
        case 'i': {
            int32_t v = va_arg(ap, int32_t);
            uint32_t mag = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
            emit_number(s, mag, 10, v < 0, width, pad, 0);
            break;
        }
        case 'u':
            emit_number(s, va_arg(ap, uint32_t), 10, 0, width, pad, 0);
            break;
        case 'x':
        case 'X':
            emit_number(s, va_arg(ap, uint32_t), 16, 0, width, pad, *fmt == 'X');
            break;
        case 'p':
            emit(s, '0');
            emit(s, 'x');
            emit_number(s, (uint32_t)(uintptr_t)va_arg(ap, void *), 16, 0, 8, '0', 0);
            break;
        case 'c':
            emit(s, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *str = va_arg(ap, const char *);
            int len = (int)strlen(str);
            while (len < width--) {
                emit(s, ' ');
            }
            while (*str) {
                emit(s, *str++);
            }
            break;
        }
        case '%':
            emit(s, '%');
            break;
        case '\0':
            return;
        default:
            emit(s, '%');
            emit(s, *fmt);
            break;
        }
    }
}

int printf(const char *fmt, ...) {
    sink_t s = {NULL, 0, 0};
    va_list ap;
    va_start(ap, fmt);
    format(&s, fmt, ap);
    va_end(ap);
    return (int)s.len;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    sink_t s = {buf, size, 0};
    format(&s, fmt, ap);
    if (size > 0) {
        buf[s.len < size ? s.len : size - 1] = '\0';
    }
    return (int)s.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
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

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
