"""2D texture inpainting: fill a square texture's interior from its four edges.

Two algorithms are kept, both settled on the flagstone photo:

  * ``quilting``  - Efros & Freeman 2001, fast, preserves grain;
  * ``criminisi`` - Criminisi et al. 2004, slower, best structure continuation.

The GPU is optional and confined to ``backends.score_map``; everything else is
numpy, so the full pipeline runs on a CPU-only CI runner.
"""
