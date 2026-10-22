// JPOV 模型编辑器 — 两个 glb 的「断面缝合」（纯函数 / GL-free，可单测）
//
// 需求（2026-10-22 Danis）：
//   用户把 target 与 reference 摆好相对位置，给**一个** y_min（同一个水平面同时切两个模型）：
//     - target   保留 **平面以上**（下半截被砍掉 → 露出底口断面）
//     - reference 保留 **平面以下**（上半截被砍掉 → 露出顶口断面）
//   然后把 target 断面附近的顶点做「形变雕刻」，使 target 的底口
//     **形状 + 位置**都严格等于 reference 的顶口，法线尽量连续。
//   这样得到的断面能直接焊到别的模型上（例：路灯上半截焊到蓝人脖子上 = 路灯人）。
//
// 为什么不能只把边界顶点搬过去：只搬边界会把相邻一圈三角形挤成折痕 / 翻面。正确做法是
//   把「边界位移」当成热源，在一个**影响带**里求光滑形变场（带外冻结），见下。
//
// 本文件的算法（三种可切换，2026-10-22 Danis 要求都试）：
//   kHarmonic : 位移调和扩散 —— 在带内解 Laplace 方程 ∇²u = 0（边界 u 已知、带外 u = 0），
//               Gauss–Seidel 迭代。等价于「保微分坐标」的 Laplacian 编辑（Lu = 0）。
//               最稳、最快，形变温和时效果最好。
//   kArap     : As-Rigid-As-Possible —— 局部求每顶点最优旋转（极分解）、全局解同一个
//               Laplace 系统（RHS 换成旋转项），迭代数轮。形变大时不易塌陷 / 剪切。
//   kTps      : 薄板样条 / RBF —— φ(r)=r 的全局光滑场，只用边界点当中心，纯代数、不看
//               网格连通性；带外按余弦权重衰减。形状差异大时最"顺"，但可能把细节抹平。
//
// 断面环（loop）处理：
//   1) 先按位置焊接（UV 缝 / 导出器拆点会把一个环拆成几段，不焊就出不了环）；
//   2) 取「落在切平面上」的边界边串成环 —— 不能取全部边界边：glb 资产本来就可能有
//      裤腰 / 袖口等既有开口；
//   3) 只接受**单个、非退化、简单**的环；否则拒绝操作（Danis：非环直接拒）。
//
// 环重建（SeamOptions::rebuild_loop，2026-10-22 Danis 选「要」，为了能焊接）：
//   形变后 target 的边界折线已经贴在 reference 断面上，但两边顶点数通常不同（T 型缝）。
//   这里把 target 贴着断面的那一圈三角形删掉，再用「拉链」三角化把新的洞边界接到
//   reference 环的**原始顶点序列**上 —— 于是 target 的边界 = reference 环，逐点一一对应，
//   后续直接按索引焊接即可。拓扑不规整（洞边界不是单环）时**自动降级**为「只保证几何重合」
//   并在 stats.loop_rebuilt 里如实标记。
//
// 语义约定：世界坐标（调用方已把 target 的变换烘进顶点）；reference 只读。
//
// Pre-condition：target / reference 的 MeshData 已 Validate()；opt.plane 在两者范围内。

#ifndef JPOV_MODEL_EDITOR_MESH_SEAM_H_
#define JPOV_MODEL_EDITOR_MESH_SEAM_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/model_editor/mesh_clip.h"

namespace jpov {
namespace model_editor {

// ==================== 对外类型 ====================

// 三种形变算法（2026-10-22 Danis：都试一试）。
enum class SeamAlgo {
    kHarmonic,  // 位移调和扩散（Laplace，Lu = 0）
    kArap,      // As-Rigid-As-Possible
    kTps,       // 薄板样条 / RBF 全局场
};

inline const char* SeamAlgoName(SeamAlgo algo) {
    switch (algo) {
        case SeamAlgo::kHarmonic:
            return "harmonic";
        case SeamAlgo::kArap:
            return "arap";
        case SeamAlgo::kTps:
            return "tps";
    }
    return "unknown";
}

// 一次缝合的参数。
struct SeamOptions {
    // 切平面：axis 分量 = plane（当前需求是 y，保留 axis 便于扩展）。
    int axis = 1;
    float plane = 0.0f;

    SeamAlgo algo = SeamAlgo::kHarmonic;

    // 影响带高度 H = band_scale × 断面等效半径（等效半径 = 周长 / 2π）。
    // 必须 > 0：给 0 会让带退化成一条线（形变直接塌）。上层用滑条给，默认 2.0。
    // ⚠️ y_span > 0 时以 y_span 为准（见下），band_scale 只作兜底。
    float band_scale = 2.0f;

    // **最大形变区高度（米，> 0 才启用；2026-10-22 Danis 定）**：
    //   在 y = plane + y_span 处给 target 插一圈顶点（切一刀但不删面，会确实增加顶点），
    //   只有 plane ~ plane+y_span 之间的顶点参与形变，该环及更上方**完全隔离**。
    //   这样形变有明确的硬边界（环 = 锚点），不会把焊缝拉到很远的地方。
    float y_span = 0.0f;

    // 环对应的旋转偏移（度）：用户手动修「扭麻花」的逃生口。
    float offset_deg = 0.0f;

    // 分步形变（continuation）步数：把边界约束从原位线性插到目标位，走 steps 步。
    // 形变大时能显著减少翻面。>= 1。
    int steps = 6;

    // 是否做环重建（把 target 边界换成 reference 环的顶点序列，保证可焊接）。
    bool rebuild_loop = true;

