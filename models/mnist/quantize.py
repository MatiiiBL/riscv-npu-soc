"""Post-training INT8 quantization of the MLP, matching the NPU arithmetic.

Scheme (symmetric, per layer, zero point 0 everywhere):

  real value  r = s * q
  input       s_x = 1/127              pixels / 255 -> 0..127
  weights     s_w = max|W| / 127       q_w in -127..127
  bias        q_b = round(b / (s_in * s_w))   (INT32, added to the accumulator)
  hidden      s_h = max over the calibration set of the float ReLU output / 127

A layer's INT32 accumulator has scale s_in * s_w, so producing an INT8 output
of scale s_out needs one rescale by M = s_in * s_w / s_out < 1. The NPU
implements it as  (acc * QMULT + 2^(SHIFT-1)) >> SHIFT  with a signed 16-bit
QMULT, so M is approximated by QMULT / 2^SHIFT with the largest SHIFT that
keeps QMULT in range. The output layer keeps INT32 logits (no rescale): the
argmax only needs their order.
"""

import numpy as np

import int8_ref

QMULT_MAX = 2**15 - 1
SHIFT_MAX = 31


def qmult_shift(m):
    """Best (QMULT, SHIFT) approximation of a positive multiplier m."""
    if not 0 < m < QMULT_MAX:
        raise ValueError(f"multiplier {m} out of range")
    shift = SHIFT_MAX
    while round(m * 2**shift) > QMULT_MAX:
        shift -= 1
    return round(m * 2**shift), shift


def quantize_weights(w):
    scale = float(np.abs(w).max()) / 127
    return np.clip(np.round(w / scale), -127, 127).astype(np.int8), scale


def quantize_model(fp32, calib_pixels):
    """Quantize a float model {w1, b1, w2, b2}; calib_pixels are uint8 images."""
    s_x = 1.0 / 127
    w1_q, s_w1 = quantize_weights(fp32["w1"])
    w2_q, s_w2 = quantize_weights(fp32["w2"])

    # Calibrate the hidden scale on what the float layer sees after input
    # quantization, so the INT8 path and the calibration agree on inputs.
    x = int8_ref.quantize_input(calib_pixels).astype(np.float32) * s_x
    hidden = np.maximum(x @ fp32["w1"].T + fp32["b1"], 0)
    s_h = float(hidden.max()) / 127

    qmult1, shift1 = qmult_shift(s_x * s_w1 / s_h)
    return {
        "w1": w1_q,
        "b1": np.round(fp32["b1"] / (s_x * s_w1)).astype(np.int32),
        "qmult1": np.int32(qmult1),
        "shift1": np.int32(shift1),
        "w2": w2_q,
        "b2": np.round(fp32["b2"] / (s_h * s_w2)).astype(np.int32),
        "s_x": np.float64(s_x),
        "s_w1": np.float64(s_w1),
        "s_h": np.float64(s_h),
        "s_w2": np.float64(s_w2),
    }
