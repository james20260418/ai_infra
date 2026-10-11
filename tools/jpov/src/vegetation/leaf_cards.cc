// JPOV 叶片卡片网格 —— 实现（GL-free、确定性、可单测）。
//
// 见 leaf_cards.h 的约定。每片叶 = 十字卡片（两片正交 quad），带 per-叶大小/roll 抖动。

#include "tools/jpov/src/vegetation/leaf_cards.h"

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
Vec3f RotateAround(const Vec3f& v, const Vec3f& k, float theta) {
    const float c = std::cos(theta);
    const float s = std::sin(theta);
    return VAdd(VAdd(VScale(v, c), VScale(VCross(k, v), s)),
                VScale(k, VDot(k, v) * (1.0f - c)));
}

// splitmix64：按 (seed, index) 派生确定性随机数。
uint64_t Mix(uint64_t seed, uint64_t index) {
    uint64_t z = seed + 0x9E3779B97F4A7C15ULL * (index + 1);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
// 由 (seed,index,salt) 派生 [0,1)。
float Rand01(uint64_t seed, uint64_t index, uint64_t salt) {
    return static_cast<float>((Mix(seed ^ (salt * 0x9E3779B97F4A7C15ULL),
                                   index) >> 11) * 0x1.0p-53);
}

// 追加一个 quad（4 顶点 + 2 三角形）；顶点按 (pos, normal, uv) 逐顶点给。
struct Vtx {
    Vec3f p;
    Vec3f n;
    jpov::Vec2f uv;
};
void AppendQuad(jpov::MeshData* mesh, const Vtx& a, const Vtx& b,
                const Vtx& c, const Vtx& d) {
    const uint32_t base = static_cast<uint32_t>(mesh->positions.size());
    for (const Vtx& v : {a, b, c, d}) {
        mesh->positions.push_back(v.p);
        mesh->normals.push_back(v.n);
        mesh->uvs.push_back(v.uv);
    }
    mesh->indices.push_back(base + 0);
    mesh->indices.push_back(base + 1);
    mesh->indices.push_back(base + 2);
    mesh->indices.push_back(base + 0);
    mesh->indices.push_back(base + 2);
    mesh->indices.push_back(base + 3);
}

}  // namespace

jpov::MeshData BuildLeafCardsMesh(const Skeleton& skel, const LeafCardConfig& cfg) {
    CHECK_GT(cfg.leaf_length, 0.0f) << "leaf_length 必须 > 0";
    CHECK_GT(cfg.aspect, 0.0f) << "aspect 必须 > 0";
    CHECK_GE(cfg.growth, 0.0f) << "growth 必须 >= 0";

    jpov::MeshData mesh;
    mesh.flags = static_cast<jpov::MeshVertexFlags>(
        static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kUV));

    const float jitter = std::max(0.0f, std::min(cfg.size_jitter, 0.95f));
    const int per_site = std::max(1, cfg.leaves_per_site);
    uint64_t leaf_index = 0;   // 全局叶序号（抖动派生用）
    for (size_t k = 0; k < skel.leaf_sites.size(); ++k) {
        const LeafSite& site = skel.leaf_sites[k];
        const Vec3f up0 = VNorm(site.axis);
        // 每个叶位发 per_site 片叶（叶丛）：各自叶轴小幅张开 + 随机 roll。
        for (int j = 0; j < per_site; ++j, ++leaf_index) {
            const uint64_t idx = leaf_index;
            // 大小 = 基准长 × 生长 × (1 ± jitter)。
            const float size_j = 1.0f + (Rand01(cfg.seed, idx, 1) * 2.0f - 1.0f) * jitter;
            const float length = cfg.leaf_length * cfg.growth * size_j;
            if (length <= 1.0e-6f) {
                continue;   // 生长为 0 → 不画
            }
            const float half_w = length * cfg.aspect * 0.5f;

            // 叶轴：绕 up0 的一个随机小倾角（叶丛张开）；per_site=1 时保持原轴。
            Vec3f up = up0;
            if (per_site > 1) {
                const float azim = Rand01(cfg.seed, idx, 3) * kTwoPi;
                const float tilt = Rand01(cfg.seed, idx, 4) * 0.7f;   // ≤ ~40°
                const Vec3f ref = (std::abs(up0.x()) < 0.9f) ? Vec3f(1, 0, 0)
                                                             : Vec3f(0, 1, 0);
                const Vec3f p1 = VNorm(VCross(up0, ref));
                const Vec3f p2 = VNorm(VCross(up0, p1));
                const Vec3f axis = VNorm(VAdd(VScale(p1, std::cos(azim)),
                                              VScale(p2, std::sin(azim))));
                up = VNorm(RotateAround(up0, axis, tilt));
            }
            // 叶面法线：基法线去垂后绕叶轴随机 roll。
            Vec3f base_n = VSub(site.normal, VScale(up, VDot(site.normal, up)));
            if (VLen(base_n) < 1.0e-6f) {
                base_n = VSub(Vec3f(0, 1, 0), VScale(up, VDot(Vec3f(0, 1, 0), up)));
            }
            base_n = VNorm(base_n);
            const Vec3f right0 = VNorm(VCross(up, base_n));
            const float roll = (Rand01(cfg.seed, idx, 2) * 2.0f - 1.0f) *
                               kTwoPi * 0.5f * cfg.roll_jitter;
            const Vec3f nrm = RotateAround(base_n, up, roll);
            const Vec3f right = RotateAround(right0, up, roll);

            const Vec3f c = site.position;                  // 叶柄着生点（卡片底边）
            const Vec3f topv = VAdd(c, VScale(up, length)); // 叶尖（沿叶轴向外）
            const Vec3f uW = VScale(right, half_w);
            // quad A：位于 (up, right) 平面，法线 = nrm。
            {
                const Vtx a{VAdd(topv, uW), nrm, jpov::Vec2f(1.0f, 0.0f)};
                const Vtx b{VAdd(topv, VScale(uW, -1.0f)), nrm, jpov::Vec2f(0.0f, 0.0f)};
                const Vtx cc{VAdd(c, VScale(uW, -1.0f)), nrm, jpov::Vec2f(0.0f, 1.0f)};
                const Vtx d{VAdd(c, uW), nrm, jpov::Vec2f(1.0f, 1.0f)};
                AppendQuad(&mesh, a, b, cc, d);
            }
            // quad B：位于 (up, nrm) 平面，法线 = right（与 A 正交）。
            {
                const Vec3f wN = VScale(nrm, half_w);
                const Vtx a{VAdd(topv, wN), right, jpov::Vec2f(1.0f, 0.0f)};
                const Vtx b{VAdd(topv, VScale(wN, -1.0f)), right, jpov::Vec2f(0.0f, 0.0f)};
                const Vtx cc{VAdd(c, VScale(wN, -1.0f)), right, jpov::Vec2f(0.0f, 1.0f)};
            const Vtx d{VAdd(c, wN), right, jpov::Vec2f(1.0f, 1.0f)};
                AppendQuad(&mesh, a, b, cc, d);
            }
        }
    }
    return mesh;
}

}  // namespace vegetation
}  // namespace jpov