    // 求解迭代上限（内部按规模自适应，这里是硬上限，防止大网格卡死）。
    int max_iterations = 400;
};

// 一条断面环（有序顶点下标，首尾相接）。
//
// members：与 verts 平行。glb 资产常在 UV 缝处把同一位置拆成多个顶点，切面上就会
//   出现「位置重合但下标不同」的边界点；这些点必须在**拓扑上当成一个点**来串环，
//   同时形变时一起搬（否则缝会裂开）。members[i] 给出 verts[i] 对应的全部物理下标
//   （含 verts[i] 本身）；空表示只有它自己。
struct SeamLoop {
    std::vector<uint32_t> verts;
    std::vector<std::vector<uint32_t>> members;
    float perimeter = 0.0f;  // 米
};

// 环上的全部物理顶点下标（逐顶点去重）。
inline std::vector<uint32_t> LoopPhysicalVerts(const SeamLoop& loop) {
    std::vector<uint32_t> out;
    out.reserve(loop.verts.size());
    for (size_t i = 0; i < loop.verts.size(); ++i) {
        if (i < loop.members.size() && !loop.members[i].empty()) {
            out.insert(out.end(), loop.members[i].begin(), loop.members[i].end());
        } else {
            out.push_back(loop.verts[i]);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// 缝合统计 / 指标（供面板回显与单测断言）。
struct SeamStats {
    bool ok = false;
    std::string message;  // 失败原因 / 成功摘要

    size_t target_loop_vertices = 0;  // 形变前 target 环顶点数
    size_t ref_loop_vertices = 0;     // reference 环顶点数
    size_t band_vertices = 0;         // 参与求解的顶点数（不含冻结锚点）
    size_t iterations = 0;            // 实际迭代次数（三种算法合计）

    // 断面重合度（米）：重建成功时应为 0；降级时是几何偏差。
    float loop_hausdorff_m = 0.0f;
    float loop_rms_m = 0.0f;

    // 接缝法线连续性：target 边界法线 vs reference 对应点法线（度）。
    float normal_angle_mean_deg = 0.0f;
    float normal_angle_max_deg = 0.0f;

    size_t flipped_triangles = 0;  // 带内翻面三角形数（0 才正常）
    // 输出网格里「不在缝合面上」的开口边数（被 1 个三角形引用的边）。
    // 0 = 除了断面本身没有任何洞；>0 说明重建/形变漏了面。
    size_t unexpected_open_edges = 0;
    bool loop_rebuilt = false;     // 是否成功把边界换成 reference 环（可 1:1 焊）
    size_t inserted_vertices = 0;  // 环重建插入的顶点数（断面被细分到 reference 环）
};

// ==================== 内部工具 ====================

namespace seam_internal {

inline uint64_t EdgeKey(uint32_t a, uint32_t b) {
    if (a > b) {
        std::swap(a, b);
    }
    return (static_cast<uint64_t>(a) << 32) | static_cast<uint64_t>(b);
}

inline size_t TriangleCount(const jpov::MeshData& mesh) {
    return mesh.indices.empty() ? (mesh.positions.size() / 3)
                                : (mesh.indices.size() / 3);
}

inline std::array<uint32_t, 3> TriangleAt(const jpov::MeshData& mesh, size_t t) {
    if (mesh.indices.empty()) {
        const uint32_t base = static_cast<uint32_t>(3 * t);
        return {base, base + 1, base + 2};
    }
    return {mesh.indices[3 * t], mesh.indices[3 * t + 1],
            mesh.indices[3 * t + 2]};
}

inline float Component(const jpov::Vec3f& p, int axis) {
    return (axis == 0) ? p.x() : ((axis == 1) ? p.y() : p.z());
}

inline jpov::Vec3f MakeVec(float x, float y, float z) {
    return jpov::Vec3f(x, y, z);
}

inline jpov::Vec3f Lerp(const jpov::Vec3f& a, const jpov::Vec3f& b, float t) {
    return a + (b - a) * t;
}

// 三角形法向（未归一化 = 2×面积向量）。
inline jpov::Vec3f TriangleNormal(const jpov::MeshData& mesh,
                                  const std::vector<jpov::Vec3f>& pos,
                                  const std::array<uint32_t, 3>& tri) {
    const jpov::Vec3f& a = pos[tri[0]];
    const jpov::Vec3f& b = pos[tri[1]];
    const jpov::Vec3f& c = pos[tri[2]];
    return (b - a).Cross(c - a);
}

// 逐顶点面积加权法线（只对 touched 里的顶点算；其余留空）。
inline void RecomputeNormalsFor(const jpov::MeshData& mesh,
                                const std::vector<uint8_t>& touched,
                                std::vector<jpov::Vec3f>* normals /*inout*/) {
    for (size_t i = 0; i < touched.size(); ++i) {
        if (touched[i]) {
            (*normals)[i] = MakeVec(0.0f, 0.0f, 0.0f);
        }
    }
    const size_t tris = TriangleCount(mesh);
    for (size_t t = 0; t < tris; ++t) {
        const auto tri = TriangleAt(mesh, t);
        if (!touched[tri[0]] && !touched[tri[1]] && !touched[tri[2]]) {
            continue;
        }
        const jpov::Vec3f n = TriangleNormal(mesh, mesh.positions, tri);
        for (int k = 0; k < 3; ++k) {
            if (touched[tri[k]]) {
                (*normals)[tri[k]] += n;
            }
        }
    }
    for (size_t i = 0; i < touched.size(); ++i) {
        if (touched[i] && (*normals)[i].Norm() > 1e-12f) {
            (*normals)[i].Normalize();
        }
    }
}

// 边 → 相邻三角形列表。
inline std::unordered_map<uint64_t, std::vector<uint32_t>> BuildEdgeTriangles(
    const jpov::MeshData& mesh) {
    std::unordered_map<uint64_t, std::vector<uint32_t>> map;
    const size_t tris = TriangleCount(mesh);
    map.reserve(tris * 3);
    for (size_t t = 0; t < tris; ++t) {
        const auto tri = TriangleAt(mesh, t);
        for (int e = 0; e < 3; ++e) {
            map[EdgeKey(tri[e], tri[(e + 1) % 3])].push_back(
                static_cast<uint32_t>(t));
        }
    }
    return map;
}

// 从切平面上「新切出来」的边界边串出唯一的断面环。
// 只收「两个端点都落在平面上」的边界边：既有的开口（裤腰等）不参与。
inline bool ExtractCutLoop(const jpov::MeshData& mesh, int axis, float plane,
                           float plane_eps, SeamLoop* loop /*output*/,
                           std::string* error) {
    loop->verts.clear();
    loop->perimeter = 0.0f;
    const size_t vcount = mesh.positions.size();
    const auto edge_tris = BuildEdgeTriangles(mesh);

    // 边界边（只被一个三角形引用）且落在平面上。
    std::vector<std::pair<uint32_t, uint32_t>> raw_edges;
    std::unordered_set<uint32_t> boundary_verts;
    for (const auto& kv : edge_tris) {
        if (kv.second.size() > 2) {
            const uint32_t a = static_cast<uint32_t>(kv.first >> 32);
            const uint32_t b = static_cast<uint32_t>(kv.first & 0xffffffffu);
            if (std::fabs(Component(mesh.positions[a], axis) - plane) <= plane_eps &&
                std::fabs(Component(mesh.positions[b], axis) - plane) <= plane_eps) {
                *error = "切平面上的边是非流形边（被 " +
                         std::to_string(kv.second.size()) +
                         " 个三角形共享），无法确定断面环";
                return false;
            }
            continue;
        }
        if (kv.second.size() != 1) {
            continue;
        }
        const uint32_t a = static_cast<uint32_t>(kv.first >> 32);
        const uint32_t b = static_cast<uint32_t>(kv.first & 0xffffffffu);
        if (std::fabs(Component(mesh.positions[a], axis) - plane) > plane_eps ||
            std::fabs(Component(mesh.positions[b], axis) - plane) > plane_eps) {
            continue;  // 既有开口，不是这次切出来的
        }
        raw_edges.push_back({a, b});
        boundary_verts.insert(a);
        boundary_verts.insert(b);
    }
    if (raw_edges.empty()) {
        *error = "切平面上没有断面（模型没有跨过该平面，或整个被切掉）";
        return false;
    }

    // 位置重合的边界顶点并成一组（UV 缝 / 导出器拆点）。只合并边界点，不动模型其余部分。
    std::vector<uint32_t> group_of(vcount, 0);
    for (uint32_t v : boundary_verts) {
        group_of[v] = v;
    }
    auto find_root = [&group_of](uint32_t v) {
        while (group_of[v] != v) {
            group_of[v] = group_of[group_of[v]];
            v = group_of[v];
        }
        return v;
    };
    {
        std::vector<uint32_t> verts(boundary_verts.begin(), boundary_verts.end());
        for (size_t i = 0; i < verts.size(); ++i) {
            for (size_t j = i + 1; j < verts.size(); ++j) {
                if ((mesh.positions[verts[i]] - mesh.positions[verts[j]]).Norm() <=
                    plane_eps) {
                    const uint32_t ri = find_root(verts[i]);
                    const uint32_t rj = find_root(verts[j]);
                    if (ri != rj) {
                        group_of[std::max(ri, rj)] = std::min(ri, rj);
                    }
                }
            }
        }
    }
    // 用「组代表」重新建邻接。
    std::unordered_map<uint32_t, std::vector<uint32_t>> nbr;
    for (const auto& [a, b] : raw_edges) {
        const uint32_t ra = find_root(a);
        const uint32_t rb = find_root(b);
        if (ra == rb) {
            continue;  // 同一组的内部边（退化），忽略
        }
        nbr[ra].push_back(rb);
        nbr[rb].push_back(ra);
    }
    // 每个环顶点必须恰好有两条断面边（否则是分叉 / 退化）。
    for (const auto& kv : nbr) {
        if (kv.second.size() != 2) {
            *error = "断面不是闭合环：顶点 " + std::to_string(kv.first) + " 有 " +
                     std::to_string(kv.second.size()) + " 条断面边（应为 2）";
            return false;
        }
    }
    if (nbr.size() < 3) {
        *error = "断面环顶点数 < 3（退化）";
        return false;
    }

    // 从最小下标出发串环。
    uint32_t start = std::numeric_limits<uint32_t>::max();
    for (const auto& kv : nbr) {
        start = std::min(start, kv.first);
    }
    std::vector<uint32_t> order;
    order.reserve(nbr.size());
    uint32_t prev = std::numeric_limits<uint32_t>::max();
    uint32_t cur = start;
    while (true) {
        order.push_back(cur);
        const auto& two = nbr[cur];
        uint32_t next = (two[0] == prev) ? two[1] : two[0];
        prev = cur;
        cur = next;
        if (cur == start) {
            break;
        }
        if (order.size() > nbr.size()) {
            *error = "断面边无法串成单个环（疑似多个环 / 分叉）";
            return false;
        }
    }
    if (order.size() != nbr.size()) {
        *error = "断面有 " + std::to_string(nbr.size()) + " 个边界顶点，但只串出 " +
                 std::to_string(order.size()) + " 个 —— 断面不是单环";
        return false;
    }
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] >= vcount) {
            *error = "断面环顶点下标越界";
            return false;
        }
    }

    // 周长 + 零长边检查。
    float perimeter = 0.0f;
    for (size_t i = 0; i < order.size(); ++i) {
        const jpov::Vec3f& a = mesh.positions[order[i]];
        const jpov::Vec3f& b = mesh.positions[order[(i + 1) % order.size()]];
        const float len = (b - a).Norm();
        if (len <= 1e-9f) {
            *error = "断面环有零长度边（退化）";
            return false;
        }
        perimeter += len;
    }
    if (perimeter <= 1e-6f) {
        *error = "断面环周长近似为 0（退化）";
        return false;
    }

    // 平面内简单性检查（非相邻边不能相交）。
    {
        const int u = (axis + 1) % 3;
        const int v = (axis + 2) % 3;
        const size_t n = order.size();
        auto seg_cross = [](float ax, float ay, float bx, float by, float cx,
                            float cy, float dx, float dy) {
            auto orient = [](float px, float py, float qx, float qy, float rx,
                             float ry) {
                const float val = (qy - py) * (rx - qx) - (qx - px) * (ry - qy);
                if (std::fabs(val) < 1e-12f) {
                    return 0;
                }
                return (val > 0.0f) ? 1 : -1;
            };
            const int o1 = orient(ax, ay, bx, by, cx, cy);
            const int o2 = orient(ax, ay, bx, by, dx, dy);
            const int o3 = orient(cx, cy, dx, dy, ax, ay);
            const int o4 = orient(cx, cy, dx, dy, bx, by);
            // 只认「真交叉」：任一顶点落在另一条线段上（o=0）不算。
            // 断面环上常有近共线的密集小边（切出来的短边），用 o1!=o2 会把「相切」
            // 误判成自交，把一个合法的环直接拒掉。
            return (o1 * o2 < 0) && (o3 * o4 < 0);
        };
        for (size_t i = 0; i < n; ++i) {
            const jpov::Vec3f& p0 = mesh.positions[order[i]];
            const jpov::Vec3f& p1 = mesh.positions[order[(i + 1) % n]];
            for (size_t j = i + 2; j < n; ++j) {
                if (i == 0 && j == n - 1) {
                    continue;  // 相邻（共享端点）
                }
                const jpov::Vec3f& p2 = mesh.positions[order[j]];
                const jpov::Vec3f& p3 = mesh.positions[order[(j + 1) % n]];
                if (seg_cross(Component(p0, u), Component(p0, v),
                              Component(p1, u), Component(p1, v),
                              Component(p2, u), Component(p2, v),
                              Component(p3, u), Component(p3, v))) {
                    *error = "断面环在平面内自交（退化 / 非简单环）";
                    return false;
                }
            }
        }
    }

    // 组 → 物理下标列表。
    loop->verts = std::move(order);
    loop->members.assign(loop->verts.size(), {});
    {
        std::unordered_map<uint32_t, std::vector<uint32_t>> groups;
        for (uint32_t v : boundary_verts) {
            groups[find_root(v)].push_back(v);
        }
        for (size_t i = 0; i < loop->verts.size(); ++i) {
            auto it = groups.find(loop->verts[i]);
            if (it != groups.end() && it->second.size() > 1) {
                loop->members[i] = it->second;
            }
        }
    }
    loop->perimeter = perimeter;
    return true;
}

// 环的归一化弧长参数（每个顶点一个值，单调递增，[0,1)）。
inline std::vector<float> LoopParams(const jpov::MeshData& mesh,
                                     const SeamLoop& loop) {
    const size_t n = loop.verts.size();
    std::vector<float> param(n, 0.0f);
    float total = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        param[i] = total;
        total += (mesh.positions[loop.verts[(i + 1) % n]] -
                  mesh.positions[loop.verts[i]])
                     .Norm();
    }
    if (total <= 1e-12f) {
        return param;
    }
    for (size_t i = 0; i < n; ++i) {
        param[i] /= total;
    }
    return param;
}

// 按归一化参数 u（可为任意实数，按 1 取模）取环上的点。
inline jpov::Vec3f SampleLoop(const jpov::MeshData& mesh, const SeamLoop& loop,
                              float u) {
    const size_t n = loop.verts.size();
    u = u - std::floor(u);
    const std::vector<float> param = LoopParams(mesh, loop);
    // param 单调 → 二分。
    size_t hi = static_cast<size_t>(
        std::upper_bound(param.begin(), param.end(), u) - param.begin());
    if (hi == 0) {
        hi = 1;
    }
    const size_t i = hi - 1;
    const size_t j = (i + 1) % n;
    const float p0 = param[i];
    const float span = (i + 1 < n) ? (param[i + 1] - p0) : (1.0f - p0);
    const float t = (span > 1e-12f) ? ((u - p0) / span) : 0.0f;
    return Lerp(mesh.positions[loop.verts[i]], mesh.positions[loop.verts[j]], t);
}

// 等弧长重采样成 K 个点。
inline std::vector<jpov::Vec3f> ResampleLoop(const jpov::MeshData& mesh,
                                             const SeamLoop& loop, int K) {
    std::vector<jpov::Vec3f> out;
    out.reserve(static_cast<size_t>(K));
    for (int k = 0; k < K; ++k) {
        out.push_back(SampleLoop(mesh, loop, static_cast<float>(k) / K));
    }
    return out;
}

// 找最佳循环对应：v = (σ·u + s/K) mod 1（σ = ±1）。返回 s ∈ [0,K)，*reversed=σ<0。
// 判据：对应点在空间上最近（位移最小），自动避开「扭一圈」。
inline void BestCyclicCorrespondence(const std::vector<jpov::Vec3f>& P,
                                     const std::vector<jpov::Vec3f>& Q,
                                     int* shift /*output*/, bool* reversed /*output*/) {
    CHECK_EQ(P.size(), Q.size());
    const int K = static_cast<int>(P.size());
    double best = std::numeric_limits<double>::max();
    int best_shift = 0;
    bool best_rev = false;
    for (int rev = 0; rev < 2; ++rev) {
        for (int s = 0; s < K; ++s) {
            double cost = 0.0;
            for (int k = 0; k < K; ++k) {
                const int idx = rev ? ((s - k) % K + K) % K : (k + s) % K;
                const jpov::Vec3f d = P[k] - Q[idx];
                cost += static_cast<double>(d.Dot(d));
            }
            if (cost < best - 1e-12) {
                best = cost;
                best_shift = s;
                best_rev = (rev != 0);
            }
        }
    }
    *shift = best_shift;
    *reversed = best_rev;
}

// 多源 Dijkstra（沿网格边）求到断面环的测地距离。
inline std::vector<float> GeodesicFromLoop(const jpov::MeshData& mesh,
                                           const SeamLoop& loop) {
    const float kInf = std::numeric_limits<float>::max();
    std::vector<float> dist(mesh.positions.size(), kInf);
    std::vector<std::vector<std::pair<uint32_t, float>>> adj(
        mesh.positions.size());
    const size_t tris = TriangleCount(mesh);
    for (size_t t = 0; t < tris; ++t) {
        const auto tri = TriangleAt(mesh, t);
        for (int e = 0; e < 3; ++e) {
            const uint32_t a = tri[e];
            const uint32_t b = tri[(e + 1) % 3];
            const float len = (mesh.positions[b] - mesh.positions[a]).Norm();
            adj[a].push_back({b, len});
            adj[b].push_back({a, len});
        }
    }
    // 固定大小的二叉堆（std::priority_queue 足够；规模是几千顶点）。
    using Item = std::pair<float, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
    for (uint32_t v : loop.verts) {
        dist[v] = 0.0f;
        heap.push({0.0f, v});
    }
    while (!heap.empty()) {
        const auto [d, v] = heap.top();
        heap.pop();
        if (d > dist[v]) {
            continue;
        }
        for (const auto& [nb, w] : adj[v]) {
            const float nd = d + w;
            if (nd < dist[nb]) {
                dist[nb] = nd;
                heap.push({nd, nb});
            }
        }
    }
    return dist;
}

// 影响带权重：1 在断面上，余弦衰减到 0（d = H）。
inline float FalloffWeight(float d, float H) {
    if (!(H > 1e-9f) || d >= H) {
        return 0.0f;
    }
    return 0.5f * (1.0f + std::cos(3.14159265358979323846f * d / H));
}

// ==================== 形变求解 ====================

// 影响带图：自由顶点 + 邻接（cotangent 权重）。
struct BandGraph {
    std::vector<uint8_t> is_free;  // 参与求解（不含断面环、不含冻结锚点）
    std::vector<float> dist;       // 到断面的测地距离
    float band_height = 0.0f;
    // 只含「至少一个端点在带内」的边。
    std::vector<std::vector<std::pair<uint32_t, float>>> nbr;
};

// 构建影响带。is_loop 标记断面环顶点。
inline BandGraph BuildBandGraph(const jpov::MeshData& mesh,
                                const std::vector<uint8_t>& is_loop,
                                const std::vector<float>& dist, float H) {
    BandGraph g;
    g.dist = dist;
    g.band_height = H;
    const size_t vcount = mesh.positions.size();
    g.is_free.assign(vcount, 0);
    g.nbr.assign(vcount, {});
    for (size_t v = 0; v < vcount; ++v) {
        g.is_free[v] = (!is_loop[v] && dist[v] <= H) ? 1 : 0;
    }

    // 每条边累加 cotangent 权重（非流形边只取前两个三角形）。
    const size_t tris = TriangleCount(mesh);
    std::unordered_map<uint64_t, float> weights;
    weights.reserve(tris * 3);
    for (size_t t = 0; t < tris; ++t) {
        const auto tri = TriangleAt(mesh, t);
        for (int e = 0; e < 3; ++e) {
            const uint32_t i = tri[e];
            const uint32_t j = tri[(e + 1) % 3];
            const uint32_t k = tri[(e + 2) % 3];
            const jpov::Vec3f u = mesh.positions[i] - mesh.positions[k];
            const jpov::Vec3f v = mesh.positions[j] - mesh.positions[k];
            const float cross = u.Cross(v).Norm();
            if (cross < 1e-12f) {
                continue;
            }
            const float cot = u.Dot(v) / cross;
            weights[EdgeKey(i, j)] += 0.5f * std::max(cot, 0.0f);
        }
    }
    for (const auto& kv : weights) {
        const uint32_t a = static_cast<uint32_t>(kv.first >> 32);
        const uint32_t b = static_cast<uint32_t>(kv.first & 0xffffffffu);
        const bool in_a = is_loop[a] || g.is_free[a];
        const bool in_b = is_loop[b] || g.is_free[b];
        if (!in_a || !in_b) {
            continue;
        }
        // 权重下限：避免退化成「无邻居」。
        const float w = std::max(kv.second, 1e-4f);
        g.nbr[a].push_back({b, w});
        g.nbr[b].push_back({a, w});
    }
    for (size_t v = 0; v < vcount; ++v) {
        if (g.is_free[v] && g.nbr[v].empty()) {
            g.is_free[v] = 0;  // 选不到邻居的孤立顶点不参与（保持原位）
        }
    }
    return g;
}

// 位置焊接分组：同一位置的多个顶点（UV 缝 / 导出器拆点）归成一组。
//
// 为什么形变必须按组算（2026-10-22 Danis 要求复查）：Laplace / ARAP 解的是**顶点邻接图**。
// 缝两侧是两套独立顶点、邻居不同 ⇒ 同一位置会被解出两个不同位移 ⇒ 缝裂开。
// 做法：按组求解，再把「组位移」原样加到每个成员上（成员各自的微小位置差保持不变）。
struct WeldGroups {
    std::vector<uint32_t> rep;     // 顶点 → 组号
    std::vector<uint32_t> leader;  // 组号 → 代表顶点
};

inline WeldGroups BuildWeldGroups(const jpov::MeshData& mesh) {
    WeldGroups wg;
    const size_t n = mesh.positions.size();
    wg.rep.resize(n);
    // 用 float 位模式做键：glb 里 UV 缝的重复顶点是**逐位相同**的，不需要容差。
    struct KeyHash {
        size_t operator()(const std::array<uint32_t, 3>& k) const {
            size_t h = k[0] * 73856093u;
            h ^= k[1] * 19349663u;
            h ^= k[2] * 83492791u;
            return h;
        }
    };
    std::unordered_map<std::array<uint32_t, 3>, uint32_t, KeyHash> map;
    map.reserve(n);
    for (size_t v = 0; v < n; ++v) {
        const jpov::Vec3f& p = mesh.positions[v];
        std::array<uint32_t, 3> key{};
        std::memcpy(&key[0], &p.x(), sizeof(float));
        std::memcpy(&key[1], &p.y(), sizeof(float));
        std::memcpy(&key[2], &p.z(), sizeof(float));
        auto it = map.find(key);
        if (it == map.end()) {
            const uint32_t g = static_cast<uint32_t>(wg.leader.size());
            map.emplace(key, g);
            wg.leader.push_back(static_cast<uint32_t>(v));
            wg.rep[v] = g;
        } else {
            wg.rep[v] = it->second;
        }
    }
    return wg;
}

// 组间多源 Dijkstra（沿网格边，长度用代表顶点位置算）。
inline std::vector<float> GeodesicOnGroups(const jpov::MeshData& mesh,
                                           const WeldGroups& wg,
                                           const SeamLoop& loop) {
    const size_t gn = wg.leader.size();
    const float kInf = std::numeric_limits<float>::max();
    std::vector<float> dist(gn, kInf);
    std::vector<std::vector<std::pair<uint32_t, float>>> adj(gn);
    const size_t tris = TriangleCount(mesh);
    for (size_t t = 0; t < tris; ++t) {
        const auto tri = TriangleAt(mesh, t);
        for (int e = 0; e < 3; ++e) {
            const uint32_t a = tri[e];
            const uint32_t b = tri[(e + 1) % 3];
            const uint32_t ga = wg.rep[a];
            const uint32_t gb = wg.rep[b];
            if (ga == gb) {
                continue;
            }
            const float len = (mesh.positions[b] - mesh.positions[a]).Norm();
            adj[ga].push_back({gb, len});
            adj[gb].push_back({ga, len});
        }
    }
    using Item = std::pair<float, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
    for (uint32_t v : LoopPhysicalVerts(loop)) {
        const uint32_t g = wg.rep[v];
        if (dist[g] > 0.0f) {
            dist[g] = 0.0f;
            heap.push({0.0f, g});
        }
    }
    while (!heap.empty()) {
        const auto [d, g] = heap.top();
        heap.pop();
        if (d > dist[g]) {
            continue;
        }
        for (const auto& [nb, w] : adj[g]) {
            const float nd = d + w;
            if (nd < dist[nb]) {
                dist[nb] = nd;
                heap.push({nd, nb});
            }
        }
    }
    return dist;
}

// 组级影响带图（cotangent 权重按三角形累加到组边上）。
inline BandGraph BuildBandGraphGrouped(const jpov::MeshData& mesh, const WeldGroups& wg,
                                       const std::vector<uint8_t>& g_is_loop,
                                       const std::vector<float>& g_dist, float H,
                                       const std::vector<uint8_t>& anchored) {
    BandGraph g;
    g.dist = g_dist;
    g.band_height = H;
    const size_t gn = wg.leader.size();
    g.is_free.assign(gn, 0);
    g.nbr.assign(gn, {});
    for (size_t gi = 0; gi < gn; ++gi) {
        const bool blocked = (!anchored.empty() && gi < anchored.size() && anchored[gi]);
        g.is_free[gi] = (!g_is_loop[gi] && !blocked && g_dist[gi] <= H) ? 1 : 0;
    }
    std::unordered_map<uint64_t, float> weights;
    const size_t tris = TriangleCount(mesh);
    for (size_t t = 0; t < tris; ++t) {
        const auto tri = TriangleAt(mesh, t);
        for (int e = 0; e < 3; ++e) {
            const uint32_t i = tri[e];
            const uint32_t j = tri[(e + 1) % 3];
            const uint32_t k = tri[(e + 2) % 3];
            const jpov::Vec3f u = mesh.positions[i] - mesh.positions[k];
            const jpov::Vec3f v = mesh.positions[j] - mesh.positions[k];
            const float cross = u.Cross(v).Norm();
            if (cross < 1e-12f) {
                continue;
            }
            const float cot = u.Dot(v) / cross;
            weights[EdgeKey(wg.rep[i], wg.rep[j])] += 0.5f * std::max(cot, 0.0f);
        }
    }
    for (const auto& kv : weights) {
        const uint32_t a = static_cast<uint32_t>(kv.first >> 32);
        const uint32_t b = static_cast<uint32_t>(kv.first & 0xffffffffu);
        if (a == b) {
            continue;
        }
        const bool in_a = g_is_loop[a] || g.is_free[a];
        const bool in_b = g_is_loop[b] || g.is_free[b];
        if (!in_a || !in_b) {
            continue;
        }
        const float w = std::max(kv.second, 1e-4f);
        g.nbr[a].push_back({b, w});
        g.nbr[b].push_back({a, w});
    }
    for (size_t gi = 0; gi < gn; ++gi) {
        if (g.is_free[gi] && g.nbr[gi].empty()) {
            g.is_free[gi] = 0;
        }
    }
    return g;
}

// 单个顶点在给定位置数组下的取值：断面环用目标位，自由顶点用当前位，其余用原位。
inline jpov::Vec3f PositionOf(uint32_t v, const std::vector<uint8_t>& is_loop,
                              const std::vector<jpov::Vec3f>& goal,
                              const std::vector<jpov::Vec3f>& cur) {
    return is_loop[v] ? goal[v] : cur[v];
}

// kHarmonic：Gauss–Seidel 解 ∇²u = 0（边界 = goal，带外 = 原位）。
inline size_t SolveHarmonic(const BandGraph& g,
                            const std::vector<uint8_t>& is_loop,
                            const std::vector<jpov::Vec3f>& goal,
                            int iters, std::vector<jpov::Vec3f>* cur /*inout*/) {
    size_t done = 0;
    for (int it = 0; it < iters; ++it) {
        for (uint32_t i = 0; i < g.is_free.size(); ++i) {
            if (!g.is_free[i]) {
                continue;
            }
            jpov::Vec3f acc = MakeVec(0.0f, 0.0f, 0.0f);
            float wsum = 0.0f;
            for (const auto& [j, w] : g.nbr[i]) {
                acc += PositionOf(j, is_loop, goal, *cur) * w;
                wsum += w;
            }
            if (wsum > 1e-12f) {
                (*cur)[i] = acc * (1.0f / wsum);
            }
        }
        ++done;
    }
    return done;
}

// 3×3 对称矩阵 Jacobi 特征分解：A = V·diag(d)·Vᵀ（V 的列是特征向量）。
inline void JacobiEigen3(double A[3][3], double V[3][3], double d[3]) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            V[i][j] = (i == j) ? 1.0 : 0.0;
        }
    }
    for (int sweep = 0; sweep < 24; ++sweep) {
        double off = 0.0;
        for (int p = 0; p < 3; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                off += A[p][q] * A[p][q];
            }
        }
        if (off < 1e-24) {
            break;
        }
        for (int p = 0; p < 3; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                if (std::fabs(A[p][q]) < 1e-18) {
                    continue;
                }
                const double theta = (A[q][q] - A[p][p]) / (2.0 * A[p][q]);
                const double t = (theta >= 0.0)
                                     ? 1.0 / (theta + std::sqrt(1.0 + theta * theta))
                                     : 1.0 / (theta - std::sqrt(1.0 + theta * theta));
                const double c = 1.0 / std::sqrt(1.0 + t * t);
                const double s = t * c;
                for (int k = 0; k < 3; ++k) {
                    const double akp = A[k][p];
                    const double akq = A[k][q];
                    A[k][p] = c * akp - s * akq;
                    A[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k) {
                    const double apk = A[p][k];
                    const double aqk = A[q][k];
                    A[p][k] = c * apk - s * aqk;
                    A[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k) {
                    const double vkp = V[k][p];
                    const double vkq = V[k][q];
                    V[k][p] = c * vkp - s * vkq;
                    V[k][q] = s * vkp + c * vkq;
                }
            }
        }
    }
    for (int i = 0; i < 3; ++i) {
        d[i] = A[i][i];
    }
}

