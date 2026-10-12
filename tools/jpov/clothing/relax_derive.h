// JPOV 穿衣工具 — 由「固定点」自动推导布料 LAG 松弛度（pure header-only，GL-free，可单测）。
//
// 背景（见 docs/jpov_instance_attr_lag_design.md §4.4 / §6.1）：
//   布料 LAG 副运动的「逐顶点松弛度」(lag_ratio ∈ [0,1]) 存在**顶点属性 loc6**（MeshData
//   的 kRelax 属性 relaxations.x）。0 = 顶点完全跟随当前 pose（绷在身体上、不摆动），
//   1 = 顶点完全滞后（自由摆动）。设计 §4.4 要求这个场**低频平滑**（别一个顶点 0、隔壁 1）。
//
// 本模块做「一根手指的活」：用户在衣物上标定若干**固定点**（缝死在身体上的点，如腰口；
//   松弛度 = 0），工具据此**自动推导**其余顶点的松弛度 —— 用「**测地距离**（沿网格表面的
//   最短路）」从固定点向外生长：离固定点越远，松弛度越大，直到饱和到 1。这样：
//     - 腰口（固定点）= 0，下摆（最远）= 1，中间**自然过渡**（不是硬切）；
//     - 距离场经**若干轮** Laplacian 平滑（默认 20 轮，见 clothing_tool_config.h 的
//       kRelaxDeriveSmoothPasses）去掉「测地脊线」的高频拐角，满足 §4.4 的低频要求。
//
// 为什么用测地距离（而非欧氏距离 / 竖直高度）：衣物是**曲面**（裙摆会绕、会叠），欧氏
//   距离会让「隔着一层布」的点互相影响；测地距离沿表面走，符合「布料从缝死处往下垂」的
//   物理直觉，且与模型朝向无关。为什么先焊接：导出器在 UV / 材质缝合处把同一位置拆成多个
//   顶点，不焊接的话松弛度会在缝两侧**跳变**（缝一侧 0、另一侧很大），驱动 LAG 时会撕裂。
//
// 依赖：weight_transfer.h 的 WeldVerticesByPosition（已有的公共焊接工具）+ mesh.h。
// 不碰 GL / glTF / 仿真器，便于纯单测。

#ifndef JPOV_CLOTHING_RELAX_DERIVE_H_
#define JPOV_CLOTHING_RELAX_DERIVE_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/clothing/weight_transfer.h"  // WeldVerticesByPosition
#include "tools/jpov/interface/mesh.h"

