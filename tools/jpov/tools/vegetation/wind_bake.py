"""Bake per-vertex wind data from a Sapling curve into mesh UVs.

Produces, per vertex:
  softness = normalized "hierarchy position" in [0,1]  (0 = root/trunk base, ->1 at tips)
             = hierarchical arc distance from the root, normalized by the global max.
  phase    = a **continuous** low-frequency field in [0,1)  (seconds), a deterministic
             smooth function of the vertex's world position.

Stored as an extra UV layer named "Wind" (-> glTF TEXCOORD_1) on each mesh object,
so the JPOV loader can read it into the per-vertex wind channel.

Why this shape (design: docs/jpov_vegetation_wind_design.md):
  - `softness` is a *continuous field* over the branch hierarchy -> it is equal on both
    sides of every joint => no tearing; and it *inherits* along parents => a twig's base
    carries its parent's softness (so it moves with the parent).
  - `phase` must be **continuous over space** or the mesh tears apart: any *discontinuity*
    in phase between two adjacent vertices (which is exactly what a per-vertex hash, or a
    per-branch *constant* at the seam where a branch meets its parent, produces) makes the
    two vertices move by unrelated amounts -> the face between them is ripped open.
    A smooth low-frequency field gives de-synchronisation (different regions of the canopy
    move out of step) *without* any discontinuity, so nothing tears.  It is also nearly
    constant across a single leaf card / tube ring (they are small vs. the field
    wavelength), so cards stay rigid.
  - Per-level motion is reconstructed in the shader by banding `softness` into N levels
    (each band gets its own frequency/amplitude) => hierarchical sway ("level version").

Key correctness details:
  - Sampling happens **before** the curve is converted to a mesh (the conversion replaces
    the object's data), and skeleton points are taken in **world space** (curve object
    matrix applied) so they live in the same frame as the finished mesh vertices.
  - Leaf-card meshes are baked **per card** (one value for the whole quadrilateral), and
    that value is copied from the **nearest solid (trunk/branch) vertex** -- so a card
    can neither be sheared by a softness gradient across it, nor drift away from the twig
    it hangs from.  (Sampling the skeleton once per card independently would let a card
    pick a *different* branch than the twig it is attached to, and then detach.)
"""

import math
from collections import deque

import bpy
from mathutils import Vector


# --- phase field -----------------------------------------------------------
# Continuous, deterministic, low-frequency.  Wavelength ~3-4 world units (tree is
# ~6-7 units) -> the canopy de-synchronises, yet a ~0.25-unit leaf card sees almost
# no variation (so it stays rigid).  NOT a hash: a hash is discontinuous and tears.
def _smooth_phase01(x, y, z):
    v = (0.55 * math.sin(1.63 * x + 0.41 * z)
         + 0.30 * math.sin(0.87 * y + 1.12 * z + 1.70)
         + 0.15 * math.sin(1.41 * x + 0.53 * y + 2.90))
    v = max(-1.0, min(1.0, v))
    return 0.5 + 0.5 * v


def _bezier_polyline(sp, matrix, per_seg=16):
    """Sample a Bezier spline into a world-space polyline of Vectors."""
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
    return [matrix @ p for p in pts]


def _build_hierarchy(polys):
    """Return per-spline (parent, level, branch_id, hd_start, cum)."""
    n = len(polys)
    # root = spline whose start is lowest (min z), tie -> closest to origin
    root = min(range(n), key=lambda i: (round(polys[i][0].z, 4), polys[i][0].length))

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
    children = {i: [] for i in range(n)}
    for i in range(n):
        if i != root:
            children[parent[i]].append(i)
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
        base = hd_start[p] + attach_t[i] * cum[p][-1]
        if attach_t[i] > 0.98:
            level[i] = level[p]
            branch_id[i] = branch_id[p]
        else:
            level[i] = level[p] + 1
            branch_id[i] = i
        hd_start[i] = base

    return dict(parent=parent, level=level, branch_id=branch_id, hd_start=hd_start,
                cum=cum, root=root, attach_t=attach_t)


def _nearest(polys, cum, hd_start, pt):
    """Return (softness_raw, phase) for a world point by snapping to the NEAREST
    skeleton sample point (not interpolated along a segment): a tube cross-section
    (ring) is perpendicular to the centreline, so all its vertices share the same
    nearest sample -> identical softness -> the ring stays rigid (no pinching)."""
    best, braw = 1e18, 0.0
    for i in range(len(polys)):
        P = polys[i]
        for k in range(len(P)):
            d = (pt - P[k]).length
            if d < best:
                best = d
                braw = hd_start[i] + cum[i][k]
    return braw, _smooth_phase01(pt.x, pt.y, pt.z)


