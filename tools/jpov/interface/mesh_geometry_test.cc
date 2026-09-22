// mesh_geometry_test — 法线/切线 CPU 重算的纯数学单测（GL-free）
//
// 覆盖（对应 Danis 2026-09-22 定稿的「自推、不焊接」约定）：
//   1. 平面网格：法线 == 面法线（面积加权不改变平面情形）。
//   2. 🔑 硬边反例（直棱柱）：**顶点分裂**时法线按索引各自为面法线（硬边保留）；
//      同一几何若**共享顶点**（= 焊接拓扑），法线被平均（硬边消失）。
//      —— 这是「法线推导不能焊接」这条定的可执行证据。
//   3. 面积加权：同顶点受两个面积悬殊的三角形影响时，法线偏向大三角形。
//   4. 切线：由 UV 推出 ∂P/∂u；已知 UV 下切线 == 期望轴。
//   5. 退化 UV 三角形被跳过（不产生 NaN/Inf，不除零）。
//   6. 位置驱动：位置改变后法线随之改变（这是"每帧必须重算"的本体）。
//   7. flags 契约：未声明 kTangent 时不动 tangents；声明而未声明 kUV 时 crash。

#include <cmath>
#include <vector>

#include <gtest/gtest.h>
#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/interface/mesh_geometry.h"

namespace jpov {
namespace {

// 两个向量夹角（度）。
float AngleDeg(const Vec3f& a, const Vec3f& b) {
    const float d = a.Unit().Dot(b.Unit());
    return std::acos(std::max(-1.0f, std::min(1.0f, d))) * 180.0f / 3.14159265f;
}

// 水平四边形（XZ 平面，法线 +Y），CCW 绕序。返回 4 顶点 + 2 三角形。
MeshData MakeHorizontalQuad() {
    MeshData m;
    m.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(MeshVertexFlags::kUV));
    m.positions = {
        {0.0f, 0.0f, 0.0f},   // 0
        {1.0f, 0.0f, 0.0f},   // 1
        {1.0f, 0.0f, -1.0f},  // 2
        {0.0f, 0.0f, -1.0f},  // 3
    };
    // UV：u 沿 +X，v 沿 −Z（与位置一一对应，便于断言切线方向）。
    m.uvs = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    m.indices = {0, 1, 2, 0, 2, 3};
    return m;
}

// 直棱柱式硬边夹具：两块平面共用一条边（沿 X 轴，从 (0,0,0) 到 (1,0,0)）。
//   · 水平块：XZ 平面，法线 +Y
//   · 竖直块：XY 平面，法线 +Z
// split_verts=true → 共享边处顶点分裂（8 个顶点）→ 硬边
// split_verts=false → 共享边处顶点共用（6 个顶点）→ 焊接拓扑（棱被磨圆）
MeshData MakeHardEdge(bool split_verts) {
    MeshData m;
    m.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal));

    // 水平块顶点：h0(0,0,0) h1(1,0,0) h2(1,0,-1) h3(0,0,-1)
    // 竖直块顶点：v0(0,0,0) v1(1,0,0) v2(1,1,0) v3(0,1,0)
    if (split_verts) {
        m.positions = {
            {0.0f, 0.0f, 0.0f},   // 0 h0（共享边副本 A）
            {1.0f, 0.0f, 0.0f},   // 1 h1（共享边副本 A）
            {1.0f, 0.0f, -1.0f},  // 2 h2
            {0.0f, 0.0f, -1.0f},  // 3 h3
            {0.0f, 0.0f, 0.0f},   // 4 v0（共享边副本 B）
            {1.0f, 0.0f, 0.0f},   // 5 v1（共享边副本 B）
            {1.0f, 1.0f, 0.0f},   // 6 v2
            {0.0f, 1.0f, 0.0f},   // 7 v3
        };
        m.indices = {
            0, 1, 2, 0, 2, 3,  // 水平块（法线 +Y）
            4, 6, 7, 4, 5, 6,  // 竖直块（法线 +Z）
        };
    } else {
        m.positions = {
            {0.0f, 0.0f, 0.0f},   // 0 共享边（焊接）
            {1.0f, 0.0f, 0.0f},   // 1 共享边（焊接）
            {1.0f, 0.0f, -1.0f},  // 2
            {0.0f, 0.0f, -1.0f},  // 3
            {1.0f, 1.0f, 0.0f},   // 4
            {0.0f, 1.0f, 0.0f},   // 5
        };
        m.indices = {
            0, 1, 2, 0, 2, 3,  // 水平块（法线 +Y）
            0, 4, 5, 0, 1, 4,  // 竖直块（法线 +Z）
        };
    }
    m.normals.assign(m.positions.size(), Vec3f(0.0f, 0.0f, 0.0f));
    return m;
}

