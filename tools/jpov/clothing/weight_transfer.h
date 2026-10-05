// JPOV 穿衣工具 — 软布自动蒙皮（weight transfer，纯 CPU / GL-free）
//
// 需求（2026-10-03 Danis 定）：给「贴合人体的软布衣物」做**全自动蒙皮** —— 衣物不自建
//   rig，直接复用身体那套骨架：对衣物每个顶点，在**身体 rest 网格**上找最近三角形，
//   用重心坐标插值该三角形 3 角的 (JOINTS_0, WEIGHTS_0)，取全局 top-K 骨、归一化后
//   写回衣物顶点。骨架语义见 docs/jpov_clothes_rig_design.md §3（本文是其算法落地）。
//
// 为什么"最近三角形 + 重心插值"就够（设计文档 §2.1 铁律）：衣物与身体在**同一 rest
//   pose（T-pose）**下几何对齐，故衣物顶点的蒙皮权重应等于它贴着的那片身体表面的权重；
//   身体权重本就在相邻三角面之间连续，取最近三角形的重心插值即得连续、贴合的那份权重。
//
// 边界（本轮范围，呼应设计文档 §2.3）：
//   - 只做**贴身软布**；宽松/叠穿不在范围（太远的顶点走 gap 兜底并被计数，不静默丢弃）。
//   - 硬部分（甲片/工具）不走这里（它们该 100% 绑单骨，是另一套机制，见后续）。
//
// 本文件只依赖 geom（三角形/匹配器）与 mesh.h，**不碰 GL / glTF**，便于纯单测。

#ifndef JPOV_CLOTHING_WEIGHT_TRANSFER_H_
#define JPOV_CLOTHING_WEIGHT_TRANSFER_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

#include <glog/logging.h>

#include "geom/3d/triangle_3.h"
#include "geom/3d/triangle_matcher_3d.h"
#include "tools/jpov/interface/mesh.h"

namespace jpov {
namespace clothing {

// 本模块统一用 double 标量（与 geom::Triangle3 / TriangleMatcher3d 一致；顶点是 float
// 时显式提升，避免插值前丢精度）。
using Triangle3d = geom::Triangle3<double>;

// 每顶点固定 4 组 joint/weight（与 MeshData.joint_indices/joint_weights 对齐）。
inline constexpr int kMaxSkinInfluences = 4;

// 缝合焊接的距离阈值（米）：位置相距在该值内的顶点视为「同一缝合点」（导出时被拆开的
// 重复顶点），在权重图上合并成一体。0.1mm —— 足以吃掉导出 / 变换带来的浮点重复，又远小于
// 任何真实布料的两层间距，不会误并相邻顶点。
inline constexpr float kWeldToleranceM = 1.0e-4f;

// 一个三角形角的蒙皮 = 身体某顶点的 (joints, weights)。
struct SkinCorner {
    std::array<int32_t, 4> joints = {0, 0, 0, 0};
    std::array<float, 4> weights = {0.0f, 0.0f, 0.0f, 0.0f};
};

// 三角形 → 3 角蒙皮 的平行表。
//
// **与 TriangleMatcher3d::triangles() 同序同长**：`corners[i]` 描述的是 `triangles()[i]`
// 这个三角形的 a/b/c 三个顶点在身体网格里的蒙皮。这正是"查最近三角形 → 拿它的 3 角权重"
// 的桥梁（matcher 只给三角形几何，给不了蒙皮，故必须并排建一份）。
struct BodySkinTable {
    // corners[tri][corner]，corner 顺序与 Triangle3 的 a/b/c 一致。
    std::vector<std::array<SkinCorner, 3>> corners;

    size_t triangle_count() const { return corners.size(); }

