// JPOV 模型编辑器 — 沿坐标平面裁剪网格（纯函数 / GL-free）
//
// 用途（2026-10-06 Danis）：模型编辑器的「裁剪」面板——把 target 模型沿一个**坐标平面**
//   切一刀，**删掉某一侧**。坐标轴可选 X / Y / Z（2026-10-07 Danis 追加 X、Z）：
//   例如瑜伽裤裤腿下方要开口，就沿水平面（Y 轴）裁掉低于某个坐标的部分。
//
// 为什么不能只挑三角形整块留/删：真正落在裁剪面上的三角形是**跨面**的——一部分在保留
//   侧、一部分在删除侧。这类三角形必须在裁剪面上截断，并按线性插值**补出新的边界顶点**；
//   否则边界会是锯齿状、且留下的几何与裁剪面不齐。
//
// 本文件负责：遍历每个三角形，全在保留侧 → 原样保留；全在删除侧 → 丢弃；跨界 → 用
//   Sutherland–Hodgman 半空间裁剪裁成 3 或 4 边形，再扇形三角化。新顶点由交叉边的两个
//   端点**按参数 t 插值**所有逐顶点属性：
//     - 位置 position（线性插值）
//     - 法线 normal（插值后重新归一化）
//     - UV（线性插值）
//     - 切线 tangent（线性插值，遵守「切线不归一化」的项目约定）
//     - 骨权 joint_indices / joint_weights（把两端点的 (joint, weight) 合并，按 (1-t)/t
//       加权，取权重最大的 4 个，再按保留的 4 个之和归一）
//
// 输出网格是**索引化**的：原始顶点按原下标去重（同一原始顶点只复制一份），跨边新建的边界
//   顶点按「无序边 (min,max)」去重（同一条边被相邻两个三角形共享时命中同一顶点）。这样既
//   不炸顶点数，也保证缝合处不出裂缝。
//
// 语义约定（与 mesh.h / gltf_loader.h 一致）：
//   - 三角形按 triangle list 语义（每 3 个索引一个三角形）；indices 为空 = non-indexed。
//   - 不改变属性 flags；input 有哪些属性，output 就有哪些。
//
// Pre-condition（不满足即 LOG(FATAL)）：in.Validate() 通过；axis ∈ {0,1,2}。
//
// 返回值：true = 输出非空（已写入 *out）；false = 裁剪后为空（*out 保持未写，调用方据此
//   「报错并不做」）。

#ifndef JPOV_MODEL_EDITOR_MESH_CLIP_H_
#define JPOV_MODEL_EDITOR_MESH_CLIP_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/interface/mesh.h"

