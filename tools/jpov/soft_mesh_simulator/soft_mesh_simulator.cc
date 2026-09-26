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

    // 保存仿真点的**绑定姿态**快照（力的公式 pij(0) 用它；含虚拟顶点）。
    bind_positions_ = sim_positions_;

    // 构建关联邻居表 nb_list（DESIGN.md §2.1）——基于绑定姿态，一次性定死。
    BuildNeighborTable(bind_distance);

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
              << " / 关联对(有向) " << neighbor_pair_count()
              << "（bind_distance=" << bind_distance << " m；M3 重力+弹簧力场+阻尼）";
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

// 某点在某位置受到的总加速度 a(x) = (重力 + 弹簧力场) / m（DESIGN.md §1.2/§2）。
//
// 弹簧力（Danis 自创，§2.3）：对该点的每个关联 j，
//     Fij = -[pij(t) - pij(0)] * F / max(|pij(0)|, d/10)
// pij(t) = x_j(t) - x_i(t)（当前位移差）；pij(0) = 初始位移（隐含在 nb_init_dist_
// 里的是它的分量——因为 §2.2 定义 pij(0) = v_j(0) - v_i(0)，而我们缓存了坐标，
// 这里用绑定姿态坐标重算分量）。
//
// 注意：本函数用 sim_positions_（**当前**位置）读 x_j，而传入的 x 是 **调用方算好的
// 该点自己的**（可能是未回写的 x_new）——二者一致：调用方在第二趟里 x==sim_positions_[idx]。
// 之所以仍把 x 传进来，是为了让「a 依赖位置」这件事在签名上显式（将来若有单点扰动
// 测试可直接传任意位置）。
geom::Vec3<float> Simulator::ComputeAccel(size_t idx,
                                          const geom::Vec3<float>& x) const {
    // a = F_total / m（质量只在末尾除；力公式本身不含质量）。
    const float m = point_mass();
    // m 应为正（Init/SetTotalMass 已保证 M_total >= kMinTotalMass 且 N >= 1）。
    CHECK_GT(m, 0.0f) << "point_mass 必须 > 0（仿真点为空或 M_total 非法）";

    // 重力：作为**力**参与总力（F_grav = m·g），最后统一除 m → 加速度恰为 (0,-g,0)。
    // （本阶段 m 对所有点相同，故重力对加速度的贡献与 m 无关；写成力是为了
    //   将来若引入逐点质量时能自然融合，也保证单位一致：F_total 是力、除以 m 得 a。）
    geom::Vec3<float> f_total(0.0f, -gravity_ * m, 0.0f);

    // 弹簧力部分：F_i = Σ_j Fij。无关联点时该项为零（孤点只受重力）。
    // 力场总开关关闭时整段跳过（与 M2 的“只重力”行为一致）。
    if (spring_enabled_) {
        const std::vector<uint32_t>& nbrs = neighbors_[idx];
        const std::vector<float>& init_dists = nb_init_dist_[idx];
        // 分母 clamp：max(|pij(0)|, d/10)（§2.3）——统一取常量，避免逐对比较。
        const float dist_floor = bind_distance_ * 0.1f;
        // 力系数 F 在进入循环前备好。
        const float F = force_coeff_;

        for (size_t k = 0; k < nbrs.size(); ++k) {
            const size_t j = nbrs[k];
            // pij(0) 分量 = 绑定姿态下 v_j(0) - v_i(0)（方向敏感，§2.3）。
            const geom::Vec3<float> p0 =
                bind_positions_[j] - bind_positions_[idx];
            // pij(t) = 当前 v_j(t) - v_i(t)。
            const geom::Vec3<float> px = sim_positions_[j] - x;
            // Δp = pij(t) - pij(0)（位移变化量，矢量 ⇒ 隐含抗扭转，§2.4）。
            const geom::Vec3<float> dp = px - p0;
            // 分母 = max(|pij(0)|, d/10)。
            const float denom = std::max(init_dists[k], dist_floor);
            // Fij = +Δp * F / denom（逐分量）。
            // ⚠️ 正号（非 DESIGN §2.3 字面的负号）：按 pij = v_j - v_i 的定义，
            //   负号会给出**反恢复（排斥）**力、导致发散；详见 DESIGN §2.3 的符号修正说明。
            geom::Vec3<float> fij(dp[0] * F / denom,
                                  dp[1] * F / denom,
                                  dp[2] * F / denom);
            // 逐分量 clamp 到 ±F_max（§2.5）。
            for (int c = 0; c < 3; ++c) {
                fij[c] = std::clamp(fij[c], -kForceMax, kForceMax);
            }
            f_total = f_total + fij;
        }
    }

    // a = F_total / m（质量只在末尾除；力公式本身不含质量）。
    return f_total * (1.0f / m);
}

