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

// AnalyzeMasses 的实现：与 BuildTopology 同一套规则（位置焊接 ε=1e-6、
// 顶点面积 = Σ 关联三角形面积/3、退化边/三角形跳过），但**不建任何状态**，
// 便于在 Build 之前就把「多重 / 稳不稳」说清楚。
MassDiagnostics AnalyzeMasses(const std::vector<jpov::MeshData>& meshes,
                              const ArapSimConfig& config) {
    return AnalyzeMasses(meshes, config.area_density_kg_per_m2);
}

MassDiagnostics AnalyzeMasses(const std::vector<jpov::MeshData>& meshes,
                              float area_density_kg_per_m2) {
    MassDiagnostics out;
    CHECK(!meshes.empty());
    CHECK_GT(area_density_kg_per_m2, 0.0f);

    // ── 与 BuildTopology 一致的全局顶点空间 + 焊接 ──
    size_t vcount = 0;
    for (const jpov::MeshData& m : meshes) {
        vcount += m.positions.size();
    }
    std::vector<jpov::Vec3f> gpos;
    gpos.reserve(vcount);
    for (const jpov::MeshData& m : meshes) {
        for (const jpov::Vec3f& p : m.positions) {
            gpos.push_back(p);
        }
    }
    UnionFind uf(vcount);
    {
        const float cell = kWeldEpsilon;
        std::unordered_map<CellKey, std::vector<uint32_t>, CellKeyHash> grid;
        grid.reserve(vcount);
        for (size_t i = 0; i < vcount; ++i) {
            const CellKey key = MakeCellKey(gpos[i], cell);
            for (int64_t dx = -1; dx <= 1; ++dx) {
                for (int64_t dy = -1; dy <= 1; ++dy) {
                    for (int64_t dz = -1; dz <= 1; ++dz) {
                        const CellKey nk{key.x + dx, key.y + dy, key.z + dz};
                        std::unordered_map<CellKey, std::vector<uint32_t>,
                                           CellKeyHash>::iterator it =
                            grid.find(nk);
                        if (it == grid.end()) {
                            continue;
                        }
                        for (uint32_t j : it->second) {
                            if ((gpos[j] - gpos[i]).Norm() < kWeldEpsilon) {
                                uf.Union(static_cast<uint32_t>(i), j);
                            }
                        }
                    }
                }
            }
            grid[key].push_back(static_cast<uint32_t>(i));
        }
    }
    std::unordered_map<uint32_t, uint32_t> root_to_pid;
    std::vector<uint32_t> pid_of_vertex(vcount, 0);
    for (size_t i = 0; i < vcount; ++i) {
        const uint32_t root = uf.Find(static_cast<uint32_t>(i));
        std::unordered_map<uint32_t, uint32_t>::iterator it =
            root_to_pid.find(root);
        if (it == root_to_pid.end()) {
            const uint32_t pid = static_cast<uint32_t>(root_to_pid.size());
            root_to_pid.emplace(root, pid);
            pid_of_vertex[i] = pid;
        } else {
            pid_of_vertex[i] = it->second;
        }
    }
    const size_t pcount = root_to_pid.size();
    if (pcount == 0) {
        return out;
    }

    // 质点级 rest（首见即取） + 顶点面积累加 + 边表（去重，记 rest 长度）
    std::vector<jpov::Vec3f> rest(pcount, jpov::Vec3f(0.0f, 0.0f, 0.0f));
    std::vector<bool> filled(pcount, false);
    for (size_t i = 0; i < vcount; ++i) {
        if (!filled[pid_of_vertex[i]]) {
            rest[pid_of_vertex[i]] = gpos[i];
            filled[pid_of_vertex[i]] = true;
        }
    }
    std::vector<float> area(pcount, 0.0f);
    std::unordered_set<uint64_t> tri_keys;
    std::unordered_map<uint64_t, float> edge_len;   // key → rest 长度
    for (const jpov::MeshData& m : meshes) {
        const size_t nt = m.indices.size() / 3;
        for (size_t t = 0; t < nt; ++t) {
            const uint32_t p[3] = {pid_of_vertex[m.indices[t * 3 + 0]],
                                   pid_of_vertex[m.indices[t * 3 + 1]],
                                   pid_of_vertex[m.indices[t * 3 + 2]]};
            uint32_t s3[3] = {p[0], p[1], p[2]};
            if (s3[0] == s3[1] || s3[1] == s3[2] || s3[0] == s3[2]) {
                continue;
            }
            std::sort(s3, s3 + 3);
            const uint64_t tkey = (static_cast<uint64_t>(s3[0]) * pcount + s3[1]) *
                                      pcount + s3[2];
            if (tri_keys.insert(tkey).second) {
                const float ta = 0.5f * ((rest[p[1]] - rest[p[0]])
                                             .Cross(rest[p[2]] - rest[p[0]]))
                                            .Norm();
                if (ta > 0.0f) {
                    for (int k = 0; k < 3; ++k) {
                        area[p[k]] += ta / 3.0f;
                    }
                }
            }
            for (int k = 0; k < 3; ++k) {
                const uint32_t a = p[k];
                const uint32_t b = p[(k + 1) % 3];
                if (a == b) {
                    continue;
                }
                const uint32_t lo = std::min(a, b);
                const uint32_t hi = std::max(a, b);
                const uint64_t key = static_cast<uint64_t>(lo) * pcount + hi;
                if (edge_len.find(key) == edge_len.end()) {
                    edge_len.emplace(key, (rest[hi] - rest[lo]).Norm());
                }
            }
        }
    }

    // 短边阈值（与 BuildTopology 同规则：rest 包围盒对角线的 1e-4）
    jpov::Vec3f lo = rest[0];
    jpov::Vec3f hi = rest[0];
    for (const jpov::Vec3f& p : rest) {
        lo = jpov::Vec3f(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()),
                         std::min(lo.z(), p.z()));
        hi = jpov::Vec3f(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()),
                         std::max(hi.z(), p.z()));
    }
    const float min_edge_length = 1e-4f * (hi - lo).Norm();

    float total_area = 0.0f;
    float total_mass = 0.0f;
    float max_k_over_m = 0.0f;
    for (size_t i = 0; i < pcount; ++i) {
        total_area += area[i];
        total_mass += area[i] * area_density_kg_per_m2;
    }
    for (const auto& kv : edge_len) {
        const float L0 = kv.second;
        if (!(L0 > min_edge_length) || !(L0 > 1e-12f)) {
            continue;   // 与仿真一致：退化短边不参与
        }
        const uint32_t a = static_cast<uint32_t>(kv.first / pcount);
        const uint32_t b = static_cast<uint32_t>(kv.first % pcount);
        const float m_a = area[a] * area_density_kg_per_m2;
        const float m_b = area[b] * area_density_kg_per_m2;
        if (!(m_a > 0.0f) || !(m_b > 0.0f)) {
            continue;
        }
        const float inv_l = 1.0f / L0;
        max_k_over_m = std::max(max_k_over_m,
                                std::max(inv_l / m_a, inv_l / m_b));
    }
    out.total_area_m2 = total_area;
    out.total_mass_kg = total_mass;
    out.max_edge_k_over_m = max_k_over_m;
    return out;
}

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
    CHECK_GE(config.substeps, 1) << "ArapSim::Build: substeps 必须 ≥ 1";
    CHECK_GT(config.area_density_kg_per_m2, 0.0f)
        << "ArapSim::Build: area_density_kg_per_m2 必须 > 0";
    CHECK_GE(config.spring_stiffness_per_area, 0.0f)
        << "ArapSim::Build: spring_stiffness_per_area 必须 ≥ 0";
    CHECK_GE(config.arap_stiffness_per_area, 0.0f)
        << "ArapSim::Build: arap_stiffness_per_area 必须 ≥ 0";
    CHECK_GE(config.damping_per_second, 0.0f)
        << "ArapSim::Build: damping_per_second 必须 ≥ 0";
    CHECK_GE(config.gravity_magnitude, 0.0f)
        << "ArapSim::Build: gravity_magnitude 必须 ≥ 0";
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
                        std::unordered_map<CellKey, std::vector<uint32_t>,
                                           CellKeyHash>::iterator it =
                            grid.find(nk);
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
        std::unordered_map<uint32_t, uint32_t>::iterator it =
            root_to_particle.find(root);
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
                    e.geom_mean_area = 0.0f;   // 面积在建完之后回填（见下）
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

    // 短边阈值（按 rest 包围盒对角线的相对量，尺度无关）+ 跳过条数统计。
    skipped_short_edges_ = 0;
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
        for (const Edge& e : edges_) {
            if (e.rest_length < min_edge_length_) {
                ++skipped_short_edges_;
            }
        }
        LOG(INFO) << "ArapSim: rest 对角线 " << diag << "，短边阈值 "
                  << min_edge_length_ << "，跳过的退化短边 "
                  << skipped_short_edges_ << "/" << edges_.size();
    }

    // ── 8. 质量分配（面密度 × 顶点面积）与两类刚度（见文件头的物理约定）。──
    //   顶点面积 = 其周围三角形面积之和 / 3（三角形三个顶点各占 1/3）。
    //   退化（零面积）三角形不贡献面积，不影响质量守恒。
    particle_area_m2_.assign(particle_count_, 0.0f);
    mass_.assign(particle_count_, 0.0f);
    float total_area = 0.0f;
    for (const std::array<uint32_t, 3>& tri : tris_) {
        const jpov::Vec3f& pa = rest_pos_[tri[0]];
        const jpov::Vec3f& pb = rest_pos_[tri[1]];
        const jpov::Vec3f& pc = rest_pos_[tri[2]];
        const float area = 0.5f * ((pb - pa).Cross(pc - pa)).Norm();
        if (!(area > 0.0f)) {
            continue;   // 退化三角形：零面积，不贡献质量
        }
        total_area += area;
        for (int k = 0; k < 3; ++k) {
            particle_area_m2_[tri[k]] += area / 3.0f;
        }
    }
    float total_mass = 0.0f;
    for (uint32_t i = 0; i < particle_count_; ++i) {
        mass_[i] = particle_area_m2_[i] * config_.area_density_kg_per_m2;
        CHECK_GT(mass_[i], 0.0f)
            << "ArapSim::BuildTopology: 质点 " << i
            << " 的面积为 0（无有效关联三角形）⇒ 质量为 0，无法仿真";
        total_mass += mass_[i];
    }
    total_mass_ = total_mass;
    surface_area_ = total_area;

    // 回填每条边的 √(a_i·a_j)（面积定标用；此时两种面积都已就绪）。
    for (Edge& e : edges_) {
        e.geom_mean_area =
            std::sqrt(particle_area_m2_[e.a] * particle_area_m2_[e.b]);
    }

    // 两类刚度：边弹簧刚度系数 T，ARAP 刚度 β（默认按 T 换算，见 config 注释）。
    spring_c_ = config_.spring_stiffness_per_area;
    arap_beta_c_ = config_.arap_stiffness_per_area;

    LOG(INFO) << "ArapSim 物理量: 面密度 " << config_.area_density_kg_per_m2
              << " kg/m² ⇒ 总面积 " << total_area << " m²，总质量 "
              << total_mass << " kg（实测密度 " << (total_area > 0.0f ? total_mass / total_area : 0.0f)
              << " kg/m²）; 胡克 c=" << spring_c_ << " N/m³; ARAP c'=" << arap_beta_c_
              << " N/m; 阻尼 u=" << config_.damping_per_second
              << "/s（每 kg）; 子步=" << config_.substeps;

    pos_.assign(particle_count_, jpov::Vec3f(0.0f, 0.0f, 0.0f));
    vel_.assign(particle_count_, jpov::Vec3f(0.0f, 0.0f, 0.0f));
    force_.assign(particle_count_, jpov::Vec3f(0.0f, 0.0f, 0.0f));
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

