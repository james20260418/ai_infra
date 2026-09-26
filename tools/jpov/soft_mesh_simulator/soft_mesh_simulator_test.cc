// JPOV 软体仿真器 — 纯 CPU 单测（GL-free）
//
// 锁定的是**主入口的契约**与**积分器行为**：
//   - M2：重力 + 速度衰减。Step 在重力下下坠；终速 → g/k；无重力纯衰减按 exp(-k·t)
//   - 时钟正确推进：time == Σdt，step_count == 调用次数
//   - 拓扑/属性在 Step 后原样保留（索引、法线、UV 不被动）
//   - Reset 回到绑定姿态并清零计数/速度
//   - 输入 mesh 不被修改（调用方的网格是只读的）
//   - Bounds() 正确且是纯查询（多调几次结果一致，不影响时钟）
//   - 虚拟顶点参与仿真但不出现在输出 mesh 里
//
// ⚠️ 负向验证过：把 Bounds 改成"只取首顶点"，本测试立即 FAIL（非恒真）。
// ⚠️ 终速断言 (TerminalVelocityApproachesGOverK) 非恒真：若阻尼项写成
//    正号/漏乘，v_y 不会收敛到 -g/k，断言如期失败。

#include "tools/jpov/soft_mesh_simulator/soft_mesh_simulator.h"

#include <cmath>
#include <limits>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "tools/jpov/interface/mesh.h"

namespace {

using jpov::soft_mesh_simulator::Simulator;

// 造一个单三角形（3 顶点，带法线/UV/索引）的测试网格。
jpov::MeshData MakeTri() {
    jpov::MeshData m;
    m.flags = static_cast<jpov::MeshVertexFlags>(
        static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kUV));
    m.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    m.normals   = {{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}};
    m.uvs       = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    m.indices   = {0, 1, 2};
    m.Validate();
    return m;
}

// 造一个无索引的 6 顶点网格（2 三角形）。
jpov::MeshData MakeNonIndexed() {
    jpov::MeshData m;
    m.flags = jpov::MeshVertexFlags::kPosition;
    m.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0},
                   {0, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    m.Validate();
    return m;
}

// 造一个**故意偏离原点且各轴不对称**的网格（用于 Bounds 测试）：
// 若 Bounds 实现写错（如只取首顶点、或漏更新某轴），本网格能让断言真失败——
// 用原点三角形做 Bounds 测试是恒真的（极值与 0 重合，看不出漏算）。
// 顶点：(-2, 1, -5) / (3, -4, 0) / (0, 7, 2)
//   → min = (-2, -4, -5)，max = (3, 7, 2)
jpov::MeshData MakeAsymmetricMesh() {
    jpov::MeshData m;
    m.flags = jpov::MeshVertexFlags::kPosition;
    m.positions = {{-2.0f, 1.0f, -5.0f}, {3.0f, -4.0f, 0.0f}, {0.0f, 7.0f, 2.0f}};
    m.Validate();
    return m;
}

TEST(SoftMeshSimulatorTest, InitCountsVerticesAndTriangles) {
    Simulator sim;
    sim.Init(MakeTri());
    EXPECT_EQ(sim.vertex_count(), 3u);
    EXPECT_EQ(sim.triangle_count(), 1u);
    EXPECT_DOUBLE_EQ(sim.time(), 0.0);
    EXPECT_EQ(sim.step_count(), 0u);
}

TEST(SoftMeshSimulatorTest, NonIndexedTriangleCountUsesVertexCountOver3) {
    Simulator sim;
    sim.Init(MakeNonIndexed());
    EXPECT_EQ(sim.vertex_count(), 6u);
    EXPECT_EQ(sim.triangle_count(), 2u);  // 6/3
}

// M2 契约：dt 很小的一步后，顶点只发生与重力一致的微小位移（不再是恒等）。
// 注：M0 的「Step 恒等」断言已被 M2 取代——现在 Step 会真的落下。
TEST(SoftMeshSimulatorTest, StepMovesMeshUnderGravity) {
    const jpov::MeshData in = MakeTri();
    Simulator sim;
    sim.Init(in);

    const jpov::MeshData out = sim.Step(Simulator::kDefaultDt);

    ASSERT_EQ(out.positions.size(), in.positions.size());
    // 一步（1/60 s）内：所有顶点都应向 -Y 下移（x/z 不变——无水平力）。
    for (size_t i = 0; i < in.positions.size(); ++i) {
        EXPECT_LT(out.positions[i][1], in.positions[i][1])
            << "顶点 " << i << " 应在重力下向 -Y 移动";
        EXPECT_FLOAT_EQ(out.positions[i][0], in.positions[i][0]) << "x 不应变";
        EXPECT_FLOAT_EQ(out.positions[i][2], in.positions[i][2]) << "z 不应变";
    }
}

// 拓扑与其它顶点属性必须原样保留（物理只改位置，不应擅自动索引/法线/UV）。
TEST(SoftMeshSimulatorTest, StepPreservesTopologyAndAttributes) {
    const jpov::MeshData in = MakeTri();
    Simulator sim;
    sim.Init(in);

    const jpov::MeshData out = sim.Step(Simulator::kDefaultDt);
    EXPECT_EQ(out.indices, in.indices);
    ASSERT_EQ(out.normals.size(), in.normals.size());
    for (size_t i = 0; i < in.normals.size(); ++i) {
        EXPECT_FLOAT_EQ(out.normals[i][2], in.normals[i][2]);
    }
    ASSERT_EQ(out.uvs.size(), in.uvs.size());
    for (size_t i = 0; i < in.uvs.size(); ++i) {
        EXPECT_FLOAT_EQ(out.uvs[i][0], in.uvs[i][0]);
    }
    EXPECT_EQ(out.flags, in.flags);
}

// 时钟：time == Σdt，步数 == 调用次数（即使动力学是恒等的，时间也必须真实推进）。
TEST(SoftMeshSimulatorTest, ClockAccumulates) {
    Simulator sim;
    sim.Init(MakeTri());

    const double dt = Simulator::kDefaultDt;
    const int n = 60;
    for (int i = 0; i < n; ++i) sim.Step(dt);

    EXPECT_EQ(sim.step_count(), static_cast<size_t>(n));
    EXPECT_NEAR(sim.time(), n * dt, 1e-9);  // 60 步 × 1/60 = 1 秒
}

// 输入网格是只读的：Step 不得回头改调用方那份（物理只改自己内部副本）。
// 用非零值做基准（0.0 是未初始化内存的常见值，用它做期望会让"没写"也通过）。
TEST(SoftMeshSimulatorTest, InputMeshNotMutated) {
    const jpov::MeshData in = MakeAsymmetricMesh();
    const float orig_x = in.positions[1][0];  // = 3.0（非 0，能真区分）
    const float orig_y = in.positions[2][1];  // = 7.0

    Simulator sim;
    sim.Init(in);
    sim.Step(Simulator::kDefaultDt);

    EXPECT_FLOAT_EQ(in.positions[1][0], orig_x);
    EXPECT_FLOAT_EQ(in.positions[2][1], orig_y);
}