// 「重力 + 对称阻尼」的 leapfrog 积分（DESIGN.md §3.3 定稿，Danis 签字）。
//
// 这是 **KDK（kick-drift-kick）** 形式的 velocity-Verlet / leapfrog：
//
//     v_half  = v(t) * exp(-k*dt/2) + a(t)   * dt/2     ← 前半 kick，用 a(t)=a(x(t))
//     x(t+dt) = x(t) + v_half * dt                        ← drift
//     a(t+dt) = a(x(t+dt))                                ← ★用【新位置】重新求力
//     v(t+dt) = (v_half + a(t+dt) * dt/2) * exp(-k*dt/2)  ← 后半 kick，用 a(t+dt)
//
// ★ 结构要点（Danis 2026-09-26 指出）：**两个 kick 用的是各自时刻的加速度**。
//   前半 kick 用 a(t) = a(x_old)，后半 kick 用 a(t+dt) = a(x_new)。
//   现力场已含位置相关弹簧力（§2），故两者**取值不同**，“用新位置求第二个 a”
//   是辛性的必要条件——若沿用旧 a，积分器会退化为非辛，能量/稳定性性质尽失。
//
//   因为 a(t+dt) 依赖 x_new，而每点的 x_new 又依赖各自的 v_half，且弹簧力是
//   **点与点之间的耦合量**，故必须分两趟：
//     【第一趟】对全部点算 v_half 与 x_new（此刻 a(t) 已知）；
//     【第二趟】全部点都有 x_new 后，逐点求 a(x_new) 并回写 v_new + 地面投影。
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

    const size_t n = sim_positions_.size();
    const float k = velocity_damping_;
    // 半步阻尼因子 exp(-k*dt_sub/2)；用 double 中间量算，避免 float 精度损失。
    const float damp_half = static_cast<float>(
        std::exp(-static_cast<double>(k) * dt_sub * 0.5));
    const float half_dt = static_cast<float>(dt_sub) * 0.5f;
    const float dt_sub_f = static_cast<float>(dt_sub);

    // 复用成员级临时缓冲（同长，避免每子步分配；见 .h 的 v_half_buf_ 说明）。
    v_half_buf_.resize(n);

    // ── 第一趟：前半 kick（用 a(x_old)）→ 得到全部点的 v_half。──
    // ⚠️ 关键：本趟**只读** sim_positions_、**只写** v_half_buf_，绝不就地改位置。
    //   否则（若边算边写位置）后算的点会读到**已推进**的邻居位置——即“半新半旧”，
    //   使结果依赖遍历顺序，破坏辛性/对称性。弹簧力是点间耦合量，此处尤须注意。
    for (size_t i = 0; i < n; ++i) {
        const geom::Vec3<float> x_old = sim_positions_[i];
        const geom::Vec3<float> a_old = ComputeAccel(i, x_old);
        // v_half = v(t)*exp(-k*dt/2) + a(x_old)*dt/2
        v_half_buf_[i] = sim_velocities_[i] * damp_half + a_old * half_dt;
    }

    // ── 第一趟（drift）：x(t+dt) = x(t) + v_half*dt。──
    // 此时全部 v_half 已算好（都基于旧的 x），可以安全地批量推进位置。
    for (size_t i = 0; i < n; ++i) {
        sim_positions_[i] = sim_positions_[i] + v_half_buf_[i] * dt_sub_f;
    }

    // ── 第二趟：全部点都已有 x_new 后，求 a(x_new) 并回写 v_new + 地面投影。──
    // ★ 关键：弹簧力是点间耦合量，ComputeAccel 会读**全部点**的当前位置；
    //   故必须等第一趟把**所有**点都推进到 x_new 后才能跑这一趟（不能合并）。
    for (size_t i = 0; i < n; ++i) {
        geom::Vec3<float> x_new = sim_positions_[i];
        // a(t+dt) = a(x_new)：用新位置求第二个加速度（含弹簧力，§2）。
        const geom::Vec3<float> a_new = ComputeAccel(i, x_new);
        // v(t+dt) = (v_half + a(x_new)*dt/2) * exp(-k*dt/2)
        geom::Vec3<float> v_new =
            (v_half_buf_[i] + a_new * half_dt) * damp_half;

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

void Simulator::BuildNeighborTable(float d) {
    CHECK_GT(d, 0.0f);

    const size_t n = sim_positions_.size();
    const float d_sq = d * d;

    neighbors_.assign(n, {});
    nb_init_dist_.assign(n, {});

    // O(N²) 暴力两两比较（DESIGN.md §3.4 “暴力解”）。
    // 先无差别收集每个点在 d 内的**所有**关联（含距离），最后再按最近 kMaxNeighbors
    // 截断（每一头独立截断——即“每点最多 20 个最近邻”）。
    for (size_t i = 0; i < n; ++i) {
        const geom::Vec3<float>& pi = sim_positions_[i];
        for (size_t j = i + 1; j < n; ++j) {
            const geom::Vec3<float>& pj = sim_positions_[j];
            const float dx = pj[0] - pi[0];
            const float dy = pj[1] - pi[1];
            const float dz = pj[2] - pi[2];
            const float dist_sq = dx * dx + dy * dy + dz * dz;
            // 严格 <= d （§2.1）；含等于边界。
            if (dist_sq > d_sq) {
                continue;
            }
            const float dist = std::sqrt(dist_sq);
            neighbors_[i].push_back(static_cast<uint32_t>(j));
            nb_init_dist_[i].push_back(dist);
            neighbors_[j].push_back(static_cast<uint32_t>(i));
            nb_init_dist_[j].push_back(dist);
        }
    }

    // ── 邻居数上限：每点只保留最近的 kMaxNeighbors 个（性能关键）。──
    //
    // 动机（2026-09-26 Danis 实测）：d=0.1 对点距 ~0.01 的密模，平均邻居数可达 300+，
    // 每个子步的力循环要跑 O(Σ_neighbors) 次随机访存 → 60fps 下亿级/帧，卡顿。
    //
    // ⚠️⚠️ **必须保持对称（无向）**：力遵循牛顿第三定律（F_ij = -F_ji），
    //   若逐点独立截断（i 保留 j 但 j 不保留 i）会破坏对称 ⇒ 系统净力非零 ⇒
    //   自发泵浦能量 ⇒ “弹着弹着就飞了”（2026-09-26 Danis 报，实测：1204 点
    //   方块无重力无地面下 KE 从 1.3 爆到 1.4e7）。
    //   ⇒ 用**无向边集**：只要 j 在 i 的最近 k 内**或** i 在 j 的最近 k 内，就保留
    //     这对关联（并集）——保证对称，每点邻居 ≤ 2k 左右。
    if (kMaxNeighbors > 0) {
        const size_t k = kMaxNeighbors;
        // 1) 每个点算出“最近 k 邻居”的**点 id 集合** topk[i]（O(n·k) 内存）。
        std::vector<std::vector<uint32_t>> topk(n);
        for (size_t i = 0; i < n; ++i) {
            auto& nbrs = neighbors_[i];
            auto& dists = nb_init_dist_[i];
            const size_t mi = nbrs.size();
            if (mi <= k) {
                topk[i] = nbrs;  // 不足 k，全部视为“在前 k 内”
                std::sort(topk[i].begin(), topk[i].end());
                continue;
            }
            std::vector<uint32_t> ord(mi);
            for (size_t t = 0; t < mi; ++t) ord[t] = static_cast<uint32_t>(t);
            std::nth_element(ord.begin(), ord.begin() + k, ord.end(),
                             [&dists](uint32_t a, uint32_t b) {
                                 return dists[a] < dists[b];
                             });
            topk[i].reserve(k);
            for (size_t t = 0; t < k; ++t) topk[i].push_back(nbrs[ord[t]]);
            std::sort(topk[i].begin(), topk[i].end());  // 便于二分查找
        }

        // 2) 无向并集：保留 (i,j) 当 j∈topk(i) 或 i∈topk(j)。
        auto in_topk = [&topk](size_t a, uint32_t b) {
            return std::binary_search(topk[a].begin(), topk[a].end(), b);
        };
        std::vector<std::vector<uint32_t>> new_nbrs(n);
        std::vector<std::vector<float>> new_dists(n);
        for (size_t i = 0; i < n; ++i) {
            for (size_t t = 0; t < neighbors_[i].size(); ++t) {
                const uint32_t j = neighbors_[i][t];
                if (in_topk(i, j) || in_topk(j, static_cast<uint32_t>(i))) {
                    new_nbrs[i].push_back(j);
                    new_dists[i].push_back(nb_init_dist_[i][t]);
                }
            }
        }
        neighbors_.swap(new_nbrs);
        nb_init_dist_.swap(new_dists);
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

void Simulator::SetTotalMass(float mass) {
    CHECK(std::isfinite(mass)) << "SetTotalMass 要求有限值，got " << mass;
    CHECK_GE(mass, kMinTotalMass)
        << "SetTotalMass 要求 mass >= " << kMinTotalMass << " kg（护栏，§3.1/§4.3），got "
        << mass;
    total_mass_ = mass;
}

void Simulator::SetForceCoeff(float f) {
    CHECK(std::isfinite(f)) << "SetForceCoeff 要求有限值，got " << f;
    CHECK_GT(f, 0.0f) << "SetForceCoeff 要求 F > 0（负 F 会变成反弹簧），got " << f;
    force_coeff_ = f;
}

void Simulator::SetVelocityDamping(float k) {
    CHECK(std::isfinite(k)) << "SetVelocityDamping 要求有限值，got " << k;
    CHECK_GE(k, 0.0f) << "SetVelocityDamping 要求 k >= 0（0 = 无阻尼），got " << k;
    velocity_damping_ = k;
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
    // 重建绑定姿态快照与关联表（d 未变，关联表内容相同，但一并重填保证一致）。
    bind_positions_ = sim_positions_;
    BuildNeighborTable(bind_distance_);
    sim_velocities_.assign(sim_positions_.size(),
                           geom::Vec3<float>(0.0f, 0.0f, 0.0f));
    time_ = 0.0;
    step_count_ = 0;

    LOG(INFO) << "soft_mesh_simulator::Reset 已回到绑定姿态（位置/速度/时间/步数清零）";
}

}  // namespace soft_mesh_simulator
}  // namespace jpov