# --- 枢轴风（pivot rotation）用的关节表 --------------------------------------
# 设计：docs/jpov_vegetation_wind_design.md。平移式风把每级贡献当**位移**叠加，
# 于是整段枝在滑动；枢轴式风把它当**绕关节的转角**累加（子级继承父级），
# 根部不动、梢部走弧线、长度守恒。运行时 VS 需要「每个顶点属于哪个关节」
# （每顶点 1 个数）与「关节表」（每关节 枢轴位置 + 父关节）。
# 关节 = 折线的**抽稀**采样点（否则树干 48+ 个采样会让链过长）：
#   每条样条保留 1 + clamp(round(弧长/间距), 1, 上限) 个采样，按下标均匀取。
#   关节的父 = 同一样条上一个保留采样；样条首采样挂在**父样条最近保留采样**上
#   （枝条分叉处）；根样条首采样无父（-1）。
def _build_joints(polys, H, target_spacing=1.5, max_per_spline=6):
    """Build the wind joint table.  Returns (joints, kept_idx).

    joints[j] = {'pivot': Vector, 'parent': int}  (parent = joint index, -1 = root)
    kept_idx[i] = kept sample indices of spline i.
    """
    cum = H['cum']
    parent_spline = H['parent']
    root = H['root']

    kept_idx = []
    for i, P in enumerate(polys):
        length = cum[i][-1] if cum[i] else 0.0
        m = int(round(length / target_spacing))
        m = max(1, min(max_per_spline, m))
        last = len(P) - 1
        idx = sorted({int(round(k * last / m)) for k in range(m + 1)})
        kept_idx.append(idx)

    joint_of = {}
    joints = []
    for i in range(len(polys)):
        for kk in range(len(kept_idx[i])):
            joint_of[(i, kk)] = len(joints)
            joints.append({'pivot': polys[i][kept_idx[i][kk]], 'parent': -1})

    for i in range(len(polys)):
        for kk in range(len(kept_idx[i])):
            jid = joint_of[(i, kk)]
            if kk > 0:
                joints[jid]['parent'] = joint_of[(i, kk - 1)]
            elif i != root:
                pj = parent_spline[i]
                s = polys[i][0]
                best, bidx = 1e18, 0
                for kk2 in range(len(kept_idx[pj])):
                    d = (polys[pj][kept_idx[pj][kk2]] - s).length_squared
                    if d < best:
                        best, bidx = d, kk2
                joints[jid]['parent'] = joint_of[(pj, bidx)]
    return joints, kept_idx


def _nearest_joint(polys, cum, hd_start, kept_idx, joint_of, pt):
    """Return (softness_raw, phase, joint_id) for a world point, snapping to the
    NEAREST **kept** skeleton sample (a joint) -- same rigidity rationale as
    `_nearest`, plus the joint index the pivot-rotation wind walks from."""
    best, braw, bjid = 1e18, 0.0, 0
    for i in range(len(polys)):
        P = polys[i]
        for kk in range(len(kept_idx[i])):
            d = (pt - P[kept_idx[i][kk]]).length
            if d < best:
                best = d
                braw = hd_start[i] + cum[i][kept_idx[i][kk]]
                bjid = joint_of[(i, kk)]
    return braw, _smooth_phase01(pt.x, pt.y, pt.z), bjid


def sample_source(curve_obj, per_seg=16):
    """Sample the Sapling curve into per-spline WORLD-space polylines (call BEFORE
    converting the curve to a mesh -- the conversion replaces the object's data)."""
    mw = curve_obj.matrix_world
    return [_bezier_polyline(sp, mw, per_seg) for sp in curve_obj.data.splines]


