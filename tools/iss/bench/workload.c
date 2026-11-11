/*
 * ISS speed benchmark: a few hundred million instructions of mixed integer
 * work (bitwise CRC, matrix multiply, division, sorting) so the measured
 * MIPS reflects ALU, branch, load/store and M-extension instructions alike.
 * Prints a checksum of every result, which the RTL must reproduce.
 */
#include "soc.h"

#define N_CRC  2048
#define N_MAT  24
#define N_SORT 256
#define ROUNDS 1400

static uint8_t crc_buf[N_CRC];
static int32_t mat_a[N_MAT][N_MAT], mat_b[N_MAT][N_MAT], mat_c[N_MAT][N_MAT];
static uint32_t sort_buf[N_SORT];

static uint32_t lcg(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static uint32_t crc32(const uint8_t *p, uint32_t n) {
    uint32_t crc = 0xffffffffu;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1u));
        }
    }
    return ~crc;
}

static uint32_t matmul(void) {
    uint32_t sum = 0;
    for (int i = 0; i < N_MAT; i++) {
        for (int j = 0; j < N_MAT; j++) {
            int32_t acc = 0;
            for (int k = 0; k < N_MAT; k++) {
                acc += mat_a[i][k] * mat_b[k][j];
            }
            mat_c[i][j] = acc;
            sum += (uint32_t)acc;
        }
    }
    return sum;
}

static uint32_t divide(uint32_t seed) {
    uint32_t sum = 0;
    for (uint32_t i = 1; i < 4000; i++) {
        sum += (seed / i) + (seed % (i + 7)) + (uint32_t)((int32_t)(seed - i * 977u) / -13);
    }
    return sum;
}

static uint32_t sort(uint32_t *state) {
    for (int i = 0; i < N_SORT; i++) {
        sort_buf[i] = lcg(state);
    }
    for (int i = 1; i < N_SORT; i++) {
        uint32_t v = sort_buf[i];
        int j = i - 1;
        while (j >= 0 && sort_buf[j] > v) {
            sort_buf[j + 1] = sort_buf[j];
            j--;
        }
        sort_buf[j + 1] = v;
    }
    return sort_buf[0] ^ sort_buf[N_SORT / 2] ^ sort_buf[N_SORT - 1];
}

int main(void) {
    uint32_t state = 12345;
    for (int i = 0; i < N_CRC; i++) {
        crc_buf[i] = (uint8_t)lcg(&state);
    }
    for (int i = 0; i < N_MAT; i++) {
        for (int j = 0; j < N_MAT; j++) {
            mat_a[i][j] = (int32_t)(lcg(&state) % 201) - 100;
            mat_b[i][j] = (int32_t)(lcg(&state) % 201) - 100;
        }
    }
    uint32_t check = 0;
    for (int r = 0; r < ROUNDS; r++) {
        crc_buf[r] ^= (uint8_t)r;
        check = check * 31u + crc32(crc_buf, N_CRC);
        check = check * 31u + matmul();
        check = check * 31u + divide(lcg(&state));
        check = check * 31u + sort(&state);
    }
    printf("workload checksum %08x\n", check);
    return 0;
}
