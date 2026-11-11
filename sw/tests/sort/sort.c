/*
 * Sorting: quicksort and merge sort must both agree with insertion sort (too
 * simple to get wrong) on random, presorted, reversed, constant and
 * few-distinct-value inputs, and on every length from 0 to 33. Exercises
 * recursion, signed compares and dense load/store traffic.
 */
#include "rng.h"
#include "soc.h"

#define N 600

static int32_t input[N], by_insertion[N], by_quick[N], by_merge[N], scratch[N];

static void insertion_sort(int32_t *a, uint32_t n) {
    for (uint32_t i = 1; i < n; i++) {
        int32_t v = a[i];
        uint32_t j = i;
        for (; j > 0 && a[j - 1] > v; j--) {
            a[j] = a[j - 1];
        }
        a[j] = v;
    }
}

static void swap(int32_t *a, int32_t *b) {
    int32_t t = *a;
    *a = *b;
    *b = t;
}

/* Hoare partition around a median-of-three pivot; recurses into the smaller
 * side and loops on the larger one so the stack stays O(log n). */
static void quick_sort(int32_t *a, int32_t lo, int32_t hi) {
    while (lo < hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if (a[mid] < a[lo]) {
            swap(&a[mid], &a[lo]);
        }
        if (a[hi] < a[lo]) {
            swap(&a[hi], &a[lo]);
        }
        if (a[hi] < a[mid]) {
            swap(&a[hi], &a[mid]);
        }
        int32_t pivot = a[mid];
        int32_t i = lo - 1;
        int32_t j = hi + 1;
        for (;;) {
            do {
                i++;
            } while (a[i] < pivot);
            do {
                j--;
            } while (a[j] > pivot);
            if (i >= j) {
                break;
            }
            swap(&a[i], &a[j]);
        }
        if (j - lo < hi - j) {
            quick_sort(a, lo, j);
            lo = j + 1;
        } else {
            quick_sort(a, j + 1, hi);
            hi = j;
        }
    }
}

static void merge_sort(int32_t *a, int32_t *tmp, uint32_t n) {
    if (n < 2) {
        return;
    }
    uint32_t half = n / 2;
    merge_sort(a, tmp, half);
    merge_sort(a + half, tmp, n - half);
    uint32_t i = 0, j = half, k = 0;
    while (i < half && j < n) {
        tmp[k++] = (a[j] < a[i]) ? a[j++] : a[i++];
    }
    while (i < half) {
        tmp[k++] = a[i++];
    }
    while (j < n) {
        tmp[k++] = a[j++];
    }
    memcpy(a, tmp, n * sizeof *a);
}

/* Sorts input[0..n) three ways; returns 1 if all three agree and are sorted. */
static int check(uint32_t n) {
    memcpy(by_insertion, input, n * sizeof *input);
    memcpy(by_quick, input, n * sizeof *input);
    memcpy(by_merge, input, n * sizeof *input);
    insertion_sort(by_insertion, n);
    if (n > 0) {
        quick_sort(by_quick, 0, (int32_t)n - 1);
    }
    merge_sort(by_merge, scratch, n);
    for (uint32_t i = 1; i < n; i++) {
        if (by_insertion[i - 1] > by_insertion[i]) {
            return 0;
        }
    }
    return memcmp(by_quick, by_insertion, n * sizeof *input) == 0 &&
           memcmp(by_merge, by_insertion, n * sizeof *input) == 0;
}

static int report(const char *name, uint32_t n, int ok) {
    printf("%s n=%u: %s\n", name, n, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

int main(void) {
    rng_t rng = {0x5eed1234u};
    int failures = 0;
    printf("sort: quicksort and merge sort vs insertion sort\n");

    for (uint32_t i = 0; i < N; i++) {
        input[i] = (int32_t)rng_next(&rng);
    }
    failures += report("random  ", N, check(N));

    for (uint32_t i = 0; i < N; i++) {
        input[i] = (int32_t)i * 3 - 900;
    }
    failures += report("sorted  ", N, check(N));

    for (uint32_t i = 0; i < N; i++) {
        input[i] = (int32_t)(N - i) * 7;
    }
    failures += report("reversed", N, check(N));

    for (uint32_t i = 0; i < N; i++) {
        input[i] = -5;
    }
    failures += report("constant", N, check(N));

    for (uint32_t i = 0; i < N; i++) {
        input[i] = rng_range(&rng, -2, 2);
    }
    failures += report("few     ", N, check(N));

    int small_ok = 1;
    for (uint32_t n = 0; n <= 33; n++) {
        for (uint32_t i = 0; i < n; i++) {
            input[i] = rng_range(&rng, -1000, 1000);
        }
        small_ok &= check(n);
    }
    failures += report("lengths ", 33, small_ok);

    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures;
}
