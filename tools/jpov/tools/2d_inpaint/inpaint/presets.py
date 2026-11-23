"""The accepted parameters for each filler, so "the good one" has a stable name.

A preset is nothing but a dict of keyword arguments for a fill function. It
exists so that the CLI, the gold test and any downstream caller all name the same
configuration instead of repeating magic numbers.

The numbers come from the sweeps recorded in the 2d inpaint lab (RESULTS.md in
the lab repository); the comment on each entry says what it was chosen for,
because "best" is only meaningful with respect to what was measured.
"""

from __future__ import annotations

# Smallest seam of every quilting variant tried (1.31 on the flagstone photo),
# and the only one whose four edges all sit within 1.3x of the source's own
# boundary step. hint_weight=0 is deliberate: the hint improves seam/colour but
# costs detail, and the accepted trade here is grain over boundary smoothing.
QUILTING_BEST = dict(patch=40, overlap=12, known_weight=8.0, hint_weight=0.0)

# The only classical variant that restores the photo's contrast: detail 0.99
# against the untouched photo's 1.12, colour 13.1 against 12.1. The "hint" and
# "confidence only" variants score better on seam/colour precisely because they
# wash towards the border; that is the failure mode, not the goal.
CRIMINISI_BEST = dict(patch_radius=5, isophote_source="current",
                      data_epsilon=1e-6, isophote_alpha=1.0)

PRESETS: dict[str, dict] = {
    "quilting": QUILTING_BEST,
    "criminisi": CRIMINISI_BEST,
}
