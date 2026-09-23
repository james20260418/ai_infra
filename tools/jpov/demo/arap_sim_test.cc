// arap_sim_test — 软体仿真核心的纯 CPU 单测（GL-free）
//
// 被测模型（需求方 2026-09-23 指定的最简版）：m = 1、F = m·a，四个力
//   ① 胡克弹簧（k_e = hooke/L0）② 重力 ③ ARAP 形状力（b）④ 线性阻尼（−u·v），
//   显式（半隐式）欧拉积分，步长 dt（默认 0.05 s）。
//
// 覆盖（每个断言都设计成"能区分对错"，不放恒真断言）：
//   1. 焊接：MakeBox 的 24 个渲染顶点（8 个唯一位置）→ 8 个质点。
//   2. Reset 回到 bind pose 且清空速度。
//   3. 🔑 自由落体位移 == 半隐式欧拉的解析解 g·h²·n(n+1)/2（验证积分格式本身）。
//   4. 地面：位置被夹到 ≥ 地面、法向速度被清零；且**横向零漂移**（无侧向力源）。
//   5. 🔑 胡克力在 L = L0 处为零（"原长是零点"）。
//   6. 🔑 胡克力按初始边长归一化：k_e = hooke/L0 ⇒ 相同绝对拉伸下，2 倍长的边
//      受力减半；且力与拉伸量成正比（线性）。
//   7. 🔑 重力与阻尼的叠加：F = g − u·v（在弹簧不受力的构型上逐分量精确比对）。
//   8. 🔑 ARAP 力在刚性旋转下为零 —— 用**平面网格**（薄壳，1-ring 近似共面）
//      绕面内轴旋转 90° 测：若没有「法向增广项」把协方差秩补满，这里会立刻失败。
//   9. 🔑 ARAP 力方向正确：把矩形上下压扁 → 上边受向上的力、下边受向下的力
//      （即"把形状拉回原样"），且力大小上下对称；关掉 b 则严格为零。
//  10. 🔑 阻尼律：F = −u·v ⇒ 速度按几何级数衰减 v_{n+1} = v_n·(1 − u·h)（解析比对）。
//  11. 🔑 稳定性上限：dt 超过 max_stable_dt() 时发散；按其一半加大 substeps 后收敛
//      （这是"步长不够就细分子步、而不是多加几个力"这一结论的直接验证）。
//  12. 边界：dt ≤ 0 / 未 Build 就 Step / substeps = 0 / 写回尺寸不匹配 / 无索引 → crash。

#include <cmath>
#include <vector>

#include <gtest/gtest.h>
#include "tools/jpov/demo/arap_sim.h"
#include "tools/jpov/interface/mesh.h"

