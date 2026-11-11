"""Train the MNIST MLP (784-HIDDEN-10, ReLU) in float32 with numpy only, then
quantize it to INT8 (see quantize.py).

Writes models/mnist/data/mlp_fp32.npz and mlp_int8.npz. Everything is seeded,
so a rerun on the same machine reproduces the same weights bit for bit.

Usage: python train.py [--hidden 128] [--epochs 20] [--seed 0]
"""

import argparse

import numpy as np

import int8_ref
import mnist_data
import quantize

CALIB_IMAGES = 10000


def init_params(rng, sizes):
    """He-normal weights in the NPU's (out x in) layout, zero biases."""
    params = {}
    for i, (n_in, n_out) in enumerate(zip(sizes[:-1], sizes[1:]), start=1):
        params[f"w{i}"] = (rng.standard_normal((n_out, n_in)) * np.sqrt(2.0 / n_in)).astype(np.float32)
        params[f"b{i}"] = np.zeros(n_out, dtype=np.float32)
    return params


def forward(p, x):
    z1 = x @ p["w1"].T + p["b1"]
    h = np.maximum(z1, 0)
    return z1, h, h @ p["w2"].T + p["b2"]


def gradients(p, x, y, weight_decay):
    z1, h, logits = forward(p, x)
    logits = logits - logits.max(axis=1, keepdims=True)
    prob = np.exp(logits)
    prob /= prob.sum(axis=1, keepdims=True)
    loss = -np.log(prob[np.arange(len(y)), y] + 1e-12).mean()

    d_logits = prob
    d_logits[np.arange(len(y)), y] -= 1
    d_logits /= len(y)
    d_z1 = (d_logits @ p["w2"]) * (z1 > 0)
    grads = {
        "w2": d_logits.T @ h + weight_decay * p["w2"],
        "b2": d_logits.sum(axis=0),
        "w1": d_z1.T @ x + weight_decay * p["w1"],
        "b1": d_z1.sum(axis=0),
    }
    return loss, grads


def accuracy(p, x, y):
    return float((np.argmax(forward(p, x)[2], axis=1) == y).mean())


def train(args, train_x, train_y, test_x, test_y):
    rng = np.random.default_rng(args.seed)
    p = init_params(rng, (train_x.shape[1], args.hidden, 10))
    m = {k: np.zeros_like(v) for k, v in p.items()}
    v = {k: np.zeros_like(v) for k, v in p.items()}
    beta1, beta2, eps = 0.9, 0.999, 1e-8
    steps_per_epoch = -(-len(train_x) // args.batch)
    total_steps = args.epochs * steps_per_epoch
    step = 0
    for epoch in range(args.epochs):
        order = rng.permutation(len(train_x))
        losses = []
        for start in range(0, len(order), args.batch):
            idx = order[start : start + args.batch]
            loss, grads = gradients(p, train_x[idx], train_y[idx], args.weight_decay)
            losses.append(loss)
            step += 1
            # Cosine learning-rate decay to zero over the whole run.
            lr = args.lr * 0.5 * (1 + np.cos(np.pi * step / total_steps))
            for k in p:
                m[k] = beta1 * m[k] + (1 - beta1) * grads[k]
                v[k] = beta2 * v[k] + (1 - beta2) * grads[k] ** 2
                m_hat = m[k] / (1 - beta1**step)
                v_hat = v[k] / (1 - beta2**step)
                p[k] = (p[k] - lr * m_hat / (np.sqrt(v_hat) + eps)).astype(np.float32)
        print(
            f"epoch {epoch + 1:2d}/{args.epochs}  loss {np.mean(losses):.4f}  "
            f"test {100 * accuracy(p, test_x, test_y):.2f}%"
        )
    return p


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--hidden", type=int, default=128)
    ap.add_argument("--epochs", type=int, default=20)
    ap.add_argument("--batch", type=int, default=128)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--weight-decay", type=float, default=1e-4)
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    train_px, train_y, test_px, test_y = mnist_data.load()
    train_x = train_px.astype(np.float32) / 255
    test_x = test_px.astype(np.float32) / 255

    fp32 = train(args, train_x, train_y, test_x, test_y)
    float_acc = accuracy(fp32, test_x, test_y)

    int8 = quantize.quantize_model(fp32, train_px[:CALIB_IMAGES])
    int8_acc = float((int8_ref.predict(int8, int8_ref.quantize_input(test_px)) == test_y).mean())

    mnist_data.DATA_DIR.mkdir(parents=True, exist_ok=True)
    np.savez(mnist_data.DATA_DIR / "mlp_fp32.npz", **fp32)
    np.savez(int8_ref.MODEL_PATH, **int8)
    print(f"float accuracy: {100 * float_acc:.2f}%")
    print(f"INT8 accuracy:  {100 * int8_acc:.2f}%  (QMULT1={int(int8['qmult1'])}, SHIFT1={int(int8['shift1'])})")


if __name__ == "__main__":
    main()