// 极分解：S = R·P，R 正交且 det>0。S 退化时退到单位阵。
inline void PolarDecompose(const double S[3][3], double R[3][3]) {
    double A[3][3];
    double V[3][3];
    double d[3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            A[i][j] = 0.0;
            for (int k = 0; k < 3; ++k) {
                A[i][j] += S[k][i] * S[k][j];  // A = SᵀS
            }
        }
    }
    JacobiEigen3(A, V, d);
    double smax = 0.0;
    for (int i = 0; i < 3; ++i) {
        d[i] = std::max(0.0, d[i]);
        smax = std::max(smax, d[i]);
    }
    if (smax <= 1e-20) {
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                R[i][j] = (i == j) ? 1.0 : 0.0;
            }
        }
        return;
    }
    const double floor = std::max(1e-12, 1e-8 * smax);
    double inv[3];
    for (int i = 0; i < 3; ++i) {
        inv[i] = 1.0 / std::sqrt(std::max(d[i], floor));
    }
    // R = S·V·diag(inv)·Vᵀ
    double SV[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            SV[i][j] = S[i][0] * V[0][j] + S[i][1] * V[1][j] + S[i][2] * V[2][j];
        }
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            R[i][j] = SV[i][0] * inv[0] * V[j][0] + SV[i][1] * inv[1] * V[j][1] +
                      SV[i][2] * inv[2] * V[j][2];
        }
    }
}

