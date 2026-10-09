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

#include "geom/common/quaternion.h"

namespace jpov {
namespace soft_mesh_simulator {

namespace {

// 旋转轴的单位方向（见 Axis）。非法值 LOG(FATAL)。
geom::Vec3<float> AxisUnit(Axis axis) {
    switch (axis) {
        case Axis::kX:
            return geom::Vec3<float>(1.0f, 0.0f, 0.0f);
        case Axis::kY:
            return geom::Vec3<float>(0.0f, 1.0f, 0.0f);
        case Axis::kZ:
            return geom::Vec3<float>(0.0f, 0.0f, 1.0f);
    }
    LOG(FATAL) << "AxisUnit: 非法 axis " << static_cast<int>(axis);
    return geom::Vec3<float>(0.0f, 0.0f, 0.0f);  // 不可达（LOG(FATAL) 已终止）
}

// 度 → 弧度。
constexpr float kDegToRad =
    static_cast<float>(3.14159265358979323846 / 180.0);

}  // namespace

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

    // 构建仿真点集合（含长边加密）。返回值（长边加密插入数）不单独用：桥接修复后
    // 虚拟点总数在下面日志里按 sim_positions_.size() - vertex_count_ 统计（含桥接）。
    BuildSimulationPoints(mesh, bind_distance);

    // 保存仿真点的**绑定姿态**快照（力的公式 pij(0) 用它；含虚拟顶点）。
    bind_positions_ = sim_positions_;

    // 构建关联邻居表 nb_list（DESIGN.md §2.1）——基于绑定姿态，一次性定死。
    BuildNeighborTable(bind_distance);

    // 连通性修复：保证关联图是**单一连通网**（否则子网会因无弹簧、只受重力而单独坠落）。
    // 追加桥接虚拟点 —— 必须放在分配速度/缓存缓冲之前。
    EnsureNeighborGraphConnected(bind_distance);

    // 启动几何快照（供 Reset 用）。此时 sim_positions_ 已含长边虚拟点 + 桥接点；
    // bind_positions_ 也已同步追加过桥接点，故两者一致。Apply* 不会动 startup_positions_。
    startup_positions_ = sim_positions_;

    // 速度数组与位置同长同序，初始全 0（绑定姿态静止）。
    sim_velocities_.assign(sim_positions_.size(),
                           geom::Vec3<float>(0.0f, 0.0f, 0.0f));
    body_outward_buf_.assign(sim_positions_.size(),
                             geom::Vec3<float>(0.0f, 0.0f, 0.0f));

    time_ = 0.0;
    step_count_ = 0;
    inited_ = true;

    LOG(INFO) << "simulator::Init 原始顶点 " << vertex_count_
              << " / 三角形 " << triangle_count_
              << " / 虚拟顶点 " << (sim_positions_.size() - vertex_count_)
              << "（含桥接） / 仿真点合计 " << sim_positions_.size()
              << " / 关联对(有向) " << neighbor_pair_count()
              << " / 连通分量 " << neighbor_component_count()
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
//     Fij = +[pij(t) - pij(0)] * F / max(|pij(0)|, d/10)     （恢复力，符号见下）
// pij(t) = x_j(t) - x_i(t)（当前位移差）；pij(0) = 初始位移（用绑定姿态坐标重算分量）。
// ⚠️ 正号（非 DESIGN §2.3 字面的负号）：按 pij = v_j - v_i 定义，负号会给出反恢复力，
//   导致发散；详见下方实现处注释与 DESIGN §2.3 的符号修正说明。
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
    body_outward_buf_.resize(n);