jpov::Vec3f ArapSim::NeighborhoodCentroid(const std::vector<jpov::Vec3f>& pos,
                                          uint32_t i) const {
    jpov::Vec3f sum = pos[i];
    for (uint32_t j : ring_[i]) {
        sum += pos[j];
    }
    return sum * (1.0f / static_cast<float>(ring_[i].size() + 1));
}

void ArapSim::Reset() {
    CHECK_GT(particle_count_, 0u) << "ArapSim::Reset: 尚未 Build";
    for (uint32_t i = 0; i < particle_count_; ++i) {
        pos_[i] = rest_pos_[i];
        vel_[i] = jpov::Vec3f(0.0f, 0.0f, 0.0f);
    }
    // 初始可行化：资产一开始就与地面相交（大模型常见）时先投射一次，免得首帧弹跳。
    // 注意显式格式里速度是独立状态，位置投影**不会**凭空造出速度
    // （旧 PBD 格式里 v=(x−prev)/h 才会有那个坑）。
    if (config_.enable_ground) {
        ProjectGround();
    }
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
        vel_[i] = ApplyRotation(r, vel_[i]);
    }
    ScatterToVertices();
}

void ArapSim::SnapshotState(
    std::vector<jpov::Vec3f>* positions /*output*/,
    std::vector<jpov::Vec3f>* velocities /*output*/) const {
    CHECK_NOTNULL(positions);
    CHECK_NOTNULL(velocities);
    *positions = pos_;
    *velocities = vel_;
}

