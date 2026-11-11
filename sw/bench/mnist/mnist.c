/*
 * MNIST benchmark: the INT8 MLP from models/mnist run over the exported test
 * images twice, once with the software kernels and once on the NPU, in
 * batches of ARR images so that the NPU's output tiles are full (layer 1 is
 * a batch x 784 by 128 x 784 GEMM). Both paths must agree bit for bit on
 * every hidden activation and logit, and their predictions must equal those
 * of the Python INT8 reference (models/mnist/int8_ref.py) on every image.
 *
 * Cycle counts are only meaningful on the RTL simulator: the ISS runs NPU
 * commands instantly and its cycle counter is not a timing model.
 */
#include "bench.h"
#include "model.h"
#include "npu.h"
#include "soc.h"
#include "testset.h"

#define MAX_BATCH 16

static const linear_t layer1 = {
    .w = &mnist_w1[0][0],
    .bias = mnist_b1,
    .in = MNIST_IN,
    .out = MNIST_HIDDEN,
    .quant = NPU_QUANT_OUT_INT8 | NPU_QUANT_RELU | NPU_QUANT_SHIFT(MNIST_SHIFT1),
    .qmult = MNIST_QMULT1,
};

/* Logits stay INT32: only their order matters. */
static const linear_t layer2 = {
    .w = &mnist_w2[0][0],
    .bias = mnist_b2,
    .in = MNIST_HIDDEN,
    .out = MNIST_OUT,
    .quant = 0,
    .qmult = 1,
};

#define LOGIT_STRIDE (MNIST_OUT * 4)

static int8_t hidden_sw[MAX_BATCH][MNIST_HIDDEN] __attribute__((aligned(4)));
static int8_t hidden_npu[MAX_BATCH][MNIST_HIDDEN] __attribute__((aligned(4)));
static int32_t logits_sw[MAX_BATCH][MNIST_OUT] __attribute__((aligned(4)));
static int32_t logits_npu[MAX_BATCH][MNIST_OUT] __attribute__((aligned(4)));

/* First index of the largest logit: the same tie rule as numpy's argmax. */
static uint32_t argmax(const int32_t *v) {
    uint32_t best = 0;
    for (uint32_t i = 1; i < MNIST_OUT; i++) {
        if (v[i] > v[best]) {
            best = i;
        }
    }
    return best;
}

typedef struct {
    uint32_t layer1, layer2; /* cycles */
    uint32_t correct;        /* predictions equal to the label            */
    uint32_t match_python;   /* predictions equal to int8_ref.py's        */
} path_stats_t;

static void print_path(const char *name, const path_stats_t *s, uint32_t busy) {
    uint32_t total = s->layer1 + s->layer2;
    char acc[RATIO_BUF], per_inf[RATIO_BUF];
    fmt_ratio(acc, s->correct * 100u, MNIST_TEST_COUNT, 2);
    fmt_ratio(per_inf, total, MNIST_TEST_COUNT, 0);
    printf("%3s: %u/%u correct (%s%%), %s cycles/inference (layer 1 %u, layer 2 %u cycles total)\n",
           name, s->correct, (uint32_t)MNIST_TEST_COUNT, acc, per_inf, s->layer1, s->layer2);
    printf("BENCH mnist %s images=%u correct=%u accuracy=%s match_python=%u cycles=%u "
           "cycles_per_inference=%s layer1_cycles=%u layer2_cycles=%u npu_busy_cycles=%u\n",
           name, (uint32_t)MNIST_TEST_COUNT, s->correct, acc, s->match_python, total, per_inf,
           s->layer1, s->layer2, busy);
}

int main(void) {
    npu_info_t info = npu_info();
    uint32_t batch = info.arr < MAX_BATCH ? info.arr : MAX_BATCH;
    path_stats_t sw = {0}, npu = {0};
    uint32_t batches = 0, mismatched_batches = 0, npu_errors = 0;

    printf("mnist: %u-%u-%u INT8 MLP, %u test images, batches of %u\n", (uint32_t)MNIST_IN,
           (uint32_t)MNIST_HIDDEN, (uint32_t)MNIST_OUT, (uint32_t)MNIST_TEST_COUNT, batch);
    npu_counters_reset();

    for (uint32_t first = 0; first < MNIST_TEST_COUNT; first += batch) {
        uint32_t n = MNIST_TEST_COUNT - first < batch ? MNIST_TEST_COUNT - first : batch;
        const int8_t *x = mnist_test_images[first];

        uint32_t t0 = rdcycle();
        sw_linear(&layer1, x, MNIST_IN, hidden_sw, MNIST_HIDDEN, n);
        uint32_t t1 = rdcycle();
        sw_linear(&layer2, hidden_sw[0], MNIST_HIDDEN, logits_sw, LOGIT_STRIDE, n);
        uint32_t t2 = rdcycle();
        npu_errors += npu_linear(&layer1, x, MNIST_IN, hidden_npu, MNIST_HIDDEN, n) != NPU_OK;
        uint32_t t3 = rdcycle();
        npu_errors += npu_linear(&layer2, hidden_npu[0], MNIST_HIDDEN, logits_npu, LOGIT_STRIDE, n) != NPU_OK;
        uint32_t t4 = rdcycle();

        sw.layer1 += t1 - t0;
        sw.layer2 += t2 - t1;
        npu.layer1 += t3 - t2;
        npu.layer2 += t4 - t3;
        batches++;
        if (memcmp(hidden_sw, hidden_npu, n * MNIST_HIDDEN) != 0 ||
            memcmp(logits_sw, logits_npu, n * LOGIT_STRIDE) != 0) {
            mismatched_batches++;
        }

        for (uint32_t i = 0; i < n; i++) {
            uint32_t label = mnist_test_labels[first + i];
            uint32_t want = mnist_test_expected[first + i];
            uint32_t p_sw = argmax(logits_sw[i]);
            uint32_t p_npu = argmax(logits_npu[i]);
            sw.correct += p_sw == label;
            npu.correct += p_npu == label;
            sw.match_python += p_sw == want;
            npu.match_python += p_npu == want;
        }
    }

    print_path("sw", &sw, 0);
    print_path("npu", &npu, npu_busy_cycles());

    char speedup[RATIO_BUF];
    fmt_ratio(speedup, sw.layer1 + sw.layer2, npu.layer1 + npu.layer2, 2);
    int ok = mismatched_batches == 0 && npu_errors == 0 && sw.match_python == MNIST_TEST_COUNT &&
             npu.match_python == MNIST_TEST_COUNT;
    printf("software vs NPU: %u of %u batches bit-exact (hidden activations and logits)\n",
           batches - mismatched_batches, batches);
    printf("predictions equal to the Python INT8 model: sw %u/%u, npu %u/%u\n", sw.match_python,
           (uint32_t)MNIST_TEST_COUNT, npu.match_python, (uint32_t)MNIST_TEST_COUNT);
    printf("speedup: %sx\n", speedup);
    printf("BENCH mnist summary images=%u batch=%u speedup=%s bit_exact=%d match_python=%d\n",
           (uint32_t)MNIST_TEST_COUNT, batch, speedup, mismatched_batches == 0 && npu_errors == 0,
           sw.match_python == MNIST_TEST_COUNT && npu.match_python == MNIST_TEST_COUNT);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