    // ── 第一趟：前半 kick（用 a(x_old)）→ 得到全部点的 v_half。──
    // ⚠️ 关键：本趟**只读** sim_positions_、**只写** v_half_buf_，绝不就地改位置。
    //   否则（若边算边写位置）后算的点会读到**已推进**的邻居位置——即“半新半旧”，
    //   使结果依赖遍历顺序，破坏辛性/对称性。弹簧力是点间耦合量，此处尤须注意。
    for (size_t i = 0; i < n; ++i) {
        const geom::Vec3<float> x_old = sim_positions_[i];
        const geom::Vec3<float> a_old = ComputeAccel(i, x_old);
        // v_half = v(t)*exp(-k*dt/2) + a(x_old)*dt/2
        geom::Vec3<float> v_half = sim_velocities_[i] * damp_half + a_old * half_dt;

        // ── 人体接触的**预速度约束**（关键，见 .h 的 body_outward_buf_ 说明）──
        // 用**上一子步**记下的接触外方向，把 v_half 里指向体内的分量去掉。
        // 这样下面的 drift 就**不会**把接触点往体内飘，从而消除“硬位置投影×硬弹簧
        // ⇒ 每子步位置锯齿（阻尼无效）”的持续抖动（初始那一子步仍靠第二趟兼底）。
        const geom::Vec3<float>& own = body_outward_buf_[i];
        if (own.Sqr() > 0.0f) {
            const float vn = v_half[0] * own[0] + v_half[1] * own[1] +
                             v_half[2] * own[2];
            if (vn < 0.0f) {
                v_half = geom::Vec3<float>(v_half[0] - own[0] * vn,
                                           v_half[1] - own[1] * vn,
                                           v_half[2] - own[2] * vn);
            }
        }
        v_half_buf_[i] = v_half;
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
        // 本子步 drift **前**的位置 x(t) = x_new − v_half·dt（drift 只为每点自身加
        // v_half·dt）。人体排斥二分求“本子步新穿入”的逃逸点需要它；须在下面地面投影/
        // 排斥改动 x_new 之前先算好（见 ApplyBodyRepulsion）。
        const geom::Vec3<float> x_pre = x_new - v_half_buf_[i] * dt_sub_f;
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

        // ── 人体排斥（可选；新逃逸算法 + 速度投影；刷接触缓存）──
        ApplyBodyRepulsion(i, x_pre, &x_new, &v_new);

        sim_positions_[i] = x_new;
        sim_velocities_[i] = v_new;
    }

