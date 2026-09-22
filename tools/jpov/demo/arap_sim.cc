#include "tools/jpov/demo/arap_sim.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include <glog/logging.h>

namespace jpov_arap {
namespace {

// ── 3×3 矩阵（仅本文件内部使用；JPOV 的 geom 库没有 3×3 类型）──
//
// 极分解 R = polar(A) 需要 3×3 的逆/转置，故在此定义一个最小实现，
// 不对外暴露（不污染命名空间，也不需要通用线性代数库）。
struct Mat3 {
    float m[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
};

Mat3 Mat3Identity() {
    Mat3 r;
    r.m[0][0] = 1.0f;
    r.m[1][1] = 1.0f;
    r.m[2][2] = 1.0f;
    return r;
}

// 行列式。
float Mat3Det(const Mat3& a) {
    return a.m[0][0] * (a.m[1][1] * a.m[2][2] - a.m[1][2] * a.m[2][1]) -
           a.m[0][1] * (a.m[1][0] * a.m[2][2] - a.m[1][2] * a.m[2][0]) +
           a.m[0][2] * (a.m[1][0] * a.m[2][1] - a.m[1][1] * a.m[2][0]);
}

// 逆矩阵。det 过小 → 返回单位阵并置 *ok=false（调用方据此判定退化）。
Mat3 Mat3Inverse(const Mat3& a, bool* ok) {
    const float det = Mat3Det(a);
    if (std::fabs(det) < 1e-12f) {
        *ok = false;
        return Mat3Identity();
    }
    const float inv_det = 1.0f / det;
    Mat3 r;
    r.m[0][0] = (a.m[1][1] * a.m[2][2] - a.m[1][2] * a.m[2][1]) * inv_det;
    r.m[0][1] = (a.m[0][2] * a.m[2][1] - a.m[0][1] * a.m[2][2]) * inv_det;
    r.m[0][2] = (a.m[0][1] * a.m[1][2] - a.m[0][2] * a.m[1][1]) * inv_det;
    r.m[1][0] = (a.m[1][2] * a.m[2][0] - a.m[1][0] * a.m[2][2]) * inv_det;
    r.m[1][1] = (a.m[0][0] * a.m[2][2] - a.m[0][2] * a.m[2][0]) * inv_det;
    r.m[1][2] = (a.m[0][2] * a.m[1][0] - a.m[0][0] * a.m[1][2]) * inv_det;
    r.m[2][0] = (a.m[1][0] * a.m[2][1] - a.m[1][1] * a.m[2][0]) * inv_det;
    r.m[2][1] = (a.m[0][1] * a.m[2][0] - a.m[0][0] * a.m[2][1]) * inv_det;
    r.m[2][2] = (a.m[0][0] * a.m[1][1] - a.m[0][1] * a.m[1][0]) * inv_det;
    *ok = true;
    return r;
}

Mat3 Mat3Transpose(const Mat3& a) {
    Mat3 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r.m[i][j] = a.m[j][i];
        }
    }
    return r;
}

// 极分解：把 A 分解为 R·S（R 正交、S 对称正定），返回旋转部分 R。
//
// 用 Higham 的不动点迭代 R ← ½(R + R⁻ᵀ)，对良态 A 收敛到正交极因子。
// 前置归一化（除以 Frobenius 范数）使迭代与整体尺度无关。
//
// 退化判定（*out_degenerate）：归一化后的相对行列式过小 ⇒ 视作退化。
// 良态 3D 邻域的 det_n 约为 O(0.05~0.3)；共面/共线邻域趋近 0。
// ⚠️ 判据必须用**相对**行列式而非 `det <= 0`：近乎压扁的邻域其 det 是一个极小
// 的**正数**，`det<=0` 会漏过，随后 3×3 求逆把 R 放大到 1e9 量级 → 仿真炸掉。
Mat3 PolarRotation(const Mat3& a, bool* out_degenerate) {
    *out_degenerate = false;
    float frob_sqr = 0.0f;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            frob_sqr += a.m[i][j] * a.m[i][j];
        }
    }
    const float frob = std::sqrt(frob_sqr);
    if (frob < 1e-12f) {
        *out_degenerate = true;
        return Mat3Identity();
    }

    Mat3 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r.m[i][j] = a.m[i][j] / frob;
        }
    }
    // 相对行列式判据：同时排除反射（det ≤ 0）与近奇异（|det| 过小）。
    constexpr float kRelDetEps = 1e-4f;
    if (Mat3Det(r) < kRelDetEps) {
        *out_degenerate = true;
        return Mat3Identity();
    }

    constexpr int kMaxIter = 8;
    for (int k = 0; k < kMaxIter; ++k) {
        bool ok = false;
        const Mat3 inv_t = Mat3Transpose(Mat3Inverse(r, &ok));
        if (!ok) {
            *out_degenerate = true;
            return Mat3Identity();
        }
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                r.m[i][j] = 0.5f * (r.m[i][j] + inv_t.m[i][j]);
            }
        }
    }
    return r;
}

// 用旋转矩阵 R 变换向量 v。
jpov::Vec3f ApplyRotation(const Mat3& r, const jpov::Vec3f& v) {
    return jpov::Vec3f(r.m[0][0] * v.x() + r.m[0][1] * v.y() + r.m[0][2] * v.z(),
                       r.m[1][0] * v.x() + r.m[1][1] * v.y() + r.m[1][2] * v.z(),
                       r.m[2][0] * v.x() + r.m[2][1] * v.y() + r.m[2][2] * v.z());
}

