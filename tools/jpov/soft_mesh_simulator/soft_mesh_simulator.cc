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

    // 速度数组与位置同长同序，初始全 0（绑定姿态静止）。
    sim_velocities_.assign(sim_positions_.size(),
                           geom::Vec3<float>(0.0f, 0.0f, 0.0f));

    time_ = 0.0;
    step_count_ = 0;
    inited_ = true;

    LOG(INFO) << "simulator::Init 原始顶点 " << vertex_count_
              << " / 三角形 " << triangle_count_
              << " / 虚拟顶点 " << virtual_count
              << " / 仿真点合计 " << sim_positions_.size()
              << "（bind_distance=" << bind_distance << " m；M2 重力+衰减）";
}

jpov::MeshData Simulator::Step(double dt) {
    CHECK(inited_) << "soft_mesh_simulator::Step 调用前必须先 Init(mesh)";
    CHECK(dt > 0.0) << "soft_mesh_simulator::Step 要求 dt > 0，got " << dt
                    << "（静默跳过时间会掩盖时钟 bug，故直接崩溃）";

    // ── 把外部步 dt 切成 kSubsteps 个子步逐步积分（DESIGN.md §3.4）。──
    // 子步是稳定性手段：dt_sub 越小，显式积分越稳（相应也会略微改变
    // 轨迹的离散化误差——1 步与 30 子步的结果相差 O(dt²) 量级，不是完全等价）。
    // 本阶段力只有常量重力，子步仅影响离散化精度；但为与将来接入的弹簧力场
    // 保持同一套结构，现在就切。
    const double dt_sub = dt / static_cast<double>(kSubsteps);
    for (int s = 0; s < kSubsteps; ++s) {
        IntegrateSubstep(dt_sub);
    }

    // 把仿真点位置写回 mesh_（只用原始顶点，虚拟顶点不输出）。
    ExtractMesh();

    // 时间与步数照常累加。
    time_ += dt;
    ++step_count_;

    return mesh_;
}

// 「重力 + 对称阻尼」的 leapfrog 积分（DESIGN.md §3.3 定稿，Danis 签字）。
//
// 逐点更新，点与点之间无耦合（本阶段力场未接入，故 a(t) = g 对每点相同）。
// 公式（逐顶点，m = 质量与力无关——本阶段只有重力，质量不影响加速度）：
//
//     v_half  = v(t) * exp(-k*dt/2) + a(t) * dt/2
//     x(t+dt) = x(t) + v_half * dt
//     a(t+dt) = g                        ← 本阶段唯一的外力
//     v(t+dt) = (v_half + a(t+dt) * dt/2) * exp(-k*dt/2)
//
// 要点：
//   * 阻尼 **对称地劈成两半**（前后各 exp(-k*dt/2)），包在 leapfrog 外侧；
//     两半相乘 = exp(-k*dt)，与「整步衰减一次」等价到 O(dt²)。
//   * 先算 v_half（半步速度），用它推位置（leapfrog 的“跳蛙”特征），
//     再用新位置的加速度回写整步速度——这是“辛”的体现：位置与速度的
//     更新互相嵌套，保证相空间体积守恒（无阻尼时）。
//   * 重力方向 -Y（地面在下方）。
//   * 每子步末做一次**地面投影**（非穿透）：低于地面 y 的顶点被拧回地面，
//     且向下的法向速度分量清零（无反弹，不注入动能）。
void Simulator::IntegrateSubstep(double dt_sub) {
    CHECK_GT(dt_sub, 0.0);

    const float k = kVelocityDamping;
    // 半步阻尼因子 exp(-k*dt_sub/2)；用 double 中间量算，避免 float 精度损失。
    const float damp_half = static_cast<float>(
        std::exp(-static_cast<double>(k) * dt_sub * 0.5));
    const float half_dt = static_cast<float>(dt_sub) * 0.5f;
    // 本阶段唯一外力：重力，方向 -Y。
    const geom::Vec3<float> accel(0.0f, -gravity_, 0.0f);

    for (size_t i = 0; i < sim_positions_.size(); ++i) {
        const geom::Vec3<float> v_old = sim_velocities_[i];
        const geom::Vec3<float> x_old = sim_positions_[i];

        // v_half = v(t)*exp(-k*dt/2) + a*dt/2
        const geom::Vec3<float> v_half = v_old * damp_half + accel * half_dt;
        // x(t+dt) = x(t) + v_half*dt
        geom::Vec3<float> x_new = x_old + v_half * static_cast<float>(dt_sub);
        // a(t+dt) = g（常量，与位置无关）；v(t+dt) = (v_half + a*dt/2)*exp(-k*dt/2)
        geom::Vec3<float> v_new = (v_half + accel * half_dt) * damp_half;

        // ── 地面投影（非穿透，DESIGN.md §1.2 机制 1 的平面简化版；M4）──
        // 「纯位置投影，不额外注入动能」：顶点落在地面下方 → 直接抬回地面。
        // 同时把向下的法向速度分量清零（消去侵入速度）——否则顶点虽被钉在
        // 地面，v.y 仍会持续累加到极大（以后接上弹簧力场就是个隐患），且
        // “不注入动能”意味着无反弹（恢复系数 0）。向上的速度分量不受影响
        // （允许被推离地面）。
        if (x_new[1] < ground_y_) {
            x_new[1] = ground_y_;
            if (v_new[1] < 0.0f) {
                v_new[1] = 0.0f;
            }
        }

        sim_positions_[i] = x_new;
        sim_velocities_[i] = v_new;
    }
}