    // 全局速度上限（兑底，默认关）。
    ClampMaxSpeed();
}

// 全局速度上限：把每个点的速度矢量的模长截到 max_speed_（> 0 时）。放在子步末（地面
// 投影之后）作为最后一道兑底：低质量 + 大刚度时，单个子步内弹簧力会把速度推到极大
// （a = F/m，m 小），显式积分随即发散（顶点被甩飞、拉出“淌”状长条）。限速把状态钉在
// 一个有界范围内，避免发散。
// 注：这是**兑底**而非根治（根治需更小 dt_sub / 隐式积分）；默认关闭（0）。
void Simulator::ClampMaxSpeed() {
    if (max_speed_ <= 0.0f) {
        return;
    }
    const float v_max_sq = max_speed_ * max_speed_;
    for (geom::Vec3<float>& v : sim_velocities_) {
        const float sp_sq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        if (sp_sq > v_max_sq) {
            const float scale = max_speed_ / std::sqrt(sp_sq);
            v = v * scale;
        }
    }
}

// ── 人体最近三角形查询 ──
Simulator::BodyHit Simulator::QueryBody(const geom::Vec3<double>& p) const {
    BodyHit hit;
    if (body_matcher_ == nullptr) {
        return hit;  // 无匹配器 = 未命中
    }
    const std::vector<int>& candidates = body_matcher_->FindNearestTriangles(p);
    if (candidates.empty()) {
        return hit;  // 逃出体表查询半径（未命中）
    }
    const std::vector<geom::Triangle3<double>>& tris = body_matcher_->triangles();
    // 在候选里找**真正最近**的三角形（体素淘汰表保证真最近邻在其中）。
    double best_sq = std::numeric_limits<double>::infinity();
    int best_tri = -1;
    geom::Vec3<double> best_cp(0.0, 0.0, 0.0);
    for (int tri_index : candidates) {
        CHECK_GE(tri_index, 0);
        CHECK_LT(tri_index, static_cast<int>(tris.size()));
        const geom::Vec3<double> cp = tris[tri_index].ClosestPointTo(p);
        const double dsq = (p - cp).Sqr();
        if (dsq < best_sq) {
            best_sq = dsq;
            best_tri = tri_index;
            best_cp = cp;
        }
    }
    if (best_tri < 0) {
        return hit;
    }
    hit.ok = true;
    hit.closest = best_cp;
    hit.normal = tris[best_tri].normal();  // 最近三角形朝外法线（假定资产绕序朝外）
    hit.signed_dist = (p - best_cp).Dot(hit.normal);
    return hit;
}

// 「带 buffer 的体内」：命中且点到体表的有符号距离 < buffer（buffer=0 即严格体内）。
bool Simulator::IsInsideBodyWithBuffer(const geom::Vec3<double>& p) const {
    const BodyHit hit = QueryBody(p);
    return hit.ok && hit.signed_dist < static_cast<double>(body_buffer_);
}

// 二分求逃逸点：[outside, inside] 上二分 rounds 轮，返回**离 inside 最近**的“不在体内”点。
// 不变量：outside 端始终“不在体内”、inside 端始终“在体内”；每轮取中点重判、收缩区间。
// 返回 outside 端（最后一次确认在体外的点）——它到 inside 的距离 ≈ |线段| / 2^rounds。
geom::Vec3<double> Simulator::BisectEscapeBoundary(
    const geom::Vec3<double>& outside, const geom::Vec3<double>& inside,
    int rounds) const {
    CHECK(rounds > 0) << "BisectEscapeBoundary: rounds 必须 > 0，got " << rounds;
    geom::Vec3<double> lo = outside;  // 已知在体外
    geom::Vec3<double> hi = inside;   // 已知在体内
    for (int r = 0; r < rounds; ++r) {
        const geom::Vec3<double> mid = (lo + hi) * 0.5;
        if (IsInsideBodyWithBuffer(mid)) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    return lo;
}

// 人体排斥（见 .h 的语义与前提）。每子步逐点调用；新逃逸算法 + 速度投影，并刷新接触缓存。
void Simulator::ApplyBodyRepulsion(size_t idx, const geom::Vec3<float>& x_pre_f,
                                   geom::Vec3<float>* x /*inout*/,
                                   geom::Vec3<float>* v /*inout*/) const {
    CHECK(x != nullptr);
    CHECK(v != nullptr);
    CHECK_LT(idx, body_outward_buf_.size());
    // 本子步默认“无接触”，查到后再写（供下一子步第一趟的预速度约束）。
    body_outward_buf_[idx] = geom::Vec3<float>(0.0f, 0.0f, 0.0f);
    if (!body_repulsion_enabled_ || body_matcher_ == nullptr) {
        return;
    }
    const geom::Vec3<double> x_new(static_cast<double>((*x)[0]),
                                   static_cast<double>((*x)[1]),
                                   static_cast<double>((*x)[2]));
    const geom::Vec3<double> x_pre(static_cast<double>(x_pre_f[0]),
                                   static_cast<double>(x_pre_f[1]),
                                   static_cast<double>(x_pre_f[2]));

    const BodyHit hit_new = QueryBody(x_new);
    if (!hit_new.ok) {
        return;  // 逃出体表查询半径（未命中）→ 不排斥
    }
    // 接触壳判据：只在 buffer + 极小裕量内才算接触（更远视为无接触、缓存清零）。
    if (hit_new.signed_dist > static_cast<double>(body_buffer_) +
                                  static_cast<double>(kBodyContactMargin)) {
        return;
    }

    // ── 求 clamp（逃逸）点 ──
    geom::Vec3<double> x_clamp;
    if (IsInsideBodyWithBuffer(x_pre)) {
        // 旧方式：仿真前点也在体内 ⇒ 投影到「最近点 + buffer·外向几何方向」。
        // 外向用**几何方向**（体外 p−cp / 体内 cp−p），在棱/顶点处连续、不随最近三角形翻面跳变。
        geom::Vec3<double> outward = (hit_new.signed_dist >= 0.0)
                                         ? (x_new - hit_new.closest)
                                         : (hit_new.closest - x_new);
        const double len = outward.Norm();
        outward = (len > 1e-12) ? (outward * (1.0 / len)) : hit_new.normal;
        x_clamp = hit_new.closest + outward * static_cast<double>(body_buffer_);
    } else {
        // 本子步**新穿入**：在 [x(t), x(t+dt)] 上二分 10 轮，取离 x(t+dt) 最近的“不在体内”点。
        x_clamp = BisectEscapeBoundary(x_pre, x_new, /*rounds=*/10);
    }
    *x = geom::Vec3<float>(static_cast<float>(x_clamp.x()),
                           static_cast<float>(x_clamp.y()),
                           static_cast<float>(x_clamp.z()));

    // ── clamp 点的法线 = 它最近邻三角形的**朝外**法线 ──
    const BodyHit hit_clamp = QueryBody(x_clamp);
    const geom::Vec3<double> n = hit_clamp.ok ? hit_clamp.normal : hit_new.normal;

    // ── 速度：消除沿 n 的分量（含向外/向内），平行分量乘 body_parallel_damping_ ──
    const geom::Vec3<double> vv(static_cast<double>((*v)[0]),
                                static_cast<double>((*v)[1]),
                                static_cast<double>((*v)[2]));
    const double vn = vv.Dot(n);
    const geom::Vec3<double> v_tangent = vv - n * vn;  // 去掉法向分量
    const geom::Vec3<double> v_out =
        v_tangent * static_cast<double>(body_parallel_damping_);
    *v = geom::Vec3<float>(static_cast<float>(v_out.x()),
                           static_cast<float>(v_out.y()),
                           static_cast<float>(v_out.z()));

    // ── 记录接触外方向（供下一子步第一趟的预速度约束）──
    // 仍用**几何方向**：clamp 点相对于它的最近点（与 9436e19“修棱/顶点抖动”一致）。
    geom::Vec3<double> contact_dir;
    if (hit_clamp.ok) {
        contact_dir = (hit_clamp.signed_dist >= 0.0)
                          ? (x_clamp - hit_clamp.closest)
                          : (hit_clamp.closest - x_clamp);
        const double len = contact_dir.Norm();
        contact_dir = (len > 1e-12) ? (contact_dir * (1.0 / len)) : n;
    } else {
        contact_dir = n;
    }
    body_outward_buf_[idx] = geom::Vec3<float>(
        static_cast<float>(contact_dir.x()), static_cast<float>(contact_dir.y()),
        static_cast<float>(contact_dir.z()));
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

// 并查集求关联图的连通分量：返回每个点的分量标签（连续 0..C-1）；空图返回空。
std::vector<uint32_t> Simulator::ComputeComponentLabels() const {
    const size_t n = neighbors_.size();
    std::vector<uint32_t> comp;
    if (n == 0) {
        return comp;
    }
    std::vector<uint32_t> parent(n);
    for (size_t i = 0; i < n; ++i) {
        parent[i] = static_cast<uint32_t>(i);
    }
    const auto find_root = [&parent](uint32_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];  // 路径减半
            x = parent[x];
        }
        return x;
    };
    for (size_t i = 0; i < n; ++i) {
        for (uint32_t j : neighbors_[i]) {
            const uint32_t ri = find_root(static_cast<uint32_t>(i));
            const uint32_t rj = find_root(j);
            if (ri != rj) {
                parent[ri] = rj;
            }
        }
    }
    // 把根 id 归一成连续标签 0..C-1（每个新遇到的根分配下一个号）。
    constexpr uint32_t kUnlabeled = 0xffffffffu;
    std::vector<uint32_t> root_label(n, kUnlabeled);
    comp.assign(n, 0);
    uint32_t num = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t r = find_root(static_cast<uint32_t>(i));
        if (root_label[r] == kUnlabeled) {
            root_label[r] = num++;
        }
        comp[i] = root_label[r];
    }
    return comp;
}

