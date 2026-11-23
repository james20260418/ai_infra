"""Command-line entry point: fill one square texture's interior from its edges.

    python3 -m inpaint.run --input assets/flagstone.png --output output/quilting.png

Run from this directory (``tools/jpov/tools/2d_inpaint``); the module imports the
sibling ``inpaint`` package, so the working directory must be the tool root or it
must be on ``PYTHONPATH``.
"""

from __future__ import annotations

import argparse
import os
import sys
import time

from . import common
from .criminisi import criminisi_fill
from .presets import PRESETS
from .quilting import quilting_fill

# Tool root, used for the default input/output paths and for the hint's
# divisibility requirement.
TOOL_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HINT_DOWNSCALE = 8


def _method_fill(name: str):
    """Return (fill function, kwargs) for a method name."""
    if name == "quilting":
        return quilting_fill, PRESETS["quilting"]
    if name == "criminisi":
        return criminisi_fill, PRESETS["criminisi"]
    raise ValueError(f"method {name!r} is not one of {sorted(PRESETS)}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", default=os.path.join(TOOL_ROOT, "assets", "flagstone.png"),
                        help="source photo; centre-cropped to a square (default: assets/flagstone.png)")
    parser.add_argument("--output", default=None,
                        help="output PNG (default: <tool>/output/<method>.png)")
    parser.add_argument("--method", default="quilting", choices=sorted(PRESETS),
                        help="filler to run (default: quilting)")
    parser.add_argument("--canvas", type=int, default=common.CANVAS,
                        help=f"square canvas side in pixels (default: {common.CANVAS})")
    parser.add_argument("--border", type=int, default=common.BORDER,
                        help=f"width of the preserved edge ring in pixels (default: {common.BORDER})")
    parser.add_argument("--hint", default="rot180", choices=("rot180", "none"),
                        help="structural prior: the source rotated 180 degrees and "
                             "downsampled 8x, or no structure (ring mean colour)")
    parser.add_argument("--backend", default=None, choices=("numpy", "torch", "auto"),
                        help="score-map backend for quilting (default: numpy, the "
                             "deterministic reference; torch is a CUDA accelerator for "
                             "bulk runs). Not valid for criminisi, which is CPU-only.")
    args = parser.parse_args(argv)

    if args.canvas % HINT_DOWNSCALE != 0:
        parser.error(f"--canvas {args.canvas} must be divisible by {HINT_DOWNSCALE}")
    if not 0 < args.border < args.canvas // 2:
        parser.error(f"--border {args.border} must be in (0, {args.canvas // 2})")
    if args.method == "criminisi" and args.backend is not None:
        parser.error("--backend applies to quilting only; criminisi is CPU-only by design")
    if not os.path.isfile(args.input):
        parser.error(f"--input {args.input} is not a file")
    backend = args.backend or "numpy"

    texture = common.load_texture(args.input, args.canvas)
    known = common.border_ring_mask(args.canvas, args.border)
    if args.hint == "rot180":
        _, hint = common.low_resolution_hint(texture, HINT_DOWNSCALE)
    else:
        hint = common.neutral_hint(texture, known)

    fill, kwargs = _method_fill(args.method)
    if args.method == "quilting":
        kwargs = dict(kwargs, backend=backend, border=args.border)

    started = time.perf_counter()
    filled = fill(texture, known, hint, **kwargs)
    elapsed = time.perf_counter() - started
    result = common.enforce_border(filled, texture, known)

    output = args.output or os.path.join(TOOL_ROOT, "output", f"{args.method}.png")
    os.makedirs(os.path.dirname(output) or ".", exist_ok=True)
    common.save_png(result, output)

    metrics = common.report(args.method, result, texture, known)
    hole_fraction = 100.0 * float((~known).mean())
    print(f"method  {args.method}  {kwargs}")
    print(f"canvas  {args.canvas}x{args.canvas}, border {args.border} "
          f"-> hole {args.canvas - 2 * args.border}x{args.canvas - 2 * args.border} px, "
          f"{hole_fraction:.1f}% of pixels")
    print(f"time    {elapsed:.1f} s")
    print(f"seam    {metrics['seam_ratio']:.2f}  (boundary step / ring's own step; "
          f"<=1 is invisible)")
    print(f"detail  {metrics['detail_ratio']:.2f}  (hole gradient / ring gradient; "
          f"becomes 0 for a blur)")
    print(f"colour  {metrics['colour_shift']:.1f}  (mean RGB offset from the ring, "
          f"8-bit levels)")
    print(f"wrote   {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