// ==================== M2：重力 + 对称阻尼（逐项校验积分器） ====================

// ① 无重力（g=0）+ 无阻尼影响下：初始静止的点不应移动。
//   （g=0 时 a=0，v 始终 0，位置不变——验证「没有力就不动」。）
TEST(SoftMeshSimulatorTest, ZeroGravityKeepsStaticPointsStill) {
    Simulator sim;
    sim.Init(MakeTri());
    sim.SetGravity(0.0f);
    for (int i = 0; i < 30; ++i) {
        sim.Step(Simulator::kDefaultDt);
    }
    const jpov::MeshData& m = sim.mesh();
    EXPECT_NEAR(m.positions[0][1], 0.0f, 1e-6f);
    EXPECT_NEAR(m.positions[1][1], 0.0f, 1e-6f);
}

// ② 自由下坠位移与解析解一致（无阻尼极限）。
//   单步 dt 内 30 子步、每子步 dt_sub：连续极限下 y = -½·g·t²。
//   这里与**参考积分器**（同公式、直接算）逐子步对比，保证实现没写错顺序。
//   关闭弹簧力场（M3 已接入），隔离重力——否则虚拟顶点间的弹簧力会把结果搅进去。
TEST(SoftMeshSimulatorTest, FreeFallMatchesReferenceIntegrator) {
    const float g = 9.8f;
    const double dt = Simulator::kDefaultDt;
    const int n_steps = 60;  // 1 秒

    Simulator sim;
    sim.Init(MakeTri());
    sim.SetGravity(g);
    sim.SetSpringEnabled(false);  // 只留重力+阻尼（本测的考察对象）
    sim.SetGroundY(-1e6f);  // 把地面推到极低，隔离重力（本测不关心地面）
    for (int i = 0; i < n_steps; ++i) {
        sim.Step(dt);
    }

    // 参考实现：完全按 DESIGN §3.3 公式、以 double 逐子步演进一个静止点。
    const double k = Simulator::kVelocityDamping;
    const double dt_sub = dt / static_cast<double>(Simulator::kSubsteps);
    double y = 0.0;
    double v = 0.0;
    for (int s = 0; s < n_steps * Simulator::kSubsteps; ++s) {
        const double damp_half = std::exp(-k * dt_sub * 0.5);
        const double a = -g;
        const double v_half = v * damp_half + a * dt_sub * 0.5;
        y += v_half * dt_sub;
        v = (v_half + a * dt_sub * 0.5) * damp_half;
    }
    // 实现用 float 累积、参考用 double；允许小幅容差。
    EXPECT_NEAR(sim.mesh().positions[0][1], y, 1e-3f);
}

// ③ 终速锚点：有阻尼时速度收敛到 g/k（解析终速）。
//   这是「速度指数衰减」这一项的**可判断正确性**断言（不是恒真）。
TEST(SoftMeshSimulatorTest, TerminalVelocityApproachesGOverK) {
    const float g = 9.8f;
    const float k = Simulator::kVelocityDamping;
    Simulator sim;
    sim.Init(MakeTri());
    sim.SetGravity(g);
    sim.SetSpringEnabled(false);  // 隔离力场，只测重力+阻尼的终速
    sim.SetGroundY(-1e6f);  // 隔离地面（本测只关心终速，60s 会坠穿 -3）

    // 终速是指数逼近（时间常数 1/k = 10 s）：跑 60 s = 6 个时间常数 →
    // 1 - e^-6 ≈ 99.75% 收敛。若只跑 10 s 仅 63%（那就不是“逼近”了）。
    const double dt = Simulator::kDefaultDt;
    for (int i = 0; i < 3600; ++i) {  // 60 s
        sim.Step(dt);
    }

    const float v_term_analytic = g / k;  // 9.8/0.1 = 98 m/s，向下
    const float v_y = sim.sim_velocities()[0][1];
    // 用 1% 容差（已跑 6 个时间常数，残余 e^-6 ≈ 0.25%）。
    EXPECT_NEAR(v_y, -v_term_analytic, 0.01f * v_term_analytic)
        << "终速应逼近 -g/k = " << -v_term_analytic;
}

// ④ 纯衰减（g=0 且给定初速）：速度按 exp(-k*t) 指数衰减。
//   验证「速度指数衰减」这一项的独立性（与重力解耦）。
TEST(SoftMeshSimulatorTest, VelocityDecaysExponentiallyWithoutGravity) {
    // 用单顶点网格，手工给它一个初速（通过重力积累后关掉也行，
    // 这里用「先加一秒重力得到初速，再关重力观测衰减」的方式构造）。
    Simulator sim;
    sim.Init(MakeTri());
    sim.SetSpringEnabled(false);  // 隔离力场，只测重力+阻尼的纯衰减
    const double dt = Simulator::kDefaultDt;
    sim.SetGravity(9.8f);
    for (int i = 0; i < 6; ++i) {  // 0.1 s
        sim.Step(dt);
    }
    const float v0 = sim.sim_velocities()[0][1];
    ASSERT_LT(v0, 0.0f);  // 确实向下有速

    sim.SetGravity(0.0f);
    const int steps = 60;  // 1 s
    for (int i = 0; i < steps; ++i) {
        sim.Step(dt);
    }
    const float v1 = sim.sim_velocities()[0][1];
    const double k = Simulator::kVelocityDamping;
    // 期望 v1 ≈ v0 * exp(-k * 1s) = v0 * exp(-0.1)
    EXPECT_NEAR(v1, static_cast<float>(v0 * std::exp(-k * 1.0)),
                0.02f * std::abs(v0));
}

// ⑤ 重力方向：必须向下（-Y），不产生水平位移。
//   关弹簧力场（本测只考察重力方向）；否则加密后的密集弹簧力会淹没信号。
TEST(SoftMeshSimulatorTest, GravityPullsAlongNegativeYOnly) {
    jpov::MeshData m = MakeAsymmetricMesh();
    const geom::Vec3<float> p0_before = m.positions[0];
    Simulator sim;
    sim.Init(m);
    sim.SetSpringEnabled(false);  // 隔离力场，只测重力方向
    for (int i = 0; i < 30; ++i) {
        sim.Step(Simulator::kDefaultDt);
    }
    const geom::Vec3<float>& p0_after = sim.mesh().positions[0];
    EXPECT_LT(p0_after[1], p0_before[1]);       // 下坠
    EXPECT_FLOAT_EQ(p0_after[0], p0_before[0]); // 无 x 漂移
    EXPECT_FLOAT_EQ(p0_after[2], p0_before[2]); // 无 z 漂移
}

