// JPOV 穿衣工具 — 衣物变换纯函数单测（无 GL / 无窗口）
//
// 覆盖：
//   1. 步长 / 系数的 clamp 语义（Danis 2026-09-30 指定的三档上限）。
//   2. 欧拉角 → 旋转基 (up, front) 的方向与"逆时针"符号。
//   3. 烘焙：把平移 / 旋转 / 缩放烘进 mesh 顶点后的期望坐标（含法线、切线、uv/索引保留）。

#include "tools/jpov/clothing/clothing_transform.h"

#include <cmath>

#include <gtest/gtest.h>

namespace jpov {
namespace clothing {
namespace {

// 造一个最小三角形 mesh（含位置 / 法线 / 切线 / uv / 索引），供烘焙测试。
MeshData MakeTriangleMesh() {
    MeshData mesh;
    mesh.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(MeshVertexFlags::kUV) |
        static_cast<uint8_t>(MeshVertexFlags::kTangent));
    mesh.positions = {Vec3f(0.0f, 0.0f, 0.0f), Vec3f(1.0f, 0.0f, 0.0f),
                      Vec3f(0.0f, 1.0f, 0.0f)};
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

// ==================== 旋转基 ====================

TEST(ClothingTransformTest, EulerIdentityKeepsAxes) {
    Vec3f up;
    Vec3f front;
    EulerDegToUpFront(Vec3f(0.0f, 0.0f, 0.0f), &up, &front);
    ExpectVec3Near(up, 0.0f, 1.0f, 0.0f);
    ExpectVec3Near(front, 0.0f, 0.0f, 1.0f);
}

TEST(ClothingTransformTest, EulerRotateAboutX) {
    Vec3f up;
    Vec3f front;
    EulerDegToUpFront(Vec3f(90.0f, 0.0f, 0.0f), &up, &front);
    ExpectVec3Near(up, 0.0f, 0.0f, 1.0f);     // +Y → +Z
    ExpectVec3Near(front, 0.0f, -1.0f, 0.0f); // +Z → -Y
}

TEST(ClothingTransformTest, EulerRotateAboutY) {
    Vec3f up;
    Vec3f front;
    EulerDegToUpFront(Vec3f(0.0f, 90.0f, 0.0f), &up, &front);
    ExpectVec3Near(up, 0.0f, 1.0f, 0.0f);   // 绕 Y 转不动 up
    ExpectVec3Near(front, 1.0f, 0.0f, 0.0f); // +Z → +X
}

TEST(ClothingTransformTest, EulerRotateAboutZ) {
    Vec3f up;
    Vec3f front;
    EulerDegToUpFront(Vec3f(0.0f, 0.0f, 90.0f), &up, &front);
    ExpectVec3Near(up, -1.0f, 0.0f, 0.0f);  // +Y → -X
    ExpectVec3Near(front, 0.0f, 0.0f, 1.0f); // 绕 Z 转不动 front
}

TEST(ClothingTransformTest, EulerAppliesXBeforeY) {
    // 先绕 X 90°、再绕 Y 90°：验证复合顺序为 Rx → Ry（若反了结果不同）。
    Vec3f up;
    Vec3f front;
    EulerDegToUpFront(Vec3f(90.0f, 90.0f, 0.0f), &up, &front);
    ExpectVec3Near(up, 1.0f, 0.0f, 0.0f);
    ExpectVec3Near(front, 0.0f, -1.0f, 0.0f);
}

TEST(ClothingTransformTest, EulerAppliesZLast) {
    // 绕 Y 90° 再绕 Z 90°（X=0）：确认 Z 是**最后**施加的（R = Rz·Ry·Rx）。
    // 若 Z 先施加（R = Rx·Ry·Rz），up/front 会是 (0,0,1)/(1,0,0)，与本断言不符。
    Vec3f up;
    Vec3f front;
    EulerDegToUpFront(Vec3f(0.0f, 90.0f, 90.0f), &up, &front);
    ExpectVec3Near(up, -1.0f, 0.0f, 0.0f);
    ExpectVec3Near(front, 0.0f, 1.0f, 0.0f);
}

// ==================== 烘焙 ====================

TEST(ClothingTransformTest, BakeIdentityIsNoOp) {
    const MeshData base = MakeTriangleMesh();
    const MeshData baked = BakeClothMesh(base, ClothTransform{});
    ASSERT_EQ(baked.positions.size(), base.positions.size());
    for (size_t i = 0; i < base.positions.size(); ++i) {
        ExpectVec3Near(baked.positions[i], base.positions[i].x(),
                       base.positions[i].y(), base.positions[i].z());
    }
    ExpectVec3Near(baked.normals[0], 0.0f, 0.0f, 1.0f);
}

TEST(ClothingTransformTest, BakeScaleThenTranslate) {
    ClothTransform t;
    t.offset = Vec3f(1.0f, 2.0f, 3.0f);
    t.scale = 2.0f;  // 无旋转
    const MeshData baked = BakeClothMesh(MakeTriangleMesh(), t);
    // v' = offset + scale·v
    ExpectVec3Near(baked.positions[0], 1.0f, 2.0f, 3.0f);
    ExpectVec3Near(baked.positions[1], 3.0f, 2.0f, 3.0f);
    ExpectVec3Near(baked.positions[2], 1.0f, 4.0f, 3.0f);
    // 法线只转不平移（缩放对单位法线等效恒等）
    ExpectVec3Near(baked.normals[0], 0.0f, 0.0f, 1.0f);
    // 切线按 scale 缩放
    ExpectVec3Near(baked.tangents[0], 2.0f, 0.0f, 0.0f);
}

TEST(ClothingTransformTest, BakeRotationRotatesVertsAndNormals) {
    ClothTransform t;
    t.rotation_deg = Vec3f(90.0f, 0.0f, 0.0f);
    const MeshData baked = BakeClothMesh(MakeTriangleMesh(), t);
    ExpectVec3Near(baked.positions[0], 0.0f, 0.0f, 0.0f);
    ExpectVec3Near(baked.positions[1], 1.0f, 0.0f, 0.0f);
    ExpectVec3Near(baked.positions[2], 0.0f, 0.0f, 1.0f);
    // 法线 +Z → -Y（只转不平移）
    ExpectVec3Near(baked.normals[0], 0.0f, -1.0f, 0.0f);
}

TEST(ClothingTransformTest, BakePreservesUvAndIndices) {
    const MeshData base = MakeTriangleMesh();
    const MeshData baked = BakeClothMesh(base, ClothTransform{});
    ASSERT_EQ(baked.uvs.size(), base.uvs.size());
    ASSERT_EQ(baked.indices.size(), base.indices.size());
    EXPECT_FLOAT_EQ(baked.uvs[1].x(), 1.0f);
    EXPECT_FLOAT_EQ(baked.uvs[2].y(), 1.0f);
    EXPECT_EQ(baked.indices[0], 0u);
    EXPECT_EQ(baked.indices[1], 1u);
    EXPECT_EQ(baked.indices[2], 2u);
}

}  // namespace
}  // namespace clothing
}  // namespace jpov
