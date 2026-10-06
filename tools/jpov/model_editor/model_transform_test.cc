// JPOV 模型编辑器 — 就地变换 单测（纯函数）
//
// 覆盖：平移 / 绕 X/Y/Z 轴的旋转方向（右手逆时针）/ 均匀缩放，以及法线、切线、
// 包围盒的行为。

#include <cmath>

#include <gtest/gtest.h>

#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/model_editor/model_transform.h"

namespace jpov {
namespace model_editor {
namespace {

using jpov::MeshData;
using jpov::MeshVertexFlags;
using jpov::Vec2f;
using jpov::Vec3f;

MeshData MakePointMesh(const Vec3f& p) {
    MeshData m;
    m.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kNormal) |
        static_cast<uint8_t>(MeshVertexFlags::kUV) |
        static_cast<uint8_t>(MeshVertexFlags::kTangent));
    m.positions = {p};
    m.normals = {Vec3f(0, 0, 1)};
    m.uvs = {Vec2f(0, 0)};
    m.tangents = {Vec3f(1, 0, 0)};
    m.indices = {};  // non-indexed 单顶点（Validate 允许 positions 非空即可）
    m.Validate();
    return m;
}

}  // namespace

TEST(ModelTransform, TranslateMovesPositionsNotNormals) {
    MeshData m = MakePointMesh({1, 2, 3});
    TranslateMesh(&m, {0.5f, -1.0f, 2.0f});
    EXPECT_FLOAT_EQ(m.positions[0].x(), 1.5f);
    EXPECT_FLOAT_EQ(m.positions[0].y(), 1.0f);
    EXPECT_FLOAT_EQ(m.positions[0].z(), 5.0f);
    EXPECT_FLOAT_EQ(m.normals[0].z(), 1.0f);  // 法线不受平移影响
}

TEST(ModelTransform, RotateZ90IsCcw) {
    // 绕 Z 逆时针 90°：(1,0,0) → (0,1,0)。
    MeshData m = MakePointMesh({1, 0, 0});
    RotateMesh(&m, /*axis*/ 2, 90.0f, {0, 0, 0});
    EXPECT_NEAR(m.positions[0].x(), 0.0f, 1e-5f);
    EXPECT_NEAR(m.positions[0].y(), 1.0f, 1e-5f);
    EXPECT_NEAR(m.positions[0].z(), 0.0f, 1e-5f);
    // 法线 (0,0,1) 绕 Z 不变。
    EXPECT_NEAR(m.normals[0].z(), 1.0f, 1e-5f);
}

TEST(ModelTransform, RotateX90) {
    // 绕 X 逆时针 90°：(0,1,0) → (0,0,1)。
    MeshData m = MakePointMesh({0, 1, 0});
    RotateMesh(&m, /*axis*/ 0, 90.0f, {0, 0, 0});
    EXPECT_NEAR(m.positions[0].y(), 0.0f, 1e-5f);
    EXPECT_NEAR(m.positions[0].z(), 1.0f, 1e-5f);
}

TEST(ModelTransform, RotateY90) {
    // 绕 Y 逆时针 90°：(0,0,1) → (1,0,0)。
    MeshData m = MakePointMesh({0, 0, 1});
    RotateMesh(&m, /*axis*/ 1, 90.0f, {0, 0, 0});
    EXPECT_NEAR(m.positions[0].x(), 1.0f, 1e-5f);
    EXPECT_NEAR(m.positions[0].z(), 0.0f, 1e-5f);
}

TEST(ModelTransform, RotateAroundPivot) {
    // 绕 pivot=(1,0,0) 绕 Z 逆时针 180°：(2,0,0) → (0,0,0)。
    MeshData m = MakePointMesh({2, 0, 0});
    RotateMesh(&m, /*axis*/ 2, 180.0f, {1, 0, 0});
    EXPECT_NEAR(m.positions[0].x(), 0.0f, 1e-5f);
    EXPECT_NEAR(m.positions[0].y(), 0.0f, 1e-5f);
}

TEST(ModelTransform, RotateNormalAndTangent) {
    // 法线 (0,0,1) 绕 X 逆时针 90° → (0,-1,0)？ 验证方向量确实被旋转。
    // 绕 X：(y,z) → (y c - z s, y s + z c)；法线 (0,0,1) → (0,-1,0)。
    MeshData m = MakePointMesh({0, 0, 0});
    RotateMesh(&m, /*axis*/ 0, 90.0f, {0, 0, 0});
    EXPECT_NEAR(m.normals[0].y(), -1.0f, 1e-5f);
    EXPECT_NEAR(m.normals[0].z(), 0.0f, 1e-5f);
    // 切线 (1,0,0) 绕 X 不变。
    EXPECT_NEAR(m.tangents[0].x(), 1.0f, 1e-5f);
}

TEST(ModelTransform, ScaleAroundPivot) {
    MeshData m = MakePointMesh({2, 0, 0});
    ScaleMesh(&m, /*factor*/ 2.0f, {0, 0, 0});
    EXPECT_FLOAT_EQ(m.positions[0].x(), 4.0f);
    // 法线方向不因均匀缩放改变。
    EXPECT_FLOAT_EQ(m.normals[0].z(), 1.0f);
    // 切线按 factor 缩放（不归一化约定）。
    EXPECT_FLOAT_EQ(m.tangents[0].x(), 2.0f);

    ScaleMesh(&m, 0.5f, {1, 0, 0});  // 绕 pivot=1 缩一半：4 → 1 + (4-1)*0.5 = 2.5
    EXPECT_FLOAT_EQ(m.positions[0].x(), 2.5f);
}

TEST(ModelTransform, ComputeBounds) {
    MeshData m;
    m.flags = MeshVertexFlags::kPosition;
    m.positions = {{-1, 2, 0}, {3, -1, 5}, {0, 0, 0}};
    m.Validate();
    const MeshBounds b = ComputeMeshBounds(m);
    EXPECT_TRUE(b.valid);
    EXPECT_FLOAT_EQ(b.min.x(), -1.0f);
    EXPECT_FLOAT_EQ(b.min.y(), -1.0f);
    EXPECT_FLOAT_EQ(b.min.z(), 0.0f);
    EXPECT_FLOAT_EQ(b.max.x(), 3.0f);
    EXPECT_FLOAT_EQ(b.max.y(), 2.0f);
    EXPECT_FLOAT_EQ(b.max.z(), 5.0f);
}

}  // namespace model_editor
}  // namespace jpov