// 空间哈希建焊接映射用的格键（坐标 → 格索引）。
struct CellKey {
    int64_t x;
    int64_t y;
    int64_t z;
    bool operator==(const CellKey& o) const {
        return x == o.x && y == o.y && z == o.z;
    }
};

struct CellKeyHash {
    size_t operator()(const CellKey& k) const {
        // 三个 int64 混合（常量取自 splitmix64）。
        uint64_t h = static_cast<uint64_t>(k.x) * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<uint64_t>(k.y) * 0xC2B2AE3D27D4EB4Full;
        h ^= static_cast<uint64_t>(k.z) * 0x165667B19E3779F9ull;
        h ^= h >> 29;
        return static_cast<size_t>(h);
    }
};

CellKey MakeCellKey(const jpov::Vec3f& p, float cell_size) {
    return CellKey{static_cast<int64_t>(std::floor(p.x() / cell_size)),
                   static_cast<int64_t>(std::floor(p.y() / cell_size)),
                   static_cast<int64_t>(std::floor(p.z() / cell_size))};
}

// 并查集（焊接位置重合顶点，跨全部输入网格）。
class UnionFind {
public:
    explicit UnionFind(size_t n) : parent_(n) {
        for (size_t i = 0; i < n; ++i) {
            parent_[i] = static_cast<uint32_t>(i);
        }
    }
    uint32_t Find(uint32_t x) {
        while (parent_[x] != x) {
            parent_[x] = parent_[parent_[x]];  // 路径减半
            x = parent_[x];
        }
        return x;
    }
    void Union(uint32_t a, uint32_t b) {
        const uint32_t ra = Find(a);
        const uint32_t rb = Find(b);
        if (ra != rb) {
            parent_[ra] = rb;
        }
    }

private:
    std::vector<uint32_t> parent_;
};

}  // namespace

void ArapSim::Build(const jpov::MeshData& mesh, const ArapSimConfig& config) {
    std::vector<jpov::MeshData> one;
    one.push_back(mesh);
    Build(one, config);
}

void ArapSim::Build(const std::vector<jpov::MeshData>& meshes,
                    const ArapSimConfig& config) {
    CHECK(!meshes.empty()) << "ArapSim::Build: meshes 不能为空";
    for (size_t i = 0; i < meshes.size(); ++i) {
        CHECK_GT(meshes[i].positions.size(), 0u)
            << "ArapSim::Build: mesh " << i << " positions 不能为空";
        CHECK(!meshes[i].indices.empty())
            << "ArapSim::Build: mesh " << i << " indices 不能为空";
        CHECK_EQ(meshes[i].indices.size() % 3, 0u)
            << "ArapSim::Build: mesh " << i << " indices 必须是 3 的倍数";
    }
    config_ = config;
    BuildTopology(meshes);
    Reset();
}

