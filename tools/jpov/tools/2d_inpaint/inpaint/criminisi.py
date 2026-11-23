"""Criminisi exemplar-based inpainting (Criminisi et al., 2004), numpy only.

Fills the hole by repeatedly pasting the most "urgent" boundary patch with the
best matching known-or-already-filled patch. Urgency is confidence x isophote
strength, which is what makes linear structures (stone joints, in our photo) run
into the hole instead of being cut off at the boundary.

Everything the priority needs is recomputed from the *current* state each step,
exactly as in the paper:

  * the boundary normal ``n_p`` comes from the current known-pixel mask, because
    the front moves inward as patches are pasted;
  * the image gradient ``grad I`` comes from the current estimate (or, optionally,
    from the low-frequency hint, which trades structure for boundary fidelity);
  * ``alpha`` normalises the data term for the image's numeric range - 255 for
    8-bit images, 1.0 for the [0, 1] floats used here.

Freezing any of those three (an earlier version froze all three) silently turns
the method into a confidence-only fill: the frozen mask has no gradient inside
the hole, so the data term is exactly zero for every pixel the front reaches
after the first layer, and the priority degenerates to confidence alone.

Why numpy and not torch: this method is *greedy*. The patch order is an argmax
over the whole hole, so a 1e-6 difference in one priority can reorder two pastes
and cascade into a different fill. A CUDA run and a CPU run of the same code can
therefore differ by 20-28 % of pixels, which means a GPU gold test would be
meaningless. Keeping one deterministic CPU implementation is the honest choice;
the gold PNG is recorded from it.
"""

from __future__ import annotations

import numpy as np

from . import backends

# Sobel kernels. The 1/8 normalisation makes the data term independent of the
# kernel's arbitrary scale; only the ratio to `data_epsilon` matters.
_SOBEL_X = np.array([[-1.0, 0.0, 1.0],
                     [-2.0, 0.0, 2.0],
                     [-1.0, 0.0, 1.0]], dtype=np.float64) / 8.0
_SOBEL_Y = _SOBEL_X.T


