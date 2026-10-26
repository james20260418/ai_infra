// JPOV 植被风 —— CPU 端参考实现（与 instanced VS 的 GLSL **逐式对应**）
//
// 用途：
//   1) MeshManager 载入期算逐顶点 L（父链总长度 + 按位置焊接 Laplacian 平滑）；
//   2) 植被工具导出「当前风形态下的网格」（--save-mesh）时做离线变形
//      —— 没有它就只能靠肉眼看 GPU 结果，没法把 joint/面积拉出来对账。
//
// 为什么放在 interface/（GL-free）：MeshManager（src/）与 demo 工具都要用，
//   而 demo 不能依赖 src/ 内部头。这里只依赖 mesh.h 的 MeshData。
//
// ⚠️ 改公式必须同步 `src/instanced/instanced_object_renderer.h` 的 kWindBlowGlsl
//   （以及 object3d 的三份）；这是「一份公式、两处实现」的**故意**重复：
//   GPU 侧要跑每帧，CPU 侧要能离线复现，两边不能互相调用。

#ifndef JPOV_INTERFACE_WIND_DEFORM_H_
#define JPOV_INTERFACE_WIND_DEFORM_H_

// geom/common/math_util.h 使用 M_PI；MinGW 下必须在**首次 include <cmath> 之前**
// 定义 _USE_MATH_DEFINES，否则 M_PI 未定义（与 render_command.h 同款保护）。
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

#include "tools/jpov/interface/mesh.h"

