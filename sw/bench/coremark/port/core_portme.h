/*
 * CoreMark port for the Pulse SoC: bare metal, a single context, the data
 * block in static DMEM, timing from the cycle counter and output through the
 * runtime's printf. See core_portme.c for how the run is validated.
 */
#ifndef CORE_PORTME_H
#define CORE_PORTME_H

#include <stddef.h>
#include <stdint.h>
#include "soc.h"

/* Number of benchmark iterations; override with make ITERATIONS=n. */
#ifndef ITERATIONS
#define ITERATIONS 10
#endif

/* Nominal clock, only used for CoreMark's whole-seconds printout. The
 * reported CoreMark/MHz comes straight from cycles and does not depend on
 * it. 25 MHz is the ULX3S board oscillator. */
#ifndef CLOCK_HZ
#define CLOCK_HZ 25000000u
#endif

/* The standard "2K performance run" (seeds 0, 0, 0x66), whose CRCs
 * core_portme.c checks. */
#ifndef PERFORMANCE_RUN
#define PERFORMANCE_RUN 1
#endif
#if !PERFORMANCE_RUN || defined(VALIDATION_RUN) || defined(PROFILE_RUN)
#error "this port only supports the 2K performance run"
#endif

#define HAS_FLOAT         0
#define HAS_TIME_H        0
#define USE_CLOCK         0
#define HAS_STDIO         0
#define HAS_PRINTF        1 /* ee_printf is the runtime's printf */
#define MAIN_HAS_NOARGC   1
#define MAIN_HAS_NORETURN 0
#define SEED_METHOD       SEED_VOLATILE
#define MEM_METHOD        MEM_STATIC
#define MULTITHREAD       1
#define USE_PTHREAD       0
#define USE_FORK          0
#define USE_SOCKET        0

#define COMPILER_VERSION "clang " __clang_version__
#ifndef FLAGS_STR
#define FLAGS_STR "unknown"
#endif
#define COMPILER_FLAGS FLAGS_STR
#define MEM_LOCATION   "static, DMEM"

typedef int16_t   ee_s16;
typedef uint16_t  ee_u16;
typedef int32_t   ee_s32;
typedef uint8_t   ee_u8;
typedef uint32_t  ee_u32;
typedef uintptr_t ee_ptr_int;
typedef size_t    ee_size_t;

/* Aligns an address up to the next 32-bit boundary (used by the matrix
 * kernel to place its operands). */
#define align_mem(x) (void *)(4 + (((ee_ptr_int)(x)-1) & ~3))

#define CORETIMETYPE ee_u32
typedef ee_u32 CORE_TICKS;

extern ee_u32 default_num_contexts;

typedef struct CORE_PORTABLE_S {
    ee_u8 portable_id;
} core_portable;

void portable_init(core_portable *p, int *argc, char *argv[]);
void portable_fini(core_portable *p);

#endif /* CORE_PORTME_H */