// 提取变形 mesh：把仿真点前 vertex_count_ 个位置写回 mesh_.positions。
// 虚拟顶点参与仿真但不输出（DESIGN.md §3.2）。只改位置，拓扑/属性不动。
void Simulator::ExtractMesh() {
    CHECK_GE(sim_positions_.size(), vertex_count_);
    for (size_t i = 0; i < vertex_count_; ++i) {
        mesh_.positions[i] = sim_positions_[i];
    }
}

void Simulator::SetGravity(float gravity) {
    CHECK(std::isfinite(gravity)) << "SetGravity 要求有限值，got " << gravity;
    CHECK_GE(gravity, 0.0f) << "SetGravity 要求 gravity >= 0，got " << gravity;
    gravity_ = gravity;
}

void Simulator::SetGroundY(float ground_y) {
    CHECK(std::isfinite(ground_y)) << "SetGroundY 要求有限值，got " << ground_y;
    ground_y_ = ground_y;
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
    size_t virtual_count = 0;

    auto process_edge = [&](uint32_t ia, uint32_t ib) {
        CHECK_LT(ia, vcount);
        CHECK_LT(ib, vcount);
        if (ia == ib) {
            return;  // 退化边忽略
        }
        const uint32_t lo = std::min(ia, ib);
        const uint32_t hi = std::max(ia, ib);
        const uint64_t key = (static_cast<uint64_t>(lo) << 32) | hi;
        if (!seen_edges.insert(key).second) {
            return;  // 已处理过
        }

        const geom::Vec3<float>& pa = mesh.positions[ia];
        const geom::Vec3<float>& pb = mesh.positions[ib];
        const geom::Vec3<float> delta = pb - pa;
        const float len_sq = delta[0] * delta[0] + delta[1] * delta[1] +
                             delta[2] * delta[2];
        if (len_sq <= max_len_sq) {
            return;  // 不过长，不加密
        }

        const float len = std::sqrt(len_sq);
        // 目标：插入 n 个点，把这条边切成 (n+1) 段，每段 <= max_len。
        //   n = ceil(len / max_len) - 1
        const int segments = static_cast<int>(std::ceil(len / max_len));
        for (int s = 1; s < segments; ++s) {  // s = 1..segments-1（不含两端）
            const float t = static_cast<float>(s) / static_cast<float>(segments);
            sim_positions_.push_back(pa + delta * t);
            ++virtual_count;
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

    return virtual_count;
}

void Simulator::Reset() {
    if (!inited_) return;  // 未初始化过：no-op（幂等，便于查看器无脑调用）

    mesh_ = bind_mesh_;
    // 位置回到绑定姿态，速度清零（重新从静止开始）。
    // 注意：BuildSimulationPoints 会重填 sim_positions_（原始顶点 + 虚拟点），
    // 其前 vertex_count_ 个即 bind_mesh_ 的原顶点位置，故无需另行拷贝。
    BuildSimulationPoints(bind_mesh_, bind_distance_);
    sim_velocities_.assign(sim_positions_.size(),
                           geom::Vec3<float>(0.0f, 0.0f, 0.0f));
    time_ = 0.0;
    step_count_ = 0;

    LOG(INFO) << "soft_mesh_simulator::Reset 已回到绑定姿态（位置/速度/时间/步数清零）";
}

}  // namespace soft_mesh_simulator
}  // namespace jpov