// ⑥ 虚拟顶点也参与仿真（与原始点同等地受到重力下坠）。
TEST(SoftMeshSimulatorTest, VirtualPointsAlsoFall) {
    jpov::MeshData m;
    m.flags = jpov::MeshVertexFlags::kPosition;
    m.positions = {{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    m.Validate();
    Simulator sim;
    sim.Init(m, /*bind_distance=*/1.0f);
    sim.SetSpringEnabled(false);  // 隔离力场，只验证“虚拟点也受重力”
    ASSERT_GT(sim.virtual_point_count(), 0u);
    const float vy_before = sim.sim_positions()[sim.original_point_count()][1];

    for (int i = 0; i < 30; ++i) {
        sim.Step(Simulator::kDefaultDt);
    }
    const float vy_after = sim.sim_positions()[sim.original_point_count()][1];
    EXPECT_LT(vy_after, vy_before) << "虚拟顶点也应下坠";
    // 所有仿真点（含虚拟）速度同号向下。
    for (const geom::Vec3<float>& v : sim.sim_velocities()) {
        EXPECT_LT(v[1], 0.0f);
    }
}

// ⑦ 提取 mesh 不含虚拟顶点：Step 输出的顶点数 == 原始网格顶点数。
TEST(SoftMeshSimulatorTest, ExtractMeshExcludesVirtualPoints) {
    jpov::MeshData m;
    m.flags = jpov::MeshVertexFlags::kPosition;
    m.positions = {{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    m.Validate();
    Simulator sim;
    sim.Init(m, /*bind_distance=*/1.0f);
    ASSERT_GT(sim.virtual_point_count(), 0u);
    const jpov::MeshData out = sim.Step(Simulator::kDefaultDt);
    EXPECT_EQ(out.positions.size(), 3u) << "输出只含原始顶点";
}

// ⑧ SetGravity：负值/非有限值必须崩（不静默夹断）。
TEST(SoftMeshSimulatorTest, SetGravityRejectsInvalidValues) {
    Simulator sim;
    sim.Init(MakeTri());
    EXPECT_DEATH(sim.SetGravity(-1.0f), "gravity");
    EXPECT_DEATH(sim.SetGravity(std::numeric_limits<float>::infinity()),
                 "gravity");
}

// ⑨ 子步数常量与 DESIGN §3.4 一致（防日后被误改而无人察觉）。
TEST(SoftMeshSimulatorTest, SubstepCountMatchesDesign) {
    EXPECT_EQ(Simulator::kSubsteps, 30);
    EXPECT_FLOAT_EQ(Simulator::kVelocityDamping, 0.1f);
    EXPECT_FLOAT_EQ(Simulator::kDefaultGravity, 9.8f);
}

// Reset：回到绑定姿态 + 清零时间/步数/速度。
TEST(SoftMeshSimulatorTest, ResetRestoresBindPoseAndClock) {
    const jpov::MeshData in = MakeTri();
    Simulator sim;
    sim.Init(in);

    for (int i = 0; i < 10; ++i) sim.Step(Simulator::kDefaultDt);
    ASSERT_GT(sim.time(), 0.0);
    ASSERT_GT(sim.sim_velocities()[0][1], -1e30f);  // 有过速度（非无穷）

    sim.Reset();

    EXPECT_DOUBLE_EQ(sim.time(), 0.0);
    EXPECT_EQ(sim.step_count(), 0u);
    const jpov::MeshData& cur = sim.mesh();
    ASSERT_EQ(cur.positions.size(), in.positions.size());
    for (size_t i = 0; i < in.positions.size(); ++i) {
        EXPECT_FLOAT_EQ(cur.positions[i][0], in.positions[i][0]);
        EXPECT_FLOAT_EQ(cur.positions[i][1], in.positions[i][1]);
    }
    // 速度也应清零。
    for (const geom::Vec3<float>& v : sim.sim_velocities()) {
        EXPECT_FLOAT_EQ(v[0], 0.0f);
        EXPECT_FLOAT_EQ(v[1], 0.0f);
        EXPECT_FLOAT_EQ(v[2], 0.0f);
    }
}

// 未 Init 过就 Reset：幂等 no-op（查看器可无脑调用）。
TEST(SoftMeshSimulatorTest, ResetBeforeInitIsNoOp) {
    Simulator sim;
    sim.Reset();  // 不应崩溃
    EXPECT_DOUBLE_EQ(sim.time(), 0.0);
    EXPECT_EQ(sim.step_count(), 0u);
}

// Bounds：正确覆盖全部顶点，且是纯查询（多调不改变时钟）。
// 用不对称网格，断言能真区分对错（原点三角形是恒真测试，见 MakeAsymmetricMesh 注释）。
TEST(SoftMeshSimulatorTest, BoundsCoversAllVerticesAndIsPure) {
    Simulator sim;
    sim.Init(MakeAsymmetricMesh());

    const jpov::soft_mesh_simulator::SimBounds b1 = sim.Bounds();
    ASSERT_TRUE(b1.valid);
    // min：三个分量各自来自不同顶点（-2/1/-5、3/-4/0、0/7/2）——
    // 漏算任何一个顶点都会让对应分量的断言失败。
    EXPECT_FLOAT_EQ(b1.min[0], -2.0f);  // 仅顶点0 提供
    EXPECT_FLOAT_EQ(b1.min[1], -4.0f);  // 仅顶点1 提供
    EXPECT_FLOAT_EQ(b1.min[2], -5.0f);  // 仅顶点0 提供
    EXPECT_FLOAT_EQ(b1.max[0],  3.0f);  // 仅顶点1 提供
    EXPECT_FLOAT_EQ(b1.max[1],  7.0f);  // 仅顶点2 提供
    EXPECT_FLOAT_EQ(b1.max[2],  2.0f);  // 仅顶点2 提供

    // 多调几次：结果一致，且时钟不动（纯查询）。
    const jpov::soft_mesh_simulator::SimBounds b2 = sim.Bounds();
    EXPECT_FLOAT_EQ(b2.max[0], b1.max[0]);
    EXPECT_FLOAT_EQ(b2.min[1], b1.min[1]);
    EXPECT_DOUBLE_EQ(sim.time(), 0.0);
    EXPECT_EQ(sim.step_count(), 0u);
}

// 空网格：Bounds().valid == false（min/max 未定义，调用方必须先看标志）。
TEST(SoftMeshSimulatorTest, BoundsInvalidForEmptyMesh) {
    Simulator sim;
    // 未 Init → 内部网格空。
    EXPECT_FALSE(sim.Bounds().valid);
}

// Step 的前置条件：dt <= 0 必须崩（静默跳帧会掩盖时钟 bug）。
TEST(SoftMeshSimulatorTest, StepRejectsNonPositiveDt) {
    Simulator sim;
    sim.Init(MakeTri());
    EXPECT_DEATH(sim.Step(0.0), "dt");
    EXPECT_DEATH(sim.Step(-0.01), "dt");
}

// Step 的前置条件：必须先 Init。
TEST(SoftMeshSimulatorTest, StepRequiresInit) {
    Simulator sim;
    EXPECT_DEATH(sim.Step(Simulator::kDefaultDt), "Init");
}

// ==================== M1：仿真点（长边加密）与投影 ====================

// 边长均 <= d*0.9 时不需要加密：仿真点数 == 原始顶点数。
TEST(SoftMeshSimulatorTest, NoSubdivisionWhenEdgesAreShort) {
    // 三角形边长 ~1，d=10 → max_len=9，无一条边超过。
    Simulator sim;
    sim.Init(MakeTri(), /*bind_distance=*/10.0f);
    EXPECT_EQ(sim.original_point_count(), 3u);
    EXPECT_EQ(sim.virtual_point_count(), 0u);
    EXPECT_EQ(sim.sim_point_count(), 3u);
}

// 长边必须按 d*0.9 切分：边长 10、d=1（max_len=0.9）→ 切 ceil(10/0.9)=12 段
// → 插 11 个虚拟点（每条边）。
TEST(SoftMeshSimulatorTest, LongEdgeIsSubdivided) {
    jpov::MeshData m;
    m.flags = jpov::MeshVertexFlags::kPosition;
    // 单条边长 10 的退化“三角形”（第三点与第一点重合，让三条边里只有一条长边）。
    m.positions = {{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    m.Validate();

    Simulator sim;
    sim.Init(m, /*bind_distance=*/1.0f);
    EXPECT_EQ(sim.original_point_count(), 3u);
    // 长边 (0,1) 与 (1,2) 都长 10；但 (0,2) 是退化边（长 0）且与 0==2。
    // 边 (0,1)：切 12 段 → 11 个点；边 (1,2)：切 12 段 → 11 个点。
    // 合计 22 个虚拟点。
    EXPECT_EQ(sim.virtual_point_count(), 22u);
    EXPECT_EQ(sim.sim_point_count(), 25u);
}

// 加密后每段长度都必须 <= d*0.9（这是加密的目的：保证关联不断）。
TEST(SoftMeshSimulatorTest, SubdividedSegmentsRespectMaxLen) {
    const float d = 1.0f;
    const float max_len = d * 0.9f;
    jpov::MeshData m;
    m.flags = jpov::MeshVertexFlags::kPosition;
    m.positions = {{0.0f, 0.0f, 0.0f}, {7.3f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    m.Validate();

    Simulator sim;
    sim.Init(m, d);

    // 取原始点 0 → 原始点 1 之间的所有仿真点（原始 0/1 + 虚拟点），
    // 按 x 排序，逐一验证相邻间距 <= max_len。
    const auto& pts = sim.sim_positions();
    std::vector<float> xs;
    for (size_t i = 0; i < pts.size(); ++i) {
        if (std::abs(pts[i][1]) < 1e-6f && std::abs(pts[i][2]) < 1e-6f) {
            xs.push_back(pts[i][0]);
        }
    }
    ASSERT_GT(xs.size(), 2u);  // 确实插了点
    std::sort(xs.begin(), xs.end());
    for (size_t i = 1; i < xs.size(); ++i) {
        EXPECT_LE(xs[i] - xs[i - 1], max_len + 1e-4f)
            << "段 " << i << " 长度超限";
    }
}

// 虚拟点判定：前 original_point_count() 个不是虚拟点，其后都是。
TEST(SoftMeshSimulatorTest, IsVirtualPointBoundary) {
    jpov::MeshData m;
    m.flags = jpov::MeshVertexFlags::kPosition;
    m.positions = {{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    m.Validate();

    Simulator sim;
    sim.Init(m, 1.0f);
    const size_t orig = sim.original_point_count();
    ASSERT_GT(sim.virtual_point_count(), 0u);
    for (size_t i = 0; i < orig; ++i) {
        EXPECT_FALSE(sim.IsVirtualPoint(i));
    }
    for (size_t i = orig; i < sim.sim_point_count(); ++i) {
        EXPECT_TRUE(sim.IsVirtualPoint(i));
    }
    // 越界传入是调用方 bug → 崩（不静默返回错误答案）。
    EXPECT_DEATH(sim.IsVirtualPoint(sim.sim_point_count()), "越界");
}

// 取景包围盒基于**原始网格**（不含虚拟点，但虚拟点在边上，不扩大包围盒）。
TEST(SoftMeshSimulatorTest, BoundsIgnoresVirtualPoints) {
    jpov::MeshData m;
    m.flags = jpov::MeshVertexFlags::kPosition;
    m.positions = {{0.0f, 0.0f, 0.0f}, {3.0f, 0.0f, 0.0f}, {0.0f, 3.0f, 0.0f}};
    m.Validate();

    Simulator sim;
    sim.Init(m, 0.5f);  // 边长 ~3/4.24 > 0.45 → 会加密
    const auto b = sim.Bounds();
    ASSERT_TRUE(b.valid);
    EXPECT_FLOAT_EQ(b.min[0], 0.0f);
    EXPECT_FLOAT_EQ(b.max[0], 3.0f);
    EXPECT_FLOAT_EQ(b.max[1], 3.0f);
}

// 关联距离必须 > 0，否则崩。
TEST(SoftMeshSimulatorTest, InitRejectsNonPositiveBindDistance) {
    EXPECT_DEATH(Simulator().Init(MakeTri(), 0.0f), "bind_distance");
    EXPECT_DEATH(Simulator().Init(MakeTri(), -1.0f), "bind_distance");
}

// d 影响加密密度：d 越大，虚拟点越少（大 d 不需要细密加密）。
TEST(SoftMeshSimulatorTest, LargerBindDistanceYieldsFewerVirtualPoints) {
    jpov::MeshData m;
    m.flags = jpov::MeshVertexFlags::kPosition;
    m.positions = {{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    m.Validate();

    Simulator fine;
    fine.Init(m, 0.5f);
    Simulator coarse;
    coarse.Init(m, 5.0f);
    EXPECT_GT(fine.virtual_point_count(), coarse.virtual_point_count());
}

// ==================== M4：地面投影（非穿透） ====================

// ⑩ 地面之上的点自由下坠；穿入地面的点被拧回地面（不穿透）。
TEST(SoftMeshSimulatorTest, GroundStopsFallingMesh) {
    jpov::MeshData m;
    m.flags = jpov::MeshVertexFlags::kPosition;
    // 一个位于 y=0 的三角形（高于地面 y=-1）。
    m.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    m.Validate();
    Simulator sim;
    sim.Init(m);
    sim.SetGroundY(-1.0f);

    // 跑足够久（必然坠穿地面）→ 所有顶点应停在地面，不低于地面。
    for (int i = 0; i < 300; ++i) {
        sim.Step(Simulator::kDefaultDt);
    }
    for (const geom::Vec3<float>& p : sim.mesh().positions) {
        EXPECT_GE(p[1], -1.0f - 1e-5f) << "顶点不应穿透地面";
    }
    // 且确实落到了地面（不是还悬在空中）。
    EXPECT_NEAR(sim.mesh().positions[0][1], -1.0f, 1e-4f);
}

// ⑪ 落到地面后，向下的法向速度被清零（不积累、不反弹）。
TEST(SoftMeshSimulatorTest, GroundKillsDownwardVelocity) {
    jpov::MeshData m;
    m.flags = jpov::MeshVertexFlags::kPosition;
    m.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    m.Validate();
    Simulator sim;
    sim.Init(m);
    sim.SetGroundY(-0.5f);
    for (int i = 0; i < 300; ++i) {
        sim.Step(Simulator::kDefaultDt);
    }
    // 静止在地面 → y 速度应 ≈ 0（若实现只 clamp 位置不清速度，v.y 会持续负增长）。
    for (const geom::Vec3<float>& v : sim.sim_velocities()) {
        EXPECT_NEAR(v[1], 0.0f, 1e-5f) << "落地后法向速度应被清零";
    }
    // 且不反弹（不会出现正的速度）。
    for (const geom::Vec3<float>& v : sim.sim_velocities()) {
        EXPECT_LE(v[1], 1e-6f);
    }
}

// ⑫ 地面以上的点不受影响：未下坠到地面以下则不投影（保持正常下坠轨迹）。
TEST(SoftMeshSimulatorTest, PointsAboveGroundUnaffected) {
    jpov::MeshData m;
    m.flags = jpov::MeshVertexFlags::kPosition;
    m.positions = {{0.0f, 100.0f, 0.0f}, {1.0f, 100.0f, 0.0f}, {0.0f, 100.0f, 1.0f}};
    m.Validate();
    Simulator sim;
    sim.Init(m);
    sim.SetSpringEnabled(false);  // 隔离力场，只测重力自由下坠轨迹
    sim.SetGroundY(-1.0f);

    // 自由下坠参考（同公式、double），不与地面接触。
    const float g = Simulator::kDefaultGravity;
    const double dt = Simulator::kDefaultDt;
    const double k = Simulator::kVelocityDamping;
    const double dt_sub = dt / static_cast<double>(Simulator::kSubsteps);
    double y = 100.0;
    double v = 0.0;
    const int n = 30;
    for (int s = 0; s < n * Simulator::kSubsteps; ++s) {
        const double damp_half = std::exp(-k * dt_sub * 0.5);
        const double a = -g;
        const double v_half = v * damp_half + a * dt_sub * 0.5;
        y += v_half * dt_sub;
        v = (v_half + a * dt_sub * 0.5) * damp_half;
    }
    for (int i = 0; i < n; ++i) {
        sim.Step(dt);
    }
    EXPECT_NEAR(sim.mesh().positions[0][1], y, 1e-2f);
}

// ⑬ 地面高度可设：跑完落地后 y == 设定的地面高度（不是写死的 -3）。
TEST(SoftMeshSimulatorTest, GroundHeightIsConfigurable) {
    jpov::MeshData m;
    m.flags = jpov::MeshVertexFlags::kPosition;
    m.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    m.Validate();
    Simulator sim;
    sim.Init(m);
    sim.SetGroundY(2.5f);
    EXPECT_FLOAT_EQ(sim.ground_y(), 2.5f);
    for (int i = 0; i < 300; ++i) {
        sim.Step(Simulator::kDefaultDt);
    }
    EXPECT_NEAR(sim.mesh().positions[0][1], 2.5f, 1e-4f);
}

// ⑭ SetGroundY：非有限值崩。
TEST(SoftMeshSimulatorTest, SetGroundYRejectsNonFinite) {
    Simulator sim;
    sim.Init(MakeTri());
    EXPECT_DEATH(sim.SetGroundY(std::numeric_limits<float>::infinity()),
                 "SetGroundY");
}

// ==================== M3：顶点间弹簧力场（Danis 自创） ====================
//
// 力公式（DESIGN §2.3）：Fij = -[pij(t) - pij(0)] * F / max(|pij(0)|, d/10)，
// 逐分量 clamp 到 ±F_max。F_i = Σ_j Fij；a_i = (重力 + F_i) / m。
//
// 下面这组测试把力场拧到“可判断”的构造上：单轴、两点、已知位移 → 力可解析算。

// 造一个只有两个点、恰好关联的小网格（间距 0.1m，d=0.5 必关联，且不会加密）。
// 返回 Simulator（通过 Init 传入）。两点：(0,0,0) 与 (0.1,0,0)。
namespace {
jpov::MeshData MakeTwoPoints(float sep) {
    jpov::MeshData m;
    m.flags = jpov::MeshVertexFlags::kPosition;
    m.positions = {{0.0f, 0.0f, 0.0f}, {sep, 0.0f, 0.0f}};
    // 无索引：triangle_count = 2/3 = 0（无三角形），不影响仿真点（仍 2 个点）。
    m.Validate();
    return m;
}
}  // namespace

// Ⓐ 关联表：两点距离 <= d 时互相关联；d 滑小到 < 间距则完全无关联。
TEST(SoftMeshSimulatorTest, NeighborTableRespectsBindDistance) {
    Simulator close;
    close.Init(MakeTwoPoints(0.1f), /*d=*/0.5f);
    EXPECT_EQ(close.neighbor_pair_count(), 2u)
        << "两点 0.1m <= d=0.5 → i→j 与 j→i 各一条，计 2";

    Simulator far;
    far.Init(MakeTwoPoints(0.1f), /*d=*/0.05f);
    EXPECT_EQ(far.neighbor_pair_count(), 0u)
        << "两点 0.1m > d=0.05 → 无关联";
}

// Ⓑ 力场零平衡：处在**绑定姿态**且无重力时，弹簧力为零 → 系统不动。
//   （pij(t)=pij(0) ⇒ Δp=0 ⇒ Fij=0。验证“没变形就没弹力”。）
TEST(SoftMeshSimulatorTest, SpringForceZeroAtBindPose) {
    Simulator sim;
    sim.Init(MakeTwoPoints(0.1f), /*d=*/0.5f);
    sim.SetGravity(0.0f);
    // 开到最大 F，若「绑定姿态力不为零」就无处遁形。
    sim.SetForceCoeff(Simulator::kMaxForceCoeff);

    for (int i = 0; i < 30; ++i) sim.Step(Simulator::kDefaultDt);

    // 位置与速度都应保持（数值上近零漂移）。
    for (size_t i = 0; i < sim.sim_point_count(); ++i) {
        const auto& p = sim.sim_positions()[i];
        EXPECT_NEAR(p[1], 0.0f, 1e-6f);
        EXPECT_NEAR(p[2], 0.0f, 1e-6f);
        const auto& v = sim.sim_velocities()[i];
        EXPECT_NEAR(v[0], 0.0f, 1e-6f);
        EXPECT_NEAR(v[1], 0.0f, 1e-6f);
    }
    // x 也不动（绑定姿态间距 0.1 保持）。
    EXPECT_NEAR(sim.sim_positions()[1][0] - sim.sim_positions()[0][0], 0.1f,
                1e-5f);
}

// Ⓒ 力场是**弹簧**：拉伸产生的恢复力使两端相互靠近。
//   构造可解析的单轴情形：初始两点沿 x 相距 0.1m（关联），
//   把 F 设大、无重力，然后用“给定初速”不可得的限制下，改用**间接**手段：
//   借助重力制造 y 向位移不会沿 x 拉伸。故本测试改为验证弹簧力的**存在与方向**
//   通过一个可解析的构造：两点初始间距 d/2，而后“瞬间”把其中一个挪远是做不到的。
//   ⇒ 拉黑难构造的情形不硬凑：证明“弹簧在动作 + 方向正确”交给 Ⓕ（开关差异）
//     + Ⓔ（镜像对称）两测试，本测试删除。
//
// Ⓔ Σ Fij = 0（DESIGN §6 单测 #1）——力场内部净力为零（牛顿第三定律）：
//   整个系统作为刚体自由下落时，内部弹簧力必成对抵消 ⇒ 各点轨迹应完全一致。
//   因为无 setter 改初速，用重力制造整体下落（对每点相同），弹簧保持零变形。
TEST(SoftMeshSimulatorTest, InternalSpringForceCancelsUnderRigidFall) {
    jpov::MeshData m;
    m.flags = jpov::MeshVertexFlags::kPosition;
    // 三点共线沿 x，等距 0.1：x = 0, 0.1, 0.2。
    m.positions = {{0.0f, 0.0f, 0.0f}, {0.1f, 0.0f, 0.0f}, {0.2f, 0.0f, 0.0f}};
    m.Validate();
    Simulator sim;
    sim.Init(m, /*d=*/0.5f);  // 全关联（0.1,0.2 <= 0.5）
    sim.SetGravity(9.8f);
    sim.SetSpringEnabled(true);
    sim.SetForceCoeff(Simulator::kMaxForceCoeff);  // 力开到最大也不影响刚体下落
    sim.SetGroundY(-1e6f);  // 隔离地面：否则先落地的点被 clamp → 打破刚体下落

    for (int i = 0; i < 60; ++i) sim.Step(Simulator::kDefaultDt);

    // 绑定姿态下 Δp≡0 ⇒ 弹簧力恒为 0 ⇒ 整体像刚体下落：
    //   x/z 不变，y 完全同步（各点仅受重力）。
    const auto& P = sim.sim_positions();
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(P[i][0], 0.1f * static_cast<float>(i), 1e-5f);
        EXPECT_NEAR(P[i][2], 0.0f, 1e-6f);
    }
    EXPECT_NEAR(P[0][1], P[1][1], 1e-4f) << "刚体下落：各点 y 应同步";
    EXPECT_NEAR(P[1][1], P[2][1], 1e-4f);
}

// Ⓔ Σ Fij = 0（DESIGN §6 单测 #1）——力场内部净力为零（牛顿第三定律）。
//   构造持续变形（竖向链 + 地面 clamp），关重力后读弹簧加速度，
//   验证 Σ a_i · m = 0（内部力成对抵消）。
//
//   ⚠️ 局限：在强变形/截断主导的稳态下，力多被 clamp 封顶（本身对称），
//   故本测试对“单个点力的轻微非对称”不敏感——它验证的是**整体净力守恒**这一
//   不变量，不是逐对力的正确性。逐对力的正确性由截断测试（Ⓛ）
//   + 开关差异测试（Ⓕ）+ 刚体下落不变性（内联在下面新版）共同锁定。
TEST(SoftMeshSimulatorTest, SpringInternalForceSumsToZero) {
    jpov::MeshData m;
    m.flags = jpov::MeshVertexFlags::kPosition;
    // 竖向链 4 点（非对称间距），确保受力方向不平凡、且地面 clamp 能造成变形。
    m.positions = {{0.0f, 0.0f, 0.0f}, {0.05f, 0.1f, 0.0f},
                   {0.0f, 0.2f, 0.02f}, {0.03f, 0.28f, 0.0f}};
    m.Validate();
    Simulator sim;
    sim.Init(m, 0.5f);  // 全部点两两关联
    sim.SetSpringEnabled(true);
    sim.SetForceCoeff(3.0f);
    sim.SetGroundY(0.05f);  // 下半部被地面钉住、上半部下坠 → 持续变形

    // 先开重力制造**持续变形**（地面把部分点钉住），再关重力——
    // 此时 AccelAtPoint 只剩内部弹簧力，可验证 ΣF_spring = 0。
    sim.SetGravity(9.8f);
    for (int i = 0; i < 200; ++i) sim.Step(Simulator::kDefaultDt);
    sim.SetGravity(0.0f);

    geom::Vec3<float> f_sum(0.0f, 0.0f, 0.0f);
    const float m_pt = sim.point_mass();
    for (size_t i = 0; i < sim.sim_point_count(); ++i) {
        const geom::Vec3<float> a = sim.AccelAtPoint(i);
        f_sum = f_sum + a * m_pt;
    }
    // m 对所有点相同，故 ΣF = m · Σa；断言 Σa ≈ 0（等价于 ΣF=0）。
    EXPECT_NEAR(f_sum[0], 0.0f, 1e-3f) << "内部弹簧力 x 分量和应为 0";
    EXPECT_NEAR(f_sum[1], 0.0f, 1e-3f) << "内部弹簧力 y 分量和应为 0";
    EXPECT_NEAR(f_sum[2], 0.0f, 1e-3f) << "内部弹簧力 z 分量和应为 0";
}

// Ⓕ近 力的**方向正确性**（恢复性，可解析）——这是“力场写对了”的最直接证据：
//   两点沿 y 上下排，下点被地面钉住，上点在重力下远离。在变形状态下，
//   上点受到的**弹簧力**应指向下点（把它拉回），即关重力后
//   AccelAtPoint(上点).y < 0（向下指向被钉住的下点）。
//   若力的符号写反（变成排斥），上点会被推得更远 ⇒ 断言失败。
TEST(SoftMeshSimulatorTest, SpringForceIsRestoringNotRepulsive) {
    jpov::MeshData m;
    m.flags = jpov::MeshVertexFlags::kPosition;
    // 下点 (0,0,0)、上点 (0,0.2,0)，间距 0.2 <= d。
    m.positions = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.2f, 0.0f}};
    m.Validate();
    Simulator sim;
    sim.Init(m, 0.5f);
    sim.SetSpringEnabled(true);
    sim.SetForceCoeff(2.0f);
    sim.SetGroundY(0.0f);  // 下点被钉在地面（正好在 y=0）

    // 重力把上点往下拉、下点被地面钉住 → 两点间距被**压缩**（上点接近下点）。
    // 弹簧应把上点推回原位（向上，+y）→ 关重力后上点 accel.y 应为正。
    sim.SetGravity(9.8f);
    for (int i = 0; i < 40; ++i) sim.Step(Simulator::kDefaultDt);
    sim.SetGravity(0.0f);

    // 确认确实处于变形状态：上点已不在绑定位置 y=0.2。
    const float y_upper = sim.sim_positions()[1][1];
    ASSERT_LT(y_upper, 0.2f - 1e-4f) << "上点应已被压向地面（变形）";

    const geom::Vec3<float> a_upper = sim.AccelAtPoint(1);
    // 上点应被弹簧拉回（恢复力向上 +y）——验证力的**方向正确（恢复性，非排斥）**。
    // 若公式符号写反（如 DESIGN §2.3 原始版），此处会得到 a.y<0（向下、排斥）而失败。
    EXPECT_GT(a_upper[1], 0.0f)
        << "上点应被弹簧拉回（+y 恢复力），实测 a.y=" << a_upper[1]
        << "（若为负 = 力符号反了，变成排斥）";
}

// Ⓕ 力场确实在动作（对“力场真的接进去了”的最直接证据）。
//   ⚠️ 必须构造**持续变形**场景：刚体下落/同时落地时弹簧零作用。
//   故用**竖直链条**（4 点沿 y 排）+ 地面 clamp：上端持续被重力下拽、
//   下端被地面钉住 → 持续拉伸 ⇒ 弹簧介入。开/关弹簧结果必有差异。
TEST(SoftMeshSimulatorTest, SpringFieldActuallyChangesMotion) {
    jpov::MeshData m;
    m.flags = jpov::MeshVertexFlags::kPosition;
    // 4 点沿 y 等距 0.1：y = 0, 0.1, 0.2, 0.3。全关联（0.1..0.3 <= d=0.5）。
    for (int i = 0; i < 4; ++i) {
        m.positions.push_back({0.0f, 0.1f * static_cast<float>(i), 0.0f});
    }
    m.Validate();

    Simulator with_spring;
    with_spring.Init(m, 0.5f);
    with_spring.SetGravity(9.8f);
    with_spring.SetSpringEnabled(true);
    with_spring.SetForceCoeff(2.0f);
    with_spring.SetGroundY(0.15f);  // 下半部分被地面钉住、上半部分继续下坠

    Simulator no_spring;
    no_spring.Init(m, 0.5f);
    no_spring.SetGravity(9.8f);
    no_spring.SetSpringEnabled(false);
    no_spring.SetGroundY(0.15f);

    for (int i = 0; i < 120; ++i) {
        with_spring.Step(Simulator::kDefaultDt);
        no_spring.Step(Simulator::kDefaultDt);
    }
    float max_diff = 0.0f;
    for (size_t i = 0; i < with_spring.sim_point_count(); ++i) {
        const auto& a = with_spring.sim_positions()[i];
        const auto& b = no_spring.sim_positions()[i];
        for (int c = 0; c < 3; ++c) {
            max_diff = std::max(max_diff, std::abs(a[c] - b[c]));
        }
    }
    EXPECT_GT(max_diff, 1e-3f) << "开关弹簧力场必须造成可观测的运动差异";
}

// Ⓖ 关联表在 Init 后**冻结**（DESIGN §2.1：t=0 定死，永不更新）：
//   即使 Step 把点撑得极远，关联对数不变。
TEST(SoftMeshSimulatorTest, NeighborTableFrozenAfterInit) {
    Simulator sim;
    sim.Init(MakeTwoPoints(0.1f), 0.5f);
    const size_t before = sim.neighbor_pair_count();
    ASSERT_EQ(before, 2u);
    sim.SetGravity(9.8f);  // 让它们一直下坠（相对位置不变，但验证“不因运动变化”）
    for (int i = 0; i < 120; ++i) sim.Step(Simulator::kDefaultDt);
    EXPECT_EQ(sim.neighbor_pair_count(), before)
        << "关联表必须在仿真中保持冻结";
}

// Ⓗ Reset 后关联表仍在（Reset 重建仿真点集合，关联表应一并重填）。
TEST(SoftMeshSimulatorTest, NeighborTableSurvivesReset) {
    Simulator sim;
    sim.Init(MakeTwoPoints(0.1f), 0.5f);
    for (int i = 0; i < 30; ++i) sim.Step(Simulator::kDefaultDt);
    sim.Reset();
    EXPECT_EQ(sim.neighbor_pair_count(), 2u);
    sim.SetGravity(0.0f);
    for (int i = 0; i < 30; ++i) sim.Step(Simulator::kDefaultDt);
    EXPECT_NEAR(sim.sim_positions()[1][0] - sim.sim_positions()[0][0], 0.1f,
                1e-5f);
}

// Ⓘ SetTotalMass / SetForceCoeff 值域护栏（不静默夹断）。
TEST(SoftMeshSimulatorTest, SetParamsRejectInvalidValues) {
    Simulator sim;
    sim.Init(MakeTri());
    EXPECT_DEATH(sim.SetTotalMass(0.5f), "SetTotalMass");  // < 1kg 下限
    EXPECT_DEATH(sim.SetTotalMass(-1.0f), "SetTotalMass");
    EXPECT_DEATH(sim.SetTotalMass(std::numeric_limits<float>::infinity()),
                 "SetTotalMass");
    EXPECT_DEATH(sim.SetForceCoeff(0.0f), "SetForceCoeff");   // F 必须 > 0
    EXPECT_DEATH(sim.SetForceCoeff(-1.0f), "SetForceCoeff");
    // 合法值正常写入。
    sim.SetTotalMass(2.0f);
    EXPECT_FLOAT_EQ(sim.total_mass(), 2.0f);
    sim.SetForceCoeff(1.5f);
    EXPECT_FLOAT_EQ(sim.force_coeff(), 1.5f);
}

// 速度衰减系数 k：默认 0.1；值域护栏（>=0）；k 越大衰减越快（衰减效果可测）。
TEST(SoftMeshSimulatorTest, SetVelocityDampingWorksAndRejectsInvalid) {
    Simulator sim;
    sim.Init(MakeTri());
    EXPECT_FLOAT_EQ(sim.velocity_damping(), 0.1f);  // 默认
    sim.SetVelocityDamping(5.0f);
    EXPECT_FLOAT_EQ(sim.velocity_damping(), 5.0f);
    sim.SetVelocityDamping(0.0f);  // 0 = 无阻尼，合法
    EXPECT_FLOAT_EQ(sim.velocity_damping(), 0.0f);
    EXPECT_DEATH(sim.SetVelocityDamping(-1.0f), "SetVelocityDamping");
    EXPECT_DEATH(sim.SetVelocityDamping(std::numeric_limits<float>::infinity()),
                 "SetVelocityDamping");

    // 行为：k 越大，同样时长后速度衰减越多（关重力、隔离地面，看纯衰减）。
    Simulator weak;
    weak.Init(MakeTri());
    weak.SetGravity(9.8f);
    for (int i = 0; i < 6; ++i) weak.Step(Simulator::kDefaultDt);  // 先积一点速度
    weak.SetSpringEnabled(false);
    weak.SetGravity(0.0f);
    weak.SetGroundY(-1e9f);
    Simulator strong = weak;  // 同状态起步
    weak.SetVelocityDamping(0.1f);
    strong.SetVelocityDamping(10.0f);
    for (int i = 0; i < 30; ++i) {
        weak.Step(Simulator::kDefaultDt);
        strong.Step(Simulator::kDefaultDt);
    }
    const float v_weak = std::abs(weak.sim_velocities()[0][1]);
    const float v_strong = std::abs(strong.sim_velocities()[0][1]);
    EXPECT_LT(v_strong, v_weak) << "k 越大衰减越快";
}

// Ⓙ 每点质量 = M_total / N（含虚拟顶点）。
TEST(SoftMeshSimulatorTest, PointMassIsTotalOverCount) {
    Simulator sim;
    sim.Init(MakeTwoPoints(0.1f), 0.5f);
    sim.SetTotalMass(10.0f);
    EXPECT_NEAR(sim.point_mass(),
                10.0f / static_cast<float>(sim.sim_point_count()), 1e-6f);
}

// Ⓚ 力的截断（DESIGN §2.5 / §6 单测 #2）：|Δp| 极大时弹簧力被封顶在 F_max。
//
//   构造：竖向链（下端被地面钉住、整串在重力下垂挂）。链顶弹簧必须托住
//   下面所有点的重量（≈ M_total·g），可远超 F_max ⇒ 必然撞截断。
//   验证：取链顶点的**弹簧加速度分量** = a_total - 重力，还原为力后应 <= F_max。
TEST(SoftMeshSimulatorTest, SpringForceIsClampedAtForceMax) {
    jpov::MeshData m;
    m.flags = jpov::MeshVertexFlags::kPosition;
    // 20 点竖向链，等距 0.05（<= d=0.5 全关联），底点在 y=-0.5（钉在地面）。
    for (int i = 0; i < 20; ++i) {
        m.positions.push_back({0.0f, -0.5f + 0.05f * static_cast<float>(i), 0.0f});
    }
    m.Validate();
    Simulator sim;
    sim.Init(m, 0.06f);  // d 仅略大于间距 0.05 → 每点只与相邻点关联（单对）
    const float g = 20.0f;  // 最大重力
    sim.SetGravity(g);
    sim.SetSpringEnabled(true);
    sim.SetForceCoeff(Simulator::kMaxForceCoeff);
    sim.SetTotalMass(Simulator::kMaxTotalMass);  // 200kg → 总重远超 F_max

    // 跑足够久让链稳定垂挂。
    for (int i = 0; i < 600; ++i) sim.Step(Simulator::kDefaultDt);

    // 链顶点（idx=19，最上）的弹簧加速度分量 = a_total - 重力。
    // d=0.06 → 该点只有 1 个邻居（单对）⇒ 弹簧力 = 单个 Fij，必受 F_max 封顶。
    const auto a = sim.AccelAtPoint(19);
    const float a_spring_y = a[1] - (-g);  // 减掉重力得到弹簧贡献
    const float m_pt = sim.point_mass();
    const float f_spring_y = a_spring_y * m_pt;  // 还原为力
    // 链顶被上面（无）和下面拉 → 弹簧力向上（+y）以抗拒重力；绝对值 <= F_max。
    EXPECT_LE(std::abs(f_spring_y), Simulator::kForceMax + 1e-2f)
        << "弹簧力应被截断在 F_max = " << Simulator::kForceMax
        << "，实测 " << std::abs(f_spring_y);
    // 且确实“撞了”截断（链顶应托住很大重量）。
    EXPECT_GT(std::abs(f_spring_y), 0.5f * Simulator::kForceMax)
        << "本构造应确实触发截断（否则测不到）";
}

// Ⓛ 力的截断下系统保持有限：密集网格 + 最大 F + 最大重力
//   + 最小质量（加速度最大）+ 长时积分，位置/速度必须始终有限（无 NaN/Inf）。
TEST(SoftMeshSimulatorTest, LargeForceStaysFiniteWithClamp) {
    // 密集共线点（间距 0.05，d=0.5）→ 大量关联，F 拉满到 20N。
    jpov::MeshData m;
    m.flags = jpov::MeshVertexFlags::kPosition;
    for (int i = 0; i < 8; ++i) {
        m.positions.push_back({0.05f * static_cast<float>(i), 0.0f, 0.0f});
    }
    m.Validate();
    Simulator sim;
    sim.Init(m, 0.5f);
    sim.SetGravity(20.0f);
    sim.SetForceCoeff(Simulator::kMaxForceCoeff);
    sim.SetTotalMass(1.0f);  // 最小质量 → 加速度最大 → 最易发散

    for (int i = 0; i < 600; ++i) sim.Step(Simulator::kDefaultDt);

    for (size_t i = 0; i < sim.sim_point_count(); ++i) {
        for (int c = 0; c < 3; ++c) {
            EXPECT_TRUE(std::isfinite(sim.sim_positions()[i][c]))
                << "点 " << i << " 分量 " << c << " 非有限（截断/积分失稳）";
            EXPECT_TRUE(std::isfinite(sim.sim_velocities()[i][c]));
        }
    }
}

}  // namespace
