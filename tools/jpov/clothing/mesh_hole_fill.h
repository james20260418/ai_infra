// JPOV 衣物网格「补洞」（推进法 / Liepa 2003 风格的简化实现）—— 纯 CPU、GL-free、可单测。
//
// 用途：穿衣工具在把衣服挂到软体仿真器（建关联邻居表）**之前**，先把网格里的破洞/裂缝补上，
// 避免缝合处在仿真中开裂（"关联前先补洞"）。也适用于任何需要水密化的软布资产。
//
// 算法（业界主流的「边界环三角化」路线，见 Liepa 2003《Filling Holes in Meshes》）：
//   1. **焊接（weld）**：按位置把距离 ≤ weld_tolerance 的顶点并成同一「代表点」重建拓扑。
//      ⚠️ 必须：资产在 UV / 材质缝合处会拆出重复顶点，不先 weld 会把所有缝合线都当成"洞"。
//   2. 找**边界边**（只被 1 个三角形使用的边）→ 按**有向**边界追踪**边界环**（洞的边界）。
//   3. 每个环：周长 ≤ max_hole_perimeter 才补（滤掉腰口 / 下摆 / 袖口这类"本来就该开"的口）。
//   4. **初始三角化**：边界多边形做「最小面积」DP（Barequet–Sharir 思路，权重取三角形面积，
//      O(n³)）；非凸 / 非平面洞也稳。
//   5. （可选）**细分**：把补丁里过长的**内部边**（两侧都是补丁三角形）在中间插 Steiner 点，
//      避免拉出大三角面片；不碰与原网格共享的边 → 不产生 T-junction。
//   6. （可选）**fairing**：对新增（Steiner）顶点做 Laplacian 平滑（原始顶点固定），让补丁
//      贴合周围曲面。
//
// 只**新增**三角形 + Steiner 顶点；原有顶点位置 / 三角形不动。新顶点的法线 / UV / 骨骼权重
// 由两端点插值（骨骼索引取端点 A；补洞通常在蒙皮前，无骨骼通道）。
//
// 边界（v1）：不做自交消解、不做基于周围曲面的大洞重建；拓扑以"流形为主"为前提，非流形 /
// 蝴蝶结处按启发式断开（不 crash）。

#ifndef JPOV_CLOTHING_MESH_HOLE_FILL_H_
#define JPOV_CLOTHING_MESH_HOLE_FILL_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/mesh.h"

namespace jpov {
namespace clothing {

// 补洞参数。
struct HoleFillOptions {
    // 焊接容差（米）：距离 ≤ 该值的顶点视为同一「代表点」（重建拓扑用；不改变输出顶点）。
    // 必须 > 0。
    float weld_tolerance = 1.0e-4f;

    // 只补周长 ≤ 该值（米）的洞；<= 0 = 不限制（所有边界环都补）。
    // 用来滤掉腰口 / 下摆 / 袖口这类"本来就该开"的大口。
    float max_hole_perimeter = 0.0f;

    // 细分：把补丁里过长的内部边按周围边长拆分（插 Steiner 点）。
    bool refine = true;
    // 细分目标：补丁内部边 > refine_ratio × 周围中位边长 → 拆分。
    float refine_ratio = 1.5f;
    // 细分迭代上限（防止病态网格无限拆）。
    int refine_max_iterations = 20;