void ArapSim::BuildTopology(const std::vector<jpov::MeshData>& meshes) {
    // ── 0. 把全部网格拼成一个「全局顶点空间」（跨 primitive 一起焊接）。──
    mesh_vertex_begin_.assign(meshes.size() + 1, 0);
    for (size_t i = 0; i < meshes.size(); ++i) {
        mesh_vertex_begin_[i + 1] =
            mesh_vertex_begin_[i] + meshes[i].positions.size();
    }
    const size_t vcount = mesh_vertex_begin_.back();
    std::vector<jpov::Vec3f> gpos;
    gpos.reserve(vcount);
    for (const jpov::MeshData& m : meshes) {
        for (const jpov::Vec3f& p : m.positions) {
            gpos.push_back(p);
        }
    }

    // ── 1. 按位置焊接（ε 内合并）：UV 缝/硬边/材质切分造成的重合副本 → 同一质点。──
    UnionFind uf(vcount);
    {
        const float cell = kWeldEpsilon;
        std::unordered_map<CellKey, std::vector<uint32_t>, CellKeyHash> grid;
        grid.reserve(vcount);
        for (size_t i = 0; i < vcount; ++i) {
            const jpov::Vec3f& p = gpos[i];
            const CellKey key = MakeCellKey(p, cell);
            // 只与相邻 27 格内的点比较（同格内必然相邻，已包含）。
            for (int64_t dx = -1; dx <= 1; ++dx) {
                for (int64_t dy = -1; dy <= 1; ++dy) {
                    for (int64_t dz = -1; dz <= 1; ++dz) {
                        const CellKey nk{key.x + dx, key.y + dy, key.z + dz};
                        auto it = grid.find(nk);
                        if (it == grid.end()) {
                            continue;
                        }
                        for (uint32_t j : it->second) {
                            if ((gpos[j] - p).Norm() < kWeldEpsilon) {
                                uf.Union(static_cast<uint32_t>(i), j);
                            }
                        }
                    }
                }
            }
            grid[key].push_back(static_cast<uint32_t>(i));
        }
    }

    // 质点编号压缩：root → [0, particle_count)。
    std::unordered_map<uint32_t, uint32_t> root_to_particle;
    root_to_particle.reserve(vcount);
    particle_of_vertex_.assign(vcount, 0);
    for (size_t i = 0; i < vcount; ++i) {
        const uint32_t root = uf.Find(static_cast<uint32_t>(i));
        auto it = root_to_particle.find(root);
        if (it == root_to_particle.end()) {
            const uint32_t pid = static_cast<uint32_t>(root_to_particle.size());
            root_to_particle.emplace(root, pid);
            particle_of_vertex_[i] = pid;
        } else {
            particle_of_vertex_[i] = it->second;
        }
    }
    particle_count_ = root_to_particle.size();
    CHECK_GT(particle_count_, 0u);

    // ── 2. 质点级 rest 位置：同质点各副本位置重合，取首个即可。──
    rest_pos_.assign(particle_count_, jpov::Vec3f(0.0f, 0.0f, 0.0f));
    std::vector<bool> rest_filled(particle_count_, false);
    for (size_t i = 0; i < vcount; ++i) {
        const uint32_t pid = particle_of_vertex_[i];
        if (!rest_filled[pid]) {
            rest_pos_[pid] = gpos[i];
            rest_filled[pid] = true;
        }
    }

    // ── 3. 三角形表（质点级，去重）+ 边表（质点级，去重）。──
    std::unordered_set<uint64_t> edge_keys;
    std::unordered_set<uint64_t> tri_keys;
    edges_.clear();
    tris_.clear();
    size_t total_tris = 0;
    for (const jpov::MeshData& m : meshes) {
        total_tris += m.indices.size() / 3;
    }
    edge_keys.reserve(total_tris * 3);
    tri_keys.reserve(total_tris);
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        const jpov::MeshData& m = meshes[mi];
        const size_t base = mesh_vertex_begin_[mi];
        const size_t num_tris = m.indices.size() / 3;
        for (size_t t = 0; t < num_tris; ++t) {
            const uint32_t v[3] = {
                static_cast<uint32_t>(base + m.indices[t * 3 + 0]),
                static_cast<uint32_t>(base + m.indices[t * 3 + 1]),
                static_cast<uint32_t>(base + m.indices[t * 3 + 2])};
            for (int k = 0; k < 3; ++k) {
                CHECK_LT(v[k], vcount);
            }
            const uint32_t p[3] = {particle_of_vertex_[v[0]],
                                   particle_of_vertex_[v[1]],
                                   particle_of_vertex_[v[2]]};
            // 边（焊后同质点的边丢弃）。
            for (int k = 0; k < 3; ++k) {
                const uint32_t a = p[k];
                const uint32_t b = p[(k + 1) % 3];
                if (a == b) {
                    continue;
                }
                const uint32_t lo = std::min(a, b);
                const uint32_t hi = std::max(a, b);
                const uint64_t key =
                    static_cast<uint64_t>(lo) * particle_count_ + hi;
                if (edge_keys.insert(key).second) {
                    Edge e;
                    e.a = lo;
                    e.b = hi;
                    e.rest_length = (rest_pos_[hi] - rest_pos_[lo]).Norm();
                    edges_.push_back(e);
                }
            }
            // 三角形（焊后退化的丢弃；用排序后的三元组去重，避免同一三角形
            // 因材质切分被登记两次而重复计入法线）。
            uint32_t s3[3] = {p[0], p[1], p[2]};
            if (s3[0] == s3[1] || s3[1] == s3[2] || s3[0] == s3[2]) {
                continue;
            }
            std::sort(s3, s3 + 3);
            const uint64_t tkey = (static_cast<uint64_t>(s3[0]) * particle_count_ +
                                   s3[1]) * particle_count_ +
                                  s3[2];
            if (tri_keys.insert(tkey).second) {
                tris_.push_back({p[0], p[1], p[2]});
            }
        }
    }
    CHECK_GT(edges_.size(), 0u) << "ArapSim::BuildTopology: 没有有效边，网格退化";
    CHECK_GT(tris_.size(), 0u) << "ArapSim::BuildTopology: 没有有效三角形";

    // ── 3b. 弯曲约束：相邻两三角形共享一条边时，把它们的【两个对顶点】连一根
    //         距离约束（rest 长度 = rest 时的对顶点距离）。折叠会让该距离缩短，
    //         从而被拉回 —— 等效二面角弹簧，但实现代价极低（复用距离约束）。
    //         这是「抵抗折叠」的来源；ARAP 本身完全不抵抗折叠（见 config 注释）。
    {
        std::unordered_map<uint64_t, std::vector<uint32_t>> edge_tris;
        edge_tris.reserve(edges_.size() * 2);
        for (size_t t = 0; t < tris_.size(); ++t) {
            const std::array<uint32_t, 3>& tri = tris_[t];
            for (int k = 0; k < 3; ++k) {
                const uint32_t a = tri[k];
                const uint32_t b = tri[(k + 1) % 3];
                const uint32_t lo = std::min(a, b);
                const uint32_t hi = std::max(a, b);
                const uint64_t key =
                    static_cast<uint64_t>(lo) * particle_count_ + hi;
                edge_tris[key].push_back(static_cast<uint32_t>(t));
            }
        }
        std::unordered_set<uint64_t> bend_keys;
        bend_edges_.clear();
        for (auto& kv : edge_tris) {
            if (kv.second.size() != 2) {
                continue;   // 边界边（1 个三角形）或非流形（>2）：弯曲无定义/歧义
            }
            const uint32_t lo = static_cast<uint32_t>(kv.first / particle_count_);
            const uint32_t hi = static_cast<uint32_t>(kv.first % particle_count_);
            uint32_t opp[2] = {0, 0};
            for (int k = 0; k < 2; ++k) {
                const std::array<uint32_t, 3>& tri = tris_[kv.second[k]];
                uint32_t o = tri[0];
                for (int j = 0; j < 3; ++j) {
                    if (tri[j] != lo && tri[j] != hi) {
                        o = tri[j];
                        break;
                    }
                }
                opp[k] = o;
            }
            if (opp[0] == opp[1]) {
                continue;
            }
            const uint32_t blo = std::min(opp[0], opp[1]);
            const uint32_t bhi = std::max(opp[0], opp[1]);
            const uint64_t bkey =
                static_cast<uint64_t>(blo) * particle_count_ + bhi;
            if (bend_keys.insert(bkey).second) {
                Edge e;
                e.a = blo;
                e.b = bhi;
                e.rest_length = (rest_pos_[bhi] - rest_pos_[blo]).Norm();
                if (e.rest_length > 1e-9f) {
                    bend_edges_.push_back(e);
                }
            }
        }
    }

    // ── 4. 1-ring 邻域（由边双向建立）。──
    ring_.assign(particle_count_, {});
    for (const Edge& e : edges_) {
        ring_[e.a].push_back(e.b);
        ring_[e.b].push_back(e.a);
    }
    for (uint32_t i = 0; i < particle_count_; ++i) {
        CHECK(!ring_[i].empty())
            << "ArapSim::BuildTopology: 质点 " << i << " 无邻域（孤立点）";
    }

    // ── 5. 质点的关联三角形表（算法向用）。──
    particle_tris_.assign(particle_count_, {});
    for (size_t t = 0; t < tris_.size(); ++t) {
        for (int k = 0; k < 3; ++k) {
            particle_tris_[tris_[t][k]].push_back(static_cast<uint32_t>(t));
        }
    }

    // ── 6. 形状匹配的原始目标：rest_c_i（含自身的邻域质心）。──
    //   注意「含自身」：goal_i = c_i + R_i·(rest_i − rest_c_i)，若不含自身，
    //   该式在刚性变换下不严格成立。
    rest_local_centroid_.assign(particle_count_, jpov::Vec3f(0.0f, 0.0f, 0.0f));
    for (uint32_t i = 0; i < particle_count_; ++i) {
        jpov::Vec3f sum = rest_pos_[i];
        for (uint32_t j : ring_[i]) {
            sum += rest_pos_[j];
        }
        const float inv_n = 1.0f / static_cast<float>(ring_[i].size() + 1);
        rest_local_centroid_[i] = sum * inv_n;
    }

    // ── 7. 薄壳法向增广所需：rest 法线 + 邻域尺度 δ²。──
    //   δ 取「邻域内所有边的平均长度」；权重 δ² 与协方差项的量级一致
    //   （协方差每项是 (长度)⊗(长度)）。
    rest_normal_.assign(particle_count_, jpov::Vec3f(0.0f, 0.0f, 0.0f));
    ring_scale_sqr_.assign(particle_count_, 0.0f);
    for (uint32_t i = 0; i < particle_count_; ++i) {
        rest_normal_[i] = ParticleNormal(i, rest_pos_);
        float sum_len = 0.0f;
        for (uint32_t j : ring_[i]) {
            sum_len += (rest_pos_[j] - rest_pos_[i]).Norm();
        }
        const float mean_len = sum_len / static_cast<float>(ring_[i].size());
        ring_scale_sqr_[i] = mean_len * mean_len;
    }

    // 短边阈值：按 rest 包围盒对角线取相对量（尺度无关）。
    {
        jpov::Vec3f lo = rest_pos_[0];
        jpov::Vec3f hi = rest_pos_[0];
        for (const jpov::Vec3f& p : rest_pos_) {
            lo = jpov::Vec3f(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()),
                             std::min(lo.z(), p.z()));
            hi = jpov::Vec3f(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()),
                             std::max(hi.z(), p.z()));
        }
        const float diag = (hi - lo).Norm();
        min_edge_length_ = 1e-4f * diag;
        LOG(INFO) << "ArapSim: rest 对角线 " << diag << "，短边阈值 "
                  << min_edge_length_;
    }

    rot_.assign(particle_count_, jpov::Vec3f(0.0f, 0.0f, 0.0f));
    ground_contact_.assign(particle_count_, 0);
    pos_.assign(particle_count_, jpov::Vec3f(0.0f, 0.0f, 0.0f));
    prev_pos_.assign(particle_count_, jpov::Vec3f(0.0f, 0.0f, 0.0f));
    vel_.assign(particle_count_, jpov::Vec3f(0.0f, 0.0f, 0.0f));
    vertex_positions_.assign(vcount, jpov::Vec3f(0.0f, 0.0f, 0.0f));
}