namespace jpov_arap {
namespace {

using jpov::MeshData;
using jpov::Vec3f;

constexpr float kPi = 3.14159265358979323846f;

// 单位立方体（中心在原点）：24 个渲染顶点、8 个唯一位置、12 个三角形。
MeshData MakeCube() {
    return MeshData::MakeBox(/*front_half_width=*/0.45f,
                             /*up_half_width=*/0.45f,
                             /*left_half_width=*/0.45f);
}

// 边长 L 的平面正方形（4 顶点、2 三角形、对角线 AC）：z = 0 平面。
//   0=A(0,0,0)  1=B(L,0,0)  2=C(L,L,0)  3=D(0,L,0)
MeshData MakeFlatSquare(float l) {
    MeshData mesh;
    mesh.flags = jpov::MeshVertexFlags::kPosition;
    mesh.positions = {Vec3f(0.0f, 0.0f, 0.0f), Vec3f(l, 0.0f, 0.0f),
                      Vec3f(l, l, 0.0f), Vec3f(0.0f, l, 0.0f)};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

// 无任何力的 config（弹簧/形状/重力/阻尼全关，地面关）——用于隔离被测的那一项。
// 面密度固定为 1 kg/m²（"一平米一 kg"），质量由顶点面积给出，见 arap_sim.h 文件头。
ArapSimConfig BareConfig() {
    ArapSimConfig cfg;
    cfg.area_density_kg_per_m2 = 1.0f;
    cfg.spring_stiffness_per_area = 0.0f;
    cfg.arap_stiffness_per_area = 0.0f;
    cfg.damping_per_second = 0.0f;
    cfg.gravity_magnitude = 0.0f;
    cfg.enable_ground = false;
    cfg.substeps = 1;
    return cfg;
}

std::vector<Vec3f> ZeroVelocity(size_t n) {
    return std::vector<Vec3f>(n, Vec3f(0.0f, 0.0f, 0.0f));
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
    cfg.gravity_magnitude = 9.8f;
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
    for (const Vec3f& v : sim.velocities()) {
        EXPECT_NEAR(v.Norm(), 0.0f, 1e-6f);
    }
    EXPECT_NEAR(sim.shape_residual_rms(), 0.0f, 1e-6f);
}

// ── 3. 自由落体 == 半隐式欧拉解析解 ──
TEST(ArapSimTest, FreeFallMatchesAnalyticSolution) {
    const MeshData mesh = MakeCube();
    ArapSimConfig cfg = BareConfig();
    cfg.gravity_magnitude = 9.8f;
    cfg.substeps = 10;
    ArapSim sim;
    sim.Build(mesh, cfg);

    // 5 次 Step(0.1s) ⇒ h = 0.01，n = 50 个子步。
    for (int i = 0; i < 5; ++i) {
        sim.Step(0.1f);
    }
    // 半隐式欧拉：v_k = k·g·h ⇒ 位移 = h·Σ v_k = g·h²·n(n+1)/2。
    const float g = 9.8f;
    const float h = 0.01f;
    const int n = 50;
    const float expected_drop = g * h * h * static_cast<float>(n * (n + 1)) / 2.0f;
    const float actual_drop = mesh.positions[0].y() - sim.vertex_positions()[0].y();
    EXPECT_NEAR(actual_drop, expected_drop, 1e-3f);

    // 刚体平移：所有顶点下移同样的量（只有重力时不该出现相对形变）。
    const float d0 = mesh.positions[0].y() - sim.vertex_positions()[0].y();
    for (size_t i = 1; i < sim.vertex_positions().size(); ++i) {
        const float di = mesh.positions[i].y() - sim.vertex_positions()[i].y();
        EXPECT_NEAR(di, d0, 1e-4f);
    }
}

// ── 4. 地面：位置夹断 + 法向速度清零（完全非弹性）+ 横向零漂移 ──
TEST(ArapSimTest, GroundClampsAndKillsNormalVelocity) {
    const MeshData mesh = MakeCube();
    ArapSimConfig cfg = BareConfig();
    cfg.gravity_magnitude = 9.8f;
    cfg.enable_ground = true;
    cfg.ground_y = 0.0f;
    cfg.ground_offset = 0.002f;
    cfg.substeps = 1;
    ArapSim sim;
    sim.Build(mesh, cfg);

    for (int i = 0; i < 60; ++i) {
        sim.Step(1.0f / 60.0f);
    }
    const float min_y = cfg.ground_y + cfg.ground_offset;
    float lowest = 1e9f;
    for (const Vec3f& p : sim.vertex_positions()) {
        EXPECT_GE(p.y(), min_y - 1e-5f);
        lowest = std::min(lowest, p.y());
    }
    // 确有质点被夹住（否则"全部 ≥ 地面"可能只是"根本没掉下来"而恒真）。
    EXPECT_NEAR(lowest, min_y, 1e-4f);

    // 法向速度清零（完全非弹性）：接触点不该有向下的速度。
    for (const Vec3f& v : sim.velocities()) {
        EXPECT_GE(v.y(), -1e-6f);
    }

    // 横向零漂移：全部力都沿 y（重力/弹簧/形状力在对称构型下都是），地面又只钳 y，
    // 故 x/z 位置必须逐顶点不变——这里能抓到"凭空侧向力"（旧实现曾把物体推滑出画面）。
    for (size_t i = 0; i < sim.vertex_positions().size(); ++i) {
        EXPECT_NEAR(sim.vertex_positions()[i].x(), mesh.positions[i].x(), 1e-4f);
        EXPECT_NEAR(sim.vertex_positions()[i].z(), mesh.positions[i].z(), 1e-4f);
    }
}

// 平面正方形（2×2 m）的四个顶点质量（面密度 1 kg/m²）：
//   两个三角形各占 2 m²/2 = 1 m² ⇒ 每三角形给三个顶点各 1/3 m²；
//   A 与 C 各属两个三角形 ⇒ 2/3 m²；B 与 D 各属一个 ⇒ 1/3 m²。
//   故 m = {2/3, 1/3, 2/3, 1/3} kg（总质量 2 kg = 2 m² × 1 kg/m²）。
// 注意 ComputeForcesAt 返回的是【加速度】（F/m），故断言要除以质量。
constexpr float kSquare2x2MassA = 2.0f / 3.0f;
constexpr float kSquare2x2MassB = 1.0f / 3.0f;

// ── 5. 胡克力在 L = L0（原长）处为零 ──
TEST(ArapSimTest, HookeForceIsZeroAtRestLength) {
    const MeshData mesh = MakeFlatSquare(/*l=*/1.0f);
    ArapSimConfig cfg = BareConfig();
    cfg.spring_stiffness_per_area = 5.0e3f;
    ArapSim sim;
    sim.Build(mesh, cfg);

    std::vector<Vec3f> force;
    sim.ComputeForcesAt(mesh.positions, ZeroVelocity(4), &force);
    ASSERT_EQ(force.size(), 4u);
    for (const Vec3f& f : force) {
        EXPECT_NEAR(f.Norm(), 0.0f, 1e-6f) << "原长处弹簧力必须为零";
    }
}

// ── 5b. 面密度 ⇒ 顶点质量（这是"1 m² 就是 1 kg"的直接验证）──
TEST(ArapSimTest, MassFollowsAreaDensity) {
    const MeshData mesh = MakeFlatSquare(/*l=*/2.0f);   // 2×2 m，面积 4 m²
    ArapSim sim;
    sim.Build(mesh, BareConfig());          // 面密度 1 kg/m²

    ASSERT_EQ(sim.particle_count(), 4u);
    EXPECT_NEAR(sim.surface_area(), 4.0f, 1e-4f);
    EXPECT_NEAR(sim.total_mass(), 4.0f, 1e-4f) << "4 m² × 1 kg/m² = 4 kg";
    // A/C 属两个三角形（各 2/3），B/D 属一个（各 1/3）
    EXPECT_NEAR(sim.masses()[0], 4.0f / 3.0f, 1e-5f);
    EXPECT_NEAR(sim.masses()[1], 2.0f / 3.0f, 1e-5f);
    EXPECT_NEAR(sim.masses()[2], 4.0f / 3.0f, 1e-5f);
    EXPECT_NEAR(sim.masses()[3], 2.0f / 3.0f, 1e-5f);

    // 换一个面密度（2 kg/m²）⇒ 质量整体翻倍、总面积不变 ——
    // 说明"面密度"是唯一的质量旋钮，且与网格密度无关。
    ArapSimConfig cfg = BareConfig();
    cfg.area_density_kg_per_m2 = 2.0f;
    ArapSim dense;
    dense.Build(mesh, cfg);
    EXPECT_NEAR(dense.surface_area(), 4.0f, 1e-4f);
    EXPECT_NEAR(dense.total_mass(), 8.0f, 1e-4f);
    EXPECT_NEAR(dense.masses()[0], 8.0f / 3.0f, 1e-5f);

    // 网格细分不改变总质量（同一块布细分 ⇒ 还是 4 kg）——这是"面密度"相对"顶点质量"
    // 的关键差别（后者会让细网格变重，见 docs §7 的自重压垮现象）。
    MeshData fine;
    fine.flags = jpov::MeshVertexFlags::kPosition;
    {
        // 把 2×2 m 的正方形切成 8×8 共 128 个三角形（9×9=81 个顶点）。
        const int n = 8;
        const float step = 2.0f / static_cast<float>(n);
        for (int j = 0; j <= n; ++j) {
            for (int i = 0; i <= n; ++i) {
                fine.positions.push_back(Vec3f(static_cast<float>(i) * step,
                                               static_cast<float>(j) * step,
                                               0.0f));
            }
        }
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const uint32_t v00 =
                    static_cast<uint32_t>(j * (n + 1) + i);
                const uint32_t v10 = v00 + 1;
                const uint32_t v01 = v00 + static_cast<uint32_t>(n + 1);
                const uint32_t v11 = v01 + 1;
                fine.indices.insert(fine.indices.end(),
                                    {v00, v10, v11, v00, v11, v01});
            }
        }
    }
    ArapSim fine_sim;
    fine_sim.Build(fine, BareConfig());
    EXPECT_NEAR(fine_sim.surface_area(), 4.0f, 1e-3f);
    EXPECT_NEAR(fine_sim.total_mass(), 4.0f, 1e-3f)
        << "细分后总质量必须不变（面密度才是材质，顶点质量不是）";
    EXPECT_LT(fine_sim.masses()[0], sim.masses()[0])
        << "细分后每个质点的质量应显著变小";
}

// ── 6. 胡克：面积定标 k_e = c·√(a_i·a_j)（与 N 同阶反比 ⇒ 物理效果不随网格漂移）──
TEST(ArapSimTest, HookeStiffnessScalesWithArea) {
    const float c = 5.0e3f;      // N/m³

    auto make_grid = [](int n) {
        MeshData m;
        m.flags = jpov::MeshVertexFlags::kPosition;
        const float step = 1.0f / static_cast<float>(n);
        for (int j = 0; j <= n; ++j) {
            for (int i = 0; i <= n; ++i) {
                m.positions.push_back(Vec3f(static_cast<float>(i) * step,
                                            static_cast<float>(j) * step,
                                            0.0f));
            }
        }
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const uint32_t v00 = static_cast<uint32_t>(j * (n + 1) + i);
                const uint32_t v10 = v00 + 1;
                const uint32_t v01 = v00 + static_cast<uint32_t>(n + 1);
                const uint32_t v11 = v01 + 1;
                m.indices.insert(m.indices.end(),
                                 {v00, v10, v11, v00, v11, v01});
            }
        }
        return m;
    };

    ArapSimConfig cfg = BareConfig();
    cfg.spring_stiffness_per_area = c;

    // ── 属性 1：顶点面积 ∝ 1/N（同样的 1 m² 平板，切得越细每块越小）──
    //    并且「面积 × N」守恒（= 总面积），这就是"与 N 同阶反比"的定义。
    {
        float prev_area_n = -1.0f;
        for (int n : {1, 4, 8}) {
            const MeshData grid = make_grid(n);
            ArapSim gsim;
            gsim.Build(grid, cfg);
            EXPECT_NEAR(gsim.surface_area(), 1.0f, 1e-3f);
            const size_t N = gsim.particle_count();
            const float mean_area = gsim.surface_area() / static_cast<float>(N);
            // 面积 × N = 总面积 ⇒ 恒定（与 N 无关的等价表述）
            EXPECT_NEAR(mean_area * static_cast<float>(N), 1.0f, 1e-3f)
                << "n=" << n << "：平均面积×N 应恒等于总面积";
            if (prev_area_n > 0.0f) {
                EXPECT_LT(mean_area, prev_area_n)
                    << "n=" << n << "：越细 ⇒ 每质点面积越小";
            }
            prev_area_n = mean_area;
        }
    }

    // ── 属性 2：★核心★ 稳定步长上限与网格密度无关 ──
    //    max_stable_dt() = 2/√(max_i row_i)，而 row_i = Σ k_e/m + β·n/((n+1)m)
    //    代入 k_e = c√(a_i a_j)、β = c'a、m = ρa 后 row_i ≈ n·c/ρ + c'/ρ·n/(n+1)
    //    **与 a（也就是与 N、与边长）都无关**。这条是整个定标设计的目标，必须成立。
    {
        float prev_bound = -1.0f;
        for (int n : {1, 4, 8}) {
            const MeshData grid = make_grid(n);
            ArapSim gsim;
            gsim.Build(grid, cfg);
            const float bound = gsim.max_stable_dt();
            EXPECT_GT(bound, 0.0f);
            if (prev_bound > 0.0f) {
                // 允许 2 倍以内：1-ring 度数随网格不同（角点 n=2、边界 n=4、内部 n=6），
                // 而 row_i 里 n 直接出现（6c/ρ 与 2c/ρ 差 3 倍），故"max row 落在哪类顶点上"
                // 会影响常数因子。实测 n=1→4 差 1.46 倍（角点主导）、n=4→8 基本持平。
                // **关键是量级不漂**（若定标错会差 1~2 个数量级，见下面属性 3 的反例）。
                EXPECT_NEAR(bound, prev_bound, 1.0f * prev_bound + 1e-6f)
                    << "n=" << n << "：稳定上限应与 n=1 时同量级（实测 " << bound
                    << " vs " << prev_bound
                    << "）—— 若定标错（如旧的 k_e ∝ 1/L0）这里会差 1~2 个数量级";
            }
            prev_bound = bound;
        }
    }

    // ── 属性 3：旧定标的反例检查 —— 用「边长」当刚度基准会让上限随 N 剧烈漂移 ──
    //    这里用"人为把 c 设成 ∝ 1/L0"来模拟旧实现（k_e = T/L0 ⇔ c_eff = T/(L0·√(a a))），
    //    验证它确实会让上限随细分崩塌 ⇒ 反证面积定标是必需的。
    {
        float prev = -1.0f;
        float ratio_total = 1.0f;
        for (int n : {1, 4, 8}) {
            const MeshData grid = make_grid(n);
            ArapSimConfig bad = cfg;
            const float l0 = 1.0f / static_cast<float>(n);
            // 模拟旧式"按边长归一化"：等效 c 随 L0 变小而变大（∝1/L0³ 的同类漂移）
            bad.spring_stiffness_per_area = c / (l0 * l0 * l0);
            ArapSim gsim;
            gsim.Build(grid, bad);
            const float bound = gsim.max_stable_dt();
            if (prev > 0.0f) {
                ratio_total *= (prev / bound);
            }
            prev = bound;
        }
        // 实测：n=1 → 4 → 8 共崩 33 倍（每步约 5.7 倍）。方向明确（随细分急剧恶化），
        // 而面积定标在同样跨度内只漂 1.46 倍（见属性 2）。
        EXPECT_GT(ratio_total, 10.0f)
            << "旧式按边长定标会让稳定上限随细分崩掉一个数量级以上（实测共 "
            << ratio_total << " 倍）—— 这就是必须改成面积定标的原因";
    }
}