// 关联图的连通分量数（= ComputeComponentLabels 的标签数）。
size_t Simulator::neighbor_component_count() const {
    const std::vector<uint32_t> comp = ComputeComponentLabels();
    if (comp.empty()) {
        return 0;
    }
    uint32_t max_label = 0;
    for (uint32_t c : comp) {
        if (c > max_label) {
            max_label = c;
        }
    }
    return static_cast<size_t>(max_label) + 1;
}

// 连通性修复（见 .h 说明）。分两段处理两种成因：
//   ① “真有空间间隙”（子网到主网最近距离 > 0.95d）：沿最近点对线段**插桥接虚拟点**，
//      再重建关联表（迭代，直到没有“真间隙”为止）。
//   ② “间隙 ≤ d 但被 kMaxNeighbors 顶 k 截断砍掉了跨网边”（实测战术背心就是这种：
//      两片子网最近点仅 1.4cm，远小于 d，却因各点各自保留最近 20 邻居而断开）：
//      直接**强制补一条对称边**连接最近点对（不再重建——重建会再次截断掉）。
// 两种补法都是**对称**加边，不破坏牛顿第三定律（不泵浦能量）。
void Simulator::EnsureNeighborGraphConnected(float d) {
    CHECK_GT(d, 0.0f);
    const int kMaxIters = 8;
    // 桥接点间距上限：留 5% 余量，保证相邻桥接点（及两端点）严格互为邻居（|Δ| ≤ d）。
    const float kBridgeSpacing = 0.95f * d;

    // ---- 工具：把当前关联图的连通分量整理成“每分量一点索引表” ----
    const auto components = [this](std::vector<std::vector<uint32_t>>* pts_out,
                                   uint32_t* num_out) {
        const std::vector<uint32_t> comp = ComputeComponentLabels();
        if (comp.empty()) {
            *pts_out = {};
            *num_out = 0;
            return;
        }
        uint32_t max_label = 0;
        for (uint32_t c : comp) {
            if (c > max_label) {
                max_label = c;
            }
        }
        std::vector<std::vector<uint32_t>> pts(max_label + 1);
        for (size_t i = 0; i < comp.size(); ++i) {
            pts[comp[i]].push_back(static_cast<uint32_t>(i));
        }
        *pts_out = std::move(pts);
        *num_out = max_label + 1;
    };

    // ---- 工具：求两分量间最近点对（返回距离平方；a_out/b_out 为两点索引）----
    const auto closest_pair = [this](const std::vector<uint32_t>& A,
                                     const std::vector<uint32_t>& B,
                                     uint32_t* a_out, uint32_t* b_out) {
        double best = std::numeric_limits<double>::infinity();
        for (uint32_t a : A) {
            const geom::Vec3<float>& pa = sim_positions_[a];
            for (uint32_t b : B) {
                const geom::Vec3<float>& pb = sim_positions_[b];
                const double dx = static_cast<double>(pa[0]) - pb[0];
                const double dy = static_cast<double>(pa[1]) - pb[1];
                const double dz = static_cast<double>(pa[2]) - pb[2];
                const double dsq = dx * dx + dy * dy + dz * dz;
                if (dsq < best) {
                    best = dsq;
                    *a_out = a;
                    *b_out = b;
                }
            }
        }
        return best;
    };

    // ---- 阶段①：真·间隙（> 0.95d）→ 插桥接虚拟点（迭代）----
    for (int iter = 0; iter < kMaxIters; ++iter) {
        std::vector<std::vector<uint32_t>> pts;
        uint32_t num = 0;
        components(&pts, &num);
        if (num <= 1) {
            break;
        }
        uint32_t main = 0;
        for (uint32_t c = 1; c < num; ++c) {
            if (pts[c].size() > pts[main].size()) {
                main = c;
            }
        }
        std::vector<geom::Vec3<float>> new_pts;
        for (uint32_t c = 0; c < num; ++c) {
            if (c == main) {
                continue;
            }
            uint32_t q = 0;
            uint32_t p = 0;
            const double dsq = closest_pair(pts[c], pts[main], &q, &p);
            const float length = static_cast<float>(std::sqrt(dsq));
            if (!(length > kBridgeSpacing)) {
                continue;  // ≤ 0.95d：留给阶段②（被截断砍的边，插点无用）
            }
            const geom::Vec3<float>& qp = sim_positions_[q];
            const geom::Vec3<float>& pp = sim_positions_[p];
            // 内点数 k 使每段长 = length/(k+1) ≤ kBridgeSpacing。
            const int k = static_cast<int>(std::ceil(length / kBridgeSpacing)) - 1;
            for (int t = 1; t <= k; ++t) {
                const float u = static_cast<float>(t) / static_cast<float>(k + 1);
                new_pts.push_back(geom::Vec3<float>(
                    qp[0] + (pp[0] - qp[0]) * u, qp[1] + (pp[1] - qp[1]) * u,
                    qp[2] + (pp[2] - qp[2]) * u));
            }
        }
        if (new_pts.empty()) {
            break;  // 没有“真间隙”→ 交给阶段②
        }
        const size_t added = new_pts.size();
        for (const geom::Vec3<float>& pt : new_pts) {
            sim_positions_.push_back(pt);
            bind_positions_.push_back(pt);  // 桥接点绑定姿态 = 初始位置
        }
        BuildNeighborTable(d);
        LOG(INFO) << "EnsureNeighborGraphConnected①：为空间间隙补桥，新增虚拟点 "
                  << added << "（当前仿真点 " << sim_positions_.size() << "）";
    }

    // ---- 阶段②：顶 k 截断导致的断网 → 强制补对称边（迭代，不重建）----
    for (int iter = 0; iter < kMaxIters; ++iter) {
        std::vector<std::vector<uint32_t>> pts;
        uint32_t num = 0;
        components(&pts, &num);
        if (num <= 1) {
            break;
        }
        uint32_t main = 0;
        for (uint32_t c = 1; c < num; ++c) {
            if (pts[c].size() > pts[main].size()) {
                main = c;
            }
        }
        size_t added = 0;
        for (uint32_t c = 0; c < num; ++c) {
            if (c == main) {
                continue;
            }
            uint32_t q = 0;
            uint32_t p = 0;
            closest_pair(pts[c], pts[main], &q, &p);
            // 强制加一条对称边 q—p（含初始距离），绕过 k 上限。
            const std::vector<uint32_t>& nq = neighbors_[q];
            if (std::find(nq.begin(), nq.end(), p) != nq.end()) {
                continue;  // 边已存在（理论上不会：存在就同分量了）
            }
            const geom::Vec3<float>& qp = sim_positions_[q];
            const geom::Vec3<float>& pp = sim_positions_[p];
            const float dist = std::sqrt((qp[0] - pp[0]) * (qp[0] - pp[0]) +
                                         (qp[1] - pp[1]) * (qp[1] - pp[1]) +
                                         (qp[2] - pp[2]) * (qp[2] - pp[2]));
            neighbors_[q].push_back(p);
            nb_init_dist_[q].push_back(dist);
            neighbors_[p].push_back(q);
            nb_init_dist_[p].push_back(dist);
            ++added;
        }
        LOG(INFO) << "EnsureNeighborGraphConnected②：顶 k 截断导致的断网，强制补边 "
                  << added << " 条";
        if (added == 0) {
            break;  // 防死循环
        }
    }
}

