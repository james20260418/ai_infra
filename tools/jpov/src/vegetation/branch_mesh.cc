// JPOV 枝干管网格 —— 实现（GL-free、确定性、可单测）。
//
// 见 branch_mesh.h 的约定说明。核心：逐枝链扫圆环 + 平行传输标架 + 三角带缝合。

#include "tools/jpov/src/vegetation/branch_mesh.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include <glog/logging.h>

namespace jpov {
namespace vegetation {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

using Vec3f = geom::Vec3<float>;

inline Vec3f VAdd(const Vec3f& a, const Vec3f& b) {
    return Vec3f(a.x() + b.x(), a.y() + b.y(), a.z() + b.z());
}
inline Vec3f VSub(const Vec3f& a, const Vec3f& b) {
    return Vec3f(a.x() - b.x(), a.y() - b.y(), a.z() - b.z());
}
inline Vec3f VScale(const Vec3f& a, float s) {
    return Vec3f(a.x() * s, a.y() * s, a.z() * s);
}
inline float VDot(const Vec3f& a, const Vec3f& b) {
    return a.x() * b.x() + a.y() * b.y() + a.z() * b.z();
}
inline Vec3f VCross(const Vec3f& a, const Vec3f& b) {
    return Vec3f(a.y() * b.z() - a.z() * b.y(),
                 a.z() * b.x() - a.x() * b.z(),
                 a.x() * b.y() - a.y() * b.x());
}
inline float VLen(const Vec3f& a) { return std::sqrt(VDot(a, a)); }
inline Vec3f VNorm(const Vec3f& a) {
    const float l = VLen(a);
    CHECK_GT(l, 1.0e-12f) << "零向量无法归一化";
    return VScale(a, 1.0f / l);
}

// 绕单位轴 k 旋转 v 角度 θ（Rodrigues）。
Vec3f RotateAround(const Vec3f& v, const Vec3f& k, float theta) {
    const float c = std::cos(theta);
    const float s = std::sin(theta);
    return VAdd(VAdd(VScale(v, c), VScale(VCross(k, v), s)),
                VScale(k, VDot(k, v) * (1.0f - c)));
}

// 取任意与 t 垂直的单位向量（选与 t 最不平行的坐标轴叉乘）。
Vec3f AnyPerp(const Vec3f& t) {
    const Vec3f ref = (std::abs(t.x()) < 0.9f) ? Vec3f(1.0f, 0.0f, 0.0f)
                                               : Vec3f(0.0f, 1.0f, 0.0f);
    return VNorm(VCross(t, ref));
}

}  // namespace

jpov::MeshData BuildBranchMesh(const Skeleton& skel, const BranchMeshConfig& cfg) {
    CHECK_GE(cfg.ring_edges, 3) << "ring_edges 至少 3";
    CHECK_GT(cfg.uv_tile_length, 0.0f) << "uv_tile_length 必须 > 0";

    const int edges = cfg.ring_edges;
    const int ring_verts = edges + 1;   // 缝处重复一个顶点（u=1）以便纹理循环

    jpov::MeshData mesh;
    mesh.flags = static_cast<jpov::MeshVertexFlags>(
        static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kUV));

    for (const Polyline& pl : skel.polylines) {
        const size_t n = pl.points.size();
        if (n < 2) {
            continue;
        }
        CHECK_EQ(pl.radii.size(), n) << "枝链点与半径数量不一致";

        // 1) 逐点切线（中间点用前后差分，端点用单侧差分的切线）。
        std::vector<Vec3f> tan(n);
        for (size_t i = 0; i < n; ++i) {
            Vec3f dir;
            if (i == 0) {
                dir = VSub(pl.points[1], pl.points[0]);
            } else if (i == n - 1) {
                dir = VSub(pl.points[n - 1], pl.points[n - 2]);
            } else {
                dir = VSub(pl.points[i + 1], pl.points[i - 1]);
            }
            const float l = VLen(dir);
            tan[i] = (l > 1.0e-9f) ? VScale(dir, 1.0f / l)
                                   : Vec3f(0.0f, 1.0f, 0.0f);
        }

        // 2) 平行传输标架（rotation-minimizing）：把初始法线沿切线最小旋转地推进。
        std::vector<Vec3f> nrm(n), bin(n);
        nrm[0] = AnyPerp(tan[0]);
        bin[0] = VNorm(VCross(tan[0], nrm[0]));
        for (size_t i = 1; i < n; ++i) {
            const Vec3f axis = VCross(tan[i - 1], tan[i]);
            const float s = VLen(axis);
            const float c = VDot(tan[i - 1], tan[i]);
            if (s > 1.0e-9f) {
                const float ang = std::atan2(s, c);
                nrm[i] = RotateAround(nrm[i - 1], VScale(axis, 1.0f / s), ang);
            } else {
                nrm[i] = nrm[i - 1];   // 切线近共线 → 不变
            }
            bin[i] = VNorm(VCross(tan[i], nrm[i]));
        }

        // 3) 沿链的弧长（供 UV 的 v）。
        std::vector<float> arc(n, 0.0f);
        for (size_t i = 1; i < n; ++i) {
            arc[i] = arc[i - 1] + VLen(VSub(pl.points[i], pl.points[i - 1]));
        }

        // 4) 发顶点（每点一圈 ring_verts 个） + 缝合相邻两圈。
        const uint32_t base = static_cast<uint32_t>(mesh.positions.size());
        for (size_t i = 0; i < n; ++i) {
            const float r = std::max(pl.radii[i], 1.0e-4f);
            const float v = arc[i] / cfg.uv_tile_length;
            for (int j = 0; j < ring_verts; ++j) {
                const float theta =
                    kTwoPi * static_cast<float>(j) / static_cast<float>(edges);
                const Vec3f dir = VAdd(VScale(nrm[i], std::cos(theta)),
                                       VScale(bin[i], std::sin(theta)));
                const Vec3f p = VAdd(pl.points[i], VScale(dir, r));
                mesh.positions.push_back(p);
                mesh.normals.push_back(VNorm(dir));
                mesh.uvs.push_back(jpov::Vec2f(
                    static_cast<float>(j) / static_cast<float>(edges), v));
            }
        }
        for (size_t i = 0; i + 1 < n; ++i) {
            for (int j = 0; j < edges; ++j) {
                const uint32_t a = base + static_cast<uint32_t>(i * ring_verts + j);
                const uint32_t b = a + 1;
                const uint32_t c =
                    base + static_cast<uint32_t>((i + 1) * ring_verts + j);
                const uint32_t d = c + 1;
                mesh.indices.push_back(a);
                mesh.indices.push_back(b);
                mesh.indices.push_back(d);
                mesh.indices.push_back(a);
                mesh.indices.push_back(d);
                mesh.indices.push_back(c);
            }
        }
    }
    return mesh;
}

}  // namespace vegetation
}  // namespace jpov