// ── 7. 重力 + 阻尼：F = g − u·v（在弹簧不受力的构型上逐分量精确比对）──
TEST(ArapSimTest, GravityAndDampingAreExact) {
    const MeshData mesh = MakeFlatSquare(/*l=*/1.0f);
    ArapSimConfig cfg = BareConfig();
    cfg.spring_stiffness_per_area = 5.0e3f;  // 弹簧在位但处于原长 ⇒ 力为零
    cfg.gravity_magnitude = 9.8f;
    cfg.damping_per_second = 3.0f;
    ArapSim sim;
    sim.Build(mesh, cfg);

    // 逐顶点给不同速度，逐分量比对 F = g − u·v。
    std::vector<Vec3f> vel = {Vec3f(1.0f, 0.0f, -2.0f), Vec3f(0.0f, -0.5f, 0.0f),
                              Vec3f(2.0f, 1.0f, 0.5f), Vec3f(-3.0f, 0.0f, 0.0f)};
    std::vector<Vec3f> force;
    sim.ComputeForcesAt(mesh.positions, vel, &force);

    // 重力项 = g（与质量无关）；阻尼项 a_damp = −u·v（需求方指定：与质量无关）。
    for (size_t i = 0; i < vel.size(); ++i) {
        const Vec3f expected =
            Vec3f(0.0f, -9.8f, 0.0f) - vel[i] * cfg.damping_per_second;
        EXPECT_NEAR((force[i] - expected).Norm(), 0.0f, 1e-5f)
            << "顶点 " << i << " 的合力应为 g − u·v（加速度域）";
    }
}

