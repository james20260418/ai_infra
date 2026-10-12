// JPOV 枝干管网格 —— 实现（GL-free、确定性、可单测）。
//
// 见 branch_mesh.h 的约定说明。核心：逐枝链扫圆环 + 平行传输标架 + 三角带缝合。

#include "tools/jpov/src/vegetation/branch_mesh.h"

#include <algorithm>
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

// 由世界坐标确定性派生一个 [0, 2π) 相位（不同枝链相位不同 ⇒ 虬曲/噪声不整齐划一）。
float ChainPhase(float x, float y, float z) {
    uint64_t h = 1469598103934665603ULL;   // FNV offset basis
    const auto mix = [&h](float v) {
        const int32_t q = static_cast<int32_t>(std::lround(v * 1000.0f));
        h ^= static_cast<uint64_t>(static_cast<uint32_t>(q));
        h *= 1099511628211ULL;
    };
    mix(x);
    mix(y);
    mix(z);
    return static_cast<float>((h >> 11) * 0x1.0p-53) * kTwoPi;
}

// 逐点切线（中间点前后差分，端点单侧差分）+ 平行传输标架（rotation-minimizing，防扭转）。
void ComputeTangentFrame(const std::vector<Vec3f>& p, std::vector<Vec3f>* tan,
                         std::vector<Vec3f>* nrm, std::vector<Vec3f>* bin) {
    const size_t n = p.size();
    tan->assign(n, Vec3f(0.0f, 1.0f, 0.0f));
    for (size_t i = 0; i < n; ++i) {
        Vec3f dir;
        if (i == 0) {
            dir = VSub(p[1], p[0]);
        } else if (i == n - 1) {
            dir = VSub(p[n - 1], p[n - 2]);
        } else {
            dir = VSub(p[i + 1], p[i - 1]);
        }
        const float l = VLen(dir);
        (*tan)[i] = (l > 1.0e-9f) ? VScale(dir, 1.0f / l) : Vec3f(0.0f, 1.0f, 0.0f);
    }
    nrm->resize(n);
    bin->resize(n);
    (*nrm)[0] = AnyPerp((*tan)[0]);
    (*bin)[0] = VNorm(VCross((*tan)[0], (*nrm)[0]));
    for (size_t i = 1; i < n; ++i) {
        const Vec3f axis = VCross((*tan)[i - 1], (*tan)[i]);
        const float s = VLen(axis);
        const float c = VDot((*tan)[i - 1], (*tan)[i]);
        if (s > 1.0e-9f) {
            (*nrm)[i] = RotateAround((*nrm)[i - 1], VScale(axis, 1.0f / s),
                                     std::atan2(s, c));
        } else {
            (*nrm)[i] = (*nrm)[i - 1];   // 切线近共线 → 不变
        }
        (*bin)[i] = VNorm(VCross((*tan)[i], (*nrm)[i]));
    }
}

}  // namespace