// ── 1. 平面网格：法线 == 面法线 ──
TEST(MeshGeometryTest, FlatQuadNormalsAreUp) {
    MeshData m = MakeHorizontalQuad();
    RecomputeVertexNormals(&m);
    ASSERT_EQ(m.normals.size(), 4u);
    for (const Vec3f& n : m.normals) {
        EXPECT_NEAR(n.x(), 0.0f, 1e-6f);
        EXPECT_NEAR(n.y(), 1.0f, 1e-6f);   // 绕序保证法线朝 +Y
        EXPECT_NEAR(n.z(), 0.0f, 1e-6f);
    }
}

// ── 2. 硬边反例：分裂保留棱，焊接磨圆棱 ──
TEST(MeshGeometryTest, SplitVerticesPreserveHardEdge) {
    MeshData m = MakeHardEdge(/*split_verts=*/true);
    RecomputeVertexNormals(&m);
    ASSERT_EQ(m.normals.size(), 8u);

    // 共享边副本 A（顶点 0/1）应得水平块的 +Y；副本 B（顶点 4/5）应得竖直块的 +Z。
    EXPECT_NEAR(AngleDeg(m.normals[0], Vec3f(0.0f, 1.0f, 0.0f)), 0.0f, 0.1f);
    EXPECT_NEAR(AngleDeg(m.normals[1], Vec3f(0.0f, 1.0f, 0.0f)), 0.0f, 0.1f);
    EXPECT_NEAR(AngleDeg(m.normals[4], Vec3f(0.0f, 0.0f, 1.0f)), 0.0f, 0.1f);
    EXPECT_NEAR(AngleDeg(m.normals[5], Vec3f(0.0f, 0.0f, 1.0f)), 0.0f, 0.1f);

    // 关键：同一【位置】的两个副本法线【不相同】——这正是硬边被保住的判据。
    // （若实现改成按位置焊接，这里会变成 0°，棱被磨圆。）
    const float between =
        AngleDeg(m.normals[0], m.normals[4]);
    EXPECT_NEAR(between, 90.0f, 0.1f)
        << "分裂顶点的法线必须各自为面法线（相差 90°），不能相等";
}

TEST(MeshGeometryTest, SharedVerticesRoundTheEdge) {
    MeshData m = MakeHardEdge(/*split_verts=*/false);
    RecomputeVertexNormals(&m);
    ASSERT_EQ(m.normals.size(), 6u);

    // 焊接拓扑：共享边顶点（0/1）的法线 = (+Y + +Z) 归一 = 45° 夹在两面之间。
    EXPECT_NEAR(AngleDeg(m.normals[0], Vec3f(0.0f, 1.0f, 0.0f)), 45.0f, 0.1f);
    EXPECT_NEAR(AngleDeg(m.normals[0], Vec3f(0.0f, 0.0f, 1.0f)), 45.0f, 0.1f);
}

// ── 3. 面积加权：法线偏向面积大的面 ──
TEST(MeshGeometryTest, NormalsAreAreaWeighted) {
    // 一个顶点被两个三角形共享：一个小平面（法线 +Y，|cross| = 0.01）和一个大平面
    // （法线 +Z，|cross| = 100）。面积相差 10000 倍 ⇒ 合成法线几乎就是 +Z；
    // 而「等权平均」会是 45°。两个断言放在一起才能区分「加权」与「未加权」。
    MeshData m;
    m.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal));
    m.positions = {
        {0.0f, 0.0f, 0.0f},   // 0 共享顶点
        {1.0f, 0.0f, 0.0f},   // 1 小三角形（XZ 平面，法线 +Y）
        {0.0f, 0.0f, -0.01f}, // 2 小三角形的另一个角
        {0.0f, 10.0f, 0.0f},  // 3 大三角形（XY 平面，法线 +Z）
        {10.0f, 0.0f, 0.0f},  // 4
    };
    m.normals.assign(5, Vec3f(0.0f, 0.0f, 0.0f));
    // 小三角形 (0,1,2)：e1=(1,0,0), e2=(0,0,-0.01) → cross=(0,0.01,0) → 模长 0.01
    // 大三角形 (0,4,3)：e1=(10,0,0), e2=(0,10,0) → cross=(0,0,100) → 模长 100
    m.indices = {0, 1, 2, 0, 4, 3};

    RecomputeVertexNormals(&m);
    const float weighted_dev =
        AngleDeg(m.normals[0], Vec3f(0.0f, 0.0f, 1.0f));
    // 面积加权后：偏向 +Z 的偏差量级 ≈ atan(0.01/100) ≈ 0.006°。
    EXPECT_LT(weighted_dev, 1.0f)
        << "面积加权后应几乎完全被大三角形主导（实测偏差 " << weighted_dev
        << "°）";

    // 对照组：等权（两个单位面法线相加）平均会是 45°。若实现改成等权，
    // 上面的断言会失败、且本断言给出的 45° 就是它本该拿到的值。
    const float unweighted_dev = AngleDeg(
        Vec3f(0.0f, 1.0f, 0.0f) + Vec3f(0.0f, 0.0f, 1.0f),
        Vec3f(0.0f, 0.0f, 1.0f));
    EXPECT_NEAR(unweighted_dev, 45.0f, 0.1f);
    EXPECT_LT(weighted_dev, unweighted_dev) << "加权结果必须比等权更靠向大面";
}