// kArap：局部旋转 + 全局 Laplace（同一套 GS 迭代）。
inline size_t SolveArap(const jpov::MeshData& mesh, const BandGraph& g,
                        const std::vector<uint8_t>& is_loop,
                        const std::vector<jpov::Vec3f>& orig,
                        const std::vector<jpov::Vec3f>& goal, int outer_iters,
                        int inner_iters, std::vector<jpov::Vec3f>* cur /*inout*/) {
    (void)mesh;
    // 注意：顶点数取 g.is_free.size()（组级求解时这里是组数，不是 mesh 顶点数）。
    const size_t vcount = g.is_free.size();
    std::vector<std::array<double, 9>> rot(vcount);
    for (size_t v = 0; v < vcount; ++v) {
        rot[v] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    }
    size_t iters = 0;
    for (int outer = 0; outer < outer_iters; ++outer) {
        // 局部步：S = Σ w (p_j - p_i)(p'_j - p'_i)ᵀ → 极分解。
        for (size_t i = 0; i < vcount; ++i) {
            if (!g.is_free[i]) {
                continue;
            }
            double S[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
            for (const auto& [j, w] : g.nbr[i]) {
                const jpov::Vec3f e0 = orig[j] - orig[i];
                const jpov::Vec3f pj = PositionOf(j, is_loop, goal, *cur);
                const jpov::Vec3f e1 = pj - (*cur)[i];
                const double a[3] = {e0.x(), e0.y(), e0.z()};
                const double b[3] = {e1.x(), e1.y(), e1.z()};
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        S[r][c] += w * a[r] * b[c];
                    }
                }
            }
            double R[3][3];
            PolarDecompose(S, R);
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    rot[i][r * 3 + c] = R[r][c];
                }
            }
        }
        // 全局步。
        for (int it = 0; it < inner_iters; ++it) {
            for (size_t i = 0; i < vcount; ++i) {
                if (!g.is_free[i]) {
                    continue;
                }
                jpov::Vec3f acc = MakeVec(0.0f, 0.0f, 0.0f);
                float wsum = 0.0f;
                for (const auto& [j, w] : g.nbr[i]) {
                    // R_mid = (R_i + R_j)/2
                    const double rm[9] = {
                        0.5 * (rot[i][0] + rot[j][0]), 0.5 * (rot[i][1] + rot[j][1]),
                        0.5 * (rot[i][2] + rot[j][2]), 0.5 * (rot[i][3] + rot[j][3]),
                        0.5 * (rot[i][4] + rot[j][4]), 0.5 * (rot[i][5] + rot[j][5]),
                        0.5 * (rot[i][6] + rot[j][6]), 0.5 * (rot[i][7] + rot[j][7]),
                        0.5 * (rot[i][8] + rot[j][8])};
                    const double e[3] = {orig[i].x() - orig[j].x(),
                                         orig[i].y() - orig[j].y(),
                                         orig[i].z() - orig[j].z()};
                    const jpov::Vec3f rotated =
                        MakeVec(static_cast<float>(rm[0] * e[0] + rm[1] * e[1] + rm[2] * e[2]),
                                static_cast<float>(rm[3] * e[0] + rm[4] * e[1] + rm[5] * e[2]),
                                static_cast<float>(rm[6] * e[0] + rm[7] * e[1] + rm[8] * e[2]));
                    acc += rotated * w + PositionOf(j, is_loop, goal, *cur) * w;
                    wsum += w;
                }
                if (wsum > 1e-12f) {
                    (*cur)[i] = acc * (1.0f / wsum);
                }
            }
            ++iters;
        }
    }
    // 断面环永远是硬约束（全局步的 GS 不更新它，这里再兜一次）。
    for (size_t v = 0; v < vcount; ++v) {
        if (is_loop[v]) {
            (*cur)[v] = goal[v];
        }
    }
    return iters;
}

