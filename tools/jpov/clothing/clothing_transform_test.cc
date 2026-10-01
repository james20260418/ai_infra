// JPOV 穿衣工具 — 面板 clamp / 法线重算单测（无 GL / 无窗口）
//
// 覆盖：
//   1. 步长 / 系数的 clamp 语义（Danis 2026-09-30 指定的三档上限）。
//   2. 法线重算：面积加权方向 + 退化顶点回退 + 无法线网格跳过。
//
// 说明：平移 / 旋转 / 缩放已改为**作用于仿真器状态**（见 soft_mesh_simulator_test.cc 的
//   ApplyTranslation / ApplyRotation / ApplyScaling 测试），不在本文件。

#include "tools/jpov/clothing/clothing_transform.h"

#include <cmath>

#include <gtest/gtest.h>

namespace jpov {
namespace clothing {
namespace {

void ExpectVec3Near(const Vec3f& got, float x, float y, float z) {
    EXPECT_NEAR(got.x(), x, 1e-5f);
    EXPECT_NEAR(got.y(), y, 1e-5f);
    EXPECT_NEAR(got.z(), z, 1e-5f);
}

// 造一个最小三角形 mesh（含位置 / 法线 / 索引）：(0,0,0)(2,0,0)(0,2,0)，面积法线 +Z。
MeshData MakeTriangleMesh() {
    MeshData mesh;
    mesh.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal));
    mesh.positions = {Vec3f(0.0f, 0.0f, 0.0f), Vec3f(2.0f, 0.0f, 0.0f),
                      Vec3f(0.0f, 2.0f, 0.0f)};
    mesh.normals = {Vec3f(0.0f, 0.0f, 1.0f), Vec3f(0.0f, 0.0f, 1.0f),
                    Vec3f(0.0f, 0.0f, 1.0f)};
    mesh.indices = {0u, 1u, 2u};
    return mesh;
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

// ==================== 法线重算 ====================

TEST(ClothingTransformTest, RecomputeNormalsPointsOutOfPlane) {
    MeshData mesh = MakeTriangleMesh();
    mesh.normals[0] = Vec3f(1.0f, 0.0f, 0.0f);  // 故意写错
    mesh.normals[1] = Vec3f(0.0f, 1.0f, 0.0f);
    mesh.normals[2] = Vec3f(1.0f, 1.0f, 0.0f);
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
    // 两个共边三角形：面积大的对共享顶点法线贡献更多。
    // 顶点：v0=(0,0,0) v1=(1,0,0) v2=(0,2,0) v3=(0,0,4)。
    // A=(v0,v1,v2): n=(0,0,2)（面积 1）; B=(v0,v3,v1): n=(0,4,0)（面积 2）。
    MeshData mesh;
    mesh.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal));
    mesh.positions = {Vec3f(0.0f, 0.0f, 0.0f), Vec3f(1.0f, 0.0f, 0.0f),
                      Vec3f(0.0f, 2.0f, 0.0f), Vec3f(0.0f, 0.0f, 4.0f)};
    mesh.normals = {Vec3f(0.0f, 1.0f, 0.0f), Vec3f(0.0f, 1.0f, 0.0f),
                    Vec3f(0.0f, 1.0f, 0.0f), Vec3f(0.0f, 0.0f, 0.0f)};
    mesh.indices = {0u, 1u, 2u, 0u, 3u, 1u};
    RecomputeVertexNormals(&mesh);
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