namespace jpov {
namespace model_editor {

// 保留裁剪面的哪一侧（按所选坐标轴的分量大小）。
enum class ClipKeepSide {
    kGreater,  // 保留 coord >= plane 的一侧（删除 coord < plane 的一侧）
    kLess,     // 保留 coord <= plane 的一侧（删除 coord > plane 的一侧）
};

// 一次裁剪的统计（供面板显示 / 单测断言）。
struct ClipStats {
    size_t input_triangles = 0;         // 输入三角形数
    size_t output_triangles = 0;        // 输出（保留）三角形数
    size_t kept_original_vertices = 0;  // 输出中来自原始顶点的唯一数量
    size_t new_boundary_vertices = 0;   // 输出中新建的边界顶点唯一数量
};

// 坐标轴分量（0=X, 1=Y, 2=Z）。
inline float AxisComponent(const jpov::Vec3f& p, int axis) {
    switch (axis) {
        case 0:
            return p.x();
        case 1:
            return p.y();
        default:
            return p.z();
    }
}

// 「在保留侧」判定的距离容差（米）：|d| <= 该值视为落在裁剪面上（保留，不算跨界）。
inline constexpr float kClipEps = 1e-6f;
// 丢弃「零面积」退化三角形的阈值（平方米）：例如三角形恰好完全落在裁剪面上时。
// （JPOV 米制，面积量级远大于此值；这不是为了剔除 t≈0 的细小碎片，那类碎片无害。）
inline constexpr float kClipMinTriangleArea = 1e-12f;

namespace mesh_clip_internal {

// 顶点到裁剪面的「保留侧有向距离」：>= 0 表示在保留侧。两侧统一成同一表达式，
// 使「inside = d >= -eps」对 kGreater / kLess 都成立。
inline float KeepSignedDistance(const jpov::Vec3f& p, int axis, float plane,
                                ClipKeepSide side) {
    const float c = AxisComponent(p, axis);
    return (side == ClipKeepSide::kGreater) ? (c - plane) : (plane - c);
}

// 无序边 key（min,max），用于跨边新建顶点的去重。
inline uint64_t EdgeKey(uint32_t a, uint32_t b) {
    const uint32_t lo = std::min(a, b);
    const uint32_t hi = std::max(a, b);
    return (static_cast<uint64_t>(lo) << 32) | static_cast<uint64_t>(hi);
}

// 三角形面积。
inline float TriangleArea(const jpov::Vec3f& a, const jpov::Vec3f& b,
                          const jpov::Vec3f& c) {
    const float ux = b.x() - a.x();
    const float uy = b.y() - a.y();
    const float uz = b.z() - a.z();
    const float vx = c.x() - a.x();
    const float vy = c.y() - a.y();
    const float vz = c.z() - a.z();
    const float cx = uy * vz - uz * vy;
    const float cy = uz * vx - ux * vz;
    const float cz = ux * vy - uy * vx;
    return 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);
}

// 合并两组每顶点 4-joint 权重并插值：joint j 的结果权重 = (1-t)*wa[j] + t*wb[j]
// （对共同 joint 求和），取权重最大的 4 个，再按这 4 个之和归一。
//
// Pre-condition: 4 个输出指针非空。
inline void BlendJointWeights(const std::array<int32_t, 4>& ja,
                              const std::array<float, 4>& wa,
                              const std::array<int32_t, 4>& jb,
                              const std::array<float, 4>& wb, float t,
                              std::array<int32_t, 4>* oj /*output*/,
                              std::array<float, 4>* ow /*output*/) {
    int32_t joints[8];
    float weights[8];
    int n = 0;
    const auto add = [&](int32_t joint, float weight) {
        if (weight <= 0.0f) {
            return;
        }
        for (int k = 0; k < n; ++k) {
            if (joints[k] == joint) {
                weights[k] += weight;
                return;
            }
        }
        joints[n] = joint;
        weights[n] = weight;
        ++n;
    };
    for (int k = 0; k < 4; ++k) {
        add(ja[k], (1.0f - t) * wa[k]);
    }
    for (int k = 0; k < 4; ++k) {
        add(jb[k], t * wb[k]);
    }
    for (int k = 0; k < 4; ++k) {
        (*oj)[k] = 0;
        (*ow)[k] = 0.0f;
    }
    float sum = 0.0f;
    for (int slot = 0; slot < 4; ++slot) {
        int best = -1;
        float best_w = 0.0f;
        for (int k = 0; k < n; ++k) {
            if (weights[k] > best_w) {
                best_w = weights[k];
                best = k;
            }
        }
        if (best < 0) {
            break;
        }
        (*oj)[slot] = joints[best];
        (*ow)[slot] = weights[best];
        sum += weights[best];
        weights[best] = 0.0f;  // 已选中，清掉以免重复。
    }
    if (sum > 0.0f) {
        for (int k = 0; k < 4; ++k) {
            (*ow)[k] /= sum;
        }
    }
}

}  // namespace mesh_clip_internal

// 沿坐标平面（轴 = axis，值为 plane_coord）裁剪 mesh，保留 keep 侧；结果写入 *out。
//
// in          : 输入网格（不改）。
// axis        : 坐标轴 0=X / 1=Y / 2=Z。
// plane_coord : 裁剪面在该轴上的坐标。
// keep        : 保留哪一侧（见 ClipKeepSide）。
// out         : 输出网格（仅返回 true 时写入）。
// stats       : 可选统计输出（可空）。
//
// 返回 true = 输出非空；false = 裁剪后为空（调用方应「报错并不做」）。
// Pre-condition: out != nullptr；in.Validate() 通过；0 <= axis < 3。
inline bool ClipMeshByAxis(const jpov::MeshData& in, int axis, float plane_coord,
                           ClipKeepSide keep, jpov::MeshData* out /*output*/,
                           ClipStats* stats /*output, 可空*/) {
    CHECK(out != nullptr);
    CHECK_GE(axis, 0);
    CHECK_LT(axis, 3);
    in.Validate();
    if (stats != nullptr) {
        *stats = ClipStats{};
    }

    const bool has_normal =
        MeshHasFlag(in.flags, jpov::MeshVertexFlags::kNormal);
    const bool has_uv = MeshHasFlag(in.flags, jpov::MeshVertexFlags::kUV);
    const bool has_tangent =
        MeshHasFlag(in.flags, jpov::MeshVertexFlags::kTangent);
    const bool has_joints =
        MeshHasFlag(in.flags, jpov::MeshVertexFlags::kJoints);
    const size_t vcount = in.positions.size();
    const size_t tri_count =
        in.indices.empty() ? (vcount / 3) : (in.indices.size() / 3);
    if (stats != nullptr) {
        stats->input_triangles = tri_count;
    }

    constexpr uint32_t kNoEdge = 0xFFFFFFFFu;

    jpov::MeshData result;
    result.flags = in.flags;
    result.positions.reserve(vcount);
    if (has_normal) {
        result.normals.reserve(vcount);
    }
    if (has_uv) {
        result.uvs.reserve(vcount);
    }
    if (has_tangent) {
        result.tangents.reserve(vcount);
    }
    if (has_joints) {
        result.joint_indices.reserve(vcount);
        result.joint_weights.reserve(vcount);
    }

    std::unordered_map<uint32_t, uint32_t> orig_map;  // 原始顶点 → 输出下标
    std::unordered_map<uint64_t, uint32_t> edge_map;  // 边 → 输出下标
    orig_map.reserve(vcount);

    // 复制一个原始顶点（首次引用时），返回输出下标。
    const auto add_original = [&](uint32_t idx) -> uint32_t {
        const auto it = orig_map.find(idx);
        if (it != orig_map.end()) {
            return it->second;
        }
        const uint32_t ni = static_cast<uint32_t>(result.positions.size());
        result.positions.push_back(in.positions[idx]);
        if (has_normal) {
            result.normals.push_back(in.normals[idx]);
        }
        if (has_uv) {
            result.uvs.push_back(in.uvs[idx]);
        }
        if (has_tangent) {
            result.tangents.push_back(in.tangents[idx]);
        }
        if (has_joints) {
            result.joint_indices.push_back(in.joint_indices[idx]);
            result.joint_weights.push_back(in.joint_weights[idx]);
        }
        orig_map.emplace(idx, ni);
        return ni;
    };

    // 在边 (ia,ib) 上按参数 t（从 ia 指向 ib）插值出一个边界顶点。
    // 内部按无序边 (min,max) 去重，保证共享边命中同一顶点。
    const auto add_intersection = [&](uint32_t ia, uint32_t ib,
                                      float t_ab) -> uint32_t {
        const uint32_t lo = std::min(ia, ib);
        const uint32_t hi = std::max(ia, ib);
        const float t = (ia == lo) ? t_ab : (1.0f - t_ab);
        const uint64_t key = mesh_clip_internal::EdgeKey(lo, hi);
        const auto it = edge_map.find(key);
        if (it != edge_map.end()) {
            return it->second;
        }
        const auto lerp3 = [&](const jpov::Vec3f& a, const jpov::Vec3f& b) {
            return jpov::Vec3f(a.x() + (b.x() - a.x()) * t,
                               a.y() + (b.y() - a.y()) * t,
                               a.z() + (b.z() - a.z()) * t);
        };

        const uint32_t ni = static_cast<uint32_t>(result.positions.size());
        result.positions.push_back(
            lerp3(in.positions[lo], in.positions[hi]));
        if (has_normal) {
            jpov::Vec3f nrm = lerp3(in.normals[lo], in.normals[hi]);
            const float len = std::sqrt(nrm.x() * nrm.x() + nrm.y() * nrm.y() +
                                        nrm.z() * nrm.z());
            if (len > 1e-20f) {
                nrm = jpov::Vec3f(nrm.x() / len, nrm.y() / len, nrm.z() / len);
            }
            result.normals.push_back(nrm);
        }
        if (has_uv) {
            const jpov::Vec2f& ua = in.uvs[lo];
            const jpov::Vec2f& ub = in.uvs[hi];
            result.uvs.push_back(jpov::Vec2f(ua.x() + (ub.x() - ua.x()) * t,
                                             ua.y() + (ub.y() - ua.y()) * t));
        }
        if (has_tangent) {
            result.tangents.push_back(lerp3(in.tangents[lo], in.tangents[hi]));
        }
        if (has_joints) {
            std::array<int32_t, 4> oj;
            std::array<float, 4> ow;
            mesh_clip_internal::BlendJointWeights(
                in.joint_indices[lo], in.joint_weights[lo],
                in.joint_indices[hi], in.joint_weights[hi], t, &oj, &ow);
            result.joint_indices.push_back(oj);
            result.joint_weights.push_back(ow);
        }
        edge_map.emplace(key, ni);
        return ni;
    };

    // 追加一个三角形（丢弃退化碎片）。
    const auto emit_triangle = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (mesh_clip_internal::TriangleArea(result.positions[a],
                                             result.positions[b],
                                             result.positions[c]) <=
            kClipMinTriangleArea) {
            return;
        }
        result.indices.push_back(a);
        result.indices.push_back(b);
        result.indices.push_back(c);
    };