void Simulator::SetGravity(float gravity) {
    CHECK(std::isfinite(gravity)) << "SetGravity 要求有限值，got " << gravity;
    // 允许负重力（反重力 / 上浮）；不夹断到非负。
    gravity_ = gravity;
}

void Simulator::SetGroundY(float ground_y) {
    CHECK(std::isfinite(ground_y)) << "SetGroundY 要求有限值，got " << ground_y;
    ground_y_ = ground_y;
}

void Simulator::SetMaxSpeed(float v_max) {
    CHECK(std::isfinite(v_max)) << "SetMaxSpeed 要求有限值，got " << v_max;
    CHECK_GE(v_max, 0.0f) << "SetMaxSpeed 要求 v_max >= 0（0 = 不限），got " << v_max;
    max_speed_ = v_max;
}

void Simulator::SetBodyMatcher(const geom::TriangleMatcher3d<double>* matcher) {
    // 允许 nullptr（= 关查询）；借用的指针，生命周期由调用方保证（见 .h）。
    body_matcher_ = matcher;
}

void Simulator::SetBodyBuffer(float buffer) {
    CHECK(std::isfinite(buffer)) << "SetBodyBuffer 要求有限值，got " << buffer;
    CHECK_GE(buffer, kMinBodyBuffer)
        << "SetBodyBuffer 要求 buffer >= " << kMinBodyBuffer << "，got " << buffer;
    CHECK_LE(buffer, kMaxBodyBuffer)
        << "SetBodyBuffer 要求 buffer <= " << kMaxBodyBuffer << "，got " << buffer;
    body_buffer_ = buffer;
}