void ArapSim::RestoreState(const std::vector<jpov::Vec3f>& positions,
                           const std::vector<jpov::Vec3f>& velocities) {
    CHECK_EQ(positions.size(), particle_count_)
        << "ArapSim::RestoreState: positions 长度必须是 particle_count()";
    CHECK_EQ(velocities.size(), particle_count_)
        << "ArapSim::RestoreState: velocities 长度必须是 particle_count()";
    for (uint32_t i = 0; i < particle_count_; ++i) {
        // 只接受有限值：坏快照会把 NaN 永久钉在状态里（下次仿真还是一样坏）。
        CHECK(std::isfinite(positions[i].x()) && std::isfinite(positions[i].y()) &&
              std::isfinite(positions[i].z()))
            << "ArapSim::RestoreState: 快照含 NaN/inf（质点 " << i
            << "），拒绝恢复";
    }
    pos_ = positions;
    vel_ = velocities;
    ScatterToVertices();
}

void ArapSim::SetConfig(const ArapSimConfig& config) {
    config_ = config;
    // 材质刚度是「构建期派生量」（β 默认由 T 换算、质量由面密度×面积得到），
    // 故改配置时必须同步刷新它们——否则"关掉刚度"（置 0）不会生效，
    // 且把 β 从 0 改成非 0 时会静默沿用旧的自动值。质量只随面密度变。
    spring_c_ = config_.spring_stiffness_per_area;
    arap_beta_c_ = config_.arap_stiffness_per_area;
    for (uint32_t i = 0; i < particle_count_; ++i) {
        mass_[i] = particle_area_m2_[i] * config_.area_density_kg_per_m2;
        CHECK_GT(mass_[i], 0.0f) << "ArapSim::SetConfig: 质点 " << i
                                 << " 质量变为 0（面密度必须 > 0）";
    }
    total_mass_ = 0.0f;
    for (float m : mass_) {
        total_mass_ += m;
    }
}

