"""Image quilting with minimum-error boundary cuts (Efros & Freeman, 2001).

The hole is covered by a raster grid of overlapping square patches. Each patch is
cut from the known ring at the position whose overlap best continues what is
already there, and then composited along a minimum-error seam instead of a
straight line - which is why the joins disappear while the patch grid keeps its
blocky, non-averaged character.

The optional hint is used as a low-frequency bias in the patch search: candidate
patches whose downsampled appearance disagrees with the prior at that location
are penalised, so the quilt follows the hint's large shapes without copying its
blur.

Everything here is numpy; the single convolution that wants a GPU lives behind
``backends.score_map``. That split is deliberate: the algorithm - grid, legality,
seams, weighting - is testable on a machine with no CUDA at all.
"""

from __future__ import annotations

import numpy as np

from . import backends

# The hint is compared at this reduction, matching the 8x downsampling used to
# build the low-resolution hint in the first place.
HINT_REDUCTION = 8


def _minimum_error_seam(errors: np.ndarray) -> np.ndarray:
    """Least-cost path across a (length, width) error map, from row 0 to row -1.

    Returns, per row, the column index at which the seam sits. Used for the
    vertical strips; transpose the error map for horizontal ones.
    """
    length, width = errors.shape
    cost = errors.astype(np.float64).copy()
    for row in range(1, length):
        previous = cost[row - 1]
        left = np.concatenate(([np.inf], previous[:-1]))
        right = np.concatenate((previous[1:], [np.inf]))
        cost[row] += np.minimum(np.minimum(left, previous), right)
    seam = np.zeros(length, dtype=np.int64)
    seam[-1] = int(np.argmin(cost[-1]))
    for row in range(length - 2, -1, -1):
        start = max(seam[row + 1] - 1, 0)
        stop = min(seam[row + 1] + 2, width)
        seam[row] = start + int(np.argmin(cost[row, start:stop]))
    return seam