void Simulator::SetBodyParallelDamping(float factor) {
    CHECK(std::isfinite(factor))
        << "SetBodyParallelDamping 要求有限值，got " << factor;
    CHECK_GE(factor, kMinBodyParallelDamping)
        << "SetBodyParallelDamping 要求 >= " << kMinBodyParallelDamping
        << "，got " << factor;
    CHECK_LE(factor, kMaxBodyParallelDamping)
        << "SetBodyParallelDamping 要求 <= " << kMaxBodyParallelDamping
        << "，got " << factor;
    body_parallel_damping_ = factor;
}

// ── 即时操作（见 .h 的语义说明）──
//
// 位置与绑定姿态**同步变换**（避免弹簧把顶点拽回旧形状、产生炸掉的恢复力）；速度只在
// 旋转时参与。变换后立即把位置写回 mesh_（ExtractMesh）；法线/切线不在此处更新。

void Simulator::ApplyTranslation(const geom::Vec3<float>& delta) {
    CHECK(inited_) << "ApplyTranslation 调用前必须先 Init(mesh)";
    CHECK(delta.IsFinite()) << "ApplyTranslation 要求 delta 有限";
    for (geom::Vec3<float>& p : sim_positions_) {
        p = p + delta;
    }
    for (geom::Vec3<float>& p : bind_positions_) {
        p = p + delta;
    }
    // 速度不参与平移。
    ExtractMesh();
}