void ArapSim::Step(float dt_seconds) {
    CHECK_GT(particle_count_, 0u) << "ArapSim::Step: 尚未 Build";
    CHECK_GT(dt_seconds, 0.0f) << "ArapSim::Step: dt 必须 > 0，收到 " << dt_seconds;
    CHECK_GE(config_.substeps, 1) << "ArapSim::Step: substeps 必须 ≥ 1";

    const float h = dt_seconds / static_cast<float>(config_.substeps);
    for (int s = 0; s < config_.substeps; ++s) {
        // ① 力（F/m，m = 1）
        ComputeForcesAt(pos_, vel_, &force_);
        // ② 半隐式欧拉：先更新速度，再用新速度更新位置
        for (uint32_t i = 0; i < particle_count_; ++i) {
            vel_[i] += force_[i] * h;
            pos_[i] += vel_[i] * h;
        }
        // ③ 地面（位置硬约束 + 法向速度归零）
        if (config_.enable_ground) {
            ProjectGround();
        }
    }
    ScatterToVertices();
}

void ArapSim::ComputeForcesAt(const std::vector<jpov::Vec3f>& positions,
                              const std::vector<jpov::Vec3f>& velocities,
                              std::vector<jpov::Vec3f>* out /*output*/) const {
    CHECK_NOTNULL(out);
    CHECK_EQ(positions.size(), particle_count_)
        << "ArapSim::ComputeForcesAt: positions 长度必须是 particle_count()";
    CHECK_EQ(velocities.size(), particle_count_)
        << "ArapSim::ComputeForcesAt: velocities 长度必须是 particle_count()";

    const jpov::Vec3f gravity_vector(0.0f, -config_.gravity_magnitude, 0.0f);

    // 输出是【加速度】F/m：逐力累加时先除以该质点质量。
    //   重力：mg/m = g（与质量无关 —— 质量不改变自由落体）。
    out->assign(particle_count_, gravity_vector);

    // ① 胡克弹簧：每条边一根，k_e = T/L0，单测可验证「0.1 m 产生 10 N」。
    //   注意返回的是 F/m ⇒ 除以两端质量时要把"力"和"加速度"分清：
    //   这里直接在加速度域累加：a += ±F_e/m_i，m_i 是加速度侧的质量。
    if (spring_c_ > 0.0f) {
        for (const Edge& e : edges_) {
            if (e.rest_length < min_edge_length_) {
                continue;   // 退化短边（数据卫生，见 arap_sim.h）
            }
            const jpov::Vec3f d = positions[e.b] - positions[e.a];
            const float len = d.Norm();
            if (len < 1e-12f) {
                continue;   // 两端完全重合：方向未定义（下一个子步/别的力会拉开）
            }
            // k_e = c·√(a_i·a_j) [N/m]（面积定标，见文件头） × 伸长量 [m] = 力 [N]
            const float k_e = spring_c_ * e.geom_mean_area;
            const float tension_n = k_e * (len - e.rest_length);   // >0 = 被拉长
            const jpov::Vec3f unit = d * (1.0f / len);
            const jpov::Vec3f f_n = unit * tension_n;              // 作用在 b 上的力（拉向 a）
            (*out)[e.a] += f_n * (1.0f / mass_[e.a]);
            (*out)[e.b] -= f_n * (1.0f / mass_[e.b]);
        }
    }

    // ② ARAP 局部形状力：F_i = β·(goal_i − x_i)，每质点除以自身质量。
    if (arap_beta_c_ > 0.0f) {
        std::vector<jpov::Vec3f> shape_offset;
        ComputeLocalRotationsInto(positions, &shape_offset);
        for (uint32_t i = 0; i < particle_count_; ++i) {
            const jpov::Vec3f goal =
                NeighborhoodCentroid(positions, i) + shape_offset[i];
            // β_i = c'·a_i [N/m]（面积定标）⇒ 加速度 = β_i·Δ/m_i
            const float beta_i = arap_beta_c_ * particle_area_m2_[i];
            (*out)[i] += (goal - positions[i]) * (beta_i / mass_[i]);
        }
    }

    // ③ 阻尼（与质量无关）：a_damp = −u·v ⇒ 速度衰减时间常数恒为 1/u。
    if (config_.damping_per_second > 0.0f) {
        for (uint32_t i = 0; i < particle_count_; ++i) {
            (*out)[i] -= velocities[i] * config_.damping_per_second;
        }
    }
}