namespace jpov {
namespace clothing {

// 一次「由固定点推导松弛度」的统计（供面板回显 / 单测断言）。
struct RelaxDeriveStats {
    size_t vertex_count = 0;      // 处理的顶点数
    size_t anchor_count = 0;      // **焊接后去重**的固定点（组）数
    size_t reachable_count = 0;   // 与固定点连通（有有限测地距离）的顶点组数
    size_t unreachable_count = 0; // 不与任何固定点连通的顶点组数（置 1.0 = 全自由）
    float max_geodesic_m = 0.0f;  // 固定点到最远可达顶点的测地距离（米；0 = 全在固定点上）
    size_t smoothing_passes = 0;  // 实际执行的 Laplacian 平滑轮数
};

// 由「固定点」自动推导布料的逐顶点松弛度 lag_ratio ∈ [0,1]。
//
// 输入：
//   mesh              : 衣物网格（读 positions + indices；indices 为空则按每 3 顶点一组三角形）。
//   anchors           : 固定点顶点 index（松弛度恒为 0；**必须非空**，越界 → CHECK 崩溃）。
//   weld_tolerance_m  : 先按「位置相距 ≤ 该值」焊接重复顶点（缝合缝两侧当同一点；0 = 不焊）。
//   relax_radius_m    : 松弛度饱和（升到 1）的测地距离（米）；<= 0 = 用「到最远顶点的测地距离」
//                       自动定标（最远点恰为 1）。
//   smoothing_passes  : Laplacian 平滑迭代次数（固定点冻结在 0）；0 = 不平滑。
//   stats             : 可选输出统计（可为 nullptr）。
//
// 返回：长度 == mesh.positions.size() 的数组，逐顶点松弛度 ∈ [0,1]。
//   - 固定点所在组恒为 0；
//   - 与固定点同一连通分量 → 按测地距离 / 定标 归一（可 > 0 至 1）；
//   - 不与任何固定点连通的分量 → 1.0（没有东西约束它 ⇒ 全自由）。
//
// 确定性：Dijkstra + 固定轮数 Jacobi 平滑，同输入同输出。
//
// Pre-condition: mesh.positions 非空；anchors 非空且每个 index < positions.size()；
//                weld_tolerance_m >= 0；smoothing_passes >= 0。
std::vector<float> DeriveLagRatioFromAnchors(
    const MeshData& mesh,
    const std::vector<uint32_t>& anchors,
    float weld_tolerance_m,
    float relax_radius_m,
    int smoothing_passes,
    RelaxDeriveStats* stats /*output*/);

// 把逐顶点松弛度注入 mesh 的布料 LAG 属性：relaxations[v] = (lag_ratio[v], max_lag)，
// 并置 kRelax flag（保存 glb 时写出 _JPOV_RELAX；见 gltf_saver.cc）。
//
// 返回 false（**不动** mesh）：lag_ratio.size() != mesh.positions.size() —— 视为「推导结果
//   已过期」（如补洞改了顶点数），由调用方决定重新推导。
//   lag_ratio 元素会被 clamp 到 [0,1]。
// Pre-condition: mesh != nullptr；max_lag >= 0。
bool ApplyRelaxToMesh(const std::vector<float>& lag_ratio, float max_lag,
                      MeshData* mesh /*inout*/);

// ==================== 实现 ====================

inline std::vector<float> DeriveLagRatioFromAnchors(
    const MeshData& mesh,
    const std::vector<uint32_t>& anchors,
    float weld_tolerance_m,
    float relax_radius_m,
    int smoothing_passes,
    RelaxDeriveStats* stats /*output*/) {
    const size_t vcount = mesh.positions.size();
    CHECK_GT(vcount, 0u) << "DeriveLagRatioFromAnchors: 顶点为空";
    CHECK(!anchors.empty()) << "DeriveLagRatioFromAnchors: 固定点为空";
    CHECK_GE(weld_tolerance_m, 0.0f);
    CHECK_GE(smoothing_passes, 0);

    // 1) 焊接重复顶点（缝合缝两侧当同一点）→ 每个顶点的「组代表」（组内最小下标），
    //    再压成稠密编号 0..rep_count-1，代表位置 = 组内最小下标顶点的位置。
    const std::vector<int> rep_of =
        WeldVerticesByPosition(mesh.positions, weld_tolerance_m);
    std::vector<int> dense(vcount, -1);
    int rep_count = 0;
    for (size_t i = 0; i < vcount; ++i) {
        const int root = rep_of[i];
        CHECK_GE(root, 0);
        CHECK_LT(root, static_cast<int>(vcount));
        if (dense[static_cast<size_t>(root)] < 0) {
            dense[static_cast<size_t>(root)] = rep_count++;
        }
    }
    std::vector<int> vrep(vcount);
    for (size_t i = 0; i < vcount; ++i) {
        vrep[i] = dense[static_cast<size_t>(rep_of[i])];
    }
    std::vector<Vec3f> rep_pos(static_cast<size_t>(rep_count));
    for (size_t i = 0; i < vcount; ++i) {
        if (rep_of[i] == static_cast<int>(i)) {  // i 是组内最小下标 = 代表
            rep_pos[static_cast<size_t>(dense[i])] = mesh.positions[i];
        }
    }

    // 2) 代表相邻接表（无向；去重）。邻接只记拓扑，边权按代表位置现算（欧氏距离）。
    std::vector<std::vector<int>> nbr(static_cast<size_t>(rep_count));
    const auto add_edge = [&](uint32_t a, uint32_t b) {
        CHECK_LT(a, vcount);
        CHECK_LT(b, vcount);
        const int ra = vrep[a];
        const int rb = vrep[b];
        if (ra == rb) {
            return;  // 同组（缝合）内部不连边
        }
        nbr[static_cast<size_t>(ra)].push_back(rb);
        nbr[static_cast<size_t>(rb)].push_back(ra);
    };
    if (!mesh.indices.empty()) {
        CHECK_EQ(mesh.indices.size() % 3, 0u)
            << "DeriveLagRatioFromAnchors: indices 长度不是 3 的倍数";
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            add_edge(mesh.indices[i], mesh.indices[i + 1]);
            add_edge(mesh.indices[i + 1], mesh.indices[i + 2]);
            add_edge(mesh.indices[i + 2], mesh.indices[i]);
        }
    } else {
        CHECK_EQ(vcount % 3, 0u)
            << "DeriveLagRatioFromAnchors: 非索引网格顶点数不是 3 的倍数";
        for (size_t i = 0; i + 2 < vcount; i += 3) {
            add_edge(static_cast<uint32_t>(i), static_cast<uint32_t>(i + 1));
            add_edge(static_cast<uint32_t>(i + 1), static_cast<uint32_t>(i + 2));
            add_edge(static_cast<uint32_t>(i + 2), static_cast<uint32_t>(i));
        }
    }
    for (std::vector<int>& nb : nbr) {
        std::sort(nb.begin(), nb.end());
        nb.erase(std::unique(nb.begin(), nb.end()), nb.end());
    }

