// JPOV 穿衣工具 — 衣物 mesh 就地几何操作单测（无 GL / 无窗口）
//
// 覆盖：
//   1. 步长 / 系数的 clamp 语义（Danis 2026-09-30 指定的三档上限）。
//   2. 旋转方向：绕 X/Y/Z 的"逆时针"符号。
//   3. 就地平移 / 旋转 / 缩放后的期望坐标（含法线、切线的处理）。
//   4. 法线重算：面积加权方向 + 退化顶点回退。

#include "tools/jpov/clothing/clothing_transform.h"

#include <cmath>

#include <gtest/gtest.h>

namespace jpov {
namespace clothing {
namespace {

// 造一个最小三角形 mesh（含位置 / 法线 / 切线 / uv / 索引）。
// 三个顶点 (0,0,0) (2,0,0) (0,2,0)：包围盒中心 (1,1,0)，面积法线 +Z。
MeshData MakeTriangleMesh() {
    MeshData mesh;
    mesh.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(MeshVertexFlags::kUV) |
        static_cast<uint8_t>(MeshVertexFlags::kTangent));
    mesh.positions = {Vec3f(0.0f, 0.0f, 0.0f), Vec3f(2.0f, 0.0f, 0.0f),
                      Vec3f(0.0f, 2.0f, 0.0f)};
    mesh.normals = {Vec3f(0.0f, 0.0f, 1.0f), Vec3f(0.0f, 0.0f, 1.0f),
                    Vec3f(0.0f, 0.0f, 1.0f)};
    mesh.tangents = {Vec3f(1.0f, 0.0f, 0.0f), Vec3f(1.0f, 0.0f, 0.0f),
                     Vec3f(1.0f, 0.0f, 0.0f)};
    mesh.uvs = {Vec2f(0.0f, 0.0f), Vec2f(1.0f, 0.0f), Vec2f(0.0f, 1.0f)};
    mesh.indices = {0u, 1u, 2u};
    return mesh;
}

void ExpectVec3Near(const Vec3f& got, float x, float y, float z) {
    EXPECT_NEAR(got.x(), x, 1e-5f);
    EXPECT_NEAR(got.y(), y, 1e-5f);
    EXPECT_NEAR(got.z(), z, 1e-5f);
}

// ==================== clamp ====================

TEST(ClothingTransformTest, ClampTranslationStep) {
    EXPECT_FLOAT_EQ(ClampTransStep(0.3f), 0.3f);
    EXPECT_FLOAT_EQ(ClampTransStep(7.0f), kTransStepAbsMax);
    EXPECT_FLOAT_EQ(ClampTransStep(-100.0f), -kTransStepAbsMax);
    EXPECT_FLOAT_EQ(ClampTransStep(5.0f), 5.0f);
}

TEST(ClothingTransformTest, ClampRotationStep) {
    EXPECT_FLOAT_EQ(ClampRotStep(45.0f), 45.0f);
    EXPECT_FLOAT_EQ(ClampRotStep(120.0f), kRotStepAbsMax);
    EXPECT_FLOAT_EQ(ClampRotStep(-91.0f), -kRotStepAbsMax);
}

TEST(ClothingTransformTest, ClampScaleStep) {
    EXPECT_FLOAT_EQ(ClampScaleStep(0.5f), kScaleStepMin);
    EXPECT_FLOAT_EQ(ClampScaleStep(3.0f), kScaleStepMax);
    EXPECT_FLOAT_EQ(ClampScaleStep(1.4f), 1.4f);
}

TEST(ClothingTransformTest, ClampClothScale) {
    EXPECT_FLOAT_EQ(ClampClothScale(-1.0f), kClothScaleMin);
    EXPECT_FLOAT_EQ(ClampClothScale(100.0f), kClothScaleMax);
    EXPECT_FLOAT_EQ(ClampClothScale(1.5f), 1.5f);
}

// ==================== 旋转方向 ====================

TEST(ClothingTransformTest, RotateAboutXIsCounterClockwise) {
    // +Y → +Z（从 +X 看向原点逆时针）。
    ExpectVec3Near(RotateClothX(Vec3f(0.0f, 1.0f, 0.0f), 90.0f), 0.0f, 0.0f, 1.0f);
    ExpectVec3Near(RotateClothX(Vec3f(0.0f, 0.0f, 1.0f), 90.0f), 0.0f, -1.0f, 0.0f);
}

TEST(ClothingTransformTest, RotateAboutYIsCounterClockwise) {
    // +Z → +X，且 +Y 不动。
    ExpectVec3Near(RotateClothY(Vec3f(0.0f, 0.0f, 1.0f), 90.0f), 1.0f, 0.0f, 0.0f);
    ExpectVec3Near(RotateClothY(Vec3f(0.0f, 1.0f, 0.0f), 90.0f), 0.0f, 1.0f, 0.0f);
}

TEST(ClothingTransformTest, RotateAboutZIsCounterClockwise) {
    // +X → +Y，且 +Z 不动。
    ExpectVec3Near(RotateClothZ(Vec3f(1.0f, 0.0f, 0.0f), 90.0f), 0.0f, 1.0f, 0.0f);
    ExpectVec3Near(RotateClothZ(Vec3f(0.0f, 0.0f, 1.0f), 90.0f), 0.0f, 0.0f, 1.0f);
}

// ==================== 就地操作 ====================

TEST(ClothingTransformTest, BoundsCenter) {
    ExpectVec3Near(MeshBoundsCenter(MakeTriangleMesh()), 1.0f, 1.0f, 0.0f);
}

TEST(ClothingTransformTest, TranslateInPlaceMovesPositionsOnly) {
    MeshData mesh = MakeTriangleMesh();
    TranslateMeshInPlace(&mesh, Vec3f(1.0f, -2.0f, 3.0f));
    ExpectVec3Near(mesh.positions[0], 1.0f, -2.0f, 3.0f);
    ExpectVec3Near(mesh.positions[1], 3.0f, -2.0f, 3.0f);
    ExpectVec3Near(mesh.positions[2], 1.0f, 0.0f, 3.0f);
    // 法线 / 切线不受平移影响。
    ExpectVec3Near(mesh.normals[0], 0.0f, 0.0f, 1.0f);
    ExpectVec3Near(mesh.tangents[0], 1.0f, 0.0f, 0.0f);
}

TEST(ClothingTransformTest, RotateInPlaceAboutBoundsCenter) {
    MeshData mesh = MakeTriangleMesh();  // 中心 (1,1,0)
    RotateMeshInPlace(&mesh, /*axis=*/2, 90.0f, MeshBoundsCenter(mesh));  // 绕 Z
    // (0,0,0) 相对中心 (-1,-1,0) → (1,-1,0) → 世界 (2,0,0)。
    ExpectVec3Near(mesh.positions[0], 2.0f, 0.0f, 0.0f);
    // (2,0,0) 相对中心 (1,-1,0) → (1,1,0) → 世界 (2,2,0)。
    ExpectVec3Near(mesh.positions[1], 2.0f, 2.0f, 0.0f);
    // 法线 +Z 绕 Z 不动。
    ExpectVec3Near(mesh.normals[0], 0.0f, 0.0f, 1.0f);
    // 切线 +X 绕 Z 90 → +Y。
    ExpectVec3Near(mesh.tangents[0], 0.0f, 1.0f, 0.0f);
}

TEST(ClothingTransformTest, ScaleInPlaceAboutBoundsCenter) {
    MeshData mesh = MakeTriangleMesh();  // 中心 (1,1,0)
    ScaleMeshInPlace(&mesh, 2.0f, MeshBoundsCenter(mesh));
    // (0,0,0) → 中心 + 2*(-1,-1,0) = (-1,-1,0)。
    ExpectVec3Near(mesh.positions[0], -1.0f, -1.0f, 0.0f);
    // (2,0,0) → 中心 + 2*(1,-1,0) = (3,-1,0)。
    ExpectVec3Near(mesh.positions[1], 3.0f, -1.0f, 0.0f);
    // 均匀缩放：单位法线不缩放。
    ExpectVec3Near(mesh.normals[0], 0.0f, 0.0f, 1.0f);
    // 切线按 factor 缩放（不归一化）。
    ExpectVec3Near(mesh.tangents[0], 2.0f, 0.0f, 0.0f);
}

TEST(ClothingTransformTest, InPlaceOpsPreserveUvAndIndices) {
    MeshData mesh = MakeTriangleMesh();
    TranslateMeshInPlace(&mesh, Vec3f(1.0f, 0.0f, 0.0f));
    RotateMeshInPlace(&mesh, 1, 30.0f, MeshBoundsCenter(mesh));
    ScaleMeshInPlace(&mesh, 1.5f, MeshBoundsCenter(mesh));
    ASSERT_EQ(mesh.uvs.size(), 3u);
    ASSERT_EQ(mesh.indices.size(), 3u);
    EXPECT_FLOAT_EQ(mesh.uvs[1].x(), 1.0f);
    EXPECT_FLOAT_EQ(mesh.uvs[2].y(), 1.0f);
    EXPECT_EQ(mesh.indices[0], 0u);
    EXPECT_EQ(mesh.indices[2], 2u);
}

// ==================== 法线重算 ====================

TEST(ClothingTransformTest, RecomputeNormalsPointsOutOfPlane) {
    MeshData mesh = MakeTriangleMesh();
    mesh.normals[0] = Vec3f(1.0f, 0.0f, 0.0f);  // 故意写错
    mesh.normals[1] = Vec3f(1.0f, 0.0f, 0.0f);
    mesh.normals[2] = Vec3f(1.0f, 0.0f, 0.0f);
    RecomputeVertexNormals(&mesh);
    ExpectVec3Near(mesh.normals[0], 0.0f, 0.0f, 1.0f);
    ExpectVec3Near(mesh.normals[1], 0.0f, 0.0f, 1.0f);
    ExpectVec3Near(mesh.normals[2], 0.0f, 0.0f, 1.0f);
}

TEST(ClothingTransformTest, RecomputeNormalsDegenerateFallsBack) {
    // 退化三角形（共线）：面积叉积为 0 → 顶点法线回退为 (0,1,0)。
    MeshData mesh;
    mesh.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal));
    mesh.positions = {Vec3f(0.0f, 0.0f, 0.0f), Vec3f(1.0f, 0.0f, 0.0f),
                      Vec3f(2.0f, 0.0f, 0.0f)};
    mesh.normals = {Vec3f(9.0f, 9.0f, 9.0f), Vec3f(9.0f, 9.0f, 9.0f),
                    Vec3f(9.0f, 9.0f, 9.0f)};
    mesh.indices = {0u, 1u, 2u};
    RecomputeVertexNormals(&mesh);
    ExpectVec3Near(mesh.normals[0], 0.0f, 1.0f, 0.0f);
}

