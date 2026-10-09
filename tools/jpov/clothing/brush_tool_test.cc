// JPOV 3D 画笔（选区）工具 + 相机投影 纯函数单测 —— 无 GL。
//
// 覆盖：相机基正交性 / 像素↔世界往返一致 / 画笔半径的「像素→世界」换算与
// 「屏幕圆」等价性 / 画笔生命周期（激活结束清空）/ 选点（射线垂距）/ 选区去重。

#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "tools/jpov/clothing/brush_tool.h"
#include "tools/jpov/clothing/camera_projection.h"

namespace jpov {
namespace clothing {
namespace {

constexpr double kPi = 3.14159265358979323846;

// 一个固定相机（eye=(0,0,5) 看原点，fov 60，1280×720）——前向 -Z、右 +X、上 +Y。
CameraBasis FixedBasis() {
    return MakeCameraBasis(/*eye*/ {0.0f, 0.0f, 5.0f},
                           /*target*/ {0.0f, 0.0f, 0.0f},
                           /*world_up*/ {0.0f, 1.0f, 0.0f},
                           /*fov_deg*/ 60.0, /*w*/ 1280.0f, /*h*/ 720.0f);
}

TEST(CameraProjectionTest, BasisIsOrthonormalAndAxesAligned) {
    const CameraBasis c = FixedBasis();
    // 前向 = -Z、右 = +X、上 = +Y（本机位）。
    EXPECT_NEAR(c.forward.x(), 0.0f, 1e-6);
    EXPECT_NEAR(c.forward.z(), -1.0f, 1e-6);
    EXPECT_NEAR(c.right.x(), 1.0f, 1e-6);
    EXPECT_NEAR(c.up.y(), 1.0f, 1e-6);
    // 正交单位。
    auto dot = [](const Vec3f& a, const Vec3f& b) {
        return a.x() * b.x() + a.y() * b.y() + a.z() * b.z();
    };
    EXPECT_NEAR(dot(c.forward, c.forward), 1.0f, 1e-6);
    EXPECT_NEAR(dot(c.right, c.right), 1.0f, 1e-6);
    EXPECT_NEAR(dot(c.up, c.up), 1.0f, 1e-6);
    EXPECT_NEAR(dot(c.forward, c.right), 0.0f, 1e-6);
    EXPECT_NEAR(dot(c.forward, c.up), 0.0f, 1e-6);
    EXPECT_NEAR(dot(c.right, c.up), 0.0f, 1e-6);
    EXPECT_NEAR(c.tan_half_fov, std::tan(30.0 * kPi / 180.0), 1e-9);
    EXPECT_NEAR(c.aspect, 1280.0 / 720.0, 1e-9);
}

TEST(CameraProjectionTest, CenterPixelRayIsForwardAndCenterProjectionIsCenter) {
    const CameraBasis c = FixedBasis();
    const CameraRay r = RayFromPixel(c, 640.0f, 360.0f);
    EXPECT_NEAR(r.dir.x(), 0.0f, 1e-6);
    EXPECT_NEAR(r.dir.y(), 0.0f, 1e-6);
    EXPECT_NEAR(r.dir.z(), -1.0f, 1e-6);

    float px = 0.0f;
    float py = 0.0f;
    float depth = 0.0f;
    ASSERT_TRUE(ProjectToScreen(c, /*world*/ {0.0f, 0.0f, 0.0f}, &px, &py, &depth));
    EXPECT_NEAR(px, 640.0f, 1e-3);
    EXPECT_NEAR(py, 360.0f, 1e-3);
    EXPECT_NEAR(depth, 5.0f, 1e-6);
}

TEST(CameraProjectionTest, PixelToRayToProjectionRoundTrips) {
    const CameraBasis c = FixedBasis();
    const float pixels[][2] = {{640.0f, 360.0f}, {300.0f, 200.0f}, {1000.0f, 600.0f},
                               {80.0f, 40.0f},   {1200.0f, 700.0f}};
    for (const auto& p : pixels) {
        const CameraRay r = RayFromPixel(c, p[0], p[1]);
        // 沿射线取一个点（深度 10），投回屏幕应回到原像素（< 0.5px）。
        const Vec3f world(r.origin.x() + 10.0f * r.dir.x(),
                          r.origin.y() + 10.0f * r.dir.y(),
                          r.origin.z() + 10.0f * r.dir.z());
        float px = 0.0f;
        float py = 0.0f;
        ASSERT_TRUE(ProjectToScreen(c, world, &px, &py, nullptr));
        EXPECT_NEAR(px, p[0], 0.5f) << "px for pixel (" << p[0] << "," << p[1] << ")";
        EXPECT_NEAR(py, p[1], 0.5f) << "py for pixel (" << p[0] << "," << p[1] << ")";
    }
}

// 画笔半径「像素 → 世界」换算：世界半径 = R·2·tan(half)·depth/screen_h；
// 并证明「射线垂距 ≤ 世界半径」⟺「屏幕投影到鼠标的像素距离 ≤ R」。
TEST(BrushToolTest, RadiusConversionMatchesScreenCircle) {
    const CameraBasis c = FixedBasis();
    const CameraRay ray = RayFromPixel(c, 640.0f, 360.0f);  // 中心，dir=-Z
    const float R = 100.0f;

    // 深度 5 处，世界半径 = 100*2*tan30*5/720 ≈ 0.80185 m。
    const double expect_world_r = static_cast<double>(R) * 2.0 * c.tan_half_fov * 5.0 /
                                  720.0;
    EXPECT_NEAR(expect_world_r, 0.80185, 1e-3);

    // 恰在世界半径内的点 → 选中；恰在外 → 不选。
    const float inside_x = static_cast<float>(expect_world_r * 0.95);
    const float outside_x = static_cast<float>(expect_world_r * 1.05);
    EXPECT_TRUE(VertexInBrush(/*v*/ {inside_x, 0.0f, 0.0f}, ray.origin, ray.dir, R,
                              720.0f, c.tan_half_fov));
    EXPECT_FALSE(VertexInBrush(/*v*/ {outside_x, 0.0f, 0.0f}, ray.origin, ray.dir, R,
                               720.0f, c.tan_half_fov));

    // 与「屏幕投影距离 ≤ R」等价（取若干垂距采样，逐点核对两种判定一致）。
    for (float x = -2.0f; x <= 2.0f; x += 0.1f) {
        const Vec3f v(x, 0.0f, 0.0f);
        const bool in_brush =
            VertexInBrush(v, ray.origin, ray.dir, R, 720.0f, c.tan_half_fov);
        float sx = 0.0f;
        float sy = 0.0f;
        ASSERT_TRUE(ProjectToScreen(c, v, &sx, &sy, nullptr));
        const double screen_dist =
            std::sqrt((sx - 640.0) * (sx - 640.0) + (sy - 360.0) * (sy - 360.0));
        EXPECT_EQ(in_brush, screen_dist <= static_cast<double>(R) + 1e-3)
            << "v.x=" << x << " perp-screen-dist=" << screen_dist;
    }
}

TEST(BrushToolTest, BehindCameraIsNeverSelected) {
    const CameraBasis c = FixedBasis();
    const CameraRay ray = RayFromPixel(c, 640.0f, 360.0f);
    // 相机后方（z=8 > eye.z=5）→ 沿 -Z 射线 t<0 → 不选。
    EXPECT_FALSE(VertexInBrush(/*v*/ {0.0f, 0.0f, 8.0f}, ray.origin, ray.dir, 200.0f,
                               720.0f, c.tan_half_fov));
}

TEST(BrushToolTest, LifecycleEndClearsSelection) {
    BrushTool b;
    b.EnsurePrimitives(2);
    EXPECT_FALSE(b.active());
    b.SetActive(true);
    EXPECT_TRUE(b.active());

    const std::vector<Vec3f> pts = {{0.0f, 0.0f, 0.0f}};
    const CameraRay ray = RayFromPixel(FixedBasis(), 640.0f, 360.0f);
    EXPECT_EQ(b.PaintAlongRay(/*prim*/ 0, pts, ray.origin, ray.dir, 100.0f, 720.0f,
                              FixedBasis().tan_half_fov),
              1u);
    EXPECT_TRUE(b.IsSelected(0, 0));
    EXPECT_EQ(b.selected_count(), 1u);

    b.SetActive(false);  // 生命周期结束 → 清空
    EXPECT_FALSE(b.active());
    EXPECT_EQ(b.selected_count(), 0u);
    EXPECT_FALSE(b.IsSelected(0, 0));
}

TEST(BrushToolTest, PaintSelectsByRayAndDedupes) {
    BrushTool b;
    b.EnsurePrimitives(1);
    b.SetActive(true);
    const CameraBasis c = FixedBasis();
    const CameraRay ray = RayFromPixel(c, 640.0f, 360.0f);  // 中心，-Z
    // 三个点：两个在射线附近（选中），一个远离（不选）。
    const std::vector<Vec3f> pts = {
        {0.0f, 0.0f, 0.0f},    // index 0：在射线上 → 选
        {0.3f, 0.0f, -1.0f},   // index 1：近射线（在半径内）→ 选
        {5.0f, 0.0f, 0.0f},    // index 2：远离 → 不选
    };
    const size_t added =
        b.PaintAlongRay(0, pts, ray.origin, ray.dir, 100.0f, 720.0f, c.tan_half_fov);
    EXPECT_EQ(added, 2u);
    EXPECT_TRUE(b.IsSelected(0, 0));
    EXPECT_TRUE(b.IsSelected(0, 1));
    EXPECT_FALSE(b.IsSelected(0, 2));
    EXPECT_EQ(b.selected(0), (std::vector<uint32_t>{0u, 1u}));  // 升序去重

    // 重复涂抹：不新增（去重）。
    EXPECT_EQ(b.PaintAlongRay(0, pts, ray.origin, ray.dir, 100.0f, 720.0f,
                              c.tan_half_fov),
              0u);
    EXPECT_EQ(b.selected_count(), 2u);
}

TEST(BrushToolTest, RadiusClampedToRange) {
    BrushTool b;
    EXPECT_FLOAT_EQ(b.radius_px(), 100.0f);
    b.SetRadiusPx(1000.0f);
    EXPECT_FLOAT_EQ(b.radius_px(), BrushTool::kMaxRadiusPx);
    b.SetRadiusPx(0.0f);
    EXPECT_FLOAT_EQ(b.radius_px(), BrushTool::kMinRadiusPx);
}

TEST(BrushToolTest, EnsurePrimitivesGrowsAndKeepsExisting) {
    BrushTool b;
    b.SetActive(true);
    b.EnsurePrimitives(2);
    const CameraBasis c = FixedBasis();
    const CameraRay ray = RayFromPixel(c, 640.0f, 360.0f);
    b.PaintAlongRay(1, {{0.0f, 0.0f, 0.0f}}, ray.origin, ray.dir, 100.0f, 720.0f,
                    c.tan_half_fov);
    EXPECT_TRUE(b.IsSelected(1, 0));
    b.EnsurePrimitives(3);  // 变长：保留已有
    EXPECT_TRUE(b.IsSelected(1, 0));
    EXPECT_EQ(b.primitive_count(), 3u);
}

}  // namespace
}  // namespace clothing
}  // namespace jpov