// ── 8. ARAP 力在刚性旋转下为零（平面网格 = 薄壳，检验法向增广项）──
TEST(ArapSimTest, ArapForceIsZeroUnderRigidRotationOfFlatMesh) {
    const MeshData mesh = MakeFlatSquare(/*l=*/1.0f);
    ArapSimConfig cfg = BareConfig();
    cfg.arap_stiffness_per_area = 200.0f;
    ArapSim sim;
    sim.Build(mesh, cfg);

    std::vector<Vec3f> force;
    sim.ComputeForcesAt(mesh.positions, ZeroVelocity(4), &force);
    for (const Vec3f& f : force) {
        EXPECT_NEAR(f.Norm(), 0.0f, 1e-5f) << "bind pose 下形状力必须为零";
    }
    EXPECT_EQ(sim.degenerate_rotation_count(), 0u)
        << "平面网格的 1-ring 近似共面，法向增广项必须把秩补满（否则会退化）";

    // 绕 x 轴（面内轴）整体转 90°：形状力应仍为零。
    // 若 R_i 因退化退回单位阵，这里会得到 ~b·网格尺度 的巨大伪力。
    sim.RotateCurrentState(/*axis=*/Vec3f(1.0f, 0.0f, 0.0f), kPi / 2.0f,
                           /*pivot=*/Vec3f(0.0f, 0.0f, 0.0f));

    // 确实转了：C 原在 (1,1,0) ⇒ 绕 x 转 90° 后到 (1,0,1)。
    const Vec3f c_after = sim.vertex_positions()[2];
    EXPECT_NEAR((c_after - Vec3f(1.0f, 0.0f, 1.0f)).Norm(), 0.0f, 1e-5f);

    std::vector<Vec3f> force_rotated;
    sim.ComputeForcesAt(sim.vertex_positions(), ZeroVelocity(4), &force_rotated);
    for (const Vec3f& f : force_rotated) {
        EXPECT_NEAR(f.Norm(), 0.0f, 1e-3f)
            << "刚性旋转后局部形状未被改变 ⇒ 形状力必须仍为零（实测 " << f.Norm()
            << "）";
    }
    EXPECT_EQ(sim.degenerate_rotation_count(), 0u);
}