    // fairing：对新增顶点做 Laplacian 平滑（原始顶点固定）。
    bool fair = true;
    int fair_iterations = 8;
};

// 一次补洞的统计（供面板回显 / 单测断言）。
struct HoleFillStats {
    size_t boundary_edges = 0;    // 检测到的边界边数（焊接后）
    size_t loops_total = 0;       // 边界环总数
    size_t loops_filled = 0;      // 实际补的环数（通过周长阈值的）
    size_t triangles_added = 0;   // 新增三角形数
    size_t vertices_added = 0;    // 新增（Steiner）顶点数
    float max_filled_perimeter = 0.0f;  // 实际补的最大环周长（米）
};

namespace mesh_hole_fill_detail {

using V3d = geom::Vec3<double>;

inline V3d ToV3d(const Vec3f& p) {
    return V3d(static_cast<double>(p.x()), static_cast<double>(p.y()),
               static_cast<double>(p.z()));
}
inline Vec3f ToVec3f(const V3d& p) {
    return Vec3f(static_cast<float>(p.x()), static_cast<float>(p.y()),
                 static_cast<float>(p.z()));
}
inline V3d Sub(const V3d& a, const V3d& b) {
    return V3d(a.x() - b.x(), a.y() - b.y(), a.z() - b.z());
}
inline V3d Cross(const V3d& a, const V3d& b) {
    return V3d(a.y() * b.z() - a.z() * b.y(), a.z() * b.x() - a.x() * b.z(),
               a.x() * b.y() - a.y() * b.x());
}
inline double Len(const V3d& a) {
    return std::sqrt(a.x() * a.x() + a.y() * a.y() + a.z() * a.z());
}
inline double Dist(const V3d& a, const V3d& b) { return Len(Sub(a, b)); }

// 无向边的打包键（小端下标在前）。
inline uint64_t EdgeKey(uint32_t a, uint32_t b) {
    if (a > b) {
        std::swap(a, b);
    }
    return (static_cast<uint64_t>(a) << 32) | static_cast<uint64_t>(b);
}

// 焊接：把距离 ≤ tol 的顶点并到同一「代表点」。返回 rep[i] = 顶点 i 所属代表点的
// **原始顶点下标**（该组第一个顶点）。空间哈希：cell = tol，查 27 邻格。
inline std::vector<uint32_t> WeldVertices(const std::vector<Vec3f>& P, double tol) {
    CHECK_GT(tol, 0.0);
    const size_t n = P.size();
    std::vector<uint32_t> rep(n);
    const double inv = 1.0 / tol;
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
    auto key = [](long long x, long long y, long long z) {
        // 三个 21-bit 分量打包（网格坐标绝对值远小于 2^21）。
        return (static_cast<uint64_t>(x + 1048576) & 0x1FFFFF) |
               ((static_cast<uint64_t>(y + 1048576) & 0x1FFFFF) << 21) |
               ((static_cast<uint64_t>(z + 1048576) & 0x1FFFFF) << 42);
    };
    for (size_t i = 0; i < n; ++i) {
        const long long cx = static_cast<long long>(std::floor(P[i].x() * inv));
        const long long cy = static_cast<long long>(std::floor(P[i].y() * inv));
        const long long cz = static_cast<long long>(std::floor(P[i].z() * inv));
        bool found = false;
        for (long long dx = -1; dx <= 1 && !found; ++dx) {
            for (long long dy = -1; dy <= 1 && !found; ++dy) {
                for (long long dz = -1; dz <= 1 && !found; ++dz) {
                    const auto it = grid.find(key(cx + dx, cy + dy, cz + dz));
                    if (it == grid.end()) {
                        continue;
                    }
                    for (uint32_t cand : it->second) {
                        if (Dist(ToV3d(P[i]), ToV3d(P[cand])) <= tol) {
                            rep[i] = cand;
                            found = true;
                            break;
                        }
                    }
                }
            }
        }
        if (!found) {
            rep[i] = static_cast<uint32_t>(i);
            grid[key(cx, cy, cz)].push_back(static_cast<uint32_t>(i));
        }
    }
    return rep;
}

// 最小面积三角化：多边形顶点 Q[0..n-1]（闭环；边 (0,n-1) 视为已存在的一条边）。
// 返回三角形（Q 的下标三元组，{i,k,j} 形式）。O(n³) DP，非凸也稳。
inline std::vector<std::array<int, 3>> MinAreaTriangulate(const std::vector<V3d>& Q) {
    const int n = static_cast<int>(Q.size());
    std::vector<std::array<int, 3>> tris;
    if (n < 3) {
        return tris;
    }
    const double kInf = std::numeric_limits<double>::infinity();
    std::vector<std::vector<double>> w(n, std::vector<double>(n, 0.0));
    std::vector<std::vector<int>> kbest(n, std::vector<int>(n, -1));
    auto area = [&](int a, int b, int c) {
        return 0.5 * Len(Cross(Sub(Q[b], Q[a]), Sub(Q[c], Q[a])));
    };
    for (int gap = 2; gap < n; ++gap) {
        for (int i = 0; i + gap < n; ++i) {
            const int j = i + gap;
            double best = kInf;
            int bk = -1;
            for (int k = i + 1; k < j; ++k) {
                const double c = w[i][k] + w[k][j] + area(i, k, j);
                if (c < best) {
                    best = c;
                    bk = k;
                }
            }
            w[i][j] = best;
            kbest[i][j] = bk;
        }
    }
    std::vector<std::pair<int, int>> stack;
    stack.emplace_back(0, n - 1);
    while (!stack.empty()) {
        const std::pair<int, int> seg = stack.back();
        stack.pop_back();
        if (seg.second <= seg.first + 1) {
            continue;
        }
        const int k = kbest[seg.first][seg.second];
        tris.push_back({seg.first, k, seg.second});
        stack.emplace_back(seg.first, k);
        stack.emplace_back(k, seg.second);
    }
    return tris;
}

}  // namespace mesh_hole_fill_detail

// 对 in 补洞，结果写 *out（= in + 新三角形 + Steiner 顶点）。返回是否有任何改动。
// Pre-condition: out != nullptr。stats 可为 nullptr（则不回填）。
inline bool FillMeshHoles(const MeshData& in, const HoleFillOptions& opts,
                          MeshData* out, HoleFillStats* stats) {
    using namespace mesh_hole_fill_detail;
    CHECK(out != nullptr);
    HoleFillStats st;
    *out = in;
    if (in.positions.size() < 3) {
        if (stats != nullptr) {
            *stats = st;
        }
        return false;
    }

    const size_t n0 = in.positions.size();
    const bool has_normal = MeshHasFlag(in.flags, MeshVertexFlags::kNormal);
    const bool has_uv = MeshHasFlag(in.flags, MeshVertexFlags::kUV);
    const bool has_tangent = MeshHasFlag(in.flags, MeshVertexFlags::kTangent);
    const bool has_joints = MeshHasFlag(in.flags, MeshVertexFlags::kJoints);

    // 顶点工作区（0..n0-1 = 原始；n0.. = 新 Steiner）。位置用 double。
    std::vector<V3d> vpos(n0);
    for (size_t i = 0; i < n0; ++i) {
        vpos[i] = ToV3d(in.positions[i]);
    }
    std::vector<Vec3f> vnorm(has_normal ? n0 : 0);
    std::vector<Vec2f> vuv(has_uv ? n0 : 0);
    std::vector<Vec3f> vtan(has_tangent ? n0 : 0);
    std::vector<std::array<int32_t, 4>> vjidx(has_joints ? n0 : 0);
    std::vector<std::array<float, 4>> vjw(has_joints ? n0 : 0);
    for (size_t i = 0; i < n0; ++i) {
        if (has_normal) {
            vnorm[i] = in.normals[i];
        }
        if (has_uv) {
            vuv[i] = in.uvs[i];
        }
        if (has_tangent) {
            vtan[i] = in.tangents[i];
        }
        if (has_joints) {
            vjidx[i] = in.joint_indices[i];
            vjw[i] = in.joint_weights[i];
        }
    }

    // 三角形列表（indexed 或 non-indexed）。
    std::vector<std::array<uint32_t, 3>> tris;
    if (!in.indices.empty()) {
        CHECK_EQ(in.indices.size() % 3, 0u);
        tris.reserve(in.indices.size() / 3);
        for (size_t t = 0; t + 2 < in.indices.size(); t += 3) {
            tris.push_back({in.indices[t], in.indices[t + 1], in.indices[t + 2]});
        }
    } else {
        tris.reserve(n0 / 3);
        for (size_t t = 0; t + 2 < n0; t += 3) {
            tris.push_back({static_cast<uint32_t>(t), static_cast<uint32_t>(t + 1),
                            static_cast<uint32_t>(t + 2)});
        }
    }
    CHECK(!tris.empty()) << "FillMeshHoles: 无三角形";

    // 1) 焊接（只用于建拓扑；输出仍用原始顶点）。
    const std::vector<uint32_t> rep =
        WeldVertices(in.positions, static_cast<double>(opts.weld_tolerance));

    // 2) 有向边统计：无向键 → 使用计数；并记住该键**首次出现的有向形式**（边界边的方向）。
    std::unordered_map<uint64_t, int> edge_count;
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> edge_dir;
    edge_count.reserve(tris.size() * 3);
    for (const std::array<uint32_t, 3>& t : tris) {
        const uint32_t A = rep[t[0]], B = rep[t[1]], C = rep[t[2]];
        const uint32_t vv[3] = {A, B, C};
        for (int e = 0; e < 3; ++e) {
            const uint32_t u = vv[e], v = vv[(e + 1) % 3];
            const uint64_t k = EdgeKey(u, v);
            if (edge_count.find(k) == edge_count.end()) {
                edge_dir[k] = {u, v};  // 首次方向
            }
            edge_count[k] += 1;
        }
    }

    // 3) 边界边（计数 == 1）→ **有向**邻接表（u → v）。代表点总数为 n0（rep 值 < n0）。
    std::vector<std::vector<uint32_t>> out_adj(n0);
    size_t boundary_edges = 0;
    for (const auto& kv : edge_count) {
        if (kv.second != 1) {
            continue;
        }
        ++boundary_edges;
        const std::pair<uint32_t, uint32_t>& d = edge_dir[kv.first];
        out_adj[d.first].push_back(d.second);
    }
    st.boundary_edges = boundary_edges;
    if (boundary_edges == 0) {
        if (stats != nullptr) {
            *stats = st;
        }
        return false;
    }

    // 4) 追踪**有向**边界环（沿未使用有向边行走 → 干净闭环）。
    std::unordered_set<uint64_t> used;
    std::vector<std::vector<uint32_t>> loops;
    for (uint32_t s = 0; s < n0; ++s) {
        if (out_adj[s].empty()) {
            continue;
        }
        bool any = false;
        for (uint32_t n : out_adj[s]) {
            if (used.count(EdgeKey(s, n)) == 0) {
                any = true;
                break;
            }
        }
        if (!any) {
            continue;
        }
        std::vector<uint32_t> loop;
        loop.push_back(s);
        uint32_t cur = s;
        size_t guard = n0 * 2 + 64;
        while (guard-- > 0) {
            uint32_t nxt = 0xFFFFFFFFu;
            for (uint32_t n : out_adj[cur]) {
                if (used.count(EdgeKey(cur, n)) == 0) {
                    nxt = n;
                    break;
                }
            }
            if (nxt == 0xFFFFFFFFu) {
                break;
            }
            used.insert(EdgeKey(cur, nxt));
            if (nxt == s) {
                break;
            }
            loop.push_back(nxt);
            cur = nxt;
        }
        if (loop.size() >= 3) {
            loops.push_back(std::move(loop));
        }
    }
    st.loops_total = loops.size();

    // 周围中位边长（细分目标用）。
    std::vector<double> edge_lens;
    edge_lens.reserve(edge_count.size());
    for (const auto& kv : edge_count) {
        const uint32_t a = static_cast<uint32_t>(kv.first >> 32);
        const uint32_t b = static_cast<uint32_t>(kv.first & 0xFFFFFFFFu);
        edge_lens.push_back(Dist(vpos[a], vpos[b]));
    }
    double median_edge = 0.0;
    if (!edge_lens.empty()) {
        const size_t mid = edge_lens.size() / 2;
        std::nth_element(edge_lens.begin(), edge_lens.begin() + mid, edge_lens.end());
        median_edge = edge_lens[mid];
    }

    // 5) 逐环：判周长 → DP 三角化 → 收集补丁三角形（下标为「工作区」顶点下标）。
    std::vector<std::array<uint32_t, 3>> patch;
    for (const std::vector<uint32_t>& loop : loops) {
        double perim = 0.0;
        for (size_t i = 0; i < loop.size(); ++i) {
            perim += Dist(vpos[loop[i]], vpos[loop[(i + 1) % loop.size()]]);
        }
        if (opts.max_hole_perimeter > 0.0f &&
            perim > static_cast<double>(opts.max_hole_perimeter)) {
            continue;  // 太大 = 自然开口，跳过（不封口）
        }
        std::vector<V3d> Q(loop.size());
        for (size_t i = 0; i < loop.size(); ++i) {
            Q[i] = vpos[loop[i]];
        }
        const std::vector<std::array<int, 3>> tri = MinAreaTriangulate(Q);
        // 绕序：边界环是有向的（网格内部在边左侧）；补丁要反向绕 → DP 的 {i,k,j}
        // 翻成 {i,j,k}（即把 loop[t0],loop[t2],loop[t1] 顺序写出）。
        for (const std::array<int, 3>& t : tri) {
            patch.push_back({loop[static_cast<size_t>(t[0])],
                             loop[static_cast<size_t>(t[2])],
                             loop[static_cast<size_t>(t[1])]});
        }
        ++st.loops_filled;
        st.max_filled_perimeter =
            std::max(st.max_filled_perimeter, static_cast<float>(perim));
    }
    if (patch.empty()) {
        if (stats != nullptr) {
            *stats = st;
        }
        return false;
    }

    // 新增顶点（Steiner）：从两端点插值全部属性。
    auto add_mid = [&](uint32_t a, uint32_t b) -> uint32_t {
        const uint32_t id = static_cast<uint32_t>(vpos.size());
        vpos.push_back(V3d((vpos[a].x() + vpos[b].x()) * 0.5,
                           (vpos[a].y() + vpos[b].y()) * 0.5,
                           (vpos[a].z() + vpos[b].z()) * 0.5));
        if (has_normal) {
            vnorm.push_back(Vec3f((vnorm[a].x() + vnorm[b].x()) * 0.5f,
                                  (vnorm[a].y() + vnorm[b].y()) * 0.5f,
                                  (vnorm[a].z() + vnorm[b].z()) * 0.5f));
        }
        if (has_uv) {
            vuv.push_back(Vec2f((vuv[a].x() + vuv[b].x()) * 0.5f,
                                (vuv[a].y() + vuv[b].y()) * 0.5f));
        }
        if (has_tangent) {
            vtan.push_back(Vec3f((vtan[a].x() + vtan[b].x()) * 0.5f,
                                 (vtan[a].y() + vtan[b].y()) * 0.5f,
                                 (vtan[a].z() + vtan[b].z()) * 0.5f));
        }
        if (has_joints) {
            // 骨骼索引取端点 A；权重取平均（近似）。
            vjidx.push_back(vjidx[a]);
            std::array<float, 4> w{};
            for (int k = 0; k < 4; ++k) {
                w[k] = (vjw[a][k] + vjw[b][k]) * 0.5f;
            }
            vjw.push_back(w);
        }
        return id;
    };

    // 6) 细分：反复拆「补丁内部长边」（两侧都是补丁三角形 → 无 T-junction）。
    if (opts.refine && median_edge > 0.0) {
        const double target = static_cast<double>(opts.refine_ratio) * median_edge;
        for (int iter = 0; iter < opts.refine_max_iterations; ++iter) {
            std::unordered_map<uint64_t, std::vector<size_t>> pe;
            for (size_t ti = 0; ti < patch.size(); ++ti) {
                const std::array<uint32_t, 3>& t = patch[ti];
                pe[EdgeKey(t[0], t[1])].push_back(ti);
                pe[EdgeKey(t[1], t[2])].push_back(ti);
                pe[EdgeKey(t[2], t[0])].push_back(ti);
            }
            uint64_t best_edge = 0;
            double best_len = target;
            bool found = false;
            for (const auto& kv : pe) {
                if (kv.second.size() != 2) {
                    continue;  // 只有内部边（两侧补丁）才安全拆
                }
                const uint32_t a = static_cast<uint32_t>(kv.first >> 32);
                const uint32_t b = static_cast<uint32_t>(kv.first & 0xFFFFFFFFu);
                const double len = Dist(vpos[a], vpos[b]);
                if (len > best_len) {
                    best_len = len;
                    best_edge = kv.first;
                    found = true;
                }
            }
            if (!found) {
                break;
            }
            const uint32_t a = static_cast<uint32_t>(best_edge >> 32);
            const uint32_t b = static_cast<uint32_t>(best_edge & 0xFFFFFFFFu);
            const uint32_t m = add_mid(a, b);
            std::vector<std::array<uint32_t, 3>> next;
            next.reserve(patch.size() + 4);
            for (const std::array<uint32_t, 3>& t : patch) {
                // 找到含边 (a,b) 的**有向**位置（u→v），保持绕序地插 m（u→m→v）。
                int e = -1;
                for (int i = 0; i < 3; ++i) {
                    const uint32_t u = t[i], v = t[(i + 1) % 3];
                    if ((u == a && v == b) || (u == b && v == a)) {
                        e = i;
                        break;
                    }
                }
                if (e < 0) {
                    next.push_back(t);
                    continue;
                }
                const uint32_t u = t[e], v = t[(e + 1) % 3], w = t[(e + 2) % 3];
                next.push_back({u, m, w});
                next.push_back({m, v, w});
            }
            patch.swap(next);
        }
    }

    // 7) fairing：对新增（Steiner）顶点做 Laplacian 平滑（原始顶点固定）。
    const size_t n_new_lo = n0;
    const size_t n_new_hi = vpos.size();
    if (opts.fair && n_new_hi > n_new_lo && opts.fair_iterations > 0) {
        std::vector<std::vector<uint32_t>> nb(vpos.size());
        for (const std::array<uint32_t, 3>& t : patch) {
            for (int e = 0; e < 3; ++e) {
                const uint32_t u = t[e], v = t[(e + 1) % 3];
                nb[u].push_back(v);
                nb[v].push_back(u);
            }
        }
        for (int it = 0; it < opts.fair_iterations; ++it) {
            for (uint32_t v = static_cast<uint32_t>(n_new_lo);
                 v < static_cast<uint32_t>(n_new_hi); ++v) {
                if (nb[v].empty()) {
                    continue;
                }
                V3d sum(0.0, 0.0, 0.0);
                for (uint32_t u : nb[v]) {
                    sum = V3d(sum.x() + vpos[u].x(), sum.y() + vpos[u].y(),
                              sum.z() + vpos[u].z());
                }
                const double inv_cnt = 1.0 / static_cast<double>(nb[v].size());
                const double lam = 0.5;  // 部分平滑，避免塌陷
                vpos[v] = V3d(
                    (1.0 - lam) * vpos[v].x() + lam * sum.x() * inv_cnt,
                    (1.0 - lam) * vpos[v].y() + lam * sum.y() * inv_cnt,
                    (1.0 - lam) * vpos[v].z() + lam * sum.z() * inv_cnt);
            }
        }
    }

    // 8) 落盘：追加新顶点（插值属性）+ 新三角形。原始顶点/三角形不动。
    // ⚠️ non-indexed 网格按顶点序三三成三角形，**不能**在末尾插「游离顶点」（会被当成三角形
    //    的一角）；故只有 indexed 网格才单独追加新顶点，non-indexed 直接按三角形写顶点。
    const size_t added = vpos.size() - n0;
    auto append_vertex = [&](size_t v) {
        out->positions.push_back(ToVec3f(vpos[v]));
        if (has_normal) {
            out->normals.push_back(vnorm[v]);
        }
        if (has_uv) {
            out->uvs.push_back(vuv[v]);
        }
        if (has_tangent) {
            out->tangents.push_back(vtan[v]);
        }
        if (has_joints) {
            out->joint_indices.push_back(vjidx[v]);
            out->joint_weights.push_back(vjw[v]);
        }
    };
    if (!out->indices.empty()) {
        for (size_t v = n0; v < vpos.size(); ++v) {
            append_vertex(v);
        }
        for (const std::array<uint32_t, 3>& t : patch) {
            out->indices.push_back(t[0]);
            out->indices.push_back(t[1]);
            out->indices.push_back(t[2]);
        }
    } else {
        // non-indexed：按三角形顺序把顶点属性直接追加（顶点可重复）。
        for (const std::array<uint32_t, 3>& t : patch) {
            for (uint32_t v : t) {
                append_vertex(v);
            }
        }
    }
    st.triangles_added = patch.size();
    st.vertices_added = added;
    if (stats != nullptr) {
        *stats = st;
    }
    return true;
}

}  // namespace clothing
}  // namespace jpov

#endif  // JPOV_CLOTHING_MESH_HOLE_FILL_H_