void ArapSim::ComputeLocalRotationsInto(const std::vector<jpov::Vec3f>& pos,
                                        std::vector<jpov::Vec3f>* out) const {
    CHECK_NOTNULL(out);
    degenerate_rotation_count_ = 0;
    out->resize(particle_count_);
    for (uint32_t i = 0; i < particle_count_; ++i) {
        const jpov::Vec3f centroid = NeighborhoodCentroid(pos, i);

        // 协方差 A = Σ_j (x_j − c_i) ⊗ (rest_j − rest_c_i)。
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
        accumulate(pos[i], rest_pos_[i]);
        for (uint32_t j : ring_[i]) {
            accumulate(pos[j], rest_pos_[j]);
        }

        // ── 薄壳法向增广项（关键修正）──
        //   薄片结构（灯罩玻璃、单层纸面、**衣物**）的 1-ring 近似共面 ⇒ 上面这个
        //   协方差秩亏（det≈0）⇒ 极分解没有唯一解 ⇒ 该处失去旋转不变性，
        //   形状恢复力方向跑偏（现象：同一条链上「一部分立住、一部分彻底软化」）。
        //   补法：把「沿表面法向的一对虚邻居」计入协方差：  A += δ²·n_cur⊗n_rest。
        //   对刚性运动 x = R·rest + t 有 n_cur = R·n_rest ⇒
        //       A = R·(Σ d⊗d + δ²·n_rest⊗n_rest) = R·B，B 对称正定 ⇒ polar(A) = R，
        //   即**不破坏刚性不变性**，只是把秩补满到 3。
        const jpov::Vec3f n_cur = ParticleNormal(i, pos);
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
        // 记录 R_i · (rest_i − rest_c_i)：形状力目标的「旋转后偏移」。
        (*out)[i] = ApplyRotation(rot_mat, rest_pos_[i] - rest_c);
    }
}