namespace jpov {

// 与 WindCommand 同义的一小份参数（避免 interface 头互相包含 render_command.h）。
struct WindDeformParams {
    float time = 0.0f;
    float amplitude = 0.35f;
    float frequency = 0.5f;
    Vec3f direction{1.0f, 0.0f, 0.0f};
    float turb_amplitude = 0.12f;
    float turb_frequency = 2.5f;
    float gust_frequency = 0.6f;
    float gust_strength = 0.7f;
    // 吹倒（逐实例）：位移 ≈ ratio × L × (L/H)^(n−1)，绕根枢轴做**刚性旋转**。
    float blow_ratio = 0.0f;
    float blow_exponent = 2.0f;
    float ref_length = 1.0f;   // H：模型系包围盒高
};

namespace wind_deform_internal {

inline float Dist(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0];
    const float dy = a[1] - b[1];
    const float dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

inline int ParentOf(const MeshData::WindJoint& j) {
    return static_cast<int>(std::lround(j.parent));
}

// Rodrigues：绕单位轴 axis 转 th。
inline void RotAxis(const float v[3], const float axis[3], float th, float out[3]) {
    const float c = std::cos(th);
    const float s = std::sin(th);
    const float cross[3] = {
        axis[1] * v[2] - axis[2] * v[1],
        axis[2] * v[0] - axis[0] * v[2],
        axis[0] * v[1] - axis[1] * v[0]};
    const float dot = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
    for (int k = 0; k < 3; ++k) {
        out[k] = v[k] * c + cross[k] * s + axis[k] * dot * (1.0f - c);
    }
}

}  // namespace wind_deform_internal

// 逐顶点「到 root 的父链总长度」（米）。
//   L = Σ|pivot_k − pivot_parent(k)| + |p_rest − pivot_own|
//
// 算完后按位置焊接（1e-3 m）+ 20 轮 Laplacian 平滑：
//   joint_id 是离散标签，树的分叉在网格里是焊接的连续曲面、骨架却是两棵子树；
//   相邻顶点可能拿到 L 差 2m 以上的关节（willow 实测 4.45 vs 6.52）。
//   不平滑的话，任何用 L 做位移的方案都会在分叉处把三角形撕成薄片。
inline std::vector<float> BuildWindPathLengths(const MeshData& data) {
    using namespace wind_deform_internal;
    const size_t n = data.positions.size();
    const size_t nj = data.wind_joints.size();
    std::vector<float> L(n, 0.0f);
    if (nj == 0 || data.wind_link.size() != n) {
        return L;
    }

    for (size_t i = 0; i < n; ++i) {
        const int jid = static_cast<int>(std::lround(data.wind_link[i].x()));
        if (jid < 0 || static_cast<size_t>(jid) >= nj) {
            continue;
        }
        const MeshData::WindJoint& wj = data.wind_joints[static_cast<size_t>(jid)];
        const Vec3f& p = data.positions[i];
        const float px[3] = {p.x(), p.y(), p.z()};
        float len = Dist(px, wj.pivot);
        int cur = jid;
        for (int step = 0; step < 64; ++step) {
            const MeshData::WindJoint& c = data.wind_joints[static_cast<size_t>(cur)];
            const int par = ParentOf(c);
            if (par < 0 || par == cur || static_cast<size_t>(par) >= nj) {
                break;
            }
            len += Dist(c.pivot, data.wind_joints[static_cast<size_t>(par)].pivot);
            cur = par;
        }
        L[i] = len;
    }

    // ---- 按位置焊接（半径 5mm 的邻域合并）----
    //   ⚠️ 为什么不是「量化到 1mm 网格」：UV 缝/硬边会产生**相距 1~2mm 的重复顶点**，
    //   量化会把它们分到相邻格子而漏焊；漏焊的两个顶点 L 可能差 0.45m，位移后
    //   从 1.5mm 的缝被拉成 5~12cm 的**可见薄片**（willow 实测放大 35~83 倍）。
    //   这里用空间哈希 + 27 邻域实际距离判定（r=5mm），保证"肉眼同一个点"共享 L。
    constexpr float kWeldRadius = 0.005f;
    constexpr float kCell = 0.01f;
    std::map<std::array<int, 3>, std::vector<uint32_t>> buckets;
    std::vector<uint32_t> rep(n);
    for (size_t i = 0; i < n; ++i) {
        const Vec3f& p = data.positions[i];
        const std::array<int, 3> base{
            static_cast<int>(std::floor(p.x() / kCell)),
            static_cast<int>(std::floor(p.y() / kCell)),
            static_cast<int>(std::floor(p.z() / kCell))};
        uint32_t found = static_cast<uint32_t>(i);
        bool merged = false;
        for (int dx = -1; dx <= 1 && !merged; ++dx) {
            for (int dy = -1; dy <= 1 && !merged; ++dy) {
                for (int dz = -1; dz <= 1 && !merged; ++dz) {
                    const std::array<int, 3> key{base[0] + dx, base[1] + dy,
                                                 base[2] + dz};
                    const auto it = buckets.find(key);
                    if (it == buckets.end()) {
                        continue;
                    }
                    for (uint32_t cand : it->second) {
                        const Vec3f& q = data.positions[cand];
                        const float ddx = p.x() - q.x();
                        const float ddy = p.y() - q.y();
                        const float ddz = p.z() - q.z();
                        if (ddx * ddx + ddy * ddy + ddz * ddz <=
                            kWeldRadius * kWeldRadius) {
                            found = cand;
                            merged = true;
                            break;
                        }
                    }
                }
            }
        }
        rep[i] = found;
        if (!merged) {
            buckets[base].push_back(static_cast<uint32_t>(i));
        }
    }

    // ---- 焊接图邻接（三角网）----
    std::vector<std::vector<uint32_t>> adj(n);
    auto add_edge = [&adj](uint32_t a, uint32_t b) {
        if (a != b) {
            adj[a].push_back(b);
            adj[b].push_back(a);
        }
    };
    if (!data.indices.empty()) {
        for (size_t k = 0; k + 2 < data.indices.size(); k += 3) {
            const uint32_t a = rep[data.indices[k + 0]];
            const uint32_t b = rep[data.indices[k + 1]];
            const uint32_t c = rep[data.indices[k + 2]];
            add_edge(a, b);
            add_edge(b, c);
            add_edge(c, a);
        }
    } else {
        for (size_t k = 0; k + 2 < n; k += 3) {
            const uint32_t a = rep[k + 0];
            const uint32_t b = rep[k + 1];
            const uint32_t c = rep[k + 2];
            add_edge(a, b);
            add_edge(b, c);
            add_edge(c, a);
        }
    }

    // ---- Laplacian 平滑（20 轮，λ=0.5）----
    std::vector<float> cur = L;
    std::vector<float> nxt = L;
    for (int it = 0; it < 20; ++it) {
        for (size_t i = 0; i < n; ++i) {
            if (rep[i] != i) {
                continue;
            }
            const std::vector<uint32_t>& nb = adj[i];
            if (nb.empty()) {
                nxt[i] = cur[i];
                continue;
            }
            float sum = 0.0f;
            for (uint32_t k : nb) {
                sum += cur[k];
            }
            nxt[i] = 0.5f * cur[i] + 0.5f * (sum / static_cast<float>(nb.size()));
        }
        for (size_t i = 0; i < n; ++i) {
            if (rep[i] == i) {
                cur[i] = nxt[i];
            }
        }
    }
    for (size_t i = 0; i < n; ++i) {
        L[i] = cur[rep[i]];
    }

    // ---- 空间平滑（半径 5cm，2 遍）----
    //   网格在分叉处常常是**断开的若干小块**（willow 的 bark 有 980 个连通块），
    //   边上的 Laplacian 传不过去；这里按「空间邻域」再抹一遍，保证肉眼相邻的点
    //   L 接近。跨枝（两根枝条空间上贴得很近）也会被平均——这正是要的：
    //   L 只是位移的标量场，宁可两根枝弯得一样，也不要撕开。
    constexpr float kSpatialRadius = 0.05f;
    constexpr float kSpatialCell = 0.05f;
    std::map<std::array<int, 3>, std::vector<uint32_t>> sb;
    for (size_t i = 0; i < n; ++i) {
        const Vec3f& p = data.positions[i];
        sb[{static_cast<int>(std::floor(p.x() / kSpatialCell)),
            static_cast<int>(std::floor(p.y() / kSpatialCell)),
            static_cast<int>(std::floor(p.z() / kSpatialCell))}]
            .push_back(static_cast<uint32_t>(i));
    }
    std::vector<float> sp = L;
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < n; ++i) {
            const Vec3f& p = data.positions[i];
            const int bx = static_cast<int>(std::floor(p.x() / kSpatialCell));
            const int by = static_cast<int>(std::floor(p.y() / kSpatialCell));
            const int bz = static_cast<int>(std::floor(p.z() / kSpatialCell));
            float wsum = 0.0f;
            float vsum = 0.0f;
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dz = -1; dz <= 1; ++dz) {
                        const auto it = sb.find({bx + dx, by + dy, bz + dz});
                        if (it == sb.end()) {
                            continue;
                        }
                        for (uint32_t k : it->second) {
                            const Vec3f& q = data.positions[k];
                            const float ddx = p.x() - q.x();
                            const float ddy = p.y() - q.y();
                            const float ddz = p.z() - q.z();
                            const float d2 = ddx * ddx + ddy * ddy + ddz * ddz;
                            if (d2 > kSpatialRadius * kSpatialRadius) {
                                continue;
                            }
                            const float w = 1.0f - std::sqrt(d2) / kSpatialRadius;
                            wsum += w;
                            vsum += w * L[k];
                        }
                    }
                }
            }
            sp[i] = (wsum > 1e-6f) ? (vsum / wsum) : L[i];
        }
        L = sp;
    }
    return L;
}

