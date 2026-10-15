"""Bake per-vertex wind data from a Sapling curve into mesh UVs.

Produces, per vertex:
  softness = normalized "hierarchy position" in [0,1]  (0 = root/trunk base, ->1 at tips)
             = hierarchical arc distance from the root, normalized by the global max.
  phase    = per-branch constant in [0,1)  (deterministic hash of the branch id)

Stored as an extra UV layer named "Wind" (-> glTF TEXCOORD_1) on each mesh object,
so the JPOV loader can read it into the per-vertex wind channel.

Why this shape (design: docs/jpov_vegetation_wind_design.md):
  - `softness` is a *continuous field* over the branch hierarchy -> it is equal on both
    sides of every joint => no tearing; and it *inherits* along parents => a twig's base
    carries its parent's softness (so it moves with the parent).
  - `phase` is *per-branch constant* (NOT per-vertex) -> whole rings / whole leaf cards
    share one phase => no tearing, while different branches desync.
  - Per-level motion is reconstructed in the shader by banding `softness` into N levels
    (each band gets its own frequency/amplitude) => hierarchical sway ("level version").
"""

import math

import bpy
from mathutils import Vector


def _bezier_polyline(sp, per_seg=16):
    """Sample a Bezier spline into a polyline of Vectors (handles included)."""
    pts = []
    if sp.type == 'BEZIER':
        bp = sp.bezier_points
        for i in range(len(bp) - 1):
            p0, p1, p2, p3 = bp[i].co, bp[i].handle_right, bp[i + 1].handle_left, bp[i + 1].co
            for k in range(per_seg):
                t = k / per_seg
                mt = 1.0 - t
                pts.append(mt**3 * p0 + 3 * mt * mt * t * p1 + 3 * mt * t * t * p2 + t**3 * p3)
        pts.append(bp[-1].co.copy())
    else:
        pts = [p.co.copy() for p in sp.points]
    return pts


def _hash01(n, salt=0):
    h = (int(n) * 2654435761 + salt * 40503) & 0xFFFFFFFF
    h ^= h >> 13
    h = (h * 0x85EBCA6B) & 0xFFFFFFFF
    h ^= h >> 16
    return (h & 0xFFFFFF) / 16777216.0


def _build_hierarchy(polys):
    """Return per-spline (parent, level, branch_id, hd_start, length)."""
    n = len(polys)
    lens = [sum((polys[i][k + 1] - polys[i][k]).length for k in range(len(polys[i]) - 1))
            for i in range(n)]
    # root = spline whose start is lowest (min z), tie -> closest to origin
    root = min(range(n), key=lambda i: (round(polys[i][0].z, 4), polys[i][0].length))

    def hier_dist(i, t):  # not used directly; computed iteratively below
        raise NotImplementedError

    parent = [-1] * n
    attach_t = [0.0] * n
    for i in range(n):
        if i == root:
            continue
        s = polys[i][0]
        best, bp_, bt = 1e18, -1, 0.0
        for j in range(n):
            if j == i:
                continue
            P = polys[j]
            for k in range(len(P) - 1):
                a, b = P[k], P[k + 1]
                ab = b - a
                L2 = ab.length_squared
                t = 0.0 if L2 < 1e-12 else max(0.0, min(1.0, (s - a).dot(ab) / L2))
                d = (s - (a + ab * t)).length
                if d < best:
                    best, bp_, bt = d, j, (k + t) / max(1, (len(P) - 1))
        parent[i] = bp_
        attach_t[i] = bt

    # measure distance from root to a point (spline j at param t) along the hierarchy
    hd_at = {}  # (j) -> list of cumulative dist at each polyline vertex

    def poly_cum(poly):
        cum = [0.0]
        for k in range(len(poly) - 1):
            cum.append(cum[-1] + (poly[k + 1] - poly[k]).length)
        return cum

    cum = [poly_cum(polys[i]) for i in range(n)]
    hd_start = [0.0] * n
    level = [0] * n
    branch_id = [0] * n
    order = []
    # topological: process by BFS from root over parent links
    children = {i: [] for i in range(n)}
    for i in range(n):
        if i != root:
            children[parent[i]].append(i)
    from collections import deque
    dq = deque([root])
    while dq:
        i = dq.popleft()
        order.append(i)
        for c in children[i]:
            dq.append(c)
    for i in order:
        if i == root:
            hd_start[i] = 0.0
            level[i] = 0
            branch_id[i] = root
            continue
        p = parent[i]
        # distance from root to the attach point on parent p
        base = hd_start[p] + attach_t[i] * cum[p][-1]
        # continuation?  spline starts at the parent's END -> same level/branch
        if attach_t[i] > 0.98:
            level[i] = level[p]
            branch_id[i] = branch_id[p]
        else:
            level[i] = level[p] + 1
            branch_id[i] = i
        hd_start[i] = base

    return dict(parent=parent, level=level, branch_id=branch_id, hd_start=hd_start,
                cum=cum, length=lens, root=root, attach_t=attach_t)


def _nearest(polys, cum, hd_start, level, branch_id, pt):
    """Return (softness_raw, phase, level, branch_id) for a world point.

    Snaps to the NEAREST SKELETON SAMPLE POINT (not interpolated along a segment):
    a tube cross-section (ring) is perpendicular to the centreline, so all its
    vertices share the same nearest sample -> identical (softness, phase) -> the
    ring stays rigid (no pinching / tearing). Leaf cards are small and likewise
    share one sample -> rigid whole card.
    """
    best, braw, bphase, bl, bid = 1e18, 0.0, 0.0, 0, 0
    for i in range(len(polys)):
        P = polys[i]
        for k in range(len(P)):
            d = (pt - P[k]).length
            if d < best:
                best = d
                braw = hd_start[i] + cum[i][k]
                bl = level[i]
                bid = branch_id[i]
    return braw, _hash01(bid), bl, bid


def sample_source(curve_obj, per_seg=16):
    """Sample the Sapling curve into per-spline polylines (call BEFORE converting
    the curve to a mesh — the conversion replaces the object's data)."""
    return [_bezier_polyline(sp, per_seg) for sp in curve_obj.data.splines]


def bake_meshes(polys, mesh_objs):
    H = _build_hierarchy(polys)
    cum, hd_start, level, bid = H['cum'], H['hd_start'], H['level'], H['branch_id']
    # global max hierarchical distance -> normalize softness to [0,1]
    gmax = 1e-9
    for i in range(len(polys)):
        gmax = max(gmax, hd_start[i] + cum[i][-1])
    print(f"[wind_bake] splines={len(polys)} max_hd={gmax:.3f} "
          f"levels={max(level)+1}")

    for obj in mesh_objs:
        me = obj.data
        if "Wind" in me.uv_layers:
            me.uv_layers.remove(me.uv_layers["Wind"])
        uv = me.uv_layers.new(name="Wind")
        mw = obj.matrix_world
        for li, loop in enumerate(me.loops):
            v = me.vertices[loop.vertex_index].co
            wp = mw @ v
            raw, phase, lv, br = _nearest(polys, cum, hd_start, level, bid, wp)
            s = max(0.0, min(1.0, raw / gmax))
            uv.data[li].uv = (s, phase)
        print(f"[wind_bake] baked '{obj.name}' verts={len(me.vertices)} uv={uv.name}")