def bake_meshes(polys, mesh_objs, card_objs=()):
    """Bake per-vertex wind UVs (`Wind` = (softness, phase); `WindJoint` = (joint_id, 0))
    on every mesh object, and return the joint table (list of {'pivot','parent'})."""
    H = _build_hierarchy(polys)
    cum, hd_start = H['cum'], H['hd_start']
    joints, kept_idx = _build_joints(polys, H)
    # (spline, kept-position) -> joint id（与 _build_joints 同一编号顺序）
    joint_of = {}
    n = 0
    for i in range(len(polys)):
        for kk in range(len(kept_idx[i])):
            joint_of[(i, kk)] = n
            n += 1
    # global max hierarchical distance -> normalize softness to [0,1]
    gmax = 1e-9
    for i in range(len(polys)):
        gmax = max(gmax, hd_start[i] + cum[i][-1])
    print(f"[wind_bake] splines={len(polys)} max_hd={gmax:.3f} levels={max(H['level'])+1} "
          f"joints={len(joints)}")
    max_chain = 0
    for j in range(len(joints)):
        d = 0
        p = joints[j]['parent']
        while p >= 0:
            d += 1
            p = joints[p]['parent']
        max_chain = max(max_chain, d)
    print(f"[wind_bake] max_chain_len={max_chain + 1}")

    card_names = {o.name for o in card_objs}
    if card_names and not any(o.name not in card_names for o in mesh_objs):
        raise SystemExit("wind_bake.bake_meshes: 有 card 网格但无实体网格（叶子必须能挂到枝干顶点）")

    # --- pass 1: solid meshes (trunk / branches) baked per VERTEX -----------------
    # Collect (world_pos, (softness, phase)) of every solid vertex so that the leaf
    # cards (pass 2) can inherit the wind value of the *nearest solid vertex* -- this
    # keeps a card glued to the twig it hangs from (no detachment), which sampling the
    # skeleton independently cannot guarantee.
    solid_pts, solid_uv = [], []
    for obj in mesh_objs:
        if obj.name in card_names:
            continue
        me = obj.data
        if "Wind" in me.uv_layers:
            me.uv_layers.remove(me.uv_layers["Wind"])
        if "WindJoint" in me.uv_layers:
            me.uv_layers.remove(me.uv_layers["WindJoint"])
        uv = me.uv_layers.new(name="Wind")
        uvj = me.uv_layers.new(name="WindJoint")
        mw = obj.matrix_world
        vert_uv = [None] * len(me.vertices)
        vert_uj = [None] * len(me.vertices)
        for vi, v in enumerate(me.vertices):
            wp = mw @ v.co
            # softness/phase 取自**全采样**（与旧版一致，平移式风逐字节不变）；
            # joint_id 取自抽稀后的关节（枢轴式风用）。
            raw, phase = _nearest(polys, cum, hd_start, wp)
            _, _, jid = _nearest_joint(polys, cum, hd_start, kept_idx, joint_of, wp)
            s = max(0.0, min(1.0, raw / gmax))
            vert_uv[vi] = (s, phase)
            vert_uj[vi] = (float(jid), 0.0)
            solid_pts.append(wp)
            solid_uv.append((s, phase, float(jid)))
        for li, loop in enumerate(me.loops):
            uv.data[li].uv = vert_uv[loop.vertex_index]
            uvj.data[li].uv = vert_uj[loop.vertex_index]
        print(f"[wind_bake] baked {obj.name!r} verts={len(me.vertices)} "
              f"faces={len(me.polygons)} mode=vertex")

    # --- pass 2: card meshes (leaves) baked per CARD, from nearest solid vertex -----
    for obj in mesh_objs:
        if obj.name not in card_names:
            continue
        me = obj.data
        if "Wind" in me.uv_layers:
            me.uv_layers.remove(me.uv_layers["Wind"])
        if "WindJoint" in me.uv_layers:
            me.uv_layers.remove(me.uv_layers["WindJoint"])
        uv = me.uv_layers.new(name="Wind")
        uvj = me.uv_layers.new(name="WindJoint")
        mw = obj.matrix_world
        for poly in me.polygons:
            c = Vector((0.0, 0.0, 0.0))
            for vi in poly.vertices:
                c += mw @ me.vertices[vi].co
            c /= len(poly.vertices)
            best, bsu = 1e18, (0.0, 0.0, 0.0)
            for j, sp in enumerate(solid_pts):
                d = (sp - c).length_squared
                if d < best:
                    best, bsu = d, solid_uv[j]
            for li in poly.loop_indices:
                uv.data[li].uv = (bsu[0], bsu[1])
                uvj.data[li].uv = (bsu[2], 0.0)
        print(f"[wind_bake] baked {obj.name!r} verts={len(me.vertices)} "
              f"faces={len(me.polygons)} mode=card")

    return joints