void Simulator::ApplyRotation(Axis axis, float degrees,
                              const geom::Vec3<float>& pivot) {
    CHECK(inited_) << "ApplyRotation 调用前必须先 Init(mesh)";
    CHECK(std::isfinite(degrees)) << "ApplyRotation 要求 degrees 有限";
    CHECK(pivot.IsFinite()) << "ApplyRotation 要求 pivot 有限，got "
                            << pivot.DebugString();

    const geom::Vec3<float> axis_dir = AxisUnit(axis);
    const geom::Quaternion<float> q =
        geom::Quaternion<float>::FromAxisAngle(axis_dir, degrees * kDegToRad);
    auto rotate = [&q, &pivot](const geom::Vec3<float>& v) {
        return pivot + geom::RotateVector(q, v - pivot);
    };

    for (geom::Vec3<float>& p : sim_positions_) {
        p = rotate(p);
    }
    // **速度参与旋转**（旋转 => 角速度）。
    for (geom::Vec3<float>& v : sim_velocities_) {
        v = geom::RotateVector(q, v);
    }
    for (geom::Vec3<float>& p : bind_positions_) {
        p = rotate(p);
    }
    ExtractMesh();
}

void Simulator::ApplyScaling(float factor, const geom::Vec3<float>& pivot) {
    CHECK(inited_) << "ApplyScaling 调用前必须先 Init(mesh)";
    CHECK(std::isfinite(factor)) << "ApplyScaling 要求 factor 有限";
    CHECK_GT(factor, 0.0f) << "ApplyScaling 要求 factor > 0，got " << factor;
    CHECK(pivot.IsFinite()) << "ApplyScaling 要求 pivot 有限，got "
                            << pivot.DebugString();
    auto scale = [factor, &pivot](const geom::Vec3<float>& v) {
        return pivot + (v - pivot) * factor;
    };
    for (geom::Vec3<float>& p : sim_positions_) {
        p = scale(p);
    }
    for (geom::Vec3<float>& p : bind_positions_) {
        p = scale(p);
    }
    // 绑定 offset 的**模长缓存**（= 力的分母 |pij(0)|）同步缩放：它是绑定的 offset 的模长，
    // 理应随绑定姿态缩放（否则分母陈旧而与分子 p0 不一致）。旋转 / 平移不改模长，不此处处理。
    for (std::vector<float>& row : nb_init_dist_) {
        for (float& d : row) {
            d *= factor;
        }
    }
    // 速度**不**参与缩放。
    ExtractMesh();
}

void Simulator::ApplyPartialTranslation(const std::vector<uint32_t>& vertex_indices,
                                        const geom::Vec3<float>& delta) {
    CHECK(inited_) << "ApplyPartialTranslation 调用前必须先 Init(mesh)";
    CHECK(delta.IsFinite()) << "ApplyPartialTranslation 要求 delta 有限";
    for (uint32_t idx : vertex_indices) {
        CHECK_LT(idx, vertex_count_)
            << "ApplyPartialTranslation: 仅接受原始顶点 index，got " << idx
            << "（original_point_count()=" << vertex_count_ << "）";
        sim_positions_[idx] = sim_positions_[idx] + delta;
    }
    // 速度不参与平移；绑定姿态（力学参照）/ 关联邻居表**均不**更新（见 .h 说明）。
    ExtractMesh();
}