    const auto tri_index = [&](size_t k, int corner) -> uint32_t {
        return in.indices.empty()
                   ? static_cast<uint32_t>(k * 3 + corner)
                   : in.indices[k * 3 + corner];
    };

    for (size_t k = 0; k < tri_count; ++k) {
        const uint32_t idx[3] = {tri_index(k, 0), tri_index(k, 1),
                                 tri_index(k, 2)};
        float d[3];
        bool inside[3];
        int n_in = 0;
        for (int c = 0; c < 3; ++c) {
            CHECK_LT(idx[c], vcount) << "ClipMeshByAxis: 索引越界 " << idx[c];
            d[c] = mesh_clip_internal::KeepSignedDistance(
                in.positions[idx[c]], axis, plane_coord, keep);
            inside[c] = d[c] >= -kClipEps;
            if (inside[c]) {
                ++n_in;
            }
        }
        if (n_in == 0) {
            continue;  // 全在删除侧。
        }
        if (n_in == 3) {  // 全在保留侧，原样保留。
            emit_triangle(add_original(idx[0]), add_original(idx[1]),
                          add_original(idx[2]));
            continue;
        }

        // 跨界：Sutherland–Hodgman 保序裁出多边形（3 或 4 个顶点）。
        struct PolyPt {
            uint32_t a;
            uint32_t b;  // b == kNoEdge 表示「原始顶点 a」；否则表示边 (a,b) 上的交点
            float t;     // 交点参数（从 a 指向 b）
        };
        PolyPt poly[4];
        int pn = 0;
        for (int c = 0; c < 3; ++c) {
            const int nx = (c + 1) % 3;
            if (inside[c]) {
                poly[pn++] = PolyPt{idx[c], kNoEdge, 0.0f};
            }
            if (inside[c] != inside[nx]) {
                const float t = d[c] / (d[c] - d[nx]);
                poly[pn++] = PolyPt{idx[c], idx[nx], t};
            }
        }

        uint32_t r[4];
        for (int m = 0; m < pn; ++m) {
            r[m] = (poly[m].b == kNoEdge)
                       ? add_original(poly[m].a)
                       : add_intersection(poly[m].a, poly[m].b, poly[m].t);
        }
        // t≈0/1 时交点与原顶点重合，去掉相邻重复（含首尾环绕）。
        uint32_t poly2[4];
        int pn2 = 0;
        for (int m = 0; m < pn; ++m) {
            if (pn2 > 0 && poly2[pn2 - 1] == r[m]) {
                continue;
            }
            poly2[pn2++] = r[m];
        }
        if (pn2 > 1 && poly2[0] == poly2[pn2 - 1]) {
            --pn2;
        }
        // 扇形三角化（凸多边形）。
        for (int m = 1; m + 1 < pn2; ++m) {
            emit_triangle(poly2[0], poly2[m], poly2[m + 1]);
        }
    }

    if (result.indices.empty()) {
        return false;  // 裁剪后为空。
    }
    result.Validate();
    if (stats != nullptr) {
        stats->output_triangles = result.indices.size() / 3;
        stats->kept_original_vertices = orig_map.size();
        stats->new_boundary_vertices = edge_map.size();
    }
    *out = std::move(result);
    return true;
}

// 便捷：沿水平面 y = plane_y 裁剪（= ClipMeshByAxis(axis=1)）。
inline bool ClipMeshByY(const jpov::MeshData& in, float plane_y,
                        ClipKeepSide keep, jpov::MeshData* out /*output*/,
                        ClipStats* stats /*output, 可空*/) {
    return ClipMeshByAxis(in, /*axis*/ 1, plane_y, keep, out, stats);
}

}  // namespace model_editor
}  // namespace jpov

#endif  // JPOV_MODEL_EDITOR_MESH_CLIP_H_