void ArapSim::ProjectGround() {
    const float min_y = config_.ground_y + config_.ground_offset;
    for (uint32_t i = 0; i < particle_count_; ++i) {
        if (pos_[i].y() < min_y) {
            pos_[i] = jpov::Vec3f(pos_[i].x(), min_y, pos_[i].z());
            // 完全非弹性：法向（y）速度归零；切向不动（四参数模型里没有摩擦）。
            if (vel_[i].y() < 0.0f) {
                vel_[i] = jpov::Vec3f(vel_[i].x(), 0.0f, vel_[i].z());
            }
        }
    }
}

float ArapSim::max_stable_dt() const {
    if (particle_count_ == 0) {
        return 0.0f;
    }
    std::vector<float> row_sum(particle_count_, 0.0f);
    if (spring_c_ > 0.0f) {
        for (const Edge& e : edges_) {
            if (e.rest_length < min_edge_length_) {
                continue;
            }
            const float k_e = spring_c_ * e.geom_mean_area;
            row_sum[e.a] += k_e / mass_[e.a];
            row_sum[e.b] += k_e / mass_[e.b];
        }
    }
    if (arap_beta_c_ > 0.0f) {
        for (uint32_t i = 0; i < particle_count_; ++i) {
            const float n = static_cast<float>(ring_[i].size());
            const float beta_i = arap_beta_c_ * particle_area_m2_[i];
            row_sum[i] += beta_i * n / ((n + 1.0f) * mass_[i]);
        }
    }
    float max_row = 0.0f;
    for (float r : row_sum) {
        max_row = std::max(max_row, r);
    }
    if (max_row <= 0.0f) {
        return 1e9f;   // 无刚度 ⇒ 无稳定上限
    }
    return 2.0f / std::sqrt(max_row);
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
    std::vector<jpov::Vec3f> shape_offset;
    ComputeLocalRotationsInto(pos_, &shape_offset);
    double sum_sqr = 0.0;
    for (uint32_t i = 0; i < particle_count_; ++i) {
        const jpov::Vec3f goal =
            NeighborhoodCentroid(pos_, i) + shape_offset[i];
        sum_sqr += static_cast<double>((goal - pos_[i]).Sqr());
    }
    return static_cast<float>(
        std::sqrt(sum_sqr / static_cast<double>(particle_count_)));
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

}  // namespace jpov_arap