    // 该表引用到的最大骨序号 + 1（= 需要的骨数下界）。空表返回 0。
    // 注意：这是"表里出现过的骨"，未必等于骨架总骨数（未被子网覆盖的骨不出现）。
    int referenced_bone_count() const {
        int max_joint = -1;
        for (const std::array<SkinCorner, 3>& tri : corners) {
            for (const SkinCorner& c : tri) {
                for (int32_t j : c.joints) {
                    max_joint = std::max(max_joint, static_cast<int>(j));
                }
            }
        }
        return max_joint + 1;
    }
};

// 把一个 mesh 的三角形追加到 tris（triangle list；索引网格按 indices 三三分组，
// 非索引网格按顶点序每 3 个一组，与渲染/仿真解释一致）。
//
// table 非空时**同步**追加每个三角形的 3 角蒙皮（保证与 tris 同序同长）；退化三角形
// （Create 返回 nullopt：共线/重合/面积趋近 0）在两侧**一起跳过**，故两表下标恒对齐。
//
// Pre-condition: table 为 nullptr 或 table->corners.size() == tris->size()（进入时一致）；
//   table 非空时 mesh.flags 必须含 kJoints（否则拿不到蒙皮，属调用错误 → LOG(FATAL)）。
void AppendMeshTriangles(const MeshData& mesh,
                         std::vector<Triangle3d>* tris /*inout*/,
                         BodySkinTable* table /*inout, 可为 nullptr*/);

// 点 p 在三角形 (a,b,c) 上的重心坐标 (wa, wb, wc)（分别对应 a/b/c），三者之和 == 1。
// p 应≈位于三角形所在平面（本模块传入的是 ClosestPointTo 的投影点，天然满足）。
// Pre-condition: (a,b,c) 非退化（Create 已保证）⇒ 分母 > 0。
std::array<double, 3> BarycentricOnTriangle(const geom::Vec3<double>& a,
                                            const geom::Vec3<double>& b,
                                            const geom::Vec3<double>& c,
                                            const geom::Vec3<double>& p);

// 一次自动蒙皮的统计（供面板回显 / 单测断言）。
struct SkinTransferStats {
    size_t vertex_count = 0;      // 处理的衣物顶点数
    size_t gap_vertex_count = 0;  // 到身体最近距离 > gap_threshold 的顶点数（兜底）
    size_t no_candidate_count = 0;// 最近三角形候选为空的顶点数（走全局线性兜底）
    float max_gap_m = 0.0f;       // 最大 gap 距离（米）
    size_t smooth_passes = 0;     // 实际执行的平滑迭代次数
    size_t weld_merged_vertex_count = 0;  // 因位置重合被焊接合并掉的重复顶点数（0 = 无重复）
};

// 位置重合顶点焊接：把位置相距 <= tolerance_m 的顶点并成一组（并查集），返回每个顶点的
// **组代表下标**（组代表 = 组内最小下标，满足 rep_of[v] == v）。用于让导出器在 UV / 材质
// 缝合处拆开的**重复顶点**在权重图上重新连成一体 —— 否则平滑会把缝合两侧朝不同邻居拉，
// 权重发散、蒙皮后缝合分离（“开裂”）。
//   tolerance_m <= 0：不做任何合并（恒等映射 rep_of[v] == v）。
// Pre-condition: positions 元素均为有限值。
std::vector<int> WeldVerticesByPosition(const std::vector<Vec3f>& positions,
                                        float tolerance_m);

// 软布自动蒙皮（weight transfer）：
//   对 cloth 每个顶点 v：
//     1) 在 body_matcher 找候选三角形（FindNearestTriangles）→ 取真正最近的 tri；
//     2) cp = tri.ClosestPointTo(v)；重心坐标 (wa,wb,wc)；
//     3) 按重心坐标 + 3 角 (joints,weights) 累加，取全局 top-`max_influences` 骨；
//     4) 归一化（Σ=1）写入 cloth->joint_indices / joint_weights。
//   到最近三角形距离 > gap_threshold_m 的顶点：仍用"最近三角形的最近角权重"兜底
//     （不用重心插值——离太远时插值无意义），并计入 gap 统计（不静默丢弃）。
//   smooth_iterations > 0：先把位置重合的**缝合重复顶点**焊接成组，再在**组代表图**上做
//     若干轮拉普拉斯平滑（既消除关节附近"相邻衣物顶点落到不同身体部位"的权重条带，又保证
//     缝合两侧权重逐位一致、不裂开），随后逐代表 top-K 剪枝 + 归一化并散射回所有组员。
//
// 就地写 cloth->joint_indices/weights 并置 kJoints flag。**不改顶点位置**（rest 形状
// 由调用方定稿后传入；蒙皮后再改几何会破坏权重↔顶点对应，须冻结）。
//
// 确定性：top-K 选取按 (权重降序, 骨号升序) 排序 ⇒ 同输入同输出。
//
// Pre-condition:
//   - body_skin.corners.size() == body_matcher.triangles().size()（同源同序）；
//   - cloth->positions 非空；
//   - cloth->joint_indices / joint_weights 为空，或长度 == positions.size()；
//   - 1 <= max_influences <= kMaxSkinInfluences；
//   - gap_threshold_m > 0；smooth_iterations >= 0。
SkinTransferStats TransferSkinWeights(
    const BodySkinTable& body_skin,
    const geom::TriangleMatcher3d<double>& body_matcher,
    MeshData* cloth /*inout*/,
    float gap_threshold_m,
    int max_influences,
    int smooth_iterations);

// ==================== 实现 ====================

inline void AppendMeshTriangles(const MeshData& mesh,
                                std::vector<Triangle3d>* tris /*inout*/,
                                BodySkinTable* table /*inout*/) {
    CHECK(tris != nullptr);
    if (table != nullptr) {
        CHECK_EQ(table->corners.size(), tris->size())
            << "AppendMeshTriangles: 蒙皮表与三角形不同步（进入时长度应一致）";
        CHECK(MeshHasFlag(mesh.flags, MeshVertexFlags::kJoints))
            << "AppendMeshTriangles: 需要蒙皮表但该 mesh 无 kJoints（JOINTS/WEIGHTS）";
        CHECK_EQ(mesh.joint_indices.size(), mesh.positions.size());
        CHECK_EQ(mesh.joint_weights.size(), mesh.positions.size());
    }

    const std::vector<Vec3f>& pos = mesh.positions;
    if (pos.empty()) {
        return;
    }

    // 追加一个三角形：退化（Create 返回 nullopt）则整体跳过（三角形与蒙皮角同进同出）。
    const auto append_one = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
        CHECK_LT(i0, pos.size());
        CHECK_LT(i1, pos.size());
        CHECK_LT(i2, pos.size());
        const geom::Vec3<double> a(pos[i0].x(), pos[i0].y(), pos[i0].z());
        const geom::Vec3<double> b(pos[i1].x(), pos[i1].y(), pos[i1].z());
        const geom::Vec3<double> c(pos[i2].x(), pos[i2].y(), pos[i2].z());
        std::optional<Triangle3d> tri = Triangle3d::Create(a, b, c);
        if (!tri.has_value()) {
            return;  // 退化：两侧一起跳过，保持下表对齐
        }
        tris->push_back(tri.value());
        if (table != nullptr) {
            std::array<SkinCorner, 3> corners;
            const uint32_t idx[3] = {i0, i1, i2};
            for (int k = 0; k < 3; ++k) {
                corners[k].joints = mesh.joint_indices[idx[k]];
                corners[k].weights = mesh.joint_weights[idx[k]];
            }
            table->corners.push_back(corners);
        }
    };