void Simulator::ApplyPartialRotation(const std::vector<uint32_t>& vertex_indices,
                                     Axis axis, float degrees,
                                     const geom::Vec3<float>& pivot) {
    CHECK(inited_) << "ApplyPartialRotation 调用前必须先 Init(mesh)";
    CHECK(std::isfinite(degrees)) << "ApplyPartialRotation 要求 degrees 有限";
    CHECK(pivot.IsFinite()) << "ApplyPartialRotation 要求 pivot 有限，got "
                            << pivot.DebugString();
    const geom::Vec3<float> axis_dir = AxisUnit(axis);
    const geom::Quaternion<float> q =
        geom::Quaternion<float>::FromAxisAngle(axis_dir, degrees * kDegToRad);
    auto rotate = [&q, &pivot](const geom::Vec3<float>& v) {
        return pivot + geom::RotateVector(q, v - pivot);
    };
    for (uint32_t idx : vertex_indices) {
        CHECK_LT(idx, vertex_count_)
            << "ApplyPartialRotation: 仅接受原始顶点 index，got " << idx
            << "（original_point_count()=" << vertex_count_ << "）";
        sim_positions_[idx] = rotate(sim_positions_[idx]);
        // 速度参与旋转（旋转 ⇒ 角速度），与全体 ApplyRotation 一致。
        sim_velocities_[idx] = geom::RotateVector(q, sim_velocities_[idx]);
    }
    // 绑定姿态（力学参照）/ 关联邻居表**均不**更新（见 .h 说明）。
    ExtractMesh();
}

void Simulator::ApplyPartialScaling(const std::vector<uint32_t>& vertex_indices,
                                    float factor, const geom::Vec3<float>& pivot) {
    CHECK(inited_) << "ApplyPartialScaling 调用前必须先 Init(mesh)";
    CHECK(std::isfinite(factor)) << "ApplyPartialScaling 要求 factor 有限";
    CHECK_GT(factor, 0.0f) << "ApplyPartialScaling 要求 factor > 0，got " << factor;
    CHECK(pivot.IsFinite()) << "ApplyPartialScaling 要求 pivot 有限，got "
                            << pivot.DebugString();
    auto scale = [factor, &pivot](const geom::Vec3<float>& v) {
        return pivot + (v - pivot) * factor;
    };
    for (uint32_t idx : vertex_indices) {
        CHECK_LT(idx, vertex_count_)
            << "ApplyPartialScaling: 仅接受原始顶点 index，got " << idx
            << "（original_point_count()=" << vertex_count_ << "）";
        sim_positions_[idx] = scale(sim_positions_[idx]);
    }
    // 速度不参与缩放；绑定姿态（力学参照）/ 关联邻居表 / nb_init_dist_ **均不**更新（见 .h）。
    ExtractMesh();
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
    // 位置回到**启动几何**——必须用 startup_positions_（Apply* 从不动它）；
    // 不能用 bind_positions_（已被 ApplyTranslation/Rotation/Scaling 污染，见 .h 说明），
    // 也不能用 BuildSimulationPoints 重建（那样会丢掉连通性修复补出的桥接点）。
    sim_positions_ = startup_positions_;
    // 物理参考形状（力的 pij(0)）也回到启动几何；否则弹簧会把顶点拽回“变换后的形状”。
    bind_positions_ = startup_positions_;
    // 关联表按启动几何重建（nb_init_dist_ 也回到初始；清掉 ApplyScaling 对它的缩放）。
    BuildNeighborTable(bind_distance_);
    sim_velocities_.assign(sim_positions_.size(),
                           geom::Vec3<float>(0.0f, 0.0f, 0.0f));
    body_outward_buf_.assign(sim_positions_.size(),
                             geom::Vec3<float>(0.0f, 0.0f, 0.0f));
    time_ = 0.0;
    step_count_ = 0;

    LOG(INFO) << "soft_mesh_simulator::Reset 已回到**启动几何**"
                 "（位置 / 绑定姿态 / 速度 / 时间 / 步数全清零）";
}

}  // namespace soft_mesh_simulator
}  // namespace jpov
