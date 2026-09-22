// arap_sim_test — 软体仿真核心的纯 CPU 单测（GL-free）
//
// 覆盖（每个断言都设计成"能区分对错"，不放恒真断言）：
//   1. 焊接：MakeBox 的 24 个渲染顶点（8 个唯一位置）→ 8 个质点。
//   2. Reset 回到 bind pose 且清空速度。
//   3. 自由落体：位移严格等于 PBD 预测步的解析解 g·h²·n(n+1)/2（验证积分方案本身，
//      不是"位移>0"这种谁都能过的弱断言）。
//   4. 地面碰撞（PBD 位置投影）：所有质点被夹到 ≥ ground_y + offset，且确有质点被夹住。
//   5. 🔑 形状恢复力有效：把一个盒子在重力下压扁到地面后撤掉重力——
//      开形状恢复的残余 < 关掉的残余（同一初始态、只改一个变量）。
//   6. 🔑 形状恢复力【旋转不变】：整体绕 Z 轴转 90° 后残余仍 ≈ 0
//      （若把局部旋转 R_i 写成单位阵，这里会立刻爆开）。
//   7. 边界：dt ≤ 0 / 未 Build 就 Step / 写回尺寸不匹配 → crash（不静默）。

#include <cmath>
#include <vector>

#include <gtest/gtest.h>
#include "tools/jpov/demo/arap_sim.h"
#include "tools/jpov/interface/mesh.h"

