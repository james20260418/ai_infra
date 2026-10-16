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
    best, braw, bsel = 1e18, 0.0, 0
    for i in range(len(polys)):
        P = polys[i]
        for k in range(len(P)):
            d = (pt - P[k]).length
            if d < best:
                best = d
                braw = hd_start[i] + cum[i][k]
                bsel = i
    return braw, _smooth_phase01(pt.x, pt.y, pt.z), bsel, best


# --- 枢轴风（pivot rotation）用的关节表 --------------------------------------
# 设计：docs/jpov_vegetation_wind_design.md。平移式风把每级贡献当**位移**叠加，
# 于是整段枝在滑动；枢轴式风把它当**绕关节的转角**累加（子级继承父级），
# 根部不动、梢部走弧线、长度守恒。运行时 VS 需要「每个顶点属于哪个关节」
# （每顶点 1 个数）与「关节表」（每关节 枢轴位置 + 父关节）。
# 关节 = 折线的**抽稀**采样点（否则树干 48+ 个采样会让链过长）：
#   每条样条保留 1 + clamp(round(弧长/间距), 1, 上限) 个采样，按下标均匀取。
#   关节的父 = 同一样条上一个保留采样；样条首采样挂在**父样条最近保留采样**上
#   （枝条分叉处）；根样条首采样无父（-1）。
def _build_joints(polys, H):
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
        # 关节密度**按枝的粗细分级**：树干/一级枝（level<=1）粗而少关节（链短 ⇒ 段少、
        #   残余误差不放大）；细软的下垂枝（level>=2）密关节（多环 ⇒ 能真正弯/下垂，
        #   而不是整根硬棍）。这是“细枝支棱”的正解：给它自由度，而不是给位移。
        if H['level'][i] <= 1:
            target_spacing, cap = 1.5, 6
        else:
            target_spacing, cap = 0.5, 8
        m = int(round(length / target_spacing))
        m = max(1, min(cap, m))
        last = len(P) - 1
        idx = sorted({int(round(k * last / m)) for k in range(m + 1)})
        kept_idx.append(idx)

    joint_of = {}
    joints = []
    for i in range(len(polys)):
        for kk in range(len(kept_idx[i])):
            joint_of[(i, kk)] = len(joints)
            joints.append({'pivot': polys[i][kept_idx[i][kk]], 'parent': -1,
                           'level': 0, 'phase': 0.0, 'spline': i,
                           'arc': cum[i][kept_idx[i][kk]], 'soft': 0.0})

    for i in range(len(polys)):
        for kk in range(len(kept_idx[i])):
            jid = joint_of[(i, kk)]
            if kk > 0:
                joints[jid]['parent'] = joint_of[(i, kk - 1)]
            elif i != root:
                # 父关节 = 父样条上**不超过接点的最后一个保留采样**（floor，与顶点归属同一规则）——
                #   这样分叉处“子枝的父链”与“母枝同位置的链”取的是同一根链，焊缝才对齐。
                pj = parent_spline[i]
                t_att = H['attach_t'][i]
                k_att = int(round(t_att * (len(polys[pj]) - 1)))
                bidx = 0
                for kk2 in range(len(kept_idx[pj])):
                    if kept_idx[pj][kk2] <= k_att:
                        bidx = kk2
                    else:
                        break
                joints[jid]['parent'] = joint_of[(pj, bidx)]

    # 合并“同位置”的关节：Sapling 把一条枝拆成多条**续接**样条（attach_t>0.98），
    #   续接处会出现两个同坐标的关节 —— 必须并成一个，否则同一处叠两次铰链，
    #   且相邻顶点分属不同链 => 焊缝撕裂。canon[j] = 该关节的代表关节。
    canon = list(range(len(joints)))
    for j in range(len(joints)):
        pj = joints[j]['pivot']
        for k in range(j):
            if canon[k] != k:
                continue
            if (pj - joints[k]['pivot']).length_squared < 1e-6:
                canon[j] = k
                break
    for j in range(len(joints)):
        par = joints[j]['parent']
        joints[j]['parent'] = canon[par] if par >= 0 else -1
    for key in joint_of:
        joint_of[key] = canon[joint_of[key]]

    # 层级（距根的关节数，非样条 level）：θ 的频率/增益只能依赖**关节自身**的量，
    #   否则同一关节对不同顶点会给不同角 → 段内不再刚性。
    # 相位：连续空间场在**该关节枢轴**处的取值 → 逐关节常量（同上，保刚性）。
    for jid in range(len(joints)):
        d = 0
        p = joints[jid]['parent']
        while p >= 0 and d < 4096:
            d += 1
            p = joints[p]['parent']
        joints[jid]['level'] = d
        pv = joints[jid]['pivot']
        joints[jid]['phase'] = _smooth_phase01(pv.x, pv.y, pv.z)
    return joints, kept_idx