jpov::MeshData BuildBranchMesh(const Skeleton& skel, const BranchMeshConfig& cfg) {
    CHECK_GE(cfg.ring_edges, 3) << "ring_edges 至少 3";
    CHECK_GT(cfg.bark_tile_u_meters, 0.0f) << "bark_tile_u_meters 必须 > 0";
    CHECK_GT(cfg.bark_tile_v_meters, 0.0f) << "bark_tile_v_meters 必须 > 0";

    const int edges = cfg.ring_edges;
    const int ring_verts = edges + 1;   // 缝处重复一个顶点（u=1）以便纹理循环

    jpov::MeshData mesh;
    mesh.flags = static_cast<jpov::MeshVertexFlags>(
        static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kUV));

    for (const Polyline& pl : skel.polylines) {
        if (pl.points.size() < 2) {
            continue;
        }
        CHECK_EQ(pl.radii.size(), pl.points.size()) << "枝链点与半径数量不一致";

        // 0) 分叉领口（branch collar）：子枝起点半径从**母枝半径**平滑收细到子枝自身
        //    半径（smoothstep），过渡长度 = junction_blend × 母枝半径；过渡区内按需
        //    细分（圈数足够→肉饱满）。子管于是从母管上"领口"渐缩长出，而非硬台阶。
        //    junction_blend<=0 或非子枝（attach_radius==0）→ 原样（零回归）。
        std::vector<Vec3f> pts = pl.points;
        std::vector<float> radii = pl.radii;
        if (cfg.junction_blend > 0.0f && pl.attach_radius > 0.0f) {
            const float blend_len = cfg.junction_blend * pl.attach_radius;
            const float target = std::max(1.0e-4f, blend_len / 6.0f);   // 细分粒度
            const size_t m = pl.points.size();
            pts.clear();
            radii.clear();
            pts.push_back(pl.points[0]);
            radii.push_back(pl.attach_radius);
            float acc = 0.0f;
            for (size_t i = 1; i < m; ++i) {
                const Vec3f seg = VSub(pl.points[i], pl.points[i - 1]);
                const float seg_len = VLen(seg);
                int pieces = 1;
                if (acc < blend_len && seg_len > 1.0e-6f) {
                    pieces = std::min(16, static_cast<int>(
                        std::ceil(seg_len / target) + 0.5f));
                }
                for (int s = 1; s <= pieces; ++s) {
                    const float u = static_cast<float>(s) /
                                    static_cast<float>(pieces);
                    pts.push_back(VAdd(pl.points[i - 1], VScale(seg, u)));
                    const float r_child = pl.radii[i - 1] +
                        (pl.radii[i] - pl.radii[i - 1]) * u;
                    const float arc_here = acc + seg_len * u;
                    float r = r_child;
                    if (arc_here < blend_len) {
                        const float t = arc_here / blend_len;
                        const float sm = t * t * (3.0f - 2.0f * t);   // smoothstep
                        r = pl.attach_radius * (1.0f - sm) + r_child * sm;
                    }
                    radii.push_back(r);
                }
                acc += seg_len;
            }
        }

        const size_t n = pts.size();

        // 弧长（供 taper / 噪声 / 虬曲 与 UV 的 u）。
        std::vector<float> arc(n, 0.0f);
        for (size_t i = 1; i < n; ++i) {
            arc[i] = arc[i - 1] + VLen(VSub(pts[i], pts[i - 1]));
        }
        const float arc_total = arc[n - 1];

        // ③ 幂律 taper：在首末端半径之间按 (1−t)^p 重塑（t = 弧长占比）。
        if (cfg.taper_power > 0.0f && n >= 2 && arc_total > 1.0e-6f) {
            const float r_start = radii[0];
            const float r_end   = radii[n - 1];
            for (size_t i = 0; i < n; ++i) {
                const float t = arc[i] / arc_total;
                const float shaped =
                    std::pow(std::max(0.0f, 1.0f - t), cfg.taper_power);
                radii[i] = r_end + (r_start - r_end) * shaped;
            }
        }

        // ② 半径噪声：沿枝低频噪声乘半径（±比例），破"定半径"的规整感。
        if (cfg.radius_noise > 0.0f && arc_total > 1.0e-6f &&
            cfg.radius_noise_wavelength > 0.0f) {
            const float phase = ChainPhase(pts[0].x(), pts[0].y(), pts[0].z());
            for (size_t i = 0; i < n; ++i) {
                const float u = arc[i] / cfg.radius_noise_wavelength;
                const float nz = 0.6f * std::sin(kTwoPi * u + phase) +
                                 0.4f * std::sin(kTwoPi * 2.3f * u + phase * 1.7f);
                radii[i] *= (1.0f + cfg.radius_noise * nz);
            }
        }

        std::vector<Vec3f> tan, nrm, bin;
        ComputeTangentFrame(pts, &tan, &nrm, &bin);

        // ① 中心线虬曲：沿枝低频正弦横向扰动（破"直管"）；基部锚定（不撕裂领口）。
        if (cfg.centerline_gnarl > 0.0f && arc_total > 1.0e-6f &&
            cfg.gnarl_wavelength > 0.0f) {
            const float phase = ChainPhase(pts[0].x(), pts[0].y(), pts[0].z());
            std::vector<Vec3f> moved(n);
            for (size_t i = 0; i < n; ++i) {
                const float u = arc[i] / cfg.gnarl_wavelength;
                const float anchor =
                    std::min(1.0f, arc[i] / (0.25f * arc_total));
                const float g =
                    cfg.centerline_gnarl * std::sin(kTwoPi * u + phase) * anchor;
                moved[i] = VAdd(pts[i], VScale(nrm[i], g));
            }
            pts = moved;
            ComputeTangentFrame(pts, &tan, &nrm, &bin);
        }

        // 4) 发顶点（每点一圈 ring_verts 个） + 缝合相邻两圈。

        // 4) 发顶点（每点一圈 ring_verts 个） + 缝合相邻两圈。
        const uint32_t base = static_cast<uint32_t>(mesh.positions.size());
        for (size_t i = 0; i < n; ++i) {
            const float r = std::max(radii[i], 1.0e-4f);
            // u = 沿枝弧长 / u 瓦长：纹理的「长向」（贴图 X）顺枝干轴走，
            //     故树皮的纵向纹理沿枝、横向纹理绕枝（见本文件头 UV 约定）。
            const float u = arc[i] / cfg.bark_tile_u_meters;
            for (int j = 0; j < ring_verts; ++j) {
                const float theta =
                    kTwoPi * static_cast<float>(j) / static_cast<float>(edges);
                const Vec3f dir = VAdd(VScale(nrm[i], std::cos(theta)),
                                       VScale(bin[i], std::sin(theta)));
                const Vec3f p = VAdd(pts[i], VScale(dir, r));
                mesh.positions.push_back(p);
                mesh.normals.push_back(VNorm(dir));
                // v = 周向弧长 / v 瓦长（随半径变）：绕枝一圈。
                const float v = theta * r / cfg.bark_tile_v_meters;
                mesh.uvs.push_back(jpov::Vec2f(u, v));
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
        // 端盖（三角扇）：枝尖封口，避免露"平切"空洞。
        if (cfg.cap_ends) {
            // 末端（法线 +tan[n-1]）。
            {
                const uint32_t cbase = static_cast<uint32_t>(mesh.positions.size());
                mesh.positions.push_back(pts[n - 1]);
                mesh.normals.push_back(tan[n - 1]);
                mesh.uvs.push_back(jpov::Vec2f(arc[n - 1] / cfg.bark_tile_u_meters, 0.5f));
                const uint32_t rb = base + static_cast<uint32_t>((n - 1) * ring_verts);
                for (int j = 0; j < edges; ++j) {
                    mesh.indices.push_back(cbase);
                    mesh.indices.push_back(rb + static_cast<uint32_t>(j));
                    mesh.indices.push_back(rb + static_cast<uint32_t>(j + 1));
                }
            }
            // 起始端（法线 -tan[0]）。
            {
                const uint32_t cbase = static_cast<uint32_t>(mesh.positions.size());
                mesh.positions.push_back(pts[0]);
                mesh.normals.push_back(VScale(tan[0], -1.0f));
                mesh.uvs.push_back(jpov::Vec2f(0.0f, 0.5f));
                for (int j = 0; j < edges; ++j) {
                    mesh.indices.push_back(cbase);
                    mesh.indices.push_back(base + static_cast<uint32_t>(j + 1));
                    mesh.indices.push_back(base + static_cast<uint32_t>(j));
                }
            }
        }
    }
    return mesh;
}

}  // namespace vegetation
}  // namespace jpov
