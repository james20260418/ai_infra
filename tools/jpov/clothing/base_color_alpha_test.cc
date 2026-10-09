// JPOV 穿衣工具 — base color alpha 归一 纯函数单测（无 GL）
//
// 覆盖：整图归一 / UV 三角形区域归一（只动覆盖到的 texel）/ 选区三角形 UV 收集
// （只收「三顶点都被选中」的三角形）/ 空区域 no-op。

#include "tools/jpov/clothing/base_color_alpha.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "tools/jpov/interface/mesh.h"

namespace {

using jpov::MeshVertexFlags;
using jpov::Vec2f;

// 造一张 W×H 的 RGBA 图：所有像素 rgba=(10,20,30,alpha0)，alpha0 由调用方给。
std::vector<unsigned char> MakeImage(int w, int h, unsigned char alpha0) {
    std::vector<unsigned char> img(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < img.size(); i += 4) {
        img[i + 0] = 10;
        img[i + 1] = 20;
        img[i + 2] = 30;
        img[i + 3] = alpha0;
    }
    return img;
}

unsigned char AlphaAt(const std::vector<unsigned char>& img, int w, int x, int y) {
    return img[(static_cast<size_t>(y) * w + x) * 4 + 3];
}

// 整图归一：所有 texel alpha → 255；颜色通道不变。
TEST(BaseColorAlphaTest, WholeImageSetsAlphaAndKeepsColor) {
    std::vector<unsigned char> img = MakeImage(3, 2, 7);
    jpov::clothing::NormalizeAlphaWholeImage(3, 2, &img);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 3; ++x) {
            const size_t off = (static_cast<size_t>(y) * 3 + x) * 4;
            EXPECT_EQ(img[off + 3], 255);
            EXPECT_EQ(img[off + 0], 10);
            EXPECT_EQ(img[off + 1], 20);
            EXPECT_EQ(img[off + 2], 30);
        }
    }
}

// 区域归一：三角形 UV 覆盖到的 texel 的 alpha → 255，其余**保持原样**。
//   8×8 图；UV 三角形 (0,0)-(0.5,0)-(0,0.5) → texel 空间 (0,0)-(4,0)-(0,4)；
//   覆盖判据（texel 中心在三角形内）= x+y ≤ 3（见下推导），共 10 个 texel。
TEST(BaseColorAlphaTest, UvTriangleSetsAlphaOnlyInside) {
    const int W = 8;
    const int H = 8;
    std::vector<unsigned char> img = MakeImage(W, H, /*alpha0=*/9);
    const std::vector<Vec2f> tris = {
        Vec2f(0.0f, 0.0f), Vec2f(0.5f, 0.0f), Vec2f(0.0f, 0.5f)};
    jpov::clothing::NormalizeAlphaInUvTriangles(tris, W, H, &img);

    // 内部（x+y ≤ 3）→ 255。
    const int inside[][2] = {{0, 0}, {3, 0}, {1, 1}, {2, 1}, {0, 3}, {1, 2}};
    for (const auto& p : inside) {
        EXPECT_EQ(AlphaAt(img, W, p[0], p[1]), 255)
            << "texel (" << p[0] << "," << p[1] << ") 应在三角形内";
    }
    // 外部（x+y ≥ 4）→ 保持 9。
    const int outside[][2] = {{4, 0}, {0, 4}, {7, 7}, {3, 1}, {2, 2}};
    for (const auto& p : outside) {
        EXPECT_EQ(AlphaAt(img, W, p[0], p[1]), 9)
            << "texel (" << p[0] << "," << p[1] << ") 应在三角形外，不该被动";
    }
}

// 空 UV 三角形列表 = no-op（不改任何像素；调用方据此提示，而不是当作"整图"）。
TEST(BaseColorAlphaTest, EmptyUvTrianglesIsNoOp) {
    std::vector<unsigned char> img = MakeImage(4, 4, 5);
    const std::vector<unsigned char> before = img;
    jpov::clothing::NormalizeAlphaInUvTriangles({}, 4, 4, &img);
    EXPECT_EQ(img, before);
}

// 收集「三顶点都被选中」的三角形的 UV。
TEST(BaseColorAlphaTest, CollectSelectedTrianglesUvIndexed) {
    jpov::MeshData m;
    m.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kUV));
    m.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
    m.uvs = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
    m.indices = {0, 1, 2, 1, 2, 3};  // 两个三角形
    m.Validate();

    // 只选 0,1,2 → 只收三角形 0。
    const std::vector<Vec2f> t0 = jpov::clothing::CollectSelectedTrianglesUv(m, {0, 1, 2});
    ASSERT_EQ(t0.size(), 3u);
    EXPECT_EQ(t0[0].x(), 0.0f);
    EXPECT_EQ(t0[1].x(), 1.0f);

    // 只选 1,2,3 → 只收三角形 1。
    const std::vector<Vec2f> t1 = jpov::clothing::CollectSelectedTrianglesUv(m, {1, 2, 3});
    ASSERT_EQ(t1.size(), 3u);
    EXPECT_EQ(t1[0].x(), 1.0f);
    EXPECT_EQ(t1[2].y(), 1.0f);

    // 单选一个顶点（凑不齐三角形）→ 空。
    EXPECT_TRUE(jpov::clothing::CollectSelectedTrianglesUv(m, {0}).empty());
    // 全选 → 两个三角形都收（6 个 UV）。
    EXPECT_EQ(jpov::clothing::CollectSelectedTrianglesUv(m, {0, 1, 2, 3}).size(), 6u);
}

// 无 UV 的网格：无法映射 → 返回空。
TEST(BaseColorAlphaTest, CollectSelectedTrianglesUvWithoutUvIsEmpty) {
    jpov::MeshData m;
    m.flags = MeshVertexFlags::kPosition;
    m.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    m.indices = {0, 1, 2};
    m.Validate();
    EXPECT_TRUE(jpov::clothing::CollectSelectedTrianglesUv(m, {0, 1, 2}).empty());
}

// non-indexed 网格：按连续 3 顶点一三角形。
TEST(BaseColorAlphaTest, CollectSelectedTrianglesUvNonIndexed) {
    jpov::MeshData m;
    m.flags = static_cast<MeshVertexFlags>(
        static_cast<uint8_t>(MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(MeshVertexFlags::kUV));
    m.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {2, 0, 0}, {3, 0, 0}, {2, 1, 0}};
    m.uvs = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f},
             {2.0f, 0.0f}, {3.0f, 0.0f}, {2.0f, 1.0f}};
    m.Validate();

    const std::vector<Vec2f> t = jpov::clothing::CollectSelectedTrianglesUv(m, {3, 4, 5});
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t[0].x(), 2.0f);
    EXPECT_EQ(t[1].x(), 3.0f);
    EXPECT_EQ(t[2].y(), 1.0f);
}

}  // namespace