    if (!mesh.indices.empty()) {
        CHECK_EQ(mesh.indices.size() % 3, 0u)
            << "索引网格的 indices 必须是 3 的倍数（triangle list）";
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            append_one(mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]);
        }
    } else {
        CHECK_EQ(pos.size() % 3, 0u)
            << "非索引网格的顶点数必须是 3 的倍数（triangle list）";
        for (size_t i = 0; i + 2 < pos.size(); i += 3) {
            append_one(static_cast<uint32_t>(i), static_cast<uint32_t>(i + 1),
                       static_cast<uint32_t>(i + 2));
        }
    }
}

inline std::vector<int> WeldVerticesByPosition(const std::vector<Vec3f>& positions,
                                               float tolerance_m) {
    CHECK_GE(tolerance_m, 0.0f);
    const size_t n = positions.size();
    std::vector<int> rep(n);
    for (size_t i = 0; i < n; ++i) {
        rep[i] = static_cast<int>(i);
    }
    if (n == 0 || tolerance_m <= 0.0f) {
        return rep;  // 不做合并（恒等映射）
    }

    // 并查集：find 带路径压缩；union 让代表恒为组内最小下标（确定性）。
    const auto find_root = [&rep](int x) {
        int r = x;
        while (rep[r] != r) {
            r = rep[r];
        }
        while (rep[x] != r) {
            const int next = rep[x];
            rep[x] = r;
            x = next;
        }
        return r;
    };
    const auto unite = [&rep, &find_root](int a, int b) {
        const int ra = find_root(a);
        const int rb = find_root(b);
        if (ra == rb) {
            return;
        }
        if (ra < rb) {
            rep[rb] = ra;
        } else {
            rep[ra] = rb;
        }
    };

    // 空间哈希：按 tolerance 量化到体素；查本格 + 26 邻居格，保证跨格边界的重合对不漏。
    struct CellKey {
        int64_t x = 0;
        int64_t y = 0;
        int64_t z = 0;
        bool operator==(const CellKey& o) const {
            return x == o.x && y == o.y && z == o.z;
        }
    };
    struct CellHash {
        size_t operator()(const CellKey& k) const {
            uint64_t h = 1469598103934665603ULL;  // FNV offset
            const int64_t v[3] = {k.x, k.y, k.z};
            for (int i = 0; i < 3; ++i) {
                uint64_t u = static_cast<uint64_t>(v[i]);
                u += 0x9e3779b97f4a7c15ULL;  // splitmix64 混合（避免相邻整数哈希扎堆）
                u = (u ^ (u >> 30)) * 0xbf58476d1ce4e5b9ULL;
                u = (u ^ (u >> 27)) * 0x94d049bb133111ebULL;
                u ^= (u >> 31);
                h ^= u;
                h *= 1099511628211ULL;
            }
            return static_cast<size_t>(h);
        }
    };
    const double inv = 1.0 / static_cast<double>(tolerance_m);
    const auto key_of = [inv](const Vec3f& p) {
        return CellKey{static_cast<int64_t>(std::floor(p.x() * inv)),
                       static_cast<int64_t>(std::floor(p.y() * inv)),
                       static_cast<int64_t>(std::floor(p.z() * inv))};
    };
    std::unordered_map<CellKey, std::vector<int>, CellHash> cells;
    for (size_t i = 0; i < n; ++i) {
        cells[key_of(positions[i])].push_back(static_cast<int>(i));
    }

    const double tol2 =
        static_cast<double>(tolerance_m) * static_cast<double>(tolerance_m);
    for (size_t i = 0; i < n; ++i) {
        const CellKey base = key_of(positions[i]);
        const double px = positions[i].x();
        const double py = positions[i].y();
        const double pz = positions[i].z();
        for (int64_t dx = -1; dx <= 1; ++dx) {
            for (int64_t dy = -1; dy <= 1; ++dy) {
                for (int64_t dz = -1; dz <= 1; ++dz) {
                    const auto found =
                        cells.find(CellKey{base.x + dx, base.y + dy, base.z + dz});
                    if (found == cells.end()) {
                        continue;
                    }
                    for (int j : found->second) {
                        if (j <= static_cast<int>(i)) {
                            continue;  // 每对只处理一次
                        }
                        const double ddx = px - positions[static_cast<size_t>(j)].x();
                        const double ddy = py - positions[static_cast<size_t>(j)].y();
                        const double ddz = pz - positions[static_cast<size_t>(j)].z();
                        if (ddx * ddx + ddy * ddy + ddz * ddz <= tol2) {
                            unite(static_cast<int>(i), j);
                        }
                    }
                }
            }
        }
    }
    // 压缩到根代表（根 = 组内最小下标）。
    for (size_t i = 0; i < n; ++i) {
        rep[i] = find_root(static_cast<int>(i));
    }
    return rep;
}