// CPU 复刻 `kWindBlowGlsl`（2026-10-19 版，**逐式对应**）：
//   · 每个关节各求一条链：pivot 摇摆（soft 衰减）+ 逐关节 DC 吹倒（不衰减）+ 链外湍流；
//   · 顶点取**空间最近的两个关节**，按 1/d 权重混合两次结果（消除 lev/soft 离散接缝）；
//   · 近重合顶点（5mm）共享同一套关节/权重（与 MeshManager 相同）。
//   ⚠️ 改 GPU 的 kWindBlowGlsl 必须同步这里，反之亦然（文件头的约定）。
inline void EvalJointDeform(const MeshData& data,
                            int j0,
                            const float p_in[3],
                            const float n_in[3],
                            float phase,
                            const WindDeformParams& w,
                            const float axis[3],
                            const float wdir[3],
                            float p_out[3],
                            float n_out[3]) {
    using namespace wind_deform_internal;
    const size_t nj = data.wind_joints.size();
    int chain[16];
    int nc = 0;
    int jj = j0;
    for (int c = 0; c < 16; ++c) {
        if (jj < 0 || static_cast<size_t>(jj) >= nj) {
            break;
        }
        chain[nc++] = jj;
        jj = ParentOf(data.wind_joints[static_cast<size_t>(jj)]);
    }
    float qpos[3] = {0.0f, 0.0f, 0.0f};
    float A = 0.0f;
    float pp[3] = {0.0f, 0.0f, 0.0f};
    float Ap = 0.0f;
    bool have_par = false;
    for (int k = nc - 1; k >= 0; --k) {
        const MeshData::WindJoint& jt = data.wind_joints[static_cast<size_t>(chain[k])];
        const float lev = std::clamp(jt.level, 0.0f, 3.0f);
        const float sof = std::clamp(jt.soft, 0.0f, 1.0f);
        const float fr = w.frequency * (1.0f + 1.6f * lev);
        const float amp = w.amplitude * 0.4f * (0.15f * lev);
        float pjw = fr * (w.time + jt.phase);
        pjw -= std::floor(pjw);
        const float th = amp * std::sin(6.28318531f * pjw);
        if (!have_par) {
            qpos[0] = jt.pivot[0];
            qpos[1] = jt.pivot[1];
            qpos[2] = jt.pivot[2];
            A = th;
            have_par = true;
        } else {
            const float d[3] = {jt.pivot[0] - pp[0], jt.pivot[1] - pp[1],
                                jt.pivot[2] - pp[2]};
            float rd[3];
            RotAxis(d, axis, Ap, rd);
            for (int c = 0; c < 3; ++c) {
                qpos[c] += rd[c];
            }
            A = th + (1.0f - sof) * Ap;
        }
        for (int c = 0; c < 3; ++c) {
            pp[c] = jt.pivot[c];
        }
        Ap = A;
    }
    const MeshData::WindJoint& own = data.wind_joints[static_cast<size_t>(j0)];
    const float rel[3] = {p_in[0] - own.pivot[0], p_in[1] - own.pivot[1],
                          p_in[2] - own.pivot[2]};
    float rr[3];
    RotAxis(rel, axis, A, rr);
    for (int c = 0; c < 3; ++c) {
        p_out[c] = qpos[c] + rr[c];
    }
    RotAxis(n_in, axis, A, n_out);



    // ---- 链外平动湍流（与 GLSL 逐式一致：相位逐顶点，幅度取本关节 soft/lev）----
    const float lev0 = std::clamp(own.level, 0.0f, 3.0f);
    const float sof0 = std::clamp(own.soft, 0.0f, 1.0f);
    const float tph = w.time + phase;
    float tpj = w.turb_frequency * (1.0f + 0.5f * lev0) * tph;
    tpj -= std::floor(tpj);
    float gph = w.gust_frequency * tph;
    gph -= std::floor(gph);
    const float burst =
        std::pow(std::max(0.0f, 0.5f + 0.5f * std::sin(6.28318531f * gph)), 4.0f);
    const float gust = 1.0f + (burst - 1.0f) * std::clamp(w.gust_strength, 0.0f, 1.0f);
    const float tamp = w.turb_amplitude * sof0 * (0.5f + 0.5f * lev0) * gust;
    float tdir[3] = {axis[0] * 0.6f + wdir[0] * 0.4f + 1e-4f,
                     axis[1] * 0.6f + 0.7f + wdir[1] * 0.4f,
                     axis[2] * 0.6f + wdir[2] * 0.4f + 1e-4f};
    const float tl = std::sqrt(tdir[0] * tdir[0] + tdir[1] * tdir[1] +
                               tdir[2] * tdir[2]);
    for (int c = 0; c < 3; ++c) {
        tdir[c] /= tl;
    }
    const float tmag = tamp * std::sin(6.28318531f * tpj);
    for (int c = 0; c < 3; ++c) {
        p_out[c] += tdir[c] * tmag;
    }
}

