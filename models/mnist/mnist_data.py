"""MNIST download and loading (numpy only).

The four IDX files are fetched into models/mnist/data/ on first use and
verified against their SHA-256 digests (the same files whose MD5s torchvision
and TensorFlow publish), so a truncated download or a tampered mirror fails
loudly instead of training on garbage.
"""

import gzip
import hashlib
import pathlib
import shutil
import subprocess
import urllib.request

import numpy as np

DATA_DIR = pathlib.Path(__file__).resolve().parent / "data"

MIRRORS = (
    "https://ossci-datasets.s3.amazonaws.com/mnist/",
    "https://storage.googleapis.com/cvdf-datasets/mnist/",
)

FILES = {
    "train-images-idx3-ubyte.gz": "440fcabf73cc546fa21475e81ea370265605f56be210a4024d2ca8f203523609",
    "train-labels-idx1-ubyte.gz": "3552534a0a558bbed6aed32b30c495cca23d567ec52cac8be1a0730e8010255c",
    "t10k-images-idx3-ubyte.gz": "8d422c7b0a1c1c79245a5bcf07fe86e33eeafee792b84584aec276f5a2dbc4e6",
    "t10k-labels-idx1-ubyte.gz": "f7ae60f92e00ec6debd23a6088c31dbd2371eca3ffa0defaefb259924204aec6",
}


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _download(url, dest):
    """Fetch url into dest with urllib, falling back to curl.

    Some Python builds (e.g. the python.org macOS installer) come without a CA
    bundle, so urllib cannot verify TLS; curl uses the system trust store. The
    SHA-256 check afterwards makes either path safe.
    """
    try:
        with urllib.request.urlopen(url, timeout=60) as resp, open(dest, "wb") as out:
            shutil.copyfileobj(resp, out)
    except OSError as e:
        curl = shutil.which("curl")
        if curl is None:
            raise
        result = subprocess.run([curl, "-fsSL", "--max-time", "300", "-o", str(dest), url], check=False)
        if result.returncode != 0:
            raise OSError(f"urllib: {e}; curl exited with {result.returncode}") from e


def _fetch(name, digest):
    path = DATA_DIR / name
    if path.exists() and _sha256(path) == digest:
        return path
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".part")
    errors = []
    for mirror in MIRRORS:
        try:
            _download(mirror + name, tmp)
        except OSError as e:
            errors.append(f"{mirror}: {e}")
            continue
        actual = _sha256(tmp)
        if actual == digest:
            tmp.replace(path)
            return path
        errors.append(f"{mirror}: checksum mismatch (got {actual})")
    tmp.unlink(missing_ok=True)
    raise RuntimeError(f"could not download {name}:\n  " + "\n  ".join(errors))


def _read_idx(path):
    with gzip.open(path, "rb") as f:
        raw = f.read()
    ndim = raw[3]
    shape = tuple(int.from_bytes(raw[4 + 4 * i : 8 + 4 * i], "big") for i in range(ndim))
    return np.frombuffer(raw, dtype=np.uint8, offset=4 + 4 * ndim).reshape(shape)


def load():
    """Return (train_x, train_y, test_x, test_y); images are uint8 N x 784."""
    arrays = [_read_idx(_fetch(name, digest)) for name, digest in FILES.items()]
    train_x, train_y, test_x, test_y = arrays
    return (
        train_x.reshape(len(train_x), -1),
        train_y.astype(np.int64),
        test_x.reshape(len(test_x), -1),
        test_y.astype(np.int64),
    )