jpov::Vec3f ArapSim::ParticleNormal(uint32_t i,
                                    const std::vector<jpov::Vec3f>& pos) const {
    jpov::Vec3f sum(0.0f, 0.0f, 0.0f);
    for (uint32_t t : particle_tris_[i]) {
        const std::array<uint32_t, 3>& tri = tris_[t];
        const jpov::Vec3f& a = pos[tri[0]];
        const jpov::Vec3f& b = pos[tri[1]];
        const jpov::Vec3f& c = pos[tri[2]];
        sum += (b - a).Cross(c - a);   // 未归一化 ⇒ 面积加权
    }
    const float len = sum.Norm();
    if (len < 1e-12f) {
        return jpov::Vec3f(0.0f, 0.0f, 0.0f);   // 退化：无有效法线
    }
    return sum * (1.0f / len);
}

void ArapSim::Reset() {
    CHECK_GT(particle_count_, 0u) << "ArapSim::Reset: 尚未 Build";
    for (uint32_t i = 0; i < particle_count_; ++i) {
        pos_[i] = rest_pos_[i];
        prev_pos_[i] = rest_pos_[i];
        vel_[i] = jpov::Vec3f(0.0f, 0.0f, 0.0f);
    }
    // ── 初始状态可行化 ──
    //   若资产一开始就与地面相交（大模型很常见：路灯高 20 单位、地面却在 −1.5），
    //   则第一步就要修掉巨大穿透 ⇒ 形状约束在该子步被剧烈违反 ⇒ **爆开**
    //   （实测：路灯首帧边长畸变 120%、随后飞到 26000）。故重置时先把地面约束
    //   投射一次、并把参考位置对齐，使初始状态天然可行、速度为零。
    //   注意只动 y（地面投影语义），不改形状。
    if (config_.enable_ground) {
        ProjectGround();
        for (uint32_t i = 0; i < particle_count_; ++i) {
            prev_pos_[i] = pos_[i];
        }
    }
    ComputeLocalRotations();  // 让 rot_ 立刻反映 bind pose（残差诊断/首帧渲染都对）
    ScatterToVertices();
}