def _sobel_gradient(image: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Sobel gradient of an (H, W, C) image with zero padding, same shape out."""
    height, width = image.shape[:2]
    padded = np.pad(image, ((1, 1), (1, 1), (0, 0)), mode="constant")
    grad_x = np.zeros_like(image)
    grad_y = np.zeros_like(image)
    for row in range(3):
        for col in range(3):
            window = padded[row:row + height, col:col + width]
            grad_x += window * _SOBEL_X[row, col]
            grad_y += window * _SOBEL_Y[row, col]
    return grad_x, grad_y


def _dilate(mask: np.ndarray) -> np.ndarray:
    """Binary dilation by a 3x3 square (zero padding), returning float {0, 1}."""
    padded = np.pad(mask, 1, mode="constant")
    pooled = np.zeros_like(mask)
    for row in range(3):
        for col in range(3):
            pooled = np.maximum(pooled, padded[row:row + mask.shape[0],
                                               col:col + mask.shape[1]])
    return (pooled > 0.5).astype(mask.dtype)


def _window_mean(values: np.ndarray, size: int) -> np.ndarray:
    """Mean over every (size x size) window, stride 1, zero padding.

    Torch's ``avg_pool2d(..., padding=radius)`` divides by ``size * size``, i.e.
    the zeros are counted. This matches that, so the confidence numbers are the
    same as the reference implementation.
    """
    radius = size // 2
    height, width = values.shape
    padded = np.pad(values, radius, mode="constant")
    integral = np.zeros((padded.shape[0] + 1, padded.shape[1] + 1), dtype=np.float64)
    integral[1:, 1:] = padded.cumsum(0).cumsum(1)
    total = (integral[size:size + height, size:size + width]
             - integral[:height, size:size + width]
             - integral[size:size + height, :width]
             + integral[:height, :width])
    return total / float(size * size)


def criminisi_fill(texture: np.ndarray, known: np.ndarray, hint: np.ndarray,
                   patch_radius: int = 5, isophote_source: str = "current",
                   max_steps: int = 200000, data_epsilon: float = 1e-6,
                   isophote_alpha: float = 1.0,
                   search_bias: float = 1e6) -> np.ndarray:
    """Fill the hole under ``~known`` and return the full image.

    Args:
        texture: (H, W, 3) float RGB in [0, 1]; content inside the hole is ignored.
        known: (H, W) bool, True where the pixel must be preserved.
        hint: (H, W, 3) float low-frequency prior for the interior.
        patch_radius: patches are (2r+1) square. 5 (11x11) is the accepted size
            on the flagstone photo; larger pastes settle more pixels per step but
            blur the structure.
        isophote_source: "current" recomputes the data term from the partly
            synthesised image every step (the paper); "hint" takes it from the
            low-resolution prior, which can never grow sharper than the hint.
        max_steps: safety bound on the number of paste operations.
        data_epsilon: floor added to the data term so ties are broken by
            confidence. Must be far below a typical isophote magnitude, or it
            becomes the whole priority.
        isophote_alpha: normalisation of the data term. 255 for 8-bit images,
            1.0 for the [0, 1] floats used here. It only rescales the data term
            uniformly, so it matters solely through its ratio to ``data_epsilon``.
        search_bias: penalty per unknown pixel in a candidate source window, in
            SSD units. Large enough to be lexicographic.
    """
    if isophote_source not in ("current", "hint"):
        raise ValueError(f"isophote_source {isophote_source!r} not in ('current', 'hint')")
    if texture.shape[:2] != known.shape or texture.shape[:2] != hint.shape[:2]:
        raise ValueError(f"shape mismatch: texture {texture.shape}, known {known.shape}, "
                         f"hint {hint.shape}")
    if patch_radius < 1:
        raise ValueError(f"patch_radius {patch_radius} must be at least 1")
    if isophote_alpha <= 0:
        raise ValueError(f"isophote_alpha {isophote_alpha} must be positive")
    if data_epsilon < 0:
        raise ValueError(f"data_epsilon {data_epsilon} must not be negative")
    if search_bias < 0:
        raise ValueError(f"search_bias {search_bias} must not be negative")
    if max_steps < 1:
        raise ValueError(f"max_steps {max_steps} must be at least 1")

    radius = patch_radius
    size = 2 * radius + 1
    height, width = known.shape
    if size > min(height, width):
        raise ValueError(f"patch {size}x{size} does not fit in {height}x{width}")

    known_f = known.astype(np.float64)
    confidence = known_f.copy()
    # Start the interior from the hint so the first data terms are meaningful.
    image = texture.astype(np.float64) * known_f[..., None] \
        + hint.astype(np.float64) * (1.0 - known_f[..., None])

    steps = 0
    while int((known_f < 0.5).sum()) > 0:
        if steps >= max_steps:
            remaining = int((known_f < 0.5).sum())
            raise RuntimeError(f"max_steps {max_steps} reached with {remaining} pixels "
                               f"still unfilled")
        steps += 1

        front = (known_f < 0.5) & (_dilate(known_f) > 0.5)
        if not bool(front.any()):
            remaining = int((known_f < 0.5).sum())
            raise RuntimeError(f"no front found with {remaining} pixels unfilled")

        # Boundary normal of the *current* front: gradient of the known mask.
        mask_x, mask_y = _sobel_gradient(known_f[..., None])
        norm = np.sqrt(mask_x * mask_x + mask_y * mask_y) + 1e-6
        normal_x = mask_x / norm
        normal_y = mask_y / norm

        guide = hint if isophote_source == "hint" else image
        gray = guide.mean(axis=2, keepdims=True)
        grad_x, grad_y = _sobel_gradient(gray)
        # Isophote = gradient rotated by 90 degrees. Drop the channel axis so the
        # product with the 2D confidence map stays (H, W).
        strength = np.abs((-grad_y) * normal_x + grad_x * normal_y)[..., 0] \
            / isophote_alpha

        confidence_mean = _window_mean(confidence, size)
        priority = confidence_mean * (strength + data_epsilon)
        priority = np.where(front, priority, -1.0)
        flat_index = int(priority.reshape(-1).argmax())
        centre_row, centre_col = divmod(flat_index, width)

        # Top-left corner of the target patch, clamped to stay inside the canvas.
        top = min(max(centre_row - radius, 0), height - size)
        left = min(max(centre_col - radius, 0), width - size)
        target = image[top:top + size, left:left + size]
        target_mask = known_f[top:top + size, left:left + size]
        centre_confidence = confidence_mean[centre_row, centre_col]

        scores = backends.score_map(image, target, target_mask, backend="numpy")
        # Only windows whose pixels are all known-or-already-filled may be
        # sources. The penalty per unfilled pixel is deliberately huge (an SSD is
        # O(100)), so the search prefers legal windows absolutely rather than
        # trading legality for a better colour fit - which would silently source
        # blurred hint pixels from inside the hole.
        unknown_count = size * size - backends.window_counts(known_f > 0.5, size)
        scores = scores + search_bias * unknown_count

        best = int(scores.reshape(-1).argmin())
        best_row, best_col = divmod(best, scores.shape[1])
        patch = image[best_row:best_row + size, best_col:best_col + size]

        fill_here = target_mask < 0.5
        image[top:top + size, left:left + size] = np.where(
            fill_here[..., None], patch, target)
        known_f[top:top + size, left:left + size] = np.where(
            fill_here, 1.0, target_mask)
        confidence[top:top + size, left:left + size] = np.where(
            fill_here, centre_confidence,
            confidence[top:top + size, left:left + size])

    return image.astype(np.float32)