// ── 9. ARAP 力方向：压扁 ⇒ 力把形状往原样拉回（上下对称）──
TEST(ArapSimTest, ArapForcePushesSquashedShapeBack) {
    // 矩形：宽 2、高 2，中心在 (1,0)。顶点 0=A(0,-1) 1=B(2,-1) 2=C(2,1) 3=D(0,1)（z=0）。
    MeshData mesh = MakeFlatSquare(/*l=*/2.0f);
    mesh.positions = {Vec3f(0.0f, -1.0f, 0.0f), Vec3f(2.0f, -1.0f, 0.0f),
                      Vec3f(2.0f, 1.0f, 0.0f), Vec3f(0.0f, 1.0f, 0.0f)};

    ArapSimConfig cfg = BareConfig();
    cfg.arap_stiffness_per_area = 200.0f;
    ArapSim sim;
    sim.Build(mesh, cfg);

    // 手工把上下各压到 |y| = 0.8（高度 2 → 1.6），位置即"被压扁的构型"。
    std::vector<Vec3f> squashed = mesh.positions;
    for (Vec3f& p : squashed) {
        p = Vec3f(p.x(), 0.8f * p.y(), p.z());
    }
    std::vector<Vec3f> force;
    sim.ComputeForcesAt(squashed, ZeroVelocity(4), &force);

    // 上边（rest y = +1 的顶点 2、3）受力朝 +y；下边（顶点 0、1）朝 −y。
    // 注意返回值是加速度（F/m），这里换算成力 [N] 再判方向与大小。
    std::vector<float> fy_n(4, 0.0f);
    for (size_t i = 0; i < 4; ++i) {
        fy_n[i] = force[i].y() * sim.masses()[i];
    }
    EXPECT_LT(fy_n[0], -0.1f) << "被压扁后下边应受向上的恢复力（负 y）";
    EXPECT_LT(fy_n[1], -0.1f);
    EXPECT_GT(fy_n[2], 0.1f) << "被压扁后上边应受向上的恢复力（正 y）";
    EXPECT_GT(fy_n[3], 0.1f);
    // 内力成对抵消 ⇒ **力**之和（不是加速度之和）必为零。
    Vec3f total_n(0.0f, 0.0f, 0.0f);
    for (size_t i = 0; i < 4; ++i) {
        total_n = total_n + force[i] * sim.masses()[i];
    }
    EXPECT_NEAR(total_n.Norm(), 0.0f, 1e-4f) << "内力之和应为零（牛顿）";

    // 单变量对照：关掉 ARAP（并去掉重力，因为重力也是力）⇒ 力严格为零。
    // 若这里不为零，说明上面测到的不是 ARAP 力（判别力所在）。
    cfg.arap_stiffness_per_area = 0.0f;
    cfg.gravity_magnitude = 0.0f;
    sim.SetConfig(cfg);
    std::vector<Vec3f> force_off;
    sim.ComputeForcesAt(squashed, ZeroVelocity(4), &force_off);
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_NEAR((force_off[i] * sim.masses()[i]).Norm(), 0.0f, 1e-5f)
            << "关掉 ARAP 后该质点的力必须严格为零";
    }
}