def _nearest_index(source_size: int, target_size: int) -> np.ndarray:
    """Index map for a nearest-neighbour resize, matching torch's floor rule."""
    return (np.arange(target_size) * source_size // target_size).astype(np.int64)


def quilting_fill(texture: np.ndarray, known: np.ndarray, hint: np.ndarray,
                  patch: int = 40, overlap: int = 12, hint_weight: float = 0.0,
                  border: int = 96, known_weight: float = 8.0,
                  backend: str = "auto") -> np.ndarray:
    """Fill the hole by quilting and return the full image.

    Args:
        texture: (H, W, 3) float RGB in [0, 1]; content inside the hole is ignored.
        known: (H, W) bool, True where the pixel must be preserved.
        hint: (H, W, 3) float low-frequency prior for the interior.
        patch: patch side length in pixels.
        overlap: width of the shared strip between neighbouring patches.
        hint_weight: weight of the low-frequency disagreement with the hint. 0
            makes this plain quilting with no prior at all.
        border: width of the preserved ring on each side.
        known_weight: SSD weight of a *given* pixel relative to an already
            synthesised one. The ring cannot be cut, while a seam between two
            patches can be routed through the cheapest place, so a patch that has
            to trade one against the other must give up the seam - this is what
            keeps the far (bottom/right) edges, which no seam ever reaches, as
            continuous as the near ones.
        backend: "auto" | "numpy" | "torch", see `backends`.
    """
    height, width = known.shape
    stride = patch - overlap
    if stride <= 0:
        raise ValueError(f"overlap {overlap} must be smaller than patch {patch}")
    if height % HINT_REDUCTION != 0 or width % HINT_REDUCTION != 0:
        raise ValueError(f"canvas {height}x{width} is not divisible by {HINT_REDUCTION}")
    if patch % HINT_REDUCTION != 0:
        raise ValueError(f"patch {patch} is not divisible by {HINT_REDUCTION}")
    if border < overlap:
        raise ValueError(f"border {border} must be at least overlap {overlap}, otherwise "
                         f"the first patch starts outside the canvas")

    canvas = texture.copy()
    canvas[~known] = hint[~known]
    written = np.zeros_like(known)

    hint_low = hint.reshape(height // HINT_REDUCTION, HINT_REDUCTION,
                            width // HINT_REDUCTION, HINT_REDUCTION, 3).mean(axis=(1, 3))
    # The candidate's appearance at the hint's own resolution, computed once.
    source_low = texture.reshape(height // HINT_REDUCTION, HINT_REDUCTION,
                                 width // HINT_REDUCTION, HINT_REDUCTION,
                                 3).mean(axis=(1, 3))
    low_size = patch // HINT_REDUCTION
    # A source window is legal only if all of it lies in the preserved ring.
    # Exact integer counts, so this cannot flip an argmin on float noise.
    legal = backends.window_counts(known, patch) == patch * patch

    positions = list(range(border - overlap, width - patch + 1, stride))
    if not positions:
        raise ValueError("border ring is too thin for this patch size")

    for row_start in positions:
        for col_start in positions:
            rows = slice(row_start, row_start + patch)
            cols = slice(col_start, col_start + patch)
            known_patch = known[rows, cols]
            decided_patch = (known | written)[rows, cols]
            if decided_patch.all():
                continue

            target = canvas[rows, cols]
            weight = decided_patch.astype(np.float64)
            if known_weight != 1.0:
                weight = weight * (1.0 + (known_weight - 1.0) * known_patch)

            scores = backends.score_map(texture, target, weight, backend)
            # Same lexicographic rule as the other matcher: a legal (fully known)
            # source window beats any illegal one, because an SSD is O(100) and
            # this penalty is 1e6.
            scores = scores + np.where(legal, 0.0, 1.0e6)

            if hint_weight > 0.0:
                # Compare the candidate's blurred appearance with the prior at the
                # same location: a structural, not merely photometric, bias.
                low_row = row_start // HINT_REDUCTION
                low_col = col_start // HINT_REDUCTION
                hint_patch = hint_low[low_row:low_row + low_size, low_col:low_col + low_size]
                if hint_patch.shape[:2] == (low_size, low_size):
                    agreement = backends.score_map(source_low, hint_patch,
                                                   np.ones((low_size, low_size)), backend)
                    row_pick = _nearest_index(agreement.shape[0], scores.shape[0])
                    col_pick = _nearest_index(agreement.shape[1], scores.shape[1])
                    scores = scores + hint_weight * agreement[np.ix_(row_pick, col_pick)]

            best = int(scores.argmin())
            best_row, best_col = divmod(best, scores.shape[1])
            new_patch = canvas[best_row:best_row + patch, best_col:best_col + patch]

            use_new = np.ones_like(decided_patch)
            if decided_patch.any():
                left_strip = slice(col_start, col_start + overlap)
                left_errors = np.abs(new_patch[:, :overlap]
                                     - canvas[rows, left_strip]).mean(axis=2)
                left_seam = _minimum_error_seam(left_errors)
                use_new[:, :overlap] = (np.arange(overlap)[None, :]
                                        >= left_seam[:, None])

                top_strip = slice(row_start, row_start + overlap)
                top_errors = np.abs(new_patch[:overlap]
                                    - canvas[top_strip, cols]).mean(axis=2).T
                top_seam = _minimum_error_seam(top_errors)
                use_new[:overlap, :] = (np.arange(overlap)[:, None]
                                        >= top_seam[None, :])

            # The preserved ring is read-only. Writing a patch over it would be
            # invisible in the output (enforce_border restores the ring) but the
            # damaged pixels are the *target* of every later patch in that row or
            # column, so the far edges of the hole would be matched against a
            # corrupt border - which is exactly the "top/left clean, bottom/right
            # ragged" asymmetry this loop used to produce.
            composite = ((use_new | ~decided_patch) & ~known_patch)[:, :, None]
            canvas[rows, cols] = np.where(composite, new_patch, canvas[rows, cols])
            written[rows, cols] = True

    result = texture.copy()
    result[~known] = canvas[~known]
    return result
