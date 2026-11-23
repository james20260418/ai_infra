"""CPU-only unit tests: everything that does not need a photo, a GPU or seconds.

These exist so that "CI has no GPU" stops being an excuse. The grid, the legality
rule, the minimum-error seam, the ring-read-only rule, the compositing and the
numpy/torch decision parity are all tested here.

Run from the tool root:

    python3 -m unittest test_inpaint_units -v

or, from anywhere:

    python3 -m unittest discover -s tools/jpov/tools/2d_inpaint -p 'test_*.py' -v
"""

from __future__ import annotations

import unittest

import numpy as np

from inpaint import backends, common
from inpaint.criminisi import criminisi_fill
from inpaint.quilting import _minimum_error_seam, quilting_fill


def _uniform_problem(canvas: int = 200, border: int = 40):
    """A ring of one colour and a hint of another: any copied hint pixel shows."""
    texture = np.full((canvas, canvas, 3), 0.20, dtype=np.float32)
    known = np.ones((canvas, canvas), dtype=bool)
    known[border:canvas - border, border:canvas - border] = False
    hint = np.full((canvas, canvas, 3), 0.90, dtype=np.float32)
    return texture, known, hint


def _brick_wall(size: int = 320, brick_w: int = 92, brick_h: int = 46,
                mortar: int = 5) -> np.ndarray:
    """Running-bond brick wall: periodic, so a perfect source window exists."""
    rng = np.random.default_rng(7)
    image = np.zeros((size, size, 3), dtype=np.float32)
    yy, xx = np.mgrid[0:size, 0:size]
    row = yy // brick_h
    offset = (row % 2) * (brick_w // 2)
    u = (xx + offset) % brick_w
    v = yy % brick_h
    is_mortar = (u < mortar) | (v < mortar)
    tint = rng.uniform(-0.06, 0.06, size=(size // brick_h + 2, 4))[row, (xx + offset) // brick_w % 4]
    brick = np.stack([0.62 + tint, 0.30 + tint * 0.5, 0.24 + tint * 0.3], -1)
    mortar_colour = np.array([0.80, 0.78, 0.72], dtype=np.float32)
    image[:] = np.where(is_mortar[..., None], mortar_colour, brick)
    image += rng.normal(0, 0.012, image.shape)
    image *= (0.85 + 0.3 * (xx / size))[..., None]
    return np.clip(image, 0, 1).astype(np.float32)


def _side_stats(result: np.ndarray, known: np.ndarray, border: int) -> dict[str, float]:
    """|step| across each hole boundary, relative to the ring's own |step|."""
    grey = result.mean(axis=2)
    ring = 0.5 * (np.abs(np.diff(grey, axis=1))[known[:, 1:] & known[:, :-1]].mean()
                  + np.abs(np.diff(grey, axis=0))[known[1:] & known[:-1]].mean())
    size = result.shape[0]
    hole = slice(border, size - border)
    pairs = {
        "left": (result[hole, border], result[hole, border - 1]),
        "right": (result[hole, size - border], result[hole, size - border - 1]),
        "top": (result[border, hole], result[border - 1, hole]),
        "bottom": (result[size - border, hole], result[size - border - 1, hole]),
    }
    return {name: float(np.abs(a - b).mean() / ring) for name, (a, b) in pairs.items()}


class SeamTest(unittest.TestCase):
    def test_seam_follows_valley(self):
        length, width = 30, 12
        errors = np.full((length, width), 5.0)
        valley = 3 + (np.arange(length) // 8)
        errors[np.arange(length), valley] = 0.0
        seam = _minimum_error_seam(errors)
        np.testing.assert_array_equal(seam, valley)
        self.assertLessEqual(int(np.abs(np.diff(seam)).max()), 1)

    def test_seam_minimises_total_not_first_row(self):
        # Greedy row-by-row would take the free column 0 in row 0, then be
        # forced to pay 100 next to it. The minimum *total* starts at column 1
        # (cost 10) and continues at column 2 (cost 10) = 20.
        errors = np.array([[0.0, 10.0, 10.0, 10.0],
                           [100.0, 100.0, 10.0, 10.0]])
        seam = _minimum_error_seam(errors)
        self.assertEqual(int(seam[0]), 1)
        self.assertEqual(int(seam[1]), 2)


class WindowCountsTest(unittest.TestCase):
    def test_matches_brute_force(self):
        rng = np.random.default_rng(3)
        mask = rng.random((40, 44)) > 0.5
        counts = backends.window_counts(mask, 5)
        brute = np.array([[mask[i:i + 5, j:j + 5].sum() for j in range(44 - 4)]
                          for i in range(40 - 4)])
        np.testing.assert_array_equal(counts, brute)

    def test_rejects_window_larger_than_image(self):
        with self.assertRaises(ValueError):
            backends.window_counts(np.zeros((4, 4), dtype=bool), 5)


class QuiltingTest(unittest.TestCase):
    def test_only_known_pixels_are_copied(self):
        texture, known, hint = _uniform_problem()
        result = quilting_fill(texture, known, hint, patch=32, overlap=10,
                               border=40, backend="numpy")
        deviation = float(np.abs(result[~known] - 0.20).max())
        self.assertLess(deviation, 1e-6)
        np.testing.assert_array_equal(result[known], texture[known])

    def test_every_hole_pixel_is_written(self):
        texture, known, hint = _uniform_problem()
        result = quilting_fill(texture, known, hint, patch=32, overlap=10,
                               border=40, backend="numpy")
        untouched = int((np.abs(result[~known] - 0.90).max(axis=1) < 1e-6).sum())
        self.assertEqual(untouched, 0)

    def test_hint_weight_changes_the_result(self):
        rng = np.random.default_rng(11)
        texture = rng.random((200, 200, 3)).astype(np.float32)
        known = np.ones((200, 200), dtype=bool)
        known[40:160, 40:160] = False
        hint = np.zeros_like(texture)
        hint[40:160, 40:160] = rng.random((120, 120, 3))
        without = quilting_fill(texture, known, hint, patch=32, overlap=10, border=40,
                                hint_weight=0.0, backend="numpy")
        with_hint = quilting_fill(texture, known, hint, patch=32, overlap=10, border=40,
                                  hint_weight=50.0, backend="numpy")
        self.assertGreater(float(np.abs(without[~known] - with_hint[~known]).mean()), 1e-3)

    def test_periodic_wall_has_no_visible_seam(self):
        """On a periodic texture a perfectly matching window exists everywhere.

        Any residual straight line is therefore a bug, not a limitation of the
        method. This is the regression test for the ring-overwrite bug: it used
        to print right=3.3-3.7 and bottom=2.0-3.3.
        """
        texture = _brick_wall()
        canvas, border = texture.shape[0], 64
        known = common.border_ring_mask(canvas, border)
        hint = common.neutral_hint(texture, known)
        result = common.enforce_border(
            quilting_fill(texture, known, hint, patch=40, overlap=12,
                          border=border, backend="numpy"), texture, known)
        values = _side_stats(result, known, border)
        for side, value in values.items():
            self.assertLess(value, 1.6, f"{side} seam {value:.2f} on a periodic wall")

    def test_rejects_bad_patch_grid(self):
        texture, known, hint = _uniform_problem()
        with self.assertRaises(ValueError):
            quilting_fill(texture, known, hint, patch=30, overlap=10, border=40,
                          backend="numpy")
        with self.assertRaises(ValueError):
            quilting_fill(texture, known, hint, patch=32, overlap=32, border=40,
                          backend="numpy")


class CriminisiTest(unittest.TestCase):
    def test_copies_only_known_colour_and_preserves_ring(self):
        texture, known, hint = _uniform_problem(160, 32)
        result = criminisi_fill(texture, known, hint, patch_radius=4)
        deviation = float(np.abs(result[~known] - 0.20).max())
        self.assertLess(deviation, 1e-6)
        np.testing.assert_array_equal(result[known], texture[known])

    def test_is_deterministic(self):
        rng = np.random.default_rng(5)
        texture = rng.random((128, 128, 3)).astype(np.float32)
        known = np.ones((128, 128), dtype=bool)
        known[24:104, 24:104] = False
        hint = rng.random((128, 128, 3)).astype(np.float32)
        first = criminisi_fill(texture, known, hint, patch_radius=3)
        second = criminisi_fill(texture, known, hint, patch_radius=3)
        np.testing.assert_array_equal(first, second)

    def test_max_steps_failure_is_loud(self):
        texture, known, hint = _uniform_problem(160, 32)
        with self.assertRaises(RuntimeError):
            criminisi_fill(texture, known, hint, patch_radius=4, max_steps=1)

    def test_rejects_bad_isophote_source(self):
        texture, known, hint = _uniform_problem(160, 32)
        with self.assertRaises(ValueError):
            criminisi_fill(texture, known, hint, isophote_source="guess")


class BackendParityTest(unittest.TestCase):
    @unittest.skipUnless(backends.torch_available(), "torch is not installed")
    def test_numpy_and_torch_pick_the_same_window(self):
        rng = np.random.default_rng(0)
        source = rng.random((96, 88, 3))
        target = rng.random((13, 13, 3))
        weight = rng.choice([0.0, 1.0, 8.0], size=(13, 13))
        left = backends._score_map_numpy(source, target, weight)
        right = backends._score_map_torch(source, target, weight)
        residual = float(np.abs(left - right).max() / max(float(np.abs(left).max()), 1.0))
        self.assertLess(residual, 1e-4)
        self.assertEqual(int(np.argmin(left)), int(np.argmin(right)))


class MaskTest(unittest.TestCase):
    def test_border_must_fit(self):
        for border in (0, 50, 120):
            with self.assertRaises(ValueError):
                common.border_ring_mask(100, border)

    def test_ring_and_hole_are_complementary(self):
        known = common.border_ring_mask(64, 8)
        self.assertEqual(int(known.sum()), 64 * 64 - 48 * 48)
        self.assertFalse(bool(known.all()))  # a hole exists
        self.assertFalse(bool(known[8:56, 8:56].any()))


if __name__ == "__main__":
    unittest.main(verbosity=2)