    // 3) 多源 Dijkstra：固定点为源（距离 0），沿代表图求测地距离。
    const double kInf = std::numeric_limits<double>::infinity();
    std::vector<double> dist(static_cast<size_t>(rep_count), kInf);
    std::vector<char> is_anchor(static_cast<size_t>(rep_count), 0);
    using QueueItem = std::pair<double, int>;  // (距离, 代表)
    std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> pq;
    size_t anchor_count = 0;
    for (uint32_t a : anchors) {
        CHECK_LT(a, vcount) << "DeriveLagRatioFromAnchors: 固定点 index 越界";
        const int r = vrep[a];
        if (is_anchor[static_cast<size_t>(r)] == 0) {
            is_anchor[static_cast<size_t>(r)] = 1;
            dist[static_cast<size_t>(r)] = 0.0;
            pq.push(QueueItem{0.0, r});
            ++anchor_count;
        }
    }
    while (!pq.empty()) {
        const QueueItem top = pq.top();
        pq.pop();
        const int u = top.second;
        if (top.first > dist[static_cast<size_t>(u)]) {
            continue;  // 过期条目
        }
        for (int v : nbr[static_cast<size_t>(u)]) {
            const double dx =
                static_cast<double>(rep_pos[static_cast<size_t>(u)].x()) -
                rep_pos[static_cast<size_t>(v)].x();
            const double dy =
                static_cast<double>(rep_pos[static_cast<size_t>(u)].y()) -
                rep_pos[static_cast<size_t>(v)].y();
            const double dz =
                static_cast<double>(rep_pos[static_cast<size_t>(u)].z()) -
                rep_pos[static_cast<size_t>(v)].z();
            const double w = std::sqrt(dx * dx + dy * dy + dz * dz);
            const double nd = dist[static_cast<size_t>(u)] + w;
            if (nd < dist[static_cast<size_t>(v)]) {
                dist[static_cast<size_t>(v)] = nd;
                pq.push(QueueItem{nd, v});
            }
        }
    }