void ArapSim::RotateCurrentState(const jpov::Vec3f& axis, float angle_rad,
                                 const jpov::Vec3f& pivot) {
    CHECK_GT(particle_count_, 0u) << "ArapSim::RotateCurrentState: 尚未 Build";
    const float axis_len = axis.Norm();
    CHECK_GT(axis_len, 1e-12f) << "ArapSim::RotateCurrentState: axis 不能为零";
    const jpov::Vec3f n = axis * (1.0f / axis_len);

    // Rodrigues 旋转矩阵 R = I·cosθ + (1−cosθ)·nnᵀ + [n]ₓ·sinθ。
    const float c = std::cos(angle_rad);
    const float s = std::sin(angle_rad);
    const float t = 1.0f - c;
    Mat3 r;
    r.m[0][0] = c + t * n.x() * n.x();
    r.m[0][1] = t * n.x() * n.y() - s * n.z();
    r.m[0][2] = t * n.x() * n.z() + s * n.y();
    r.m[1][0] = t * n.y() * n.x() + s * n.z();
    r.m[1][1] = c + t * n.y() * n.y();
    r.m[1][2] = t * n.y() * n.z() - s * n.x();
    r.m[2][0] = t * n.z() * n.x() - s * n.y();
    r.m[2][1] = t * n.z() * n.y() + s * n.x();
    r.m[2][2] = c + t * n.z() * n.z();

    for (uint32_t i = 0; i < particle_count_; ++i) {
        pos_[i] = pivot + ApplyRotation(r, pos_[i] - pivot);
        prev_pos_[i] = pivot + ApplyRotation(r, prev_pos_[i] - pivot);
        vel_[i] = ApplyRotation(r, vel_[i]);
    }
    // 局部形状目标 rot_ 需跟着状态刷新（R_i 变，R_i·Δ0 也变）。
    ComputeLocalRotations();
    ScatterToVertices();
}

void ArapSim::SetConfig(const ArapSimConfig& config) {
    config_ = config;
}