// ── 4. 切线：由 UV 推出 ∂P/∂u ──
TEST(MeshGeometryTest, TangentFollowsUvU) {
    MeshData m = MakeHorizontalQuad();
    const bool any = RecomputeVertexTangents(&m);
    ASSERT_TRUE(any);
    ASSERT_EQ(m.tangents.size(), 4u);
    // u 沿 +X ⇒ 切线 == +X。
    for (const Vec3f& t : m.tangents) {
        EXPECT_NEAR(AngleDeg(t, Vec3f(1.0f, 0.0f, 0.0f)), 0.0f, 0.1f);
    }
}

// ── 5. 退化 UV 三角形被跳过（不产生 NaN）──
TEST(MeshGeometryTest, DegenerateUvSkippedWithoutNaN) {
    MeshData m;
    m.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(MeshVertexFlags::kUV));
    m.positions = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, -1.0f},
    };
    // 三个 UV 共线 ⇒ det = 0 ⇒ 该三角形切线退化，应被跳过。
    m.uvs = {{0.0f, 0.0f}, {0.5f, 0.0f}, {1.0f, 0.0f}};
    m.indices = {0, 1, 2};
    m.normals.assign(3, Vec3f(0.0f, 0.0f, 0.0f));

    const bool any = RecomputeVertexTangents(&m);
    EXPECT_FALSE(any) << "唯一三角形 UV 退化 ⇒ 无有效切线";
    for (const Vec3f& t : m.tangents) {
        EXPECT_TRUE(std::isfinite(t.x()) && std::isfinite(t.y()) &&
                    std::isfinite(t.z()));
        EXPECT_NEAR(t.Norm(), 0.0f, 1e-6f);
    }
    // 法线不受 UV 退化影响（法线只用几何）。
    RecomputeVertexNormals(&m);
    EXPECT_NEAR(m.normals[0].Norm(), 1.0f, 1e-6f);
}

// ── 6. 位置驱动：位置改了，法线必须跟着改 ──
TEST(MeshGeometryTest, NormalsFollowPositions) {
    MeshData m = MakeHorizontalQuad();
    RecomputeVertexNormals(&m);
    EXPECT_NEAR(AngleDeg(m.normals[0], Vec3f(0.0f, 1.0f, 0.0f)), 0.0f, 0.1f);

    // 把顶点 2 沿 +Y 抬高 1：四边形被折起来，顶点 0 的法线应明显偏离 +Y。
    m.positions[2] = Vec3f(1.0f, 1.0f, -1.0f);
    RecomputeVertexNormals(&m);
    const float dev = AngleDeg(m.normals[0], Vec3f(0.0f, 1.0f, 0.0f));
    EXPECT_GT(dev, 5.0f) << "位置变形后法线必须重算（否则光照方向全错）";
}

// ── 7. flags 契约 ──
TEST(MeshGeometryTest, NoTangentFlagLeavesTangentsEmpty) {
    MeshData m = MakeHorizontalQuad();
    m.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(MeshVertexFlags::kUV));
    RecomputeTangentSpace(&m);
    EXPECT_TRUE(m.tangents.empty()) << "未声明 kTangent 不应产出切线";
    ASSERT_EQ(m.normals.size(), 4u);
}

TEST(MeshGeometryTest, TangentWithoutUvCrashes) {
    MeshData m = MakeHorizontalQuad();
    m.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(MeshVertexFlags::kTangent));
    // 声明了 kTangent 却没有 kUV（MeshData 不变式禁止）。
    EXPECT_DEATH(RecomputeTangentSpace(&m), "kUV");
}

TEST(MeshGeometryTest, RecomputeTangentSpaceFillsBoth) {
    MeshData m = MakeHorizontalQuad();
    m.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(MeshVertexFlags::kUV) |
        static_cast<uint8_t>(MeshVertexFlags::kTangent));
    m.normals.assign(4, Vec3f(0.0f, 0.0f, 0.0f));
    m.tangents.assign(4, Vec3f(0.0f, 0.0f, 0.0f));

    RecomputeTangentSpace(&m);
    EXPECT_NEAR(m.normals[0].Norm(), 1.0f, 1e-6f);
    EXPECT_NEAR(m.tangents[0].Norm(), 1.0f, 1e-6f);
    EXPECT_NEAR(AngleDeg(m.tangents[0], Vec3f(1.0f, 0.0f, 0.0f)), 0.0f, 0.1f);
    // N ⊥ T（四边形平面情形应严格垂直）。
    EXPECT_NEAR(m.normals[0].Dot(m.tangents[0]), 0.0f, 1e-5f);
}

}  // namespace
}  // namespace jpov