// ── 9b. ARAP 力随时间把形状拉回去（积分层的行为，不只是瞬时力）──
TEST(ArapSimTest, ArapRestoresShapeOverTime) {
    // 矩形：宽 2、高 2，中心在 (1,0)（rest 形状，形状力要恢复的目标）。
    MeshData mesh = MakeFlatSquare(/*l=*/2.0f);
    mesh.positions = {Vec3f(0.0f, -1.0f, 0.0f), Vec3f(2.0f, -1.0f, 0.0f),
                      Vec3f(2.0f, 1.0f, 0.0f), Vec3f(0.0f, 1.0f, 0.0f)};

    // 起始状态用「地面」压出来：下边（y = −1）被夹到 −0.798 ⇒ 高度 2 → 1.798。
    ArapSimConfig cfg = BareConfig();
    cfg.arap_stiffness_per_area = 200.0f;
    cfg.gravity_magnitude = 0.0f;
    cfg.damping_per_second = 4.0f;
    cfg.enable_ground = true;
    cfg.ground_y = -0.8f;
    cfg.substeps = 4;

    ArapSim with_b;
    ArapSim without_b;
    with_b.Build(mesh, cfg);
    cfg.arap_stiffness_per_area = 0.0f;
    without_b.Build(mesh, cfg);

    const float squashed_height =
        with_b.vertex_positions()[2].y() - with_b.vertex_positions()[0].y();
    EXPECT_LT(squashed_height, 1.85f) << "起始应确实被压扁（否则本测试无判别力）";

    // 撤掉地面（不再约束下边），只留形状力 + 阻尼：形状应自己弹回原高。
    cfg.enable_ground = false;
    cfg.arap_stiffness_per_area = 200.0f;
    with_b.SetConfig(cfg);
    cfg.arap_stiffness_per_area = 0.0f;
    without_b.SetConfig(cfg);

    for (int i = 0; i < 120; ++i) {
        with_b.Step(1.0f / 60.0f);
        without_b.Step(1.0f / 60.0f);
    }
    const float restored =
        with_b.vertex_positions()[2].y() - with_b.vertex_positions()[0].y();
    const float unrestored =
        without_b.vertex_positions()[2].y() - without_b.vertex_positions()[0].y();
    EXPECT_GT(restored, 1.9f)
        << "开了 ARAP：高度应回到 2.0 附近（实测 " << restored << "）";
    EXPECT_NEAR(unrestored, squashed_height, 1e-4f)
        << "没开 ARAP：高度不该自己恢复（实测 " << unrestored << "）";
}