void ArapSim::Step(float dt_seconds) {
    CHECK_GT(particle_count_, 0u) << "ArapSim::Step: 尚未 Build";
    CHECK_GT(dt_seconds, 0.0f) << "ArapSim::Step: dt 必须 > 0，收到 " << dt_seconds;
    CHECK_GE(config_.substeps, 1) << "substeps 必须 ≥ 1";
    CHECK_GE(config_.solver_iterations, 1) << "solver_iterations 必须 ≥ 1";
    CHECK_GE(config_.velocity_damping_per_second, 0.0f);
    CHECK_GE(config_.ground_friction_per_second, 0.0f);

    const int substeps = config_.substeps;
    const float h = dt_seconds / static_cast<float>(substeps);
    // 每子步的速度保留系数：用「每秒衰减率」换算，使阻尼与 substeps 无关。
    const float vel_retain = std::exp(-config_.velocity_damping_per_second * h);
    // 两个恢复力：由「每秒恢复率」换算成本子步的投影比例（见 arap_sim.h 量纲说明）。
    CHECK_GE(config_.shape_restore_rate_per_second, 0.0f);
    CHECK_GE(config_.stretch_restore_rate_per_second, 0.0f);
    const float shape_s =
        1.0f - std::exp(-config_.shape_restore_rate_per_second * h);
    // 边长约束每子步要投影 solver_iterations 次，把每秒恢复率均摊到每次迭代
    // （近似：几何收敛而非线性衰减，这里只求“每子步总量 ≈ 1−exp(−rate·h)”）。
    CHECK_GE(config_.bend_restore_rate_per_second, 0.0f);
    stretch_iter_factor_ =
        1.0f - std::exp(-config_.stretch_restore_rate_per_second * h /
                        static_cast<float>(config_.solver_iterations));
    bend_iter_factor_ =
        1.0f - std::exp(-config_.bend_restore_rate_per_second * h /
                        static_cast<float>(config_.solver_iterations));
    shape_substep_factor_ = shape_s;
    const float min_y = config_.ground_y + config_.ground_offset;
    const float friction_retain =
        std::exp(-config_.ground_friction_per_second * h);

    for (int s = 0; s < substeps; ++s) {
        // ── 预测：重力积分 + 位置预测（PBD 的 semi-implicit 一步）──
        //
        // ⚠️ 关键细节（实测踩过大坑）：`prev_pos_` 是后面反推速度的参考位置。
        //   若它本身落在【不可行域】（例如初始状态就与地面相交），那么
        //   `v = (pos − prev)/h` 会把「地面把它推上来的那一段位移」当成速度，
        //   于是碰撞凭空注入极大动能（实测：立方体下半截陷在地面里 ⇒ 首个子步
        //   算出 120 m/s 的向上速度 ⇒ 整个仿真炸飞）。
        //   修法：把参考位置一并投射到可行域（地面以上）。这样地面投影不再产生
        //   虚假速度；而**合法的弹起**（形状恢复力把质点拉离地面）仍然成立——
        //   因为那种情形下 prev 在地面上、pos 被约束推高，差值就是真实速度。
        for (uint32_t i = 0; i < particle_count_; ++i) {
            vel_[i] += config_.gravity * h;
            prev_pos_[i] = pos_[i];
            if (config_.enable_ground && prev_pos_[i].y() < min_y) {
                prev_pos_[i] =
                    jpov::Vec3f(prev_pos_[i].x(), min_y, prev_pos_[i].z());
            }
            pos_[i] += vel_[i] * h;
        }

        // ── 局部旋转 + 形状匹配：每子步各算/投影一次 ──
        //   形状匹配是「全局一步」而非可迭代收敛的位置约束：若放进下面的
        //   迭代循环，4 子步 × 4 迭代 = 每帧 16 次投影会把形状硬压回原样
        //   （eff. 1−(1−s)^16 ≈ 全刚性），softness 旋钮就失效了。
        if (config_.enable_shape) {
            ComputeLocalRotations();
            ProjectShape();
        }

        // ── 约束迭代（Gauss-Seidel）：边长 → 地面（地面最后，避免残余穿透）──
        std::fill(ground_contact_.begin(), ground_contact_.end(), 0);
        for (int it = 0; it < config_.solver_iterations; ++it) {
            if (config_.enable_stretch) {
                ProjectStretch();
            }
            if (config_.enable_bend) {
                ProjectBend();
            }
            if (config_.enable_ground) {
                ProjectGround();
            }
        }

        // ── 速度更新：v = (pos − prev) / h，再乘阻尼；接触质点额外施加切向摩擦 ──
        const float inv_h = 1.0f / h;
        for (uint32_t i = 0; i < particle_count_; ++i) {
            jpov::Vec3f v = (pos_[i] - prev_pos_[i]) * inv_h * vel_retain;
            if (config_.enable_ground && ground_contact_[i] != 0) {
                // 切向（x/z）衰减；法向（y）不动（摩擦不产生法向力）。
                v = jpov::Vec3f(v.x() * friction_retain, v.y(),
                                v.z() * friction_retain);
            }
            vel_[i] = v;
        }

        // ── 整体平动摩擦（滚动阻力近似）──
        //   逐质点摩擦只能衰减「接触点自身」的切向速度：物体**滚动**时接触点近于静止，
        //   物体却能一直滚走（实测抛石机落地后滚出 5 个单位）。故只要本子步有接触，
        //   就把**整体平动速度**的水平分量也按同一摩擦系数衰减一次。
        //   （只动平动、不动自转：对「滚走」这一现象已经足够，且避免引入角阻尼模型。）
        bool any_contact = false;
        for (uint32_t i = 0; i < particle_count_; ++i) {
            if (ground_contact_[i] != 0) {
                any_contact = true;
                break;
            }
        }
        if (config_.enable_ground && any_contact) {
            jpov::Vec3f vcom(0.0f, 0.0f, 0.0f);
            for (uint32_t i = 0; i < particle_count_; ++i) {
                vcom += vel_[i];
            }
            const float inv_n = 1.0f / static_cast<float>(particle_count_);
            vcom = vcom * inv_n;
            const float k = 1.0f - friction_retain;
            for (uint32_t i = 0; i < particle_count_; ++i) {
                vel_[i] = jpov::Vec3f(vel_[i].x() - k * vcom.x(), vel_[i].y(),
                                      vel_[i].z() - k * vcom.z());
            }
        }
    }

    ScatterToVertices();
}

void ArapSim::ComputeLocalRotations() {
    ComputeLocalRotationsInto(&rot_);
}