// 稠密高斯消元（列主元），解 A·X = B（B 有 nrhs 列，行主序）。
inline bool SolveDense(std::vector<double> A, std::vector<double> B, int n,
                       int nrhs, std::vector<double>* X /*output*/) {
    X->assign(static_cast<size_t>(n) * nrhs, 0.0);
    for (int col = 0; col < n; ++col) {
        int pivot = col;
        double best = std::fabs(A[static_cast<size_t>(col) * n + col]);
        for (int r = col + 1; r < n; ++r) {
            const double v = std::fabs(A[static_cast<size_t>(r) * n + col]);
            if (v > best) {
                best = v;
                pivot = r;
            }
        }
        if (best < 1e-14) {
            return false;
        }
        if (pivot != col) {
            for (int c = 0; c < n; ++c) {
                std::swap(A[static_cast<size_t>(col) * n + c],
                          A[static_cast<size_t>(pivot) * n + c]);
            }
            for (int c = 0; c < nrhs; ++c) {
                std::swap(B[static_cast<size_t>(col) * nrhs + c],
                          B[static_cast<size_t>(pivot) * nrhs + c]);
            }
        }
        const double diag = A[static_cast<size_t>(col) * n + col];
        for (int r = col + 1; r < n; ++r) {
            const double factor = A[static_cast<size_t>(r) * n + col] / diag;
            if (factor == 0.0) {
                continue;
            }
            A[static_cast<size_t>(r) * n + col] = 0.0;
            for (int c = col + 1; c < n; ++c) {
                A[static_cast<size_t>(r) * n + c] -=
                    factor * A[static_cast<size_t>(col) * n + c];
            }
            for (int c = 0; c < nrhs; ++c) {
                B[static_cast<size_t>(r) * nrhs + c] -=
                    factor * B[static_cast<size_t>(col) * nrhs + c];
            }
        }
    }
    for (int r = n - 1; r >= 0; --r) {
        for (int c = 0; c < nrhs; ++c) {
            double acc = B[static_cast<size_t>(r) * nrhs + c];
            for (int k = r + 1; k < n; ++k) {
                acc -= A[static_cast<size_t>(r) * n + k] *
                       (*X)[static_cast<size_t>(k) * nrhs + c];
            }
            (*X)[static_cast<size_t>(r) * nrhs + c] =
                acc / A[static_cast<size_t>(r) * n + r];
        }
    }
    return true;
}

// kTps：薄板样条（2D，φ(r) = r²·ln r）+ 仿射项；中心 = 断面环顶点。
//
// 必须在**断面平面内**做 2D：断面所有点共面，若直接拿 3D 的 φ(r)=r + 线性项 [1,x,y,z]，
// 由于所有中心的 y 相同，y 那一列与常数列线性相关 ⇒ 系统奇异（实测直接求解失败）。
// 平面内 2D 薄板样条是良态的，而且断面本来就是平面上的曲线，物理意义也更对。
inline bool SolveTps(const std::vector<jpov::Vec3f>& pos,
                     const BandGraph& g, const std::vector<uint8_t>& is_loop,
                     const std::vector<jpov::Vec3f>& orig,
                     const std::vector<jpov::Vec3f>& goal, int axis,
                     std::vector<jpov::Vec3f>* cur /*inout*/, size_t* iters) {
    std::vector<uint32_t> centers;
    for (size_t v = 0; v < pos.size(); ++v) {
        if (is_loop[v]) {
            centers.push_back(static_cast<uint32_t>(v));
        }
    }
    const int K = static_cast<int>(centers.size());
    if (K < 3) {
        return false;
    }
    const int iu = (axis + 1) % 3;
    const int iv = (axis + 2) % 3;
    // 归一化尺度（米）：用断面尺寸，保证 φ 数值不爆。
    float scale = 0.0f;
    for (int i = 1; i < K; ++i) {
        const jpov::Vec3f d = pos[centers[i]] - pos[centers[0]];
        scale = std::max(scale, d.Norm());
    }
    if (scale < 1e-9f) {
        return false;
    }
    const int n = K + 3;
    std::vector<double> A(static_cast<size_t>(n) * n, 0.0);
    std::vector<double> B(static_cast<size_t>(n) * 2, 0.0);
    auto kernel = [](double r) {
        return (r > 1e-9) ? (r * r * std::log(r)) : 0.0;
    };
    for (int i = 0; i < K; ++i) {
        const jpov::Vec3f& ci = pos[centers[i]];
        for (int j = 0; j < K; ++j) {
            const jpov::Vec3f d = pos[centers[j]] - ci;
            const double r = std::sqrt(static_cast<double>(d.x() * d.x() +
                                                           d.y() * d.y() +
                                                           d.z() * d.z())) /
                             scale;
            A[static_cast<size_t>(i) * n + j] = kernel(r);
        }
        A[static_cast<size_t>(i) * n + i] += 1e-8;  // 正则
        const double du =
            (pos[centers[i]][iu] - pos[centers[0]][iu]) / scale;
        const double dv =
            (pos[centers[i]][iv] - pos[centers[0]][iv]) / scale;
        A[static_cast<size_t>(i) * n + (K + 0)] = 1.0;
        A[static_cast<size_t>(i) * n + (K + 1)] = du;
        A[static_cast<size_t>(i) * n + (K + 2)] = dv;
        A[static_cast<size_t>(K + 0) * n + i] = 1.0;
        A[static_cast<size_t>(K + 1) * n + i] = du;
        A[static_cast<size_t>(K + 2) * n + i] = dv;
        const jpov::Vec3f disp = goal[centers[i]] - orig[centers[i]];
        B[static_cast<size_t>(i) * 2 + 0] = disp[iu];
        B[static_cast<size_t>(i) * 2 + 1] = disp[iv];
    }
    std::vector<double> X;
    if (!SolveDense(A, B, n, 2, &X)) {
        return false;
    }
    const float cx = pos[centers[0]][iu];
    const float cz = pos[centers[0]][iv];
    for (uint32_t v = 0; v < g.is_free.size(); ++v) {
        if (!g.is_free[v]) {
            continue;
        }
        const jpov::Vec3f& p = orig[v];
        const double du = (p[iu] - cx) / scale;
        const double dv = (p[iv] - cz) / scale;
        double out_u = X[(K + 0) * 2 + 0] + X[(K + 1) * 2 + 0] * du +
                       X[(K + 2) * 2 + 0] * dv;
        double out_v = X[(K + 0) * 2 + 1] + X[(K + 1) * 2 + 1] * du +
                       X[(K + 2) * 2 + 1] * dv;
        for (int i = 0; i < K; ++i) {
            const jpov::Vec3f d = p - pos[centers[i]];
            const double r =
                std::sqrt(static_cast<double>(d.x() * d.x() + d.y() * d.y() +
                                              d.z() * d.z())) /
                scale;
            const double k = kernel(r);
            out_u += X[static_cast<size_t>(i) * 2 + 0] * k;
            out_v += X[static_cast<size_t>(i) * 2 + 1] * k;
        }
        const float w = FalloffWeight(g.dist[v], g.band_height);
        jpov::Vec3f disp = MakeVec(0.0f, 0.0f, 0.0f);
        disp[iu] = static_cast<float>(out_u);
        disp[iv] = static_cast<float>(out_v);
        (*cur)[v] = orig[v] + disp * w;
    }
    *iters = 1;
    return true;
}

// 翻面检测：只统计「带内」三角形（法向反号）。
inline size_t CountFlipped(const jpov::MeshData& mesh,
                           const std::vector<jpov::Vec3f>& orig,
                           const std::vector<jpov::Vec3f>& cur,
                           const std::vector<uint8_t>& touch) {
    size_t flips = 0;
    const size_t tris = TriangleCount(mesh);
    auto normal_at = [](const std::vector<jpov::Vec3f>& pos,
                        const std::array<uint32_t, 3>& tri) {
        return (pos[tri[1]] - pos[tri[0]]).Cross(pos[tri[2]] - pos[tri[0]]);
    };
    for (size_t t = 0; t < tris; ++t) {
        const auto tri = TriangleAt(mesh, t);
        if (!touch[tri[0]] && !touch[tri[1]] && !touch[tri[2]]) {
            continue;
        }
        const jpov::Vec3f n0 = normal_at(orig, tri);
        const jpov::Vec3f n1 = normal_at(cur, tri);
        if (n0.Norm() > 1e-12f && n1.Norm() > 1e-12f &&
            n1.Dot(n0) < 0.0f) {
            ++flips;
        }
    }
    return flips;
}

// ==================== 环重建（保证 1:1 可焊接） ====================

// 复制逐顶点属性（uv / 骨权 / 切线 / relax / wind）。
inline void CopyVertexAttributes(const jpov::MeshData& src, uint32_t si,
                                 jpov::MeshData* dst, uint32_t di) {
    if (!src.uvs.empty()) {
        dst->uvs[di] = src.uvs[si];
    }
    if (!src.joint_indices.empty()) {
        dst->joint_indices[di] = src.joint_indices[si];
        dst->joint_weights[di] = src.joint_weights[si];
    }
    if (!src.tangents.empty()) {
        dst->tangents[di] = src.tangents[si];
    }
    if (!src.relaxations.empty()) {
        dst->relaxations[di] = src.relaxations[si];
    }
    if (!src.wind_link.empty()) {
        dst->wind_link[di] = src.wind_link[si];
    }
}

inline void ResizeVertexArrays(MeshData* mesh, size_t new_count) {
    mesh->positions.resize(new_count);
    if (!mesh->normals.empty()) {
        mesh->normals.resize(new_count, MakeVec(0.0f, 0.0f, 1.0f));
    }
    if (!mesh->uvs.empty()) {
        mesh->uvs.resize(new_count, jpov::Vec2f(0.0f, 0.0f));
    }
    if (!mesh->tangents.empty()) {
        mesh->tangents.resize(new_count, MakeVec(1.0f, 0.0f, 0.0f));
    }
    if (!mesh->relaxations.empty()) {
        mesh->relaxations.resize(new_count, jpov::Vec2f(0.0f, 0.0f));
    }
    if (!mesh->wind_link.empty()) {
        mesh->wind_link.resize(new_count, jpov::Vec2f(0.0f, 0.0f));
    }
    if (!mesh->joint_indices.empty()) {
        mesh->joint_indices.resize(new_count, {0, 0, 0, 0});
        mesh->joint_weights.resize(new_count, {1.0f, 0.0f, 0.0f, 0.0f});
    }
}

