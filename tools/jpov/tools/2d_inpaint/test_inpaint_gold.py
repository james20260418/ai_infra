"""Gold test: freeze the accepted output of both presets on the flagstone photo.

Two layers, on purpose:

  * **pixel gold** - the exact 8-bit PNG. Any change in the algorithm shows up,
    including ones the metrics cannot see (a stone joint moved by ten pixels).
    This works because the patches are picked by ``argmin`` over a fixed
    candidate set and the numpy backend is float64: repeated runs are
    byte-identical.
  * **metric gold** - seam / detail / colour with an explicit tolerance. A
    library change may perturb pixels slightly without changing the result, and
    a quality regression must fail even if somebody regenerates the PNGs.

The canonical backend is numpy. Torch is an accelerator, not a contract: on a
texture with exact ties a 1e-5 difference can pick the other, equally good
window, so pixel equality across backends is explicitly not promised. The
torch run is for diagnostics only.

Usage (from the tool root):

    python3 -m unittest test_inpaint_gold -v     # check
    python3 test_inpaint_gold.py --update        # rewrite gold/ after a deliberate change
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import unittest

import numpy as np
from PIL import Image

from inpaint import common
from inpaint.criminisi import criminisi_fill
from inpaint.presets import CRIMINISI_BEST, QUILTING_BEST
from inpaint.quilting import quilting_fill

HERE = os.path.dirname(os.path.abspath(__file__))
GOLD_DIR = os.path.join(HERE, "gold")
SOURCE = os.path.join(HERE, "assets", "flagstone.png")

# Tolerances, in 8-bit levels, measured against the stored PNG.
PIXEL_MAX = 2           # a single pixel may drift by two levels
PIXEL_MEAN = 0.05       # ... and the image as a whole by 0.05
PIXEL_FRACTION = 0.001  # at most 0.1 % of pixels may differ at all
METRIC_TOLERANCE = {"seam_ratio": 0.05, "detail_ratio": 0.05, "colour_shift": 0.6}

# The accepted configurations. Quilting runs at the shipping size because it is
# what users get; criminisi runs at 320/56 because the greedy front costs about
# three minutes at 560 on a CPU, and what the gold must catch - a change in the
# priority, the search or the paste bookkeeping - shows up at either size.
CASES = (
    {"name": "quilting_flagstone_best", "method": "quilting",
     "canvas": 560, "border": 96, "params": QUILTING_BEST},
    {"name": "criminisi_flagstone_best", "method": "criminisi",
     "canvas": 320, "border": 56, "params": CRIMINISI_BEST},
)


def _problem(case: dict):
    texture = common.load_texture(SOURCE, case["canvas"])
    known = common.border_ring_mask(case["canvas"], case["border"])
    _, hint = common.low_resolution_hint(texture, 8)
    return texture, known, hint


def render(case: dict) -> np.ndarray:
    texture, known, hint = _problem(case)
    if case["method"] == "quilting":
        fill = quilting_fill(texture, known, hint, backend="numpy", **case["params"])
    elif case["method"] == "criminisi":
        fill = criminisi_fill(texture, known, hint, **case["params"])
    else:
        raise ValueError(f"unknown method {case['method']!r}")
    return common.enforce_border(fill, texture, known)


def measure(result: np.ndarray, case: dict) -> dict:
    texture, known, _ = _problem(case)
    row = common.report(case["method"], result, texture, known)
    row.pop("method")
    return {key: round(float(value), 4) for key, value in row.items()}


def update() -> int:
    os.makedirs(GOLD_DIR, exist_ok=True)
    table = {}
    for case in CASES:
        result = render(case)
        common.save_png(result, os.path.join(GOLD_DIR, f"{case['name']}.png"))
        table[case["name"]] = {
            "method": case["method"],
            "canvas": case["canvas"],
            "border": case["border"],
            "params": case["params"],
            "metrics": measure(result, case),
        }
        print(f"wrote gold/{case['name']}.png  {table[case['name']]['metrics']}")
    with open(os.path.join(GOLD_DIR, "gold.json"), "w", encoding="utf-8") as handle:
        json.dump(table, handle, indent=2)
        handle.write("\n")
    return 0


class GoldTest(unittest.TestCase):
    def test_gold_matches(self):
        path = os.path.join(GOLD_DIR, "gold.json")
        self.assertTrue(os.path.exists(path), f"missing {path}; run --update first")
        with open(path, encoding="utf-8") as handle:
            table = json.load(handle)

        for case in CASES:
            name = case["name"]
            expected_file = os.path.join(GOLD_DIR, f"{name}.png")
            self.assertIn(name, table)
            self.assertTrue(os.path.exists(expected_file))
            self.assertEqual(table[name]["params"], case["params"],
                             f"{name}: params changed since the gold was recorded")
            self.assertEqual(table[name]["canvas"], case["canvas"])
            self.assertEqual(table[name]["border"], case["border"])

            result = render(case)
            actual = np.asarray(common.to_uint8(result), dtype=np.int16)
            expected = np.asarray(Image.open(expected_file).convert("RGB"), dtype=np.int16)
            diff = np.abs(actual - expected)
            worst = int(diff.max())
            mean = float(diff.mean())
            fraction = float((diff.max(axis=2) > 0).mean())

            self.assertLessEqual(worst, PIXEL_MAX, f"{name}: pixel max {worst}")
            self.assertLessEqual(mean, PIXEL_MEAN, f"{name}: pixel mean {mean:.4f}")
            self.assertLessEqual(fraction, PIXEL_FRACTION,
                                 f"{name}: {fraction * 100:.3f}% pixels changed")

            metrics = measure(result, case)
            for key, tolerance in METRIC_TOLERANCE.items():
                drift = abs(metrics[key] - table[name]["metrics"][key])
                self.assertLessEqual(drift, tolerance,
                                     f"{name}: {key} drifted {drift:.3f} "
                                     f"({table[name]['metrics'][key]} -> {metrics[key]})")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--update", action="store_true",
                        help="rewrite gold/ (inspect the PNGs before committing)")
    args = parser.parse_args(argv)
    if args.update:
        return update()
    parser.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