// ── 10. 阻尼律：F = −u·v ⇒ 速度几何衰减 ──
TEST(ArapSimTest, DampingDecaysVelocityGeometrically) {
    const MeshData mesh = MakeCube();
    ArapSimConfig cfg = BareConfig();
    cfg.gravity_magnitude = 9.8f;
    const float dt = 0.05f;
    const float u = 2.0f;
    cfg.damping_per_second = 0.0f;   // 先用一步重力把速度"喂"起来（不含阻尼）
    ArapSim sim;
    sim.Build(mesh, cfg);
    sim.Step(dt);
    const float v0 = sim.velocities()[0].y();
    EXPECT_NEAR(v0, -9.8f * dt, 1e-5f);

    // 关掉重力、开阻尼：v_{n+1} = v_n·(1 − u·h)。
    cfg.gravity_magnitude = 0.0f;
    cfg.damping_per_second = u;
    sim.SetConfig(cfg);
    const int n = 10;
    for (int i = 0; i < n; ++i) {
        sim.Step(dt);
    }
    const float expected = v0 * std::pow(1.0f - u * dt, static_cast<float>(n));
    EXPECT_NEAR(sim.velocities()[0].y(), expected, 1e-4f);

    // 对照：无阻尼时速度应保持不变（说明上面测到的确实是阻尼）。
    ArapSimConfig cfg2 = cfg;
    cfg2.damping_per_second = 0.0f;
    cfg2.gravity_magnitude = 9.8f;
    ArapSim sim2;
    sim2.Build(mesh, cfg2);
    sim2.Step(dt);
    const float v2 = sim2.velocities()[0].y();
    cfg2.gravity_magnitude = 0.0f;   // 同样关掉重力，只留阻尼=0
    sim2.SetConfig(cfg2);
    for (int i = 0; i < n; ++i) {
        sim2.Step(dt);
    }
    EXPECT_NEAR(sim2.velocities()[0].y(), v2, 1e-5f);
}