TEST(ClothingTransformTest, RecomputeNormalsAreaWeighted) {
    // 两个共边三角形：大三角形（面积大）应对共享顶点法线贡献更多。
    // 顶点布局：v0=(0,0,0) v1=(1,0,0) v2=(0,2,0) v3=(0,0,4)。
    // 三角形 A = (v0,v1,v2)：法线 +Z（大）; 三角形 B = (v0,v3,v1)：法线 +Y（小）。
    MeshData mesh;
    mesh.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal));
    mesh.positions = {Vec3f(0.0f, 0.0f, 0.0f), Vec3f(1.0f, 0.0f, 0.0f),
                      Vec3f(0.0f, 2.0f, 0.0f), Vec3f(0.0f, 0.0f, 4.0f)};
    mesh.normals = {Vec3f(0.0f, 1.0f, 0.0f), Vec3f(0.0f, 1.0f, 0.0f),
                    Vec3f(0.0f, 1.0f, 0.0f), Vec3f(0.0f, 0.0f, 0.0f)};
    // A: (v0,v1,v2) 面积 = 1；B: (v0,v3,v1) 面积 = 2。B 更大。
    mesh.indices = {0u, 1u, 2u, 0u, 3u, 1u};
    RecomputeVertexNormals(&mesh);
    // 顶点 0 只被 A、B 共享：贡献 = 2*areaA*(0,0,1) + 2*areaB*...(B 的法线方向)。
    // B = (v0,v3,v1)：ab = v3-v0 = (0,0,4)，ac = v1-v0 = (1,0,0)，
    // n = ab × ac = (0*0-4*0, 4*1-0*0, 0*0-0*1) = (0,4,0)。
    // A 的 n = ab × ac = (1,0,0)×(0,2,0) = (0,0,2)。
    // 顶点 0 累加 = (0,4,0)+(0,0,2) = (0,4,2) → 归一化 (0, 0.894427, 0.447214)。
    ExpectVec3Near(mesh.normals[0], 0.0f, 0.8944272f, 0.4472136f);
    // 顶点 2 只属于 A → (0,0,1)。
    ExpectVec3Near(mesh.normals[2], 0.0f, 0.0f, 1.0f);
}

TEST(ClothingTransformTest, RecomputeNormalsSkipsMeshWithoutNormals) {
    MeshData mesh;
    mesh.flags = MeshVertexFlags::kPosition;  // 无 kNormal
    mesh.positions = {Vec3f(0.0f, 0.0f, 0.0f), Vec3f(1.0f, 0.0f, 0.0f),
                      Vec3f(0.0f, 1.0f, 0.0f)};
    mesh.indices = {0u, 1u, 2u};
    RecomputeVertexNormals(&mesh);  // 不崩、不改动
    EXPECT_TRUE(mesh.normals.empty());
}

}  // namespace
}  // namespace clothing
}  // namespace jpov