// 在平面 y=plane 处给网格**插一圈顶点**：跨面的三角形被切成上下两半、共享新顶点，
// 但**不删任何面**（与 ClipMeshByAxis 的区别）。
//
// 用途（2026-10-22 Danis）：y_span 形变区需要一个明确的硬边界 —— 环上顶点当锚点，
// 环以上完全不动。没有这一圈的话，锚点落在三角形中间，最后一行三角形会被拉成长条。
//
// 返回新插入的顶点下标（调用方把它们当锚点）。失败（网格不是索引化）返回空。
inline std::vector<uint32_t> InsertRingAtPlane(MeshData* mesh, int axis, float plane,
                                               float eps) {
    std::vector<uint32_t> ring;
    if (mesh->indices.empty()) {
        return ring;
    }
    std::vector<jpov::Vec3f>& pos = mesh->positions;
    const size_t vcount_before = pos.size();
    std::unordered_map<uint64_t, uint32_t> edge_vertex;  // 无序边 → 新顶点
    std::vector<uint32_t> new_indices;
    new_indices.reserve(mesh->indices.size() + 32);

    auto coord = [axis](const jpov::Vec3f& p) {
        return (axis == 0) ? p.x() : ((axis == 1) ? p.y() : p.z());
    };
    auto lerp_vertex = [&](uint32_t a, uint32_t b, float t) {
        const uint64_t key = EdgeKey(a, b);
        auto it = edge_vertex.find(key);
        if (it != edge_vertex.end()) {
            return it->second;
        }
        const uint32_t nv = static_cast<uint32_t>(pos.size());
        ResizeVertexArrays(mesh, pos.size() + 1);
        const jpov::Vec3f p = Lerp(mesh->positions[a], mesh->positions[b], t);
        mesh->positions[nv] = p;
        if (!mesh->uvs.empty()) {
            const jpov::Vec2f& ua = mesh->uvs[a];
            const jpov::Vec2f& ub = mesh->uvs[b];
            mesh->uvs[nv] = jpov::Vec2f(ua.x() + (ub.x() - ua.x()) * t,
                                        ua.y() + (ub.y() - ua.y()) * t);
        }
        if (!mesh->joint_indices.empty()) {
            if (mesh->joint_weights[a][0] >= mesh->joint_weights[b][0]) {
                mesh->joint_indices[nv] = mesh->joint_indices[a];
                mesh->joint_weights[nv] = mesh->joint_weights[a];
            } else {
                mesh->joint_indices[nv] = mesh->joint_indices[b];
                mesh->joint_weights[nv] = mesh->joint_weights[b];
            }
        }
        edge_vertex[key] = nv;
        ring.push_back(nv);
        return nv;
    };

    const size_t tri_count = mesh->indices.size() / 3;
    for (size_t t = 0; t < tri_count; ++t) {
        const uint32_t v[3] = {mesh->indices[3 * t], mesh->indices[3 * t + 1],
                               mesh->indices[3 * t + 2]};
        const float d[3] = {coord(pos[v[0]]) - plane, coord(pos[v[1]]) - plane,
                            coord(pos[v[2]]) - plane};
        int below = 0;
        int above = 0;
        int on = 0;
        for (int k = 0; k < 3; ++k) {
            if (std::fabs(d[k]) <= eps) {
                ++on;
            } else if (d[k] > 0.0f) {
                ++above;
            } else {
                ++below;
            }
        }
        if (below == 0 || above == 0) {
            // 整块在一边（含贴面）→ 原样保留。
            for (int k = 0; k < 3; ++k) {
                new_indices.push_back(v[k]);
            }
            continue;
        }
        // 跨面：找两条「跨面边」，在交点上插顶点（按无序边去重，相邻三角形共点）。
        std::vector<uint32_t> poly_below;
        std::vector<uint32_t> poly_above;
        // 先按绕序把每个顶点/交点写进上下两个多边形。
        for (int k = 0; k < 3; ++k) {
            const int k2 = (k + 1) % 3;
            const bool cur_above = d[k] > eps;
            const bool cur_below = d[k] < -eps;
            const bool nxt_above = d[k2] > eps;
            const bool nxt_below = d[k2] < -eps;
            // 当前顶点归它的那一侧（在面上的点两边都收）。
            if (!cur_above) {
                poly_below.push_back(v[k]);
            }
            if (!cur_below) {
                poly_above.push_back(v[k]);
            }
            // 跨面边 → 交点（两边都收）。
            if ((cur_above && nxt_below) || (cur_below && nxt_above)) {
                const float t_lerp = d[k] / (d[k] - d[k2]);
                const uint32_t nv = lerp_vertex(v[k], v[k2], t_lerp);
                poly_below.push_back(nv);
                poly_above.push_back(nv);
            }
        }
        auto fan = [&new_indices](const std::vector<uint32_t>& poly) {
            for (size_t k = 1; k + 1 < poly.size(); ++k) {
                new_indices.push_back(poly[0]);
                new_indices.push_back(poly[k]);
                new_indices.push_back(poly[k + 1]);
            }
        };
        if (poly_below.size() >= 3) {
            fan(poly_below);
        }
        if (poly_above.size() >= 3) {
            fan(poly_above);
        }
        if (poly_below.size() < 3 && poly_above.size() < 3) {
            for (int k = 0; k < 3; ++k) {
                new_indices.push_back(v[k]);
            }
        }
    }
    mesh->indices = std::move(new_indices);
    (void)vcount_before;
    return ring;
}

// 把 target 的断面环换成 reference 环的**顶点序列**（逐点一一对应 ⇒ 可 1:1 焊接）。
//
// 做法（不删面、不重拓扑）：
//   1) 要求 M( target 环顶点数 ) <= N( reference 环顶点数 )；
//   2) 给 target 的每个边界顶点分配一个**不同的** reference 顶点（按环上排名单调分配），
//      并把它的位置搬到该 reference 顶点上；
//   3) 两个相邻分配之间空出来的 reference 顶点，按序**插入**到那条边界边上
//      （把边上相邻的 1~2 个三角形原地改成扇形，局部操作、保持流形）。
//   结果：target 边界 = reference 环的 N 个顶点，顺序一致（可旋转 / 可反向），
//   直接按索引合并即可焊成水密。
//
// 为什么不用「删一圈面再拉链重建」：真实资产的断面附近常带 UV 缝（同名位置重复顶点），
//   删掉那一圈后洞边界会出现分叉，拉链三角化没有唯一解（实测路灯杆就挂在这）。
//   分裂边只动局部三角形，与洞的拓扑形状无关，稳得多。
inline bool RebuildLoopToReference(
    MeshData* mesh /*inout*/, const SeamLoop& tgt_loop,
    const jpov::MeshData& ref_mesh, const SeamLoop& ref_loop, int shift,
    bool reversed, int K, size_t* inserted_vertices /*output*/,
    std::string* error) {
    const size_t m = tgt_loop.verts.size();
    const size_t n = ref_loop.verts.size();
    *inserted_vertices = 0;
    if (m == 0 || n == 0) {
        *error = "环重建失败：空环";
        return false;
    }
    if (m > n) {
        *error = "环重建失败：target 断面顶点数 " + std::to_string(m) +
                 " 多于 reference 的 " + std::to_string(n) + "（需要抽稀，暂不支持）";
        return false;
    }

    // 1) 分配：j_i = (j0 + round(i·N/M)) mod N，单调且互不相同（M<=N）。
    const std::vector<float> tparam = LoopParams(*mesh, tgt_loop);
    const std::vector<float> rparam = LoopParams(ref_mesh, ref_loop);
    // j0：离 target 第 0 个顶点对应参数最近的 reference 顶点。
    float v0 = static_cast<float>(shift) / static_cast<float>(K) +
               (reversed ? -tparam[0] : tparam[0]);
    v0 = v0 - std::floor(v0);
    size_t j0 = 0;
    float best = 1e9f;
    for (size_t j = 0; j < n; ++j) {
        float d = std::fabs(rparam[j] - v0);
        d = std::min(d, 1.0f - d);
        if (d < best) {
            best = d;
            j0 = j;
        }
    }
    // 分配：j_i = (j0 + σ·round(i·N/M)) mod N。σ = 对应方向（reversed），
    // 必须与变形阶段的映射 v = σ·u + s 一致 —— 否则参考环是镜像的，顶点被搬到对侧，
    // 领口三角形自交（现场表现为「木板扭曲 / 镂空」）。
    // 用「排名均分」而不是「按弧长找最近点」：目标环顶点常扎堆在倒角上，按弧长找最近点
    // 会出现两个顶点抢同一个参考顶点（非双射）⇒ 两者之间那条边被当成绕整圈、插入 N 个点
    // ⇒ 扇形自重叠、边界出现几十条边的非流形顶点（实测顶点 699 有 36 条边）。
    const int dir_step = reversed ? -1 : 1;
    std::vector<size_t> assign(m, 0);
    for (size_t i = 0; i < m; ++i) {
        const long long step = static_cast<long long>(
            std::llround(static_cast<double>(i) * n / static_cast<double>(m)));
        const long long j = static_cast<long long>(j0) + dir_step * step;
        assign[i] = static_cast<size_t>((j % static_cast<long long>(n) +
                                        static_cast<long long>(n)) %
                                       static_cast<long long>(n));
    }

    // 2) 物理下标 → 逻辑环下标（UV 缝处一个逻辑点可能对应多个物理点）。
    const auto edge_tris = BuildEdgeTriangles(*mesh);
    std::unordered_map<uint32_t, size_t> logical_of;
    for (size_t i = 0; i < m; ++i) {
        logical_of[tgt_loop.verts[i]] = i;
        if (i < tgt_loop.members.size()) {
            for (uint32_t mv : tgt_loop.members[i]) {
                logical_of[mv] = i;
            }
        }
    }

    // 3) 搬顶点：target 环顶点 → 分配的 reference 顶点位置。
    for (size_t i = 0; i < m; ++i) {
        const jpov::Vec3f p = ref_mesh.positions[ref_loop.verts[assign[i]]];
        if (i < tgt_loop.members.size() && !tgt_loop.members[i].empty()) {
            for (uint32_t mv : tgt_loop.members[i]) {
                mesh->positions[mv] = p;
            }
        } else {
            mesh->positions[tgt_loop.verts[i]] = p;
        }
    }

    // 4a) 每条边界边算出「插入链」（沿 a→b 方向的顶点下标）。
    if (mesh->indices.empty()) {
        *error = "环重建失败：网格不是索引化的";
        return false;
    }
    std::unordered_map<uint64_t, std::vector<uint32_t>> chains;
    size_t inserted = 0;
    // 遍历**真实**的边界边（只被一个三角形引用、两端都在环上）。
    for (const auto& kv : edge_tris) {
        if (kv.second.size() != 1) {
            continue;
        }
        const uint32_t u = static_cast<uint32_t>(kv.first >> 32);
        const uint32_t v = static_cast<uint32_t>(kv.first & 0xffffffffu);
        const auto iu = logical_of.find(u);
        const auto iv = logical_of.find(v);
        if (iu == logical_of.end() || iv == logical_of.end()) {
            continue;  // 既有开口，不是断面
        }
        const size_t lu = iu->second;
        const size_t lv = iv->second;
        const bool forward = (lv == (lu + 1) % m);
        if (!forward && lv != (lu + m - 1) % m) {
            continue;  // 同组内部边（UV 缝的两份），跳过
        }
        const uint32_t a = u;
        const uint32_t b = v;
        const size_t ja = assign[lu];
        const size_t jb = assign[lv];
        // 插入点 = 从 ja 沿**对应方向 σ** 走到 jb 之间夹着的 reference 顶点。
        // （相邻分配相差 round(N/M) 步，所以按 σ 走一定能走到 jb；用物理边方向判断是错的 ——
        //   UV 缝的同组边、以及反向对应都会走反。）
        std::vector<size_t> insert_js;
        {
            // 从 ja 到 jb 走**短弧**（相邻分配相差 round(N/M) < N/2，短弧唯一）。
            // 不能直接用 σ：UV 缝的孪生边物理走向与逻辑序相反，按 σ 走会绕一整圈
            // （实测会产生自重叠的扇形 ⇒ 边界出现 4 条边的非流形顶点）。
            const size_t fwd = (jb + n - ja) % n;
            const size_t bwd = (ja + n - jb) % n;
            const int step_dir = (fwd <= bwd) ? 1 : -1;
            const long long stride = static_cast<long long>(n);
            size_t cur = static_cast<size_t>(
                (static_cast<long long>(ja) + step_dir + stride) % stride);
            while (cur != jb) {
                insert_js.push_back(cur);
                cur = static_cast<size_t>(
                    (static_cast<long long>(cur) + step_dir + stride) % stride);
                if (insert_js.size() > n) {
                    break;  // 防死循环（分配异常时）
                }
            }
        }
        if (insert_js.empty()) {
            continue;
        }
        const jpov::Vec3f pa = mesh->positions[a];
        const jpov::Vec3f pb = mesh->positions[b];
        const float len2 = (pb - pa).Dot(pb - pa);
        std::vector<uint32_t> chain;
        chain.push_back(a);
        for (size_t kj : insert_js) {
            const jpov::Vec3f p = ref_mesh.positions[ref_loop.verts[kj]];
            const float t = (len2 > 1e-18f)
                                ? std::min(1.0f, std::max(0.0f,
                                                          (p - pa).Dot(pb - pa) / len2))
                                : 0.0f;
            const uint32_t nv = static_cast<uint32_t>(mesh->positions.size());
            ResizeVertexArrays(mesh, mesh->positions.size() + 1);
            mesh->positions[nv] = p;
            if (!mesh->uvs.empty()) {
                const jpov::Vec2f& ua = mesh->uvs[a];
                const jpov::Vec2f& ub = mesh->uvs[b];
                mesh->uvs[nv] = jpov::Vec2f(ua.x() + (ub.x() - ua.x()) * t,
                                            ua.y() + (ub.y() - ua.y()) * t);
            }
            if (!mesh->joint_indices.empty()) {
                // 骨权取两端里主权重大的那一端（切面顶点本来就是插值出来的）。
                const uint32_t src = (mesh->joint_weights[a][0] >= mesh->joint_weights[b][0])
                                         ? a
                                         : b;
                mesh->joint_indices[nv] = mesh->joint_indices[src];
                mesh->joint_weights[nv] = mesh->joint_weights[src];
            }
            chain.push_back(nv);
            ++inserted;
        }
        chain.push_back(b);
        chains[EdgeKey(a, b)] = std::move(chain);
    }

    // 4b) 重建三角形：每条边若在 chains 里就展开成折线，再扇形三角化。
    //     一个角上的三角形可能同时含两条边界边，逐边就地改索引会互相踩，所以整表重建。
    std::vector<uint32_t> new_indices;
    new_indices.reserve(mesh->indices.size() + inserted * 6);
    const size_t tri_count = mesh->indices.size() / 3;
    for (size_t t = 0; t < tri_count; ++t) {
        const uint32_t v[3] = {mesh->indices[3 * t], mesh->indices[3 * t + 1],
                               mesh->indices[3 * t + 2]};
        std::vector<uint32_t> poly;
        poly.reserve(8);
        for (int e = 0; e < 3; ++e) {
            const uint32_t x = v[e];
            const uint32_t y = v[(e + 1) % 3];
            const auto it = chains.find(EdgeKey(x, y));
            if (it == chains.end()) {
                poly.push_back(x);
                continue;
            }
            const std::vector<uint32_t>& ch = it->second;
            if (ch.front() == x && ch.back() == y) {
                poly.insert(poly.end(), ch.begin(), ch.end() - 1);
            } else {
                // 链方向与三角形绕向相反 → 从链尾（= 当前顶点 x）往回走，
                // 只写到链首的前一个（链首 = 下一段的起点 y）。
                poly.push_back(ch.back());
                for (size_t idx = ch.size() - 1; idx > 1; --idx) {
                    poly.push_back(ch[idx - 1]);
                }
            }
        }
        // 扇形（从 poly[0] 出发，保持原绕向）。
        for (size_t k = 1; k + 1 < poly.size(); ++k) {
            new_indices.push_back(poly[0]);
            new_indices.push_back(poly[k]);
            new_indices.push_back(poly[k + 1]);
        }
    }
    mesh->indices = std::move(new_indices);
    *inserted_vertices = inserted;
    return true;
}

