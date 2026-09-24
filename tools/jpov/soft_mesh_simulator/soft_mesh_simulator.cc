// JPOV 软体仿真器 — 实现（见 soft_mesh_simulator.h 的文件头）
//
// 纯 CPU / GL-free：本文件不 include 任何 GL 头，只碰 MeshData 与几何数学，
// 因此可以被单测直接跑（soft_mesh_simulator_test.cc）。

#include "tools/jpov/soft_mesh_simulator/soft_mesh_simulator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

#include <glog/logging.h>

namespace jpov {
namespace soft_mesh_simulator {

void Simulator::Init(const jpov::MeshData& mesh) {
    Init(mesh, kDefaultBindDistance);
}

void Simulator::Init(const jpov::MeshData& mesh, float bind_distance) {
    mesh.Validate();  // 长度/属性一致性：非法输入在这里就崩，不带进物理
    CHECK_GT(bind_distance, 0.0f)
        << "Simulator::Init 要求 bind_distance > 0，got " << bind_distance;

    // 绑定姿态 = 输入网格的副本；当前网格也从绑定姿态起步。
    bind_mesh_ = mesh;
    mesh_ = mesh;

    vertex_count_ = mesh_.positions.size();
    // 无索引网格按「每 3 个顶点一个三角形」计（与渲染的 triangle list 语义一致）。
    triangle_count_ = mesh_.indices.empty() ? (vertex_count_ / 3)
                                            : (mesh_.indices.size() / 3);

    bind_distance_ = bind_distance;

    // 构建仿真点集合（含长边加密）。
    const size_t virtual_count = BuildSimulationPoints(mesh, bind_distance);

    time_ = 0.0;
    step_count_ = 0;
    inited_ = true;

    LOG(INFO) << "simulator::Init 原始顶点 " << vertex_count_
              << " / 三角形 " << triangle_count_
              << " / 虚拟顶点 " << virtual_count
              << " / 仿真点合计 " << sim_positions_.size()
              << "（bind_distance=" << bind_distance << " m；M0 阶段 Step 为恒等）";
}

jpov::MeshData Simulator::Step(double dt) {
    CHECK(inited_) << "soft_mesh_simulator::Step 调用前必须先 Init(mesh)";
    CHECK(dt > 0.0) << "soft_mesh_simulator::Step 要求 dt > 0，got " << dt
                    << "（静默跳过时间会掩盖时钟 bug，故直接崩溃）";

    // ── M0（静态阶段）：恒等桩。──
    //
    // 这里**故意什么都不做**：不做位置积分、不做约束投影。它的价值是先把
    // 「输入 mesh → 输出 mesh」这条接口钉死，让查看器/单测/headless 全部围绕
    // 它建好骨架。后续每加一条物理，都是替换注释下面这几行，接口不变。
    //
    // 下一步（M1）将在此加入：重力预测 → 约束投影 → 速度回写；
    // 届时需要本类额外持有：顶点速度数组、约束集（边/面）、拓扑邻接。
    //
    // 当前恒等映射下 mesh_ 不需要变，直接返回当前值即可（零拷贝路径）。

    // 时间与步数照常累加——即使动力学是恒等的，时钟也必须真实推进，
    // 否则查看器/测试无法区分"停住了"和"没在走"。
    time_ += dt;
    ++step_count_;

    return mesh_;
}

SimBounds Simulator::Bounds() const {
    SimBounds b;
    if (mesh_.positions.empty()) return b;  // valid = false

    constexpr float kBig = std::numeric_limits<float>::max();
    geom::Vec3<float> lo(kBig, kBig, kBig);
    geom::Vec3<float> hi(-kBig, -kBig, -kBig);
    for (const geom::Vec3<float>& p : mesh_.positions) {
        lo[0] = std::min(lo[0], p[0]);
        lo[1] = std::min(lo[1], p[1]);
        lo[2] = std::min(lo[2], p[2]);
        hi[0] = std::max(hi[0], p[0]);
        hi[1] = std::max(hi[1], p[1]);
        hi[2] = std::max(hi[2], p[2]);
    }
    b.min = lo;
    b.max = hi;
    b.valid = true;
    return b;
}

size_t Simulator::BuildSimulationPoints(const jpov::MeshData& mesh, float d) {
    sim_positions_.clear();
    virtual_positions_.clear();

    // 1) 原始顶点先入队（保持输入顺序；索引 0..N-1）。
    sim_positions_.reserve(mesh.positions.size());
    for (const geom::Vec3<float>& p : mesh.positions) {
        sim_positions_.push_back(p);
    }

    // 2) 长边加密：对每条 "边长 > d*0.9" 的边，中间等距插入虚拟顶点。
    //
    // 只处理**索引化** mesh 的三角形边。无索引网格按 triangle list 语义
    // （每 3 个连续顶点一个三角形）提取边——与渲染一致。
    //
    // 去重：同一条边（i,j）会被相邻三角形各遍历一次；用 (min,max) 归一化后
    // 查 hash 集合去重，避免同一条边插入两遍虚拟点。
    const float max_len = d * 0.9f;
    const float max_len_sq = max_len * max_len;
    CHECK_GT(max_len, 0.0f);

    std::unordered_set<uint64_t> seen_edges;
    const size_t vcount = mesh.positions.size();

    auto process_edge = [&](uint32_t ia, uint32_t ib) {
        CHECK_LT(ia, vcount);
        CHECK_LT(ib, vcount);
        if (ia == ib) return;  // 退化边忽略
        const uint32_t lo = std::min(ia, ib);
        const uint32_t hi = std::max(ia, ib);
        const uint64_t key = (static_cast<uint64_t>(lo) << 32) | hi;
        if (!seen_edges.insert(key).second) return;  // 已处理过

        const geom::Vec3<float>& pa = mesh.positions[ia];
        const geom::Vec3<float>& pb = mesh.positions[ib];
        const geom::Vec3<float> delta = pb - pa;
        const float len_sq = delta[0] * delta[0] + delta[1] * delta[1] +
                             delta[2] * delta[2];
        if (len_sq <= max_len_sq) return;  // 不过长，不加密

        const float len = std::sqrt(len_sq);
        // 目标：插入 n 个点，把这条边切成 (n+1) 段，每段 <= max_len。
        //   n = ceil(len / max_len) - 1
        const int segments = static_cast<int>(std::ceil(len / max_len));
        for (int s = 1; s < segments; ++s) {  // s = 1..segments-1（不含两端）
            const float t = static_cast<float>(s) / static_cast<float>(segments);
            const geom::Vec3<float> vp = pa + delta * t;
            virtual_positions_.push_back(vp);
            sim_positions_.push_back(vp);
        }
    };

    if (mesh.indices.empty()) {
        const size_t tri_count = vcount / 3;
        for (size_t t = 0; t < tri_count; ++t) {
            const uint32_t a = 3u * static_cast<uint32_t>(t) + 0u;
            const uint32_t b = 3u * static_cast<uint32_t>(t) + 1u;
            const uint32_t c = 3u * static_cast<uint32_t>(t) + 2u;
            process_edge(a, b);
            process_edge(b, c);
            process_edge(c, a);
        }
    } else {
        const size_t tri_count = mesh.indices.size() / 3;
        for (size_t t = 0; t < tri_count; ++t) {
            const uint32_t a = mesh.indices[3 * t + 0];
            const uint32_t b = mesh.indices[3 * t + 1];
            const uint32_t c = mesh.indices[3 * t + 2];
            process_edge(a, b);
            process_edge(b, c);
            process_edge(c, a);
        }
    }

    return virtual_positions_.size();
}

void Simulator::Reset() {
    if (!inited_) return;  // 未初始化过：no-op（幂等，便于查看器无脑调用）

    mesh_ = bind_mesh_;
    time_ = 0.0;
    step_count_ = 0;

    LOG(INFO) << "soft_mesh_simulator::Reset 已回到绑定姿态（时间/步数清零）";
}

}  // namespace soft_mesh_simulator
}  // namespace jpov
