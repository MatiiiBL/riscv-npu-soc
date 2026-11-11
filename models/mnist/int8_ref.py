"""Bit-exact integer inference of the quantized MNIST MLP.

This is the golden model for the firmware: it uses only integer arithmetic
and follows the NPU write-back semantics of docs/spec.md section 5.1, so the
software kernels and the NPU must reproduce its predictions exactly.

    hidden = requant(x_q @ w1^T + b1, qmult1, shift1) with ReLU   (INT8)
    logits = hidden @ w2^T + b2                                   (INT32)
    pred   = first index of max(logits)

Usage: python int8_ref.py   (needs the model written by train.py)
"""

import sys

import numpy as np

import mnist_data

MODEL_PATH = mnist_data.DATA_DIR / "mlp_int8.npz"
INT32_MIN = -(2**31)


def quantize_input(pixels):
    """Map uint8 pixels (0..255) to INT8 activations (0..127) with scale 1/127.

    round(p * 127 / 255) in integers; p * 127 / 255 never lands exactly on .5,
    so there is no tie-breaking rule to match.
    """
    p = np.asarray(pixels, dtype=np.int64)
    return ((254 * p + 255) // 510).astype(np.int8)


def wrap_int32(v):
    return ((np.asarray(v, dtype=np.int64) - INT32_MIN) % 2**32 + INT32_MIN).astype(np.int64)


def accumulate(a, b, bias):
    """INT32 accumulate a (M x K, int8) by b (N x K, int8) plus bias (N, int32)."""
    acc = a.astype(np.int64) @ b.astype(np.int64).T
    return wrap_int32(acc + bias.astype(np.int64))


def requantize(v, qmult, shift, relu):
    """NPU OUT_INT8 path: rounding multiply-shift, optional ReLU, clamp to INT8."""
    p = np.asarray(v, dtype=np.int64) * int(np.int16(qmult))
    if shift > 0:
        p = (p + (1 << (shift - 1))) >> shift
    if relu:
        p = np.maximum(p, 0)
    return np.clip(p, -128, 127).astype(np.int8)


def forward(model, x_q):
    """Run a batch of INT8 images (N x 784) and return (hidden INT8, logits INT32)."""
    hidden = requantize(
        accumulate(x_q, model["w1"], model["b1"]),
        int(model["qmult1"]),
        int(model["shift1"]),
        relu=True,
    )
    logits = accumulate(hidden, model["w2"], model["b2"])
    return hidden, logits


def predict(model, x_q):
    # np.argmax returns the first maximum, which is what the firmware does too.
    return np.argmax(forward(model, x_q)[1], axis=1)


def load_model(path=MODEL_PATH):
    if not path.exists():
        sys.exit(f"{path} not found: run train.py first")
    return dict(np.load(path))


def main():
    model = load_model()
    _, _, test_x, test_y = mnist_data.load()
    pred = predict(model, quantize_input(test_x))
    correct = int((pred == test_y).sum())
    print(f"INT8 accuracy: {correct}/{len(test_y)} = {100.0 * correct / len(test_y):.2f}%")


if __name__ == "__main__":
    main()