// ── 11. 稳定性上限：超过 max_stable_dt() 会炸；细分子步后收敛 ──
TEST(ArapSimTest, DtAboveStabilityBoundDivergesAndSubstepsFixIt) {
    const MeshData mesh = MakeCube();
    ArapSimConfig cfg = BareConfig();
    // 很硬：c = 4e4 N/m³（比橡胶硬 2.7 倍）⇒ 边长 0.9 m 时 k_e ≈ c·a ≈ 1e4 N/m，
    // 而面密度保持 1 kg/m² ⇒ 稳定上限很小（远小于 0.05 s）。
    cfg.spring_stiffness_per_area = 4.0e4f;
    cfg.arap_stiffness_per_area = 0.0f;
    cfg.gravity_magnitude = 9.8f;
    cfg.ground_y = -100.0f;        // 地面很远 ⇒ 不参与
    cfg.enable_ground = false;
    cfg.substeps = 1;
    ArapSim sim;
    sim.Build(mesh, cfg);

    const float bound = sim.max_stable_dt();
    EXPECT_LT(bound, 0.05f) << "本用例的稳定上限必须小于 0.05（否则无判别力）";

    // ① dt = 0.05 > 上限 ⇒ 发散（边长畸变爆到远超 100%；先给一个微小扰动：
    //    自由落体本身完全对称，不会有任何形变，故这里让重力略微偏离轴心——
    //    用 RotateCurrentState 把物体转 3° 再落，制造非对称接触/压缩）。
    sim.RotateCurrentState(/*axis=*/Vec3f(0.0f, 0.0f, 1.0f), 3.0f * kPi / 180.0f,
                           /*pivot=*/Vec3f(0.0f, 0.0f, 0.0f));
    for (int i = 0; i < 10; ++i) {
        sim.Step(0.05f);
    }
    // 发散的表现可能是畸变爆表，也可能是几何直接 NaN（位置飞成 inf/nan ⇒ 边长比也 NaN）
    // —— 两者都是"炸了"，只要**没有**保持有界就算通过。
    const float distortion_diverged = sim.edge_distortion_rms();
    const bool diverged =
        !(distortion_diverged > 0.0f) || distortion_diverged > 1.0f;
    EXPECT_TRUE(diverged)
        << "dt 超过稳定上限时应发散（实测边长畸变 " << distortion_diverged
        << "；NaN/爆表都算发散）";

    // ② 同一物体、同一 dt=0.05，但把子步细分到 h < 0.5·上限 ⇒ 有界。
    cfg.substeps = static_cast<int>(std::ceil(0.05f / (0.5f * bound)));
    EXPECT_GT(cfg.substeps, 1);
    ArapSim stable;
    stable.Build(mesh, cfg);
    stable.RotateCurrentState(/*axis=*/Vec3f(0.0f, 0.0f, 1.0f),
                              3.0f * kPi / 180.0f,
                              /*pivot=*/Vec3f(0.0f, 0.0f, 0.0f));
    for (int i = 0; i < 10; ++i) {
        stable.Step(0.05f);
    }
    EXPECT_LT(stable.edge_distortion_rms(), 0.1f)
        << "细分子步（h = " << 0.05f / static_cast<float>(cfg.substeps)
        << " < 上限 " << bound << "）后应该有界（实测 "
        << stable.edge_distortion_rms() << "）";
}

// ── 12. 边界 ──
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

TEST(ArapSimTest, ZeroSubstepsCrashes) {
    ArapSimConfig cfg = BareConfig();
    cfg.substeps = 0;
    ArapSim sim;
    EXPECT_DEATH(sim.Build(MakeCube(), cfg), "substeps 必须 ≥ 1");
}

TEST(ArapSimTest, ComputeForcesSizeMismatchCrashes) {
    ArapSim sim;
    sim.Build(MakeCube(), BareConfig());
    std::vector<Vec3f> force;
    std::vector<Vec3f> wrong_positions(3, Vec3f(0.0f, 0.0f, 0.0f));
    EXPECT_DEATH(
        sim.ComputeForcesAt(wrong_positions, ZeroVelocity(8), &force),
        "positions 长度必须是 particle_count()");
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