inline std::array<double, 3> BarycentricOnTriangle(const geom::Vec3<double>& a,
                                                   const geom::Vec3<double>& b,
                                                   const geom::Vec3<double>& c,
                                                   const geom::Vec3<double>& p) {
    const geom::Vec3<double> v0 = b - a;
    const geom::Vec3<double> v1 = c - a;
    const geom::Vec3<double> v2 = p - a;
    const double d00 = v0.Dot(v0);
    const double d01 = v0.Dot(v1);
    const double d11 = v1.Dot(v1);
    const double d20 = v2.Dot(v0);
    const double d21 = v2.Dot(v1);
    const double denom = d00 * d11 - d01 * d01;
    // Create() 已保证非退化 ⇒ denom > 0（面积 > 0）。
    CHECK_GT(denom, 0.0) << "BarycentricOnTriangle: 退化三角形（denom<=0）";
    // 标准公式（Ericson §3.4）：u 对 a、v 对 b、w 对 c，且 u+v+w=1。
    const double v = (d11 * d20 - d01 * d21) / denom;  // 对 b
    const double w = (d00 * d21 - d01 * d20) / denom;  // 对 c
    const double u = 1.0 - v - w;                      // 对 a
    return {u, v, w};
}

namespace internal {

// 一个「骨 → 累加权重」项。
struct JointAccum {
    int32_t joint = 0;
    double weight = 0.0;
};

// 累加器容量 = 3 角 × 4 骨 = 12（同一骨去重合并后不会超过它）。
using JointAccumBuf = std::array<JointAccum, kMaxSkinInfluences * 3>;

// 把 (joint, weight) 累加进 acc（同骨合并）。返回更新后的有效长度 n。
inline int AccumulateJoint(JointAccumBuf* acc /*inout*/, int n, int32_t joint,
                           double weight) {
    for (int i = 0; i < n; ++i) {
        if ((*acc)[i].joint == joint) {
            (*acc)[i].weight += weight;
            return n;
        }
    }
    CHECK_LT(n, static_cast<int>(acc->size()));
    (*acc)[n].joint = joint;
    (*acc)[n].weight = weight;
    return n + 1;
}

// 从累加缓冲取 top-K（权重降序、骨号升序，保证确定性），**按 top-K 自身之和**归一化
// （剪掉尾部后重新归一，保证 Σ=1；若按全部之和归一，被剪掉的质量会让顶点向原点缩），
// 写入 out_joints/out_weights。返回是否成功（累加和 > 0）；K 之外的位置填 (0, 0)。
inline bool PickTopKNormalized(const JointAccumBuf& acc, int n, int k,
                               std::array<int32_t, 4>* out_joints,
                               std::array<float, 4>* out_weights) {
    if (n <= 0) {
        return false;
    }
    std::vector<JointAccum> items(acc.begin(), acc.begin() + n);
    std::stable_sort(items.begin(), items.end(),
                     [](const JointAccum& a, const JointAccum& b) {
                         if (a.weight != b.weight) {
                             return a.weight > b.weight;  // 权重降序
                         }
                         return a.joint < b.joint;        // 同权重按骨号升序
                     });
    out_joints->fill(0);
    out_weights->fill(0.0f);
    const int take = std::min(k, static_cast<int>(items.size()));
    double keep_sum = 0.0;
    for (int i = 0; i < take; ++i) {
        keep_sum += items[i].weight;
    }
    if (keep_sum <= 0.0) {
        return false;
    }
    for (int i = 0; i < take; ++i) {
        (*out_joints)[i] = items[i].joint;
        (*out_weights)[i] = static_cast<float>(items[i].weight / keep_sum);
    }
    return true;
}

// 三角形 3 角里离点 v 最近的角下标（gap / 权重全零两条兜底路径共用）。
// Pre-condition: v 有限。
inline int NearestCornerIndex(const Triangle3d& tri, const geom::Vec3<double>& v) {
    const geom::Vec3<double> cs[3] = {tri.a(), tri.b(), tri.c()};
    int best = 0;
    double best_d = std::numeric_limits<double>::max();
    for (int k = 0; k < 3; ++k) {
        const double d2 = (cs[k] - v).Sqr();
        if (d2 < best_d) {
            best_d = d2;
            best = k;
        }
    }
    return best;
}

// 把一个身体角的权重归一化后写入（兜底路径用：不假设资产权重已归一）。
// 权重和 <= 0 时（异常资产）写入全零权重（+ 原样 bone 下标）—— 不制造 NaN。
inline void AssignCornerNormalized(const SkinCorner& corner,
                                   std::array<int32_t, 4>* out_joints,
                                   std::array<float, 4>* out_weights) {
    double sum = 0.0;
    for (int k = 0; k < 4; ++k) {
        sum += static_cast<double>(corner.weights[k]);
    }
    *out_joints = corner.joints;
    for (int k = 0; k < 4; ++k) {
        (*out_weights)[k] =
            (sum > 0.0) ? static_cast<float>(corner.weights[k] / sum) : 0.0f;
    }
}

}  // namespace internal

