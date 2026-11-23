"""The one numeric primitive that can use a GPU: masked-SSD patch scoring.

Everything in this package is numpy except the score map, a convolution that was
written twice so large sweeps can use CUDA while CI stays on the CPU. It is small
enough to state exactly:

    score[i, j] = sum_x w(x) * (T(x) - S(x + (i, j)))^2

with three equivalent closed forms (sum wT^2 is a scalar, sum wTS and sum wS^2 are
correlations). Two implementations are provided:

  * ``"numpy"`` - float64, FFT correlation, no GPU, bit-stable across machines.
    This is the reference; the gold PNGs are recorded with it.
  * ``"torch"`` - float32, two ``conv2d`` calls, GPU when available. A pure
    accelerator; it is never the source of truth.

They are not expected to agree bitwise (float32 vs float64), only to pick the
same argmin, which is all the algorithm consumes. ``test_inpaint_units.py``
asserts that on real data and measures the residual, and skips the comparison
when torch is not installed (the CI runner has no torch, by design).

Design note: the *decision* is the algorithm's output, not the float. Any CI
strategy that protects the decision layer - "given this score map, choose this
window" - therefore protects the GPU path too, as long as the two backends agree
on argmin well below any real tie.
"""

from __future__ import annotations

import numpy as np

AVAILABLE = ("numpy", "torch")


def torch_available() -> bool:
    """True when the optional torch backend can be imported."""
    try:
        import torch  # noqa: F401
    except ImportError:
        return False
    return True


def resolve(name: str) -> str:
    """Turn "auto" into a concrete backend name.

    "auto" means "torch only if it can actually use CUDA", because on a CPU torch
    is slower than numpy/FFT. It does not mean "torch if installed".
    """
    if name not in AVAILABLE + ("auto",):
        raise ValueError(f"backend {name!r} not in {AVAILABLE + ('auto',)}")
    if name == "torch" and not torch_available():
        raise RuntimeError("backend 'torch' was requested but torch is not installed; "
                           "install torch or use backend 'numpy'")
    if name == "auto":
        if not torch_available():
            return "numpy"
        import torch
        return "torch" if torch.cuda.is_available() else "numpy"
    return name


def score_map(source: np.ndarray, target: np.ndarray, weight: np.ndarray,
              backend: str = "auto") -> np.ndarray:
    """Masked SSD between one weighted patch and every source window.

    Args:
        source: (H, W, C) float, the exemplar image.
        target: (p, p, C) float, the patch being matched. Values outside the
            weight are ignored, so they may be anything.
        weight: (p, p) float, per-pixel weight (0 = ignore, 1 = synthesised,
            >1 = given pixel). Non-binary weights are exactly what makes the
            ring outrank the already-filled area.
        backend: "auto" | "numpy" | "torch".

    Returns:
        (H-p+1, W-p+1) float64 score per top-left corner.
    """
    source = np.asarray(source)
    target = np.asarray(target)
    weight = np.asarray(weight)
    if source.ndim != 3 or target.ndim != 3 or weight.ndim != 2:
        raise ValueError(f"expected source (H,W,C), target (p,p,C), weight (p,p); "
                         f"got {source.shape}, {target.shape}, {weight.shape}")
    if target.shape[:2] != weight.shape:
        raise ValueError(f"target {target.shape[:2]} and weight {weight.shape} disagree")
    if source.shape[2] != target.shape[2]:
        raise ValueError(f"channel count {source.shape[2]} != {target.shape[2]}")
    if target.shape[0] > source.shape[0] or target.shape[1] > source.shape[1]:
        raise ValueError(f"patch {target.shape[:2]} is larger than source {source.shape[:2]}")

    backend = resolve(backend)
    if backend == "numpy":
        return _score_map_numpy(source, target, weight)
    return _score_map_torch(source, target, weight)


def window_counts(mask: np.ndarray, size: int) -> np.ndarray:
    """Exact count of True values in every (size x size) window.

    Integer arithmetic via a summed-area table, so this is identical on every
    backend and cannot flip an argmin - unlike a float ``avg_pool2d > 0.999``
    guard.
    """
    values = np.asarray(mask, dtype=np.int64)
    integral = np.zeros((values.shape[0] + 1, values.shape[1] + 1), dtype=np.int64)
    integral[1:, 1:] = values.cumsum(0).cumsum(1)
    rows = values.shape[0] - size + 1
    cols = values.shape[1] - size + 1
    if rows <= 0 or cols <= 0:
        raise ValueError(f"window {size} does not fit in {values.shape}")
    return (integral[size:size + rows, size:size + cols]
            - integral[:rows, size:size + cols]
            - integral[size:size + rows, :cols]
            + integral[:rows, :cols])


def _correlate(image: np.ndarray, kernel: np.ndarray) -> np.ndarray:
    """Valid correlation: out[i, j] = sum_uv image[i+u, j+v] * kernel[u, v]."""
    height, width = image.shape
    size = kernel.shape[0]
    shape = (height, width)
    spectrum = np.fft.rfft2(image, shape) * np.conj(np.fft.rfft2(kernel, shape))
    full = np.fft.irfft2(spectrum, shape)
    return full[:height - size + 1, :width - size + 1]


def _score_map_numpy(source: np.ndarray, target: np.ndarray,
                     weight: np.ndarray) -> np.ndarray:
    source = np.asarray(source, dtype=np.float64)
    target = np.asarray(target, dtype=np.float64)
    weight = np.asarray(weight, dtype=np.float64)

    constant = float((weight[..., None] * target * target).sum())
    cross = np.zeros(_correlate(source[..., 0], weight * target[..., 0]).shape)
    for channel in range(source.shape[2]):
        cross += _correlate(source[..., channel], weight * target[..., channel])
    energy = np.zeros(cross.shape)
    for channel in range(source.shape[2]):
        energy += _correlate(source[..., channel] ** 2, weight)
    return constant - 2.0 * cross + energy


def _score_map_torch(source: np.ndarray, target: np.ndarray,
                     weight: np.ndarray) -> np.ndarray:
    import torch
    import torch.nn.functional as functional

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    with torch.no_grad():
        src = torch.from_numpy(np.ascontiguousarray(source)).float().permute(2, 0, 1)[None].to(device)
        tgt = torch.from_numpy(np.ascontiguousarray(target)).float().permute(2, 0, 1)[None].to(device)
        mask = torch.from_numpy(np.ascontiguousarray(weight)).float()[None, None].to(device)
        cross = functional.conv2d(src, mask * tgt)
        energy = functional.conv2d((src * src).sum(dim=1, keepdim=True), mask)
        constant = (mask * tgt * tgt).sum()
        return (constant - 2.0 * cross + energy)[0, 0].double().cpu().numpy()