def _nearest_joint(polys, cum, hd_start, kept_idx, joint_of, level_of, pt, tie_eps=0.05):
    """Return (softness_raw, phase, joint_id, link_w) for a world point.

    joint_id = the base joint of the **link** the point belongs to (the kept sample
    with the largest arc <= the nearest sample's arc; floor-by-arc, NOT
    nearest-joint).  link_w ∈ [0,1] = the point's normalized position within that
    link (0 at the link's pivot, 1 at the next kept sample / branch end); the shader
    blends the parent-link and this-link transforms by it (two-bone smooth skinning)
    => C0-continuous across link boundaries and fork welds.

    tie_eps: among samples within (best_distance + tie_eps) of the point, pick the one
    whose joint is **closest to the root** (smallest level).  Without this, the fork
    region (where the trunk and a branch run within a couple of cm of each other)
    flips arbitrarily between the two chains and adjacent/coincident vertices get
    different transforms -> torn, stretched triangles.
    """
    best, cands = 1e18, []
    for i in range(len(polys)):
        P = polys[i]
        for k in range(len(P)):
            d = (pt - P[k]).length
            if d < best:
                best = d
            cands.append((d, i, k))
    # 只留“距离在 best+tie_eps 以内”的候选（不能边扫边留，否则早期远样本会残留）。
    win = [c for c in cands if c[0] <= best + tie_eps]
    if not win:
        win = [(best, 0, 0)]

    def floor_link(i, k):
        n_kept = len(kept_idx[i])
        kk = 0
        for kk2 in range(n_kept):
            if kept_idx[i][kk2] <= k:
                kk = kk2
            else:
                break
        a0 = cum[i][kept_idx[i][kk]]
        a1 = (cum[i][kept_idx[i][kk + 1]] if kk + 1 < len(kept_idx[i]) else cum[i][-1])
        w = (cum[i][k] - a0) / max(a1 - a0, 1e-9)
        return joint_of[(i, kk)], max(0.0, min(1.0, w))

    bi, bk, bjid, bw, blev = win[0][1], win[0][2], None, 0.0, None
    for (_d, i, k) in win:
        jid, w = floor_link(i, k)
        lv = level_of.get(jid, 1 << 30)
        if blev is None or lv < blev:
            bi, bk, bjid, bw, blev = i, k, jid, w, lv
    raw = hd_start[bi] + cum[bi][bk]
    return raw, _smooth_phase01(pt.x, pt.y, pt.z), bjid, bw


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
    level_of = {jid: joints[jid]['level'] for jid in range(len(joints))}
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
    sp_cnt = [0] * len(polys)      # 每样条附近的树皮顶点数
    sp_dsum = [0.0] * len(polys)   # 这些顶点到骨架的距离和（≈管半径×n）
    jid_hist = {}                  # 关节 -> 树皮顶点数（报告 s_k 覆盖率）

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
        # 先算每顶点 (softness, phase, joint, link_w)，再做**同位置一致性**：网格里常有同坐标
        #   重复顶点（UV 缝 / 焊接），它们必须落到**同一根链**，否则两顶点拿到不同变换 →
        #   拉裂三角形成“宽薄片”。裁决：取层级（更靠根）最小者。
        raw_vals = []
        pos_key = {}
        for v in me.vertices:
            wp = mw @ v.co
            # softness/phase 取自**全采样**（与旧版一致，平移式风逐字节不变）；
            # joint_id / link_w 取自抽稀后的关节（枢轴式风用）。
            raw, phase, sp_i, sp_d = _nearest(polys, cum, hd_start, wp)
            sp_cnt[sp_i] += 1          # 该样条附近的树皮顶点数
            sp_dsum[sp_i] += sp_d      # 到骨架的距离 ≈ 该处管半径
            _, _, jid, lw = _nearest_joint(polys, cum, hd_start, kept_idx, joint_of,
                                           level_of, wp)
            raw_vals.append((wp, max(0.0, min(1.0, raw / gmax)), phase, float(jid), lw))
            pos_key.setdefault((round(wp.x, 3), round(wp.y, 3), round(wp.z, 3)),
                               []).append(v.index)
        n_fix = 0
        for vids in pos_key.values():
            if len(vids) < 2:
                continue
            rep = min(vids, key=lambda vi: (level_of.get(int(raw_vals[vi][3]), 1 << 30),
                                            int(raw_vals[vi][3])))
            rj, rw = raw_vals[rep][3], raw_vals[rep][4]
            for vi in vids:
                if raw_vals[vi][3] != rj:
                    n_fix += 1
                raw_vals[vi] = (raw_vals[vi][0], raw_vals[vi][1], raw_vals[vi][2], rj, rw)
        if n_fix:
            print(f"[wind_bake] 同位置顶点链归一：{n_fix} 个顶点")

        vert_uv = [None] * len(me.vertices)
        vert_uj = [None] * len(me.vertices)
        for vi, (wp, s, phase, jid, lw) in enumerate(raw_vals):
            vert_uv[vi] = (s, phase)
            vert_uj[vi] = (jid, lw)
            jid_hist[int(jid)] = jid_hist.get(int(jid), 0) + 1
            solid_pts.append(wp)
            solid_uv.append((s, phase, jid, lw))
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
            best, bsu = 1e18, (0.0, 0.0, 0.0, 0.0)
            for j, sp in enumerate(solid_pts):
                d = (sp - c).length_squared
                if d < best:
                    best, bsu = d, solid_uv[j]
            for li in poly.loop_indices:
                uv.data[li].uv = (bsu[0], bsu[1])
                uvj.data[li].uv = (bsu[2], bsu[3])
        print(f"[wind_bake] baked {obj.name!r} verts={len(me.vertices)} "
              f"faces={len(me.polygons)} mode=card")

    # ---- s_k（朝向继承度：0=硬(全继承朝向) / 1=软(只随自己的 θ)）----
    #   柳条判据（Danis 断言，已在模型里验证）：根部 z 高于尖部 z（下垂）**且** 管半径极细。
    #   沿枝渐变：接头 s=0（硬）→ 梢部 s=s_spline（软），摆动时"从挂点自然甩出去"。
    sp_soft = [0.0] * len(polys)
    n_strand = 0
    for i in range(len(polys)):
        arc_len = cum[i][-1] if cum[i] else 0.0
        droop = polys[i][0].z - polys[i][-1].z
        radius = (sp_dsum[i] / sp_cnt[i]) if sp_cnt[i] else 0.0
        if arc_len > 1e-6 and droop > 0.15 and radius < 0.01:
            sp_soft[i] = max(0.0, min(1.0, droop / 0.5))
            n_strand += 1
    for jd in joints:
        i = jd['spline']
        arc_len = cum[i][-1] if cum[i] else 0.0
        jd['soft'] = sp_soft[i] * (jd['arc'] / arc_len if arc_len > 1e-6 else 0.0)

    n_soft = sum(1 for jd in joints if jd['soft'] > 0.05)
    tot_v = sum(jid_hist.values())
    v_soft = sum(c for j, c in jid_hist.items() if joints[j]['soft'] > 0.25)
    v_hard = sum(c for j, c in jid_hist.items() if joints[j]['soft'] <= 0.05)
    print(f"[wind_bake] 柳条样条={n_strand}/{len(polys)}  关节 s>0.05: {n_soft}/{len(joints)}")
    print(f"[wind_bake] 树皮顶点覆盖：s>0.25 占 {100.0*v_soft/max(tot_v,1):.0f}%   "
          f"s<=0.05（硬）占 {100.0*v_hard/max(tot_v,1):.0f}%")
    return joints