    // 4) 归一：定标 = 显式半径（>0）或用「到最远顶点的测地距离」（最远 = 1）。
    double max_reach = 0.0;
    size_t reachable_count = 0;
    for (int r = 0; r < rep_count; ++r) {
        if (std::isfinite(dist[static_cast<size_t>(r)])) {
            ++reachable_count;
            max_reach = std::max(max_reach, dist[static_cast<size_t>(r)]);
        }
    }
    const double scale =
        (relax_radius_m > 0.0f) ? static_cast<double>(relax_radius_m) : max_reach;
    std::vector<float> relax(static_cast<size_t>(rep_count), 0.0f);
    for (int r = 0; r < rep_count; ++r) {
        const double d = dist[static_cast<size_t>(r)];
        if (!std::isfinite(d)) {
            relax[static_cast<size_t>(r)] = 1.0f;  // 与固定点不连通 → 全自由
            continue;
        }
        if (scale > 0.0) {
            relax[static_cast<size_t>(r)] =
                static_cast<float>(std::clamp(d / scale, 0.0, 1.0));
        } else {
            relax[static_cast<size_t>(r)] = 0.0f;  // 退化：所有顶点都在固定点上
        }
    }

    // 5) Laplacian 平滑（去掉测地距离的脊线拐角，满足 §4.4 的低频要求）。固定点冻结在 0；
    //    每轮 Jacobi 更新（读 relax 写 next），结果 clamp 回 [0,1]（贴固定点处可能被拉负）。
    size_t passes_done = 0;
    if (smoothing_passes > 0) {
        std::vector<float> next = relax;
        for (int it = 0; it < smoothing_passes; ++it) {
            for (int r = 0; r < rep_count; ++r) {
                if (is_anchor[static_cast<size_t>(r)] != 0) {
                    next[static_cast<size_t>(r)] = 0.0f;  // 固定点冻结
                    continue;
                }
                const std::vector<int>& nb = nbr[static_cast<size_t>(r)];
                if (nb.empty()) {
                    next[static_cast<size_t>(r)] = relax[static_cast<size_t>(r)];
                    continue;
                }
                double acc = static_cast<double>(relax[static_cast<size_t>(r)]);
                for (int v : nb) {
                    acc += static_cast<double>(relax[static_cast<size_t>(v)]);
                }
                next[static_cast<size_t>(r)] = static_cast<float>(
                    std::clamp(acc / (1.0 + static_cast<double>(nb.size())), 0.0, 1.0));
            }
            relax.swap(next);
            passes_done = static_cast<size_t>(it + 1);
        }
    }

    // 6) 散射回逐顶点（同组顶点得到同一松弛度 ⇒ 缝合缝两侧一致）。
    std::vector<float> out(vcount, 0.0f);
    for (size_t i = 0; i < vcount; ++i) {
        out[i] = relax[static_cast<size_t>(vrep[i])];
    }

    if (stats != nullptr) {
        stats->vertex_count = vcount;
        stats->anchor_count = anchor_count;
        stats->reachable_count = reachable_count;
        stats->unreachable_count = static_cast<size_t>(rep_count) - reachable_count;
        stats->max_geodesic_m = static_cast<float>(max_reach);
        stats->smoothing_passes = passes_done;
    }
    return out;
}

inline bool ApplyRelaxToMesh(const std::vector<float>& lag_ratio, float max_lag,
                             MeshData* mesh /*inout*/) {
    CHECK(mesh != nullptr);
    CHECK_GE(max_lag, 0.0f);
    if (lag_ratio.size() != mesh->positions.size()) {
        return false;  // 顶点数不匹配：推导结果已过期，不动 mesh
    }
    mesh->relaxations.resize(lag_ratio.size());
    for (size_t i = 0; i < lag_ratio.size(); ++i) {
        mesh->relaxations[i] = Vec2f(std::clamp(lag_ratio[i], 0.0f, 1.0f), max_lag);
    }
    mesh->flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(mesh->flags) |
        static_cast<uint8_t>(MeshVertexFlags::kRelax));
    return true;
}

}  // namespace clothing
}  // namespace jpov

#endif  // JPOV_CLOTHING_RELAX_DERIVE_H_