// ==================== 主入口 ====================

// 断面环上每个顶点 → reference 环上的目标位置（按对应参数采样）。
inline std::vector<jpov::Vec3f> ComputeLoopGoals(
    const jpov::MeshData& tgt_mesh, const SeamLoop& tgt_loop,
    const jpov::MeshData& ref_mesh, const SeamLoop& ref_loop, int shift,
    bool reversed, int K) {
    const std::vector<float> param = LoopParams(tgt_mesh, tgt_loop);
    std::vector<jpov::Vec3f> goal(tgt_mesh.positions.size(),
                                  MakeVec(0.0f, 0.0f, 0.0f));
    for (size_t i = 0; i < tgt_loop.verts.size(); ++i) {
        float v = static_cast<float>(shift) / static_cast<float>(K) +
                  (reversed ? -param[i] : param[i]);
        const jpov::Vec3f p = SampleLoop(ref_mesh, ref_loop, v);
        if (i < tgt_loop.members.size() && !tgt_loop.members[i].empty()) {
            for (uint32_t m : tgt_loop.members[i]) {
                goal[m] = p;
            }
        } else {
            goal[tgt_loop.verts[i]] = p;
        }
    }
    return goal;
}

// 点到折线的最近距离。
inline float DistanceToLoop(const jpov::Vec3f& p, const jpov::MeshData& mesh,
                            const SeamLoop& loop) {
    float best = std::numeric_limits<float>::max();
    const size_t n = loop.verts.size();
    for (size_t i = 0; i < n; ++i) {
        const jpov::Vec3f& a = mesh.positions[loop.verts[i]];
        const jpov::Vec3f& b = mesh.positions[loop.verts[(i + 1) % n]];
        const jpov::Vec3f ab = b - a;
        const float len2 = ab.Dot(ab);
        float t = (len2 > 1e-12f) ? (p - a).Dot(ab) / len2 : 0.0f;
        t = std::min(1.0f, std::max(0.0f, t));
        const float d = (p - (a + ab * t)).Norm();
        best = std::min(best, d);
    }
    return best;
}

// 网格里被 1 个三角形引用的边数（= 开口边总数；用来对比缝合前后有没有新增洞）。
inline size_t CountOpenEdges(const jpov::MeshData& mesh) {
    const auto map = seam_internal::BuildEdgeTriangles(mesh);
    size_t n = 0;
    for (const auto& kv : map) {
        if (kv.second.size() == 1) {
            ++n;
        }
    }
    return n;
}

