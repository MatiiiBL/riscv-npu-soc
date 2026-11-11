/* Bare-metal helpers for the Pulse SoC. */
#ifndef PULSE_SOC_H
#define PULSE_SOC_H

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include "soc_map.h"

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

static inline void uart_putc(char c) {
    while ((REG32(UART_STATUS) & 1u) == 0) {
    }
    REG32(UART_TX) = (uint8_t)c;
}

static inline __attribute__((noreturn)) void sim_exit(uint32_t code) {
    REG32(SIM_EXIT) = code;
    for (;;) {
    }
}

static inline uint32_t rdcycle(void) {
    uint32_t v;
    __asm__ volatile("rdcycle %0" : "=r"(v));
    return v;
}

static inline uint32_t rdinstret(void) {
    uint32_t v;
    __asm__ volatile("rdinstret %0" : "=r"(v));
    return v;
}

/* 64-bit cycle counter, safe against low-word rollover. */
static inline uint64_t rdcycle64(void) {
    uint32_t hi, lo, hi2;
    do {
        __asm__ volatile("rdcycleh %0" : "=r"(hi));
        __asm__ volatile("rdcycle %0" : "=r"(lo));
        __asm__ volatile("rdcycleh %0" : "=r"(hi2));
    } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}

/* lib.c */
int putchar(int c);
int puts(const char *s);
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) __attribute__((format(printf, 3, 0)));
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);

#endif /* PULSE_SOC_H */
