// JPOV 软体仿真器 — 纯 CPU 单测（GL-free）
//
// 锁定的是**主入口的契约**，而不是某条物理规律（M0 阶段尚无物理）：
//   - Init 后：Step 输出 == 输入网格（M0 恒等桩的确切语义）
//   - 时钟正确推进：time == Σdt，step_count == 调用次数
//   - 拓扑/属性在 Step 后原样保留（索引、法线、UV 不被动）
//   - Reset 回到绑定姿态并清零计数
//   - 输入 mesh 不被修改（调用方的网格是只读的）
//   - Bounds() 正确且是纯查询（多调几次结果一致，不影响时钟）
//
// ⚠️ 负向验证过：把 Bounds 改成"只取首顶点"，本测试立即 FAIL（非恒真）。

#include "tools/jpov/soft_mesh_simulator/soft_mesh_simulator.h"

#include <cmath>

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

// M0 契约：Step 是恒等映射 —— 输出顶点与输入逐分量相同。
TEST(SoftMeshSimulatorTest, StepIsIdentityInM0) {
    const jpov::MeshData in = MakeTri();
    Simulator sim;
    sim.Init(in);

    const jpov::MeshData out = sim.Step(Simulator::kDefaultDt);

    ASSERT_EQ(out.positions.size(), in.positions.size());
    for (size_t i = 0; i < in.positions.size(); ++i) {
        for (int c = 0; c < 3; ++c) {
            EXPECT_FLOAT_EQ(out.positions[i][c], in.positions[i][c])
                << "顶点 " << i << " 分量 " << c << " 在 M0 应保持不变";
        }
    }
}

// 拓扑与其它顶点属性也必须原样保留（物理不应擅自改索引/法线/UV）。
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

// Reset：回到绑定姿态 + 清零时间/步数。
TEST(SoftMeshSimulatorTest, ResetRestoresBindPoseAndClock) {
    const jpov::MeshData in = MakeTri();
    Simulator sim;
    sim.Init(in);

    for (int i = 0; i < 10; ++i) sim.Step(Simulator::kDefaultDt);
    ASSERT_GT(sim.time(), 0.0);

    sim.Reset();

    EXPECT_DOUBLE_EQ(sim.time(), 0.0);
    EXPECT_EQ(sim.step_count(), 0u);
    const jpov::MeshData& cur = sim.mesh();
    ASSERT_EQ(cur.positions.size(), in.positions.size());
    for (size_t i = 0; i < in.positions.size(); ++i) {
        EXPECT_FLOAT_EQ(cur.positions[i][0], in.positions[i][0]);
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

}  // namespace