void ArapSim::ComputeLocalRotationsInto(std::vector<jpov::Vec3f>* out) const {
    CHECK_NOTNULL(out);
    degenerate_rotation_count_ = 0;
    out->resize(particle_count_);
    for (uint32_t i = 0; i < particle_count_; ++i) {
        // 当前邻域质心 c_i（含自身）。
        jpov::Vec3f centroid = pos_[i];
        for (uint32_t j : ring_[i]) {
            centroid += pos_[j];
        }
        const float inv_n = 1.0f / static_cast<float>(ring_[i].size() + 1);
        centroid = centroid * inv_n;

        // 协方差 A = Σ_j (pos_j − c_i) ⊗ (rest_j − rest_c_i)。
        const jpov::Vec3f& rest_c = rest_local_centroid_[i];
        Mat3 cov;
        auto accumulate = [&cov, &centroid, &rest_c](const jpov::Vec3f& p,
                                                    const jpov::Vec3f& r) {
            const jpov::Vec3f d = p - centroid;
            const jpov::Vec3f d0 = r - rest_c;
            cov.m[0][0] += d.x() * d0.x();
            cov.m[0][1] += d.x() * d0.y();
            cov.m[0][2] += d.x() * d0.z();
            cov.m[1][0] += d.y() * d0.x();
            cov.m[1][1] += d.y() * d0.y();
            cov.m[1][2] += d.y() * d0.z();
            cov.m[2][0] += d.z() * d0.x();
            cov.m[2][1] += d.z() * d0.y();
            cov.m[2][2] += d.z() * d0.z();
        };
        accumulate(pos_[i], rest_pos_[i]);
        for (uint32_t j : ring_[i]) {
            accumulate(pos_[j], rest_pos_[j]);
        }

        // ── 薄壳法向增广项（关键修正）──
        //   薄片结构（灯罩玻璃、单层纸面、**衣物**）的 1-ring 近似共面 ⇒ 上面这个
        //   协方差秩亏（det≈0）⇒ 极分解没有唯一解 ⇒ 该处失去旋转不变性，
        //   形状恢复力方向跑偏（现象：同一条链上「一部分立住、一部分彻底软化掉下去」）。
        //   补法：把「沿表面法向的一对虚邻居」计入协方差：
        //       A += δ² · n_cur ⊗ n_rest
        //   对刚性运动 x = R·rest + t，有 n_cur = R·n_rest ⇒
        //       A = R·(Σ d⊗d + δ²·n_rest⊗n_rest) = R·B，B 对称正定 ⇒ polar(A) = R，
        //   即**不破坏刚性不变性**，只是把秩补满到 3。δ 取邻域平均边长（与协方差
        //   各项同量级）；实心区域协方差本就满秩，该项只带来极小的扰动。
        const jpov::Vec3f n_cur = ParticleNormal(i, pos_);
        const jpov::Vec3f& n_rest = rest_normal_[i];
        const float delta_sqr = ring_scale_sqr_[i];
        if (delta_sqr > 0.0f) {
            cov.m[0][0] += delta_sqr * n_cur.x() * n_rest.x();
            cov.m[0][1] += delta_sqr * n_cur.x() * n_rest.y();
            cov.m[0][2] += delta_sqr * n_cur.x() * n_rest.z();
            cov.m[1][0] += delta_sqr * n_cur.y() * n_rest.x();
            cov.m[1][1] += delta_sqr * n_cur.y() * n_rest.y();
            cov.m[1][2] += delta_sqr * n_cur.y() * n_rest.z();
            cov.m[2][0] += delta_sqr * n_cur.z() * n_rest.x();
            cov.m[2][1] += delta_sqr * n_cur.z() * n_rest.y();
            cov.m[2][2] += delta_sqr * n_cur.z() * n_rest.z();
        }

        bool degenerate = false;
        const Mat3 rot_mat = PolarRotation(cov, &degenerate);
        if (degenerate) {
            ++degenerate_rotation_count_;
        }
        // 记录 R_i · (rest_i − rest_c_i)：形状约束的「目标偏移」，避免每子步重算。
        (*out)[i] = ApplyRotation(rot_mat, rest_pos_[i] - rest_c);
    }
}

void ArapSim::ProjectShape() {
    const float stiffness = shape_substep_factor_;
    if (stiffness <= 0.0f) {
        return;
    }
    for (uint32_t i = 0; i < particle_count_; ++i) {
        // 当前邻域质心（含自身）。
        jpov::Vec3f centroid = pos_[i];
        for (uint32_t j : ring_[i]) {
            centroid += pos_[j];
        }
        const float inv_n = 1.0f / static_cast<float>(ring_[i].size() + 1);
        centroid = centroid * inv_n;
        // goal_i = c_i + R_i·(rest_i − rest_c_i)（rot_ 已存后半段）。
        const jpov::Vec3f goal = centroid + rot_[i];
        pos_[i] += (goal - pos_[i]) * stiffness;
    }
}

void ArapSim::ProjectStretch() {
    const float stiffness = stretch_iter_factor_;
    if (stiffness <= 0.0f) {
        return;
    }
    for (const Edge& e : edges_) {
        if (e.rest_length < min_edge_length_) {
            continue;   // 退化短边：见 arap_sim.h 的 min_edge_length_ 说明
        }
        const jpov::Vec3f d = pos_[e.b] - pos_[e.a];
        const float len = d.Norm();
        if (len < 1e-12f) {
            continue;  // 完全重合：方向未定义，跳过（下一迭代/其他约束会拉开）
        }
        // PBD 距离约束：把两端各移误差的一半（均匀质量下最优）。
        const float corr = (len - e.rest_length) / len * 0.5f * stiffness;
        const jpov::Vec3f delta = d * corr;
        pos_[e.a] += delta;
        pos_[e.b] -= delta;
    }
}

