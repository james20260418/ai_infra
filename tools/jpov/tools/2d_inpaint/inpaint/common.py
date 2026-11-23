"""Image I/O, masking and acceptance metrics shared by both fillers.

The task definition follows the workspace term list for **2D inpaint**: a square
texture whose four edges are given, and whose interior must be synthesised.

  - the canvas is a centre crop of the source, resized to ``canvas x canvas``;
  - the *known* area is a border ring of ``border`` pixels (the four edges);
  - the *hole* is everything inside the ring;
  - the optional *hint* is the source rotated 180 degrees and downsampled to 1/8,
    i.e. a structural sketch with the grain destroyed.

Every filler runs through :func:`enforce_border`, so "the edges are preserved" is
a property of the pipeline rather than a promise each method makes.
"""

from __future__ import annotations

import numpy as np
from PIL import Image

# Square canvas the accepted output was tuned on, in pixels.
CANVAS = 560

# Width of the preserved border ring, in pixels.
BORDER = 96

# 8-bit quantisation step, used by every "is the ring bit-exact" check.
LEVELS = 255.0


def load_texture(path: str, canvas: int = CANVAS) -> np.ndarray:
    """Load ``path``, centre-crop to a square and resize to ``canvas``.

    Returns float32 RGB in [0, 1] with shape (canvas, canvas, 3).
    """
    if canvas <= 0:
        raise ValueError(f"canvas {canvas} must be positive")
    image = Image.open(path).convert("RGB")
    width, height = image.size
    side = min(width, height)
    left = (width - side) // 2
    top = (height - side) // 2
    image = image.crop((left, top, left + side, top + side))
    if side != canvas:
        image = image.resize((canvas, canvas), Image.LANCZOS)
    return np.asarray(image, dtype=np.float32) / LEVELS


def border_ring_mask(canvas: int = CANVAS, border: int = BORDER) -> np.ndarray:
    """True where the pixel is preserved: the four image edges."""
    if not 0 < border < canvas // 2:
        raise ValueError(f"border {border} must be in (0, {canvas // 2})")
    known = np.ones((canvas, canvas), dtype=bool)
    known[border:canvas - border, border:canvas - border] = False
    return known


def rotated_hint(texture: np.ndarray) -> np.ndarray:
    """The hint supplied with the task: the same texture rotated by 180 degrees."""
    return np.rot90(texture, 2)


def low_resolution_hint(texture: np.ndarray, downscale: int = 8) -> tuple[np.ndarray, np.ndarray]:
    """Build the hint actually handed to the algorithms.

    A hint is a *structural* prior, so it is the rotated texture reduced by
    ``downscale`` - enough to keep the layout readable while destroying the grain.
    Returns (low-resolution hint, its bicubic upsampling).
    """
    if downscale < 1:
        raise ValueError(f"downscale {downscale} must be at least 1")
    rotated = rotated_hint(texture)
    height, width = rotated.shape[:2]
    if height % downscale != 0 or width % downscale != 0:
        raise ValueError(f"texture {height}x{width} is not divisible by {downscale}")
    small = Image.fromarray(to_uint8(rotated)).resize(
        (width // downscale, height // downscale), Image.LANCZOS)
    low = np.asarray(small, dtype=np.float32) / LEVELS
    upsampled = np.asarray(small.resize((width, height), Image.BICUBIC), dtype=np.float32)
    return low, upsampled / LEVELS


def neutral_hint(texture: np.ndarray, known: np.ndarray) -> np.ndarray:
    """A hint with no structure: the ring's mean colour.

    Used by ``--hint none``. The hole still needs a sane starting point; plain
    black would bias the first search towards dark patches.
    """
    return np.broadcast_to(texture[known].mean(axis=0), texture.shape).copy()


def to_uint8(image: np.ndarray) -> np.ndarray:
    """Clip to [0, 1] and quantise to 8-bit, which is what gets saved and shown."""
    return (np.clip(image, 0.0, 1.0) * LEVELS + 0.5).astype(np.uint8)


def save_png(image: np.ndarray, path: str) -> None:
    """Write a float [0, 1] RGB array to ``path`` as an 8-bit PNG."""
    Image.fromarray(to_uint8(image)).save(path)


def enforce_border(fill: np.ndarray, texture: np.ndarray, known: np.ndarray) -> np.ndarray:
    """Glue the synthesised interior onto the untouched ring."""
    result = texture.copy()
    result[~known] = fill[~known]
    return result


def seam_ratio(result: np.ndarray, known: np.ndarray) -> float:
    """Colour step across the fill boundary, relative to the texture's own steps.

    ~1.0 or below means the join is no harsher than ordinary texture variation.
    This metric structurally favours smooth fills, so read it as "is there a
    visible cut", not as a quality score.
    """
    steps_h = np.abs(result[:, 1:] - result[:, :-1]).mean(axis=2)
    steps_v = np.abs(result[1:] - result[:-1]).mean(axis=2)

    crossing_h = known[:, 1:] != known[:, :-1]
    inside_h = known[:, 1:] & known[:, :-1]
    crossing_v = known[1:] != known[:-1]
    inside_v = known[1:] & known[:-1]

    ring = 0.5 * (steps_h[inside_h].mean() + steps_v[inside_v].mean())
    if ring <= 1e-9:
        return float("inf")
    cross = 0.5 * (steps_h[crossing_h].mean() + steps_v[crossing_v].mean())
    return float(cross / ring)


def detail_ratio(result: np.ndarray, known: np.ndarray) -> float:
    """Gradient energy inside the hole relative to the known ring.

    A value well under 1.0 means the method produced a smooth blob where the rest
    of the texture is rough, the usual failure mode of energy-minimising fills.
    """
    steps_h = np.abs(result[:, 1:] - result[:, :-1]).mean(axis=2)
    steps_v = np.abs(result[1:] - result[:-1]).mean(axis=2)

    hole_h = (~known[:, 1:]) & (~known[:, :-1])
    hole_v = (~known[1:]) & (~known[:-1])
    ring_h = known[:, 1:] & known[:, :-1]
    ring_v = known[1:] & known[:-1]

    hole = 0.5 * (steps_h[hole_h].mean() + steps_v[hole_v].mean())
    ring = 0.5 * (steps_h[ring_h].mean() + steps_v[ring_v].mean())
    if ring <= 1e-9:
        return float("inf")
    return float(hole / ring)


def colour_shift(result: np.ndarray, texture: np.ndarray, known: np.ndarray) -> float:
    """Mean per-channel RGB difference between the hole and the known ring.

    Reported in 8-bit levels. Catches fills that are structurally fine but sit at
    the wrong exposure or hue.
    """
    hole_mean = result[~known].mean(axis=0)
    ring_mean = texture[known].mean(axis=0)
    return float(np.abs(hole_mean - ring_mean).mean() * LEVELS)


def report(name: str, result: np.ndarray, texture: np.ndarray, known: np.ndarray) -> dict:
    """Compute the full acceptance table for one result."""
    return {
        "method": name,
        "seam_ratio": seam_ratio(result, known),
        "detail_ratio": detail_ratio(result, known),
        "colour_shift": colour_shift(result, texture, known),
    }