inline void DeformMeshWithWind(const MeshData& data,
                               const std::vector<float>& path_len,
                               const WindDeformParams& w,
                               std::vector<Vec3f>* out_pos,
                               std::vector<Vec3f>* out_nrm) {
    using namespace wind_deform_internal;
    const size_t n = data.positions.size();
    const size_t nj = data.wind_joints.size();
    out_pos->assign(n, Vec3f{0.0f, 0.0f, 0.0f});
    if (out_nrm != nullptr) {
        out_nrm->assign(n, Vec3f{0.0f, 0.0f, 0.0f});
    }
    (void)path_len;

    float wdir[3] = {w.direction.x(), w.direction.y(), w.direction.z()};
    float wl = std::sqrt(wdir[0] * wdir[0] + wdir[1] * wdir[1] + wdir[2] * wdir[2]);
    if (wl < 1e-6f) {
        wdir[0] = 1.0f;
        wdir[1] = 0.0f;
        wdir[2] = 0.0f;
        wl = 1.0f;
    }
    for (float& c : wdir) {
        c /= wl;
    }
    float axis[3] = {0.0f - wdir[1], wdir[0], 0.0f};
    float al = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (al < 1e-5f) {
        axis[0] = 1.0f;
        axis[1] = 0.0f;
        axis[2] = 0.0f;
    } else {
        for (float& c : axis) {
            c /= al;
        }
    }

    if (nj == 0 || data.wind_link.size() != n) {
        for (size_t i = 0; i < n; ++i) {
            (*out_pos)[i] = data.positions[i];
            if (out_nrm != nullptr && i < data.normals.size()) {
                (*out_nrm)[i] = data.normals[i];
            }
        }
        return;
    }

    // ---- 最近的两个关节 + 1/d 权重（与 MeshManager 相同）----
    std::vector<int> vj0(n, -1);
    std::vector<int> vj1(n, -1);
    std::vector<float> vw0(n, 1.0f);
    std::vector<float> vw1(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const Vec3f& p = data.positions[i];
        const float px[3] = {p.x(), p.y(), p.z()};
        float d0 = 1e18f;
        float d1 = 1e18f;
        int b0 = -1;
        int b1 = -1;
        for (size_t j = 0; j < nj; ++j) {
            const float d = Dist(px, data.wind_joints[j].pivot);
            if (d < d0) {
                d1 = d0;
                b1 = b0;
                d0 = d;
                b0 = static_cast<int>(j);
            } else if (d < d1) {
                d1 = d;
                b1 = static_cast<int>(j);
            }
        }
        if (b0 < 0) {
            continue;
        }
        if (b1 < 0) {
            b1 = b0;
            d1 = d0;
        }
        const float inv0 = 1.0f / (d0 + 1.0e-4f);
        const float inv1 = 1.0f / (d1 + 1.0e-4f);
        const float wsum = inv0 + inv1;
        vj0[i] = b0;
        vj1[i] = b1;
        vw0[i] = inv0 / wsum;
        vw1[i] = inv1 / wsum;
    }

    // ---- 近重合顶点（5mm）共享同一套关节/权重 ----
    {
        constexpr float kWeld = 0.005f;
        constexpr float kCell = 0.01f;
        std::map<std::array<int, 3>, std::vector<uint32_t>> buckets;
        std::vector<uint32_t> rep(n);
        for (size_t i = 0; i < n; ++i) {
            const Vec3f& p = data.positions[i];
            const std::array<int, 3> base{
                static_cast<int>(std::floor(p.x() / kCell)),
                static_cast<int>(std::floor(p.y() / kCell)),
                static_cast<int>(std::floor(p.z() / kCell))};
            uint32_t found = static_cast<uint32_t>(i);
            bool merged = false;
            for (int dx = -1; dx <= 1 && !merged; ++dx) {
                for (int dy = -1; dy <= 1 && !merged; ++dy) {
                    for (int dz = -1; dz <= 1 && !merged; ++dz) {
                        const auto it =
                            buckets.find({base[0] + dx, base[1] + dy, base[2] + dz});
                        if (it == buckets.end()) {
                            continue;
                        }
                        for (uint32_t cand : it->second) {
                            const Vec3f& q = data.positions[cand];
                            const float ddx = p.x() - q.x();
                            const float ddy = p.y() - q.y();
                            const float ddz = p.z() - q.z();
                            if (ddx * ddx + ddy * ddy + ddz * ddz <= kWeld * kWeld) {
                                found = cand;
                                merged = true;
                                break;
                            }
                        }
                    }
                }
            }
            rep[i] = found;
            if (!merged) {
                buckets[base].push_back(static_cast<uint32_t>(i));
            }
        }
        for (size_t i = 0; i < n; ++i) {
            const uint32_t r = rep[i];
            if (r != i && vj0[r] >= 0) {
                vj0[i] = vj0[r];
                vj1[i] = vj1[r];
                vw0[i] = vw0[r];
                vw1[i] = vw1[r];
            }
        }
    }

    for (size_t i = 0; i < n; ++i) {
        const Vec3f& p0 = data.positions[i];
        float p[3] = {p0.x(), p0.y(), p0.z()};
        float nrm[3] = {0.0f, 0.0f, 0.0f};
        if (i < data.normals.size()) {
            nrm[0] = data.normals[i].x();
            nrm[1] = data.normals[i].y();
            nrm[2] = data.normals[i].z();
        }
        if (vj0[i] < 0) {
            (*out_pos)[i] = p0;
            if (out_nrm != nullptr) {
                (*out_nrm)[i] = Vec3f{nrm[0], nrm[1], nrm[2]};
            }
            continue;
        }
        const float phase = data.wind_link[i].y();
        float pa[3];
        float na[3];
        EvalJointDeform(data, vj0[i], p, nrm, phase, w, axis, wdir, pa, na);
        float out[3] = {pa[0], pa[1], pa[2]};
        float on[3] = {na[0], na[1], na[2]};
        if (vj1[i] != vj0[i] && vw1[i] > 0.0f) {
            float pb[3];
            float nb[3];
            EvalJointDeform(data, vj1[i], p, nrm, phase, w, axis, wdir, pb, nb);
            const float w0 = vw0[i];
            const float w1 = vw1[i];
            for (int c = 0; c < 3; ++c) {
                out[c] = pa[c] * w0 + pb[c] * w1;
                on[c] = na[c] * w0 + nb[c] * w1;
            }
        }
        // 吹倒：逐顶点**平移**（与 soft 无关；用平滑后的 path_len ⇒ 空间连续）
        if (w.blow_ratio > 0.0f && w.ref_length > 1.0e-6f &&
            i < path_len.size() && path_len[i] > 0.0f) {
            float hx = wdir[0];
            float hz = wdir[2];
            const float hl = std::sqrt(hx * hx + hz * hz);
            if (hl < 1.0e-5f) {
                hx = 1.0f;
                hz = 0.0f;
            } else {
                hx /= hl;
                hz /= hl;
            }
            const float L = path_len[i];
            const float t = std::clamp(L / w.ref_length, 0.0f, 4.0f);
            const float mag = w.blow_ratio * L *
                              std::pow(std::max(t, 1.0e-6f), w.blow_exponent - 1.0f);
            out[0] += hx * mag;
            out[2] += hz * mag;
        }
        (*out_pos)[i] = Vec3f{out[0], out[1], out[2]};
        if (out_nrm != nullptr) {
            const float ln = std::sqrt(on[0] * on[0] + on[1] * on[1] + on[2] * on[2]);
            (*out_nrm)[i] = (ln > 1e-9f) ? Vec3f{on[0] / ln, on[1] / ln, on[2] / ln}
                                         : Vec3f{0.0f, 0.0f, 0.0f};
        }
    }
}

}  // namespace jpov

#endif  // JPOV_INTERFACE_WIND_DEFORM_H_