namespace jpov_arap {
namespace {

using jpov::MeshData;
using jpov::Vec3f;

// 单位立方体（中心在原点）：24 个渲染顶点、8 个唯一位置、12 个三角形。
MeshData MakeCube() {
    return MeshData::MakeBox(/*front_half_width=*/0.5f,
                             /*up_half_width=*/0.5f,
                             /*left_half_width=*/0.5f);
}

// 无任何约束力、无阻尼的 config（用于隔离被测的那一项）。
ArapSimConfig BareConfig() {
    ArapSimConfig cfg;
    cfg.velocity_damping_per_second = 0.0f;
    cfg.enable_shape = false;
    cfg.enable_stretch = false;
    cfg.enable_bend = false;
    cfg.enable_ground = false;
    cfg.shape_restore_rate_per_second = 0.0f;
    cfg.stretch_restore_rate_per_second = 0.0f;
    cfg.bend_restore_rate_per_second = 0.0f;
    cfg.substeps = 10;
    cfg.solver_iterations = 1;
    return cfg;
}

// ── 1. 焊接 ──
TEST(ArapSimTest, WeldMergesCoincidentVertices) {
    const MeshData mesh = MakeCube();
    ArapSim sim;
    sim.Build(mesh, BareConfig());
    EXPECT_EQ(sim.vertex_count(), 24u) << "渲染顶点数是 24（每面独立 4 顶点）";
    EXPECT_EQ(sim.particle_count(), 8u) << "按位置焊接后应为立方体的 8 个角";
    EXPECT_EQ(sim.vertex_positions().size(), 24u);
}

// ── 2. Reset 回到 bind pose ──
TEST(ArapSimTest, ResetRestoresBindPose) {
    const MeshData mesh = MakeCube();
    ArapSimConfig cfg = BareConfig();
    cfg.gravity = Vec3f(0.0f, -9.8f, 0.0f);
    ArapSim sim;
    sim.Build(mesh, cfg);

    for (int i = 0; i < 20; ++i) {
        sim.Step(1.0f / 60.0f);
    }
    // 确实动过了（否则后面的断言恒真）。
    EXPECT_GT(std::fabs(sim.vertex_positions()[0].y() - mesh.positions[0].y()),
              0.1f);

    sim.Reset();
    for (size_t i = 0; i < sim.vertex_positions().size(); ++i) {
        EXPECT_NEAR((sim.vertex_positions()[i] - mesh.positions[i]).Norm(), 0.0f,
                    1e-6f);
    }
    EXPECT_NEAR(sim.shape_residual_rms(), 0.0f, 1e-6f);
}

// ── 3. 自由落体 == 解析解 ──
TEST(ArapSimTest, FreeFallMatchesAnalyticSolution) {
    const MeshData mesh = MakeCube();
    ArapSimConfig cfg = BareConfig();
    cfg.gravity = Vec3f(0.0f, -9.8f, 0.0f);
    cfg.substeps = 10;
    ArapSim sim;
    sim.Build(mesh, cfg);

    // 5 次 Step(0.1s) ⇒ h = 0.01，n = 50 个子步。
    for (int i = 0; i < 5; ++i) {
        sim.Step(0.1f);
    }
    const float g = 9.8f;
    const float h = 0.01f;
    const int n = 50;
    const float expected_drop = g * h * h * static_cast<float>(n * (n + 1)) / 2.0f;
    const float actual_drop = mesh.positions[0].y() - sim.vertex_positions()[0].y();
    EXPECT_NEAR(actual_drop, expected_drop, 1e-3f);

    // 刚体平移：所有顶点下移同样的量（无约束时不该出现相对形变）。
    const float d0 = mesh.positions[0].y() - sim.vertex_positions()[0].y();
    for (size_t i = 1; i < sim.vertex_positions().size(); ++i) {
        const float di = mesh.positions[i].y() - sim.vertex_positions()[i].y();
        EXPECT_NEAR(di, d0, 1e-4f);
    }
}

// ── 4. 地面碰撞 ──
TEST(ArapSimTest, GroundClampsAllParticles) {
    const MeshData mesh = MakeCube();
    ArapSimConfig cfg = BareConfig();
    cfg.gravity = Vec3f(0.0f, -9.8f, 0.0f);
    cfg.enable_ground = true;
    cfg.ground_y = 0.0f;
    cfg.ground_offset = 0.002f;
    ArapSim sim;
    sim.Build(mesh, cfg);

    for (int i = 0; i < 60; ++i) {
        sim.Step(1.0f / 60.0f);
    }
    float min_y = 1e9f;
    for (const Vec3f& p : sim.vertex_positions()) {
        EXPECT_GE(p.y(), cfg.ground_y + cfg.ground_offset - 1e-5f);
        min_y = std::min(min_y, p.y());
    }
    // 确有质点被夹住（否则"全部 ≥ 地面"可能只是"根本没掉下来"而恒真）。
    EXPECT_NEAR(min_y, cfg.ground_y + cfg.ground_offset, 1e-4f);
}

// ── 5. 形状恢复力有效（单变量对照）──
TEST(ArapSimTest, ShapeForceRestoresSquashedBody) {
    const MeshData mesh = MakeCube();

    ArapSimConfig cfg = BareConfig();
    cfg.gravity = Vec3f(0.0f, -9.8f, 0.0f);
    cfg.enable_ground = true;
    cfg.ground_y = 0.0f;          // 立方体 y∈[-0.5,0.5] ⇒ 下半部穿地，被夹扁
    cfg.velocity_damping_per_second = 2.0f;
    cfg.shape_restore_rate_per_second = 30.0f;   // 硬：让“恢复”在少量帧内完成
    cfg.substeps = 4;
    cfg.solver_iterations = 4;

    // 两个仿真器走完全相同的"压扁"过程（只改后面那一个变量）。
    ArapSim with_shape;
    ArapSim without_shape;
    with_shape.Build(mesh, cfg);
    without_shape.Build(mesh, cfg);
    for (int i = 0; i < 30; ++i) {
        with_shape.Step(1.0f / 60.0f);
        without_shape.Step(1.0f / 60.0f);
    }
    const float squashed = with_shape.shape_residual_rms();
    EXPECT_GT(squashed, 0.1f) << "压扁后形变残差应显著（否则本测试无判别力）";
    EXPECT_NEAR(without_shape.shape_residual_rms(), squashed, 1e-4f);

    // 撤掉重力与地面：让形状恢复力成为【唯一】可能改变形变的力量，
    // 排除「地面继续压着导致恢复不了」的混淆（否则本测试无法判别）。
    // 同时把阻尼调到很大，让系统快速静止——否则测到的是"还在振荡中"的中间态，
    // 而不是"形状恢复力最终把形状拉回来"这个结论。
    cfg.gravity = Vec3f(0.0f, 0.0f, 0.0f);
    cfg.enable_ground = false;
    cfg.velocity_damping_per_second = 20.0f;
    cfg.enable_shape = true;
    with_shape.SetConfig(cfg);
    cfg.enable_shape = false;
    without_shape.SetConfig(cfg);

    for (int i = 0; i < 60; ++i) {
        with_shape.Step(1.0f / 60.0f);
        without_shape.Step(1.0f / 60.0f);
    }

    const float restored = with_shape.shape_residual_rms();
    const float not_restored = without_shape.shape_residual_rms();
    EXPECT_LT(restored, 0.2f * squashed)
        << "开了形状恢复：残差应大幅下降（残余 " << restored << " vs 起始 "
        << squashed << "）";
    EXPECT_NEAR(not_restored, squashed, 1e-3f)
        << "没开形状恢复：残差应基本不变（" << not_restored << " vs " << squashed
        << "）";
}

// ── 6. 形状恢复力【旋转不变】──
TEST(ArapSimTest, ShapeResidualIsRotationInvariant) {
    const MeshData mesh = MakeCube();
    ArapSimConfig cfg = BareConfig();
    cfg.enable_shape = true;
    cfg.shape_restore_rate_per_second = 1000.0f;
    ArapSim sim;
    sim.Build(mesh, cfg);

    EXPECT_NEAR(sim.shape_residual_rms(), 0.0f, 1e-6f);

    // 整体绕 Z 轴转 90°，绕原点。形状匹配若正确（R_i 由极分解得到），
    // 残差应仍 ≈ 0；若把 R_i 写成单位阵，残差会≈立方体尺度（0.5 量级）。
    const float kHalfPi = 3.14159265358979323846f / 2.0f;
    sim.RotateCurrentState(/*axis=*/Vec3f(0.0f, 0.0f, 1.0f), kHalfPi,
                           /*pivot=*/Vec3f(0.0f, 0.0f, 0.0f));

    EXPECT_LT(sim.shape_residual_rms(), 1e-3f)
        << "旋转后局部形状应被完整保持（残差必须仍≈0）";

    // 顺带验证"确实转了"：原来的顶点 0 位置应被旋到新位置。
    const Vec3f expected(mesh.positions[0].x() * 0.0f - mesh.positions[0].y() * 1.0f,
                         mesh.positions[0].x() * 1.0f + mesh.positions[0].y() * 0.0f,
                         mesh.positions[0].z());
    EXPECT_NEAR((sim.vertex_positions()[0] - expected).Norm(), 0.0f, 1e-5f);
}

// ── 7. 边界 ──
TEST(ArapSimTest, NonPositiveDtCrashes) {
    ArapSim sim;
    sim.Build(MakeCube(), BareConfig());
    EXPECT_DEATH(sim.Step(0.0f), "dt 必须 > 0");
    EXPECT_DEATH(sim.Step(-1.0f), "dt 必须 > 0");
}

TEST(ArapSimTest, StepBeforeBuildCrashes) {
    ArapSim sim;
    EXPECT_DEATH(sim.Step(1.0f / 60.0f), "尚未 Build");
}

TEST(ArapSimTest, WriteBackSizeMismatchCrashes) {
    ArapSim sim;
    sim.Build(MakeCube(), BareConfig());
    jpov::MeshData wrong;
    wrong.flags = jpov::MeshVertexFlags::kPosition;
    wrong.positions = {{0.0f, 0.0f, 0.0f}};   // 1 个顶点 ≠ 24
    EXPECT_DEATH(sim.WriteBackPositions(&wrong), "顶点数与仿真不一致");
}

TEST(ArapSimTest, BuildWithoutIndicesCrashes) {
    jpov::MeshData mesh;
    mesh.flags = jpov::MeshVertexFlags::kPosition;
    mesh.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    ArapSim sim;
    EXPECT_DEATH(sim.Build(mesh, BareConfig()), "indices 不能为空");
}

}  // namespace
}  // namespace jpov_arap
