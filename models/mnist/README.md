# MNIST MLP for the Pulse NPU

A 784-128-10 multilayer perceptron with a ReLU hidden layer, trained in
float32 with numpy alone, quantized to INT8 after training, and exported as C
headers for `sw/bench/mnist`.

| Model                                   | Test accuracy (10,000 images) |
|-----------------------------------------|-------------------------------|
| float32 (`train.py`)                    | **98.06%** |
| INT8, bit-exact NPU math (`int8_ref.py`) | **98.07%** |

The INT8 model also gets all of the first 100 test images right (these
100 images are the ones the firmware benchmark runs). INT8 coming out
0.01 points above float is noise: one more image lands on the right side
of the decision boundary.

## Reproducing

```sh
make venv                       # from the repo root: .venv with numpy
cd models/mnist
../../.venv/bin/python train.py     # downloads MNIST, trains, quantizes (~10 s)
../../.venv/bin/python int8_ref.py  # INT8 accuracy on the full test set
../../.venv/bin/python export.py    # writes sw/bench/mnist/{model,testset}.h
```

`train.py` fetches the four IDX files into `models/mnist/data/` (not in git)
from the first mirror that responds and checks their SHA-256 digests.
Training is seeded (`--seed 0`), so a rerun on the same machine produces the
same weights bit for bit. A different BLAS library can change float rounding
and therefore the exact weights; the generated headers are committed, so
firmware builds never depend on this.

Training: He-normal init, Adam (lr 1e-3, cosine decay to 0), batch 128,
20 epochs, L2 weight decay 1e-4, inputs `pixel / 255`.

## Quantization

Symmetric, per layer, zero point 0 (`quantize.py`):

| Tensor        | Encoding |
|---------------|----------|
| input         | `round(pixel * 127 / 255)`, scale `s_x = 1/127`, range 0..127 |
| weights       | `round(W / s_w)` with `s_w = max|W| / 127`, range -127..127 |
| biases        | INT32 `round(b / (s_in * s_w))`, so they add straight into the accumulator |
| hidden        | INT8, scale `s_h` = max float ReLU output on 10,000 training images / 127 |
| logits        | INT32 accumulator plus bias; only their order matters (argmax) |

The hidden layer's accumulator has scale `s_x * s_w1` and must become INT8
with scale `s_h`, a rescale by `M = s_x * s_w1 / s_h`. The NPU applies
`(acc * QMULT + 2^(SHIFT-1)) >> SHIFT` with a signed 16-bit `QMULT`
(docs/spec.md section 5.1), so `M` is represented by the largest `SHIFT`
that keeps `QMULT` within 16 bits: here `QMULT = 16464`, `SHIFT = 25`
(relative error 2e-5).

Each layer maps onto a single NPU operation (tiled by the driver):

| Layer | A (M x K) | B (N x K) | Bias | QUANT |
|-------|-----------|-----------|------|-------|
| 1 | images, batch x 784 | `w1`, 128 x 784 | `b1` | `SHIFT=25 \| OUT_INT8 \| RELU`, `QMULT=16464` |
| 2 | hidden, batch x 128 | `w2`, 10 x 128  | `b2` | INT32 output, no ReLU |

`int8_ref.py` runs exactly this integer pipeline (int64 numpy arithmetic,
wrapped to INT32 where the hardware wraps), so its predictions are the
reference that the firmware's software kernels and the NPU must match on
every image. The prediction is the first index of the largest logit, the
same tie rule as `np.argmax`.

## Firmware data size

| Header      | Contents | Bytes in DMEM |
|-------------|----------|---------------|
| `model.h`   | `w1` 100,352 + `b1` 512 + `w2` 1,280 + `b2` 40 | 102,184 |
| `testset.h` | 100 images x 784 + labels + expected predictions | 78,600 |

Together they fill about 70% of the 256 KiB DMEM, which leaves room for the
stack and the activation buffers.

## Files

| File | Purpose |
|------|---------|
| `mnist_data.py` | download, checksum and IDX parsing |
| `train.py`      | float training, then quantization; writes `data/mlp_fp32.npz`, `data/mlp_int8.npz` |
| `quantize.py`   | post-training quantization (scales, INT32 biases, QMULT/SHIFT) |
| `int8_ref.py`   | bit-exact integer inference, INT8 accuracy on the full test set |
| `export.py`     | writes the C headers for `sw/bench/mnist` |