inline SkinTransferStats TransferSkinWeights(
    const BodySkinTable& body_skin,
    const geom::TriangleMatcher3d<double>& body_matcher,
    MeshData* cloth /*inout*/,
    float gap_threshold_m,
    int max_influences,
    int smooth_iterations) {
    CHECK(cloth != nullptr);
    CHECK_EQ(body_skin.triangle_count(), body_matcher.triangles().size())
        << "TransferSkinWeights: 蒙皮表与身体匹配器不同序不同长";
    CHECK(!cloth->positions.empty()) << "TransferSkinWeights: 衣物顶点为空";
    CHECK_GE(max_influences, 1);
    CHECK_LE(max_influences, kMaxSkinInfluences);
    CHECK_GT(gap_threshold_m, 0.0f);
    CHECK_GE(smooth_iterations, 0);
    if (!cloth->joint_indices.empty()) {
        CHECK_EQ(cloth->joint_indices.size(), cloth->positions.size());
    }
    if (!cloth->joint_weights.empty()) {
        CHECK_EQ(cloth->joint_weights.size(), cloth->positions.size());
    }

    const size_t vcount = cloth->positions.size();
    const std::vector<Triangle3d>& tris = body_matcher.triangles();
    CHECK(!tris.empty()) << "TransferSkinWeights: 身体匹配器没有三角形";

    // 输出缓冲（先填零，逐顶点覆盖）。
    cloth->joint_indices.assign(vcount, std::array<int32_t, 4>{0, 0, 0, 0});
    cloth->joint_weights.assign(vcount, std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});

    SkinTransferStats stats;
    stats.vertex_count = vcount;

    for (size_t vi = 0; vi < vcount; ++vi) {
        const Vec3f& pv = cloth->positions[vi];
        const geom::Vec3<double> v(pv.x(), pv.y(), pv.z());

        // 1) 候选 → 真正最近三角形。
        const std::vector<int>& candidates = body_matcher.FindNearestTriangles(v);
        int nearest = -1;
        double nearest_sqr = std::numeric_limits<double>::max();
        for (int ti : candidates) {
            CHECK_GE(ti, 0);
            CHECK_LT(ti, static_cast<int>(tris.size()));
            const double d2 = tris[static_cast<size_t>(ti)].DistanceSquareTo(v);
            if (d2 < nearest_sqr) {
                nearest_sqr = d2;
                nearest = ti;
            }
        }
        if (nearest < 0) {
            // 候选为空（顶点离身体 > local_distance 的体素桶）：全局线性兜底（罕见）。
            // 记录并以 LOG_FIRST_N 提示一次，避免被静默忽略。
            ++stats.no_candidate_count;
            LOG_FIRST_N(WARNING, 1)
                << "TransferSkinWeights: 衣物顶点最近三角形候选为空，走全表线性兜底";
            for (int ti = 0; ti < static_cast<int>(tris.size()); ++ti) {
                const double d2 = tris[static_cast<size_t>(ti)].DistanceSquareTo(v);
                if (d2 < nearest_sqr) {
                    nearest_sqr = d2;
                    nearest = ti;
                }
            }
        }
        CHECK_GE(nearest, 0) << "TransferSkinWeights: 找不到任何最近三角形";

        const Triangle3d& tri = tris[static_cast<size_t>(nearest)];
        const std::array<SkinCorner, 3>& corners =
            body_skin.corners[static_cast<size_t>(nearest)];
        const double dist = std::sqrt(nearest_sqr);
        if (dist > static_cast<double>(gap_threshold_m)) {
            ++stats.gap_vertex_count;
            stats.max_gap_m = std::max(stats.max_gap_m, static_cast<float>(dist));
            // gap 兜底：取"离衣物顶点最近的角"的权重（离太远时重心插值无意义）。
            const int best_corner = internal::NearestCornerIndex(tri, v);
            internal::AssignCornerNormalized(corners[best_corner],
                                             &cloth->joint_indices[vi],
                                             &cloth->joint_weights[vi]);
            continue;
        }
        // 2) 重心坐标 + 3 角插值 → top-K 归一化。
        const geom::Vec3<double> cp = tri.ClosestPointTo(v);
        const std::array<double, 3> bary = BarycentricOnTriangle(tri.a(), tri.b(), tri.c(), cp);
        internal::JointAccumBuf acc;
        int n = 0;
        for (int k = 0; k < 3; ++k) {
            if (bary[k] <= 0.0) {
                continue;
            }
            for (int j = 0; j < 4; ++j) {
                const float w = corners[k].weights[j];
                if (w <= 0.0f) {
                    continue;
                }
                n = internal::AccumulateJoint(&acc, n, corners[k].joints[j],
                                              bary[k] * static_cast<double>(w));
            }
        }
        bool ok = internal::PickTopKNormalized(acc, n, max_influences,
                                               &cloth->joint_indices[vi],
                                               &cloth->joint_weights[vi]);
        if (!ok) {
            // 3 角权重全为 0（异常资产）：退回最近角的权重，避免顶点塌到原点。
            LOG_FIRST_N(WARNING, 1)
                << "TransferSkinWeights: 某顶点插值权重和为 0，退回最近角权重";
            const int best_corner = internal::NearestCornerIndex(tri, v);
            internal::AssignCornerNormalized(corners[best_corner],
                                             &cloth->joint_indices[vi],
                                             &cloth->joint_weights[vi]);
        }
    }
    // 3) 可选：衣物拓扑邻接图上的拉普拉斯平滑（去条带），再重剪枝 + 归一化。
    //
    // ⚠️ 关键：平滑图建在**焊接后的组代表**上，而不是原始顶点上。导出资产在 UV / 材质
    // 缝合处会把同一位置拆成多份互不相连的顶点（harness_vest 15123 顶点里 10227 个是这种
    // 重复），它们在原始拓扑图里没有边 ⇒ 平滑把缝合两侧朝不同邻居拉，权重发散 ⇒ 蒙皮后
    // 缝合两侧分离（“开裂”）。焊接后两侧同属一个代表，权重逐位一致，开裂消除。
    if (smooth_iterations > 0) {
        const int bones = std::max(body_skin.referenced_bone_count(), 1);

        // (a) 焊接：位置在 kWeldToleranceM 内的顶点并成一组。
        //     WeldVerticesByPosition 的组代表是「组内最小下标」（不连续），这里再压成稠密
        //     编号 0..rep_count-1，供后续数组直接索引（否则会出现下标越过 rep_count）。
        const std::vector<int> root_of =
            WeldVerticesByPosition(cloth->positions, kWeldToleranceM);
        std::vector<int> rep(vcount);
        std::vector<int> dense_of_root(vcount, -1);
        int rep_count = 0;
        for (size_t i = 0; i < vcount; ++i) {
            const int root = root_of[i];
            CHECK_GE(root, 0);
            CHECK_LT(root, static_cast<int>(vcount));
            if (dense_of_root[static_cast<size_t>(root)] < 0) {
                dense_of_root[static_cast<size_t>(root)] = rep_count++;
            }
            rep[i] = dense_of_root[static_cast<size_t>(root)];
        }
        stats.weld_merged_vertex_count = vcount - static_cast<size_t>(rep_count);

        // (b) 代表相邻接表（无向；去重，避免重复边放大某邻居的权重）。
        std::vector<std::vector<int>> adj(static_cast<size_t>(rep_count));
        const auto add_edge = [&](uint32_t i0, uint32_t i1) {
            CHECK_LT(i0, vcount);
            CHECK_LT(i1, vcount);
            const int r0 = rep[i0];
            const int r1 = rep[i1];
            if (r0 == r1) {
                return;  // 同组（缝合）内部不连边
            }
            adj[static_cast<size_t>(r0)].push_back(r1);
            adj[static_cast<size_t>(r1)].push_back(r0);
        };
        if (!cloth->indices.empty()) {
            CHECK_EQ(cloth->indices.size() % 3, 0u);
            for (size_t i = 0; i + 2 < cloth->indices.size(); i += 3) {
                add_edge(cloth->indices[i], cloth->indices[i + 1]);
                add_edge(cloth->indices[i + 1], cloth->indices[i + 2]);
                add_edge(cloth->indices[i + 2], cloth->indices[i]);
            }
        } else {
            for (size_t i = 0; i + 2 < vcount; i += 3) {
                add_edge(static_cast<uint32_t>(i), static_cast<uint32_t>(i + 1));
                add_edge(static_cast<uint32_t>(i + 1), static_cast<uint32_t>(i + 2));
                add_edge(static_cast<uint32_t>(i + 2), static_cast<uint32_t>(i));
            }
        }
        for (std::vector<int>& nb : adj) {
            std::sort(nb.begin(), nb.end());
            nb.erase(std::unique(nb.begin(), nb.end()), nb.end());
        }

        // (c) 初始稠密权重（按代表累加组内所有顶点的当前权重）。
        std::vector<double> w(
            static_cast<size_t>(rep_count) * static_cast<size_t>(bones), 0.0);
        for (size_t vi = 0; vi < vcount; ++vi) {
            const size_t row =
                static_cast<size_t>(rep[vi]) * static_cast<size_t>(bones);
            for (int j = 0; j < 4; ++j) {
                const int32_t bone = cloth->joint_indices[vi][j];
                const float weight = cloth->joint_weights[vi][j];
                if (weight > 0.0f && bone >= 0 && bone < bones) {
                    w[row + static_cast<size_t>(bone)] += weight;
                }
            }
        }
        // (d) 拉普拉斯平滑（逐代表；自项 + 邻居均值）。
        std::vector<double> w_next(w.size(), 0.0);
        for (int it = 0; it < smooth_iterations; ++it) {
            for (int r = 0; r < rep_count; ++r) {
                const size_t row = static_cast<size_t>(r) * static_cast<size_t>(bones);
                const size_t deg = adj[static_cast<size_t>(r)].size();
                const double denom = 1.0 + static_cast<double>(deg);
                for (int b = 0; b < bones; ++b) {
                    double s = w[row + static_cast<size_t>(b)];  // 自项
                    for (int nb : adj[static_cast<size_t>(r)]) {
                        s += w[static_cast<size_t>(nb) * static_cast<size_t>(bones) +
                               static_cast<size_t>(b)];
                    }
                    w_next[row + static_cast<size_t>(b)] = s / denom;
                }
            }
            w.swap(w_next);
            stats.smooth_passes = static_cast<size_t>(it + 1);
        }
        // (e) 逐代表 top-K + 归一化，再**散射**回该组所有顶点（缝合两侧得到同一份权重）。
        std::vector<std::array<int32_t, 4>> rep_joints(static_cast<size_t>(rep_count));
        std::vector<std::array<float, 4>> rep_weights(static_cast<size_t>(rep_count));
        for (int r = 0; r < rep_count; ++r) {
            const size_t row = static_cast<size_t>(r) * static_cast<size_t>(bones);
            std::vector<std::pair<double, int32_t>> items;
            for (int b = 0; b < bones; ++b) {
                if (w[row + static_cast<size_t>(b)] > 0.0) {
                    items.emplace_back(w[row + static_cast<size_t>(b)], b);
                }
            }
            rep_joints[static_cast<size_t>(r)].fill(0);
            rep_weights[static_cast<size_t>(r)].fill(0.0f);
            if (items.empty()) {
                continue;  // 理论不会：初始权重已带质量（除非该组权重全零）。
            }
            std::stable_sort(items.begin(), items.end(),
                             [](const std::pair<double, int32_t>& a,
                                const std::pair<double, int32_t>& b) {
                                 if (a.first != b.first) {
                                     return a.first > b.first;
                                 }
                                 return a.second < b.second;
                             });
            double sum = 0.0;
            const int take = std::min(max_influences, static_cast<int>(items.size()));
            for (int i = 0; i < take; ++i) {
                sum += items[i].first;
            }
            for (int i = 0; i < take; ++i) {
                rep_joints[static_cast<size_t>(r)][i] = items[i].second;
                rep_weights[static_cast<size_t>(r)][i] =
                    (sum > 0.0) ? static_cast<float>(items[i].first / sum) : 0.0f;
            }
        }
        for (size_t vi = 0; vi < vcount; ++vi) {
            cloth->joint_indices[vi] = rep_joints[static_cast<size_t>(rep[vi])];
            cloth->joint_weights[vi] = rep_weights[static_cast<size_t>(rep[vi])];
        }
    }

    cloth->flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(cloth->flags) | static_cast<uint8_t>(MeshVertexFlags::kJoints));
    return stats;
}

}  // namespace clothing
}  // namespace jpov

#endif  // JPOV_CLOTHING_WEIGHT_TRANSFER_H_