// 主入口：把 target 的底口雕成 reference 的顶口。
//
// 输入 target / reference 都是**未切**的完整网格（本函数内部各切一刀，同一个 plane）。
// 成功返回 true 并写入 *target_out；失败返回 false，*error 给原因，*target_out 不动。
inline bool StitchSeam(const jpov::MeshData& target,
                       const jpov::MeshData& reference, const SeamOptions& opt,
                       jpov::MeshData* target_out /*output*/,
                       SeamStats* stats /*output, 可空*/,
                       std::string* error /*output*/) {
    CHECK(target_out != nullptr);
    CHECK(error != nullptr);
    CHECK_GT(opt.steps, 0);
    CHECK_GT(opt.band_scale, 0.0f);
    CHECK_GE(opt.axis, 0);
    CHECK_LT(opt.axis, 3);
    target.Validate();
    reference.Validate();
    *error = "";
    SeamStats st;

    // 1) 两个模型在同一个平面上各切一刀。
    const float plane_eps = 1e-5f;
    jpov::MeshData tcut;
    jpov::MeshData rcut;
    ClipStats clip_stats;
    if (!ClipMeshByAxis(target, opt.axis, opt.plane, ClipKeepSide::kGreater, &tcut,
                        &clip_stats)) {
        *error = "target 在切平面以上没有几何（切空了），无法缝合";
        return false;
    }
    if (!ClipMeshByAxis(reference, opt.axis, opt.plane, ClipKeepSide::kLess, &rcut,
                        &clip_stats)) {
        *error = "reference 在切平面以下没有几何（切空了），无法缝合";
        return false;
    }

    // 1.5) y_span：在 plane + y_span 处插一圈顶点（不删面），给形变区一个硬边界。
    std::vector<uint32_t> ring_vertices;
    if (opt.y_span > 0.0f) {
        ring_vertices = InsertRingAtPlane(&tcut, opt.axis, opt.plane + opt.y_span,
                                         plane_eps);
    }

    // 2) 断面环 + 合法性。
    SeamLoop tloop;
    SeamLoop rloop;
    std::string loop_err;
    if (!ExtractCutLoop(tcut, opt.axis, opt.plane, plane_eps, &tloop, &loop_err)) {
        *error = "target 断面不合法：" + loop_err;
        return false;
    }
    if (!ExtractCutLoop(rcut, opt.axis, opt.plane, plane_eps, &rloop, &loop_err)) {
        *error = "reference 断面不合法：" + loop_err;
        return false;
    }
    st.target_loop_vertices = tloop.verts.size();
    st.ref_loop_vertices = rloop.verts.size();

    // 3) 环对应（弧长 + 循环移位 + 反向）。
    constexpr int kCorrSamples = 256;
    const auto P = ResampleLoop(tcut, tloop, kCorrSamples);
    const auto Q = ResampleLoop(rcut, rloop, kCorrSamples);
    int shift = 0;
    bool reversed = false;
    BestCyclicCorrespondence(P, Q, &shift, &reversed);
    // 手动旋转偏移。
    int offset = static_cast<int>(std::lround(opt.offset_deg / 360.0f * kCorrSamples));
    shift = ((shift + offset) % kCorrSamples + kCorrSamples) % kCorrSamples;

    const std::vector<jpov::Vec3f> goal =
        ComputeLoopGoals(tcut, tloop, rcut, rloop, shift, reversed, kCorrSamples);
    if (const char* dbg = std::getenv("JPOV_SEAM_DEBUG")) {
        (void)dbg;
        LOG(INFO) << "SEAMDBG shift=" << shift << " rev=" << reversed
                  << " tgt_loop=" << tloop.verts.size()
                  << " ref_loop=" << rloop.verts.size();
        for (size_t i = 0; i < tloop.verts.size(); ++i) {
            const jpov::Vec3f& a = tcut.positions[tloop.verts[i]];
            const jpov::Vec3f& b = goal[tloop.verts[i]];
            LOG(INFO) << "  i=" << i << " p=(" << a.x() << "," << a.z() << ") -> ("
                      << b.x() << "," << b.z() << ")";
        }
    }

    // 4) 影响带（**按位置焊接后的组**算：UV 缝两侧是两套顶点，按顶点解会把缝撕开）。
    const float eq_radius = tloop.perimeter / (2.0f * 3.14159265358979323846f);
    const WeldGroups wg = BuildWeldGroups(tcut);
    const size_t gn = wg.leader.size();
    std::vector<jpov::Vec3f> orig(gn);
    for (size_t g = 0; g < gn; ++g) {
        orig[g] = tcut.positions[wg.leader[g]];
    }
    std::vector<uint8_t> g_is_loop(gn, 0);
    for (uint32_t v : LoopPhysicalVerts(tloop)) {
        g_is_loop[wg.rep[v]] = 1;
    }
    std::vector<uint8_t> g_anchored(gn, 0);
    for (uint32_t v : ring_vertices) {
        g_anchored[wg.rep[v]] = 1;  // y_span 环 = 硬锚点
    }
    float max_edge_near_loop = 0.0f;
    {
        const size_t tris = TriangleCount(tcut);
        for (size_t t = 0; t < tris; ++t) {
            const auto tri = TriangleAt(tcut, t);
            for (int e = 0; e < 3; ++e) {
                const uint32_t a = tri[e];
                const uint32_t b = tri[(e + 1) % 3];
                if (!g_is_loop[wg.rep[a]] && !g_is_loop[wg.rep[b]]) {
                    continue;
                }
                max_edge_near_loop = std::max(
                    max_edge_near_loop, (tcut.positions[b] - tcut.positions[a]).Norm());
            }
        }
    }
    // y_span 给了就以它为准（明确的"最大形变区"），否则退回自适应。
    const float band_h = (opt.y_span > 0.0f)
                             ? opt.y_span
                             : std::max(opt.band_scale * eq_radius,
                                        1.5f * max_edge_near_loop);
    const std::vector<float> g_dist = GeodesicOnGroups(tcut, wg, tloop);
    const BandGraph band = BuildBandGraphGrouped(tcut, wg, g_is_loop, g_dist, band_h,
                                                 g_anchored);
    for (size_t g = 0; g < band.is_free.size(); ++g) {
        if (band.is_free[g]) {
            ++st.band_vertices;
        }
    }
    // 环的目标位置（组级）。
    std::vector<jpov::Vec3f> g_goal = orig;
    for (uint32_t v : LoopPhysicalVerts(tloop)) {
        g_goal[wg.rep[v]] = goal[v];
    }

    // 5) 分步形变（continuation）+ 每个算法。全部在**组空间**求解。
    std::vector<jpov::Vec3f> cur = orig;
    const int steps = std::max(1, opt.steps);
    const int inner_iters = (opt.algo == SeamAlgo::kArap)
                                ? std::min(opt.max_iterations / 4, 60)
                                : std::min(opt.max_iterations, 300);
    for (int s = 1; s <= steps; ++s) {
        const float t = static_cast<float>(s) / static_cast<float>(steps);
        std::vector<jpov::Vec3f> step_goal = orig;
        for (size_t g = 0; g < gn; ++g) {
            if (g_is_loop[g]) {
                step_goal[g] = Lerp(orig[g], g_goal[g], t);
            }
        }
        if (opt.algo == SeamAlgo::kHarmonic) {
            st.iterations += SolveHarmonic(band, g_is_loop, step_goal, inner_iters, &cur);
        } else if (opt.algo == SeamAlgo::kArap) {
            st.iterations += SolveArap(tcut, band, g_is_loop, orig, step_goal, 4,
                                       inner_iters, &cur);
        } else {
            size_t it = 0;
            if (!SolveTps(orig, band, g_is_loop, orig, step_goal, opt.axis, &cur, &it)) {
                *error = "TPS 求解失败（断面点共面 / 退化）";
                return false;
            }
            st.iterations += it;
        }
        // 硬约束：断面环永远落在目标位。
        for (size_t g = 0; g < gn; ++g) {
            if (g_is_loop[g]) {
                cur[g] = step_goal[g];
            }
        }
    }
    // 组位移 → 逐顶点（成员各自的微小位置差保留，缝不裂）。
    std::vector<jpov::Vec3f> new_pos(tcut.positions.size());
    for (size_t v = 0; v < tcut.positions.size(); ++v) {
        const uint32_t g = wg.rep[v];
        new_pos[v] = tcut.positions[v] + (cur[g] - orig[g]);
    }
    {
        std::vector<uint8_t> touch(tcut.positions.size(), 0);
        for (size_t v = 0; v < touch.size(); ++v) {
            touch[v] = (g_is_loop[wg.rep[v]] || band.is_free[wg.rep[v]]) ? 1 : 0;
        }
        st.flipped_triangles = CountFlipped(tcut, tcut.positions, new_pos, touch);
    }
    tcut.positions = new_pos;

    // 6) 环重建（可选，保证 1:1 可焊）。
    if (opt.rebuild_loop) {
        std::string rebuild_err;
        size_t collar = 0;
        jpov::MeshData rebuilt = tcut;
        if (RebuildLoopToReference(&rebuilt, tloop, rcut, rloop, shift, reversed,
                                   kCorrSamples, &collar, &rebuild_err)) {
            tcut = std::move(rebuilt);
            st.loop_rebuilt = true;
            st.inserted_vertices = collar;
        } else {
            st.message = "环重建降级（只保证几何重合）：" + rebuild_err;
        }
    }

    // 7) 法线：环重建会重排顶点下标，所以整网格按面积权重重算一遍。
    //    （本工具输出的是「编辑后的几何」，不保留导入时的自定义法线。）
    if (!tcut.normals.empty()) {
        const std::vector<uint8_t> all(tcut.positions.size(), 1);
        RecomputeNormalsFor(tcut, all, &tcut.normals);
    }
    // 切线：位置变了，切线必须重算；这里直接清掉（渲染端会退化到屏幕导数）。
    if (!tcut.tangents.empty()) {
        tcut.tangents.clear();
        tcut.tangents.resize(tcut.positions.size(), MakeVec(1.0f, 0.0f, 0.0f));
    }

    // 7.5) 拓扑自检：除了断面（平面附近的开口）之外，还有没有别的开口边。
    {
        const auto edge_tris = BuildEdgeTriangles(tcut);
        for (const auto& kv : edge_tris) {
            if (kv.second.size() != 1) {
                continue;
            }
            const uint32_t a = static_cast<uint32_t>(kv.first >> 32);
            const uint32_t b = static_cast<uint32_t>(kv.first & 0xffffffffu);
            const bool on_plane =
                std::fabs(seam_internal::Component(tcut.positions[a], opt.axis) - opt.plane) <=
                    1e-4f &&
                std::fabs(seam_internal::Component(tcut.positions[b], opt.axis) - opt.plane) <=
                    1e-4f;
            if (!on_plane) {
                ++st.unexpected_open_edges;
            }
        }
    }

    // 8) 指标：断面重合度 + 法线夹角。
    {
        SeamLoop out_loop;
        std::string final_err;
        if (ExtractCutLoop(tcut, opt.axis, opt.plane, plane_eps, &out_loop,
                           &final_err)) {
            float h1 = 0.0f;
            double sum1 = 0.0;
            for (uint32_t v : rloop.verts) {
                const float d = DistanceToLoop(rcut.positions[v], tcut, out_loop);
                h1 = std::max(h1, d);
                sum1 += static_cast<double>(d);
            }
            float h2 = 0.0f;
            double sum2 = 0.0;
            for (uint32_t v : out_loop.verts) {
                const float d = DistanceToLoop(tcut.positions[v], rcut, rloop);
                h2 = std::max(h2, d);
                sum2 += static_cast<double>(d);
            }
            st.loop_hausdorff_m = std::max(h1, h2);
            st.loop_rms_m = static_cast<float>(
                0.5 * (sum1 / std::max<size_t>(1, rloop.verts.size()) +
                       sum2 / std::max<size_t>(1, out_loop.verts.size())));

            // 法线夹角：output 边界顶点 ↔ reference 边界顶点（按最近点配对）。
            if (!tcut.normals.empty() && !rcut.normals.empty()) {
                double sum = 0.0;
                float max_ang = 0.0f;
                size_t count = 0;
                std::vector<jpov::Vec3f> ref_normals = rcut.normals;
                ref_normals.resize(rcut.positions.size(), MakeVec(0.0f, 1.0f, 0.0f));
                RecomputeNormalsFor(rcut, std::vector<uint8_t>(rcut.positions.size(), 1),
                                    &ref_normals);
                for (uint32_t v : out_loop.verts) {
                    // 找最近的 reference 环顶点。
                    float best = std::numeric_limits<float>::max();
                    uint32_t best_v = rloop.verts[0];
                    for (uint32_t rv : rloop.verts) {
                        const float d = (tcut.positions[v] - rcut.positions[rv]).Norm();
                        if (d < best) {
                            best = d;
                            best_v = rv;
                        }
                    }
                    const jpov::Vec3f& na = tcut.normals[v];
                    const jpov::Vec3f& nb = ref_normals[best_v];
                    if (na.Norm() < 1e-6f || nb.Norm() < 1e-6f) {
                        continue;
                    }
                    const float c = std::min(1.0f, std::max(-1.0f, na.Dot(nb)));
                    const float ang = std::acos(c) * 180.0f / 3.14159265358979323846f;
                    sum += ang;
                    max_ang = std::max(max_ang, ang);
                    ++count;
                }
                if (count > 0) {
                    st.normal_angle_mean_deg = static_cast<float>(sum / count);
                    st.normal_angle_max_deg = max_ang;
                }
            }
        } else {
            st.message += "（最终断面提取失败：" + final_err + "）";
        }
    }

    st.ok = true;
    if (st.message.empty()) {
        st.message = std::string("ok / ") + SeamAlgoName(opt.algo) +
                     (st.loop_rebuilt ? " / 环已重建" : " / 环未重建");
    }
    if (stats != nullptr) {
        *stats = std::move(st);
    }
    *target_out = std::move(tcut);
    return true;
}

}  // namespace seam_internal

// 公开入口：把 target 的底口雕成 reference 的顶口（详见文件头）。
// 失败返回 false 并写 *error；成功写 *target_out（以及 *stats）。
inline bool StitchSeam(const jpov::MeshData& target,
                       const jpov::MeshData& reference, const SeamOptions& opt,
                       jpov::MeshData* target_out /*output*/,
                       SeamStats* stats /*output, 可空*/,
                       std::string* error /*output*/) {
    return seam_internal::StitchSeam(target, reference, opt, target_out, stats, error);
}

}  // namespace model_editor
}  // namespace jpov

#endif  // JPOV_MODEL_EDITOR_MESH_SEAM_H_