void ArapSim::ProjectBend() {
    const float stiffness = bend_iter_factor_;
    if (stiffness <= 0.0f) {
        return;
    }
    for (const Edge& e : bend_edges_) {
        if (e.rest_length < min_edge_length_) {
            continue;   // 退化短边：见 arap_sim.h 的 min_edge_length_ 说明
        }
        const jpov::Vec3f d = pos_[e.b] - pos_[e.a];
        const float len = d.Norm();
        if (len < 1e-12f) {
            continue;
        }
        const float corr = (len - e.rest_length) / len * 0.5f * stiffness;
        const jpov::Vec3f delta = d * corr;
        pos_[e.a] += delta;
        pos_[e.b] -= delta;
    }
}

void ArapSim::ProjectGround() {
    const float min_y = config_.ground_y + config_.ground_offset;
    for (uint32_t i = 0; i < particle_count_; ++i) {
        if (pos_[i].y() < min_y) {
            pos_[i] = jpov::Vec3f(pos_[i].x(), min_y, pos_[i].z());
            ground_contact_[i] = 1;
        }
    }
}

void ArapSim::ScatterToVertices() {
    const size_t vcount = particle_of_vertex_.size();
    for (size_t i = 0; i < vcount; ++i) {
        vertex_positions_[i] = pos_[particle_of_vertex_[i]];
    }
}

void ArapSim::WriteBackPositions(jpov::MeshData* mesh) const {
    CHECK_NOTNULL(mesh);
    CHECK_EQ(mesh_count(), 1u)
        << "ArapSim::WriteBackPositions(单网格): 仿真是从 " << mesh_count()
        << " 个网格建的，请用多网格版";
    CHECK_EQ(mesh->positions.size(), vertex_count())
        << "ArapSim::WriteBackPositions: mesh 顶点数与仿真不一致";
    mesh->positions = vertex_positions_;
}

void ArapSim::WriteBackPositions(std::vector<jpov::MeshData>* meshes) const {
    CHECK_NOTNULL(meshes);
    CHECK_EQ(meshes->size(), mesh_count())
        << "ArapSim::WriteBackPositions: 网格数与 Build 时不一致";
    for (size_t mi = 0; mi < meshes->size(); ++mi) {
        const size_t begin = mesh_vertex_begin_[mi];
        const size_t end = mesh_vertex_begin_[mi + 1];
        CHECK_EQ((*meshes)[mi].positions.size(), end - begin)
            << "ArapSim::WriteBackPositions: 网格 " << mi << " 顶点数与 Build 时不一致";
        for (size_t k = 0; k < end - begin; ++k) {
            (*meshes)[mi].positions[k] = vertex_positions_[begin + k];
        }
    }
}

float ArapSim::shape_residual_rms() const {
    if (particle_count_ == 0) {
        return 0.0f;
    }
    // 用「当前局部旋转 + 当前邻域质心」重建目标位置，量测与实际的偏差。
    // 这里现算一份旋转以反映【当前】状态（不用 rot_ 的缓存值，避免读到陈旧数据）。
    std::vector<jpov::Vec3f> rot_now;
    ComputeLocalRotationsInto(&rot_now);

    double sum_sqr = 0.0;
    for (uint32_t i = 0; i < particle_count_; ++i) {
        jpov::Vec3f centroid = pos_[i];
        for (uint32_t j : ring_[i]) {
            centroid += pos_[j];
        }
        const float inv_n = 1.0f / static_cast<float>(ring_[i].size() + 1);
        centroid = centroid * inv_n;
        const jpov::Vec3f goal = centroid + rot_now[i];
        sum_sqr += static_cast<double>((goal - pos_[i]).Sqr());
    }
    return static_cast<float>(std::sqrt(sum_sqr / static_cast<double>(particle_count_)));
}

float ArapSim::edge_distortion_rms() const {
    if (edges_.empty()) {
        return 0.0f;
    }
    double sum_sqr = 0.0;
    size_t used = 0;
    for (const Edge& e : edges_) {
        if (e.rest_length < min_edge_length_) {
            continue;
        }
        const float len = (pos_[e.b] - pos_[e.a]).Norm();
        const double ratio = static_cast<double>(len / e.rest_length) - 1.0;
        sum_sqr += ratio * ratio;
        ++used;
    }
    if (used == 0) {
        return 0.0f;
    }
    return static_cast<float>(std::sqrt(sum_sqr / static_cast<double>(used)));
}

float ArapSim::bend_distortion_rms() const {
    if (bend_edges_.empty()) {
        return 0.0f;
    }
    double sum_sqr = 0.0;
    size_t used = 0;
    for (const Edge& e : bend_edges_) {
        if (e.rest_length < min_edge_length_) {
            continue;
        }
        const float len = (pos_[e.b] - pos_[e.a]).Norm();
        const double ratio = static_cast<double>(len / e.rest_length) - 1.0;
        sum_sqr += ratio * ratio;
        ++used;
    }
    if (used == 0) {
        return 0.0f;
    }
    return static_cast<float>(std::sqrt(sum_sqr / static_cast<double>(used)));
}

}  // namespace jpov_arap
