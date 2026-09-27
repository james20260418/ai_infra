// JPOV 软体仿真器 — 世界→屏幕投影 单测（纯 CPU、GL-free）
//
// 关注点：投影结果的**像素坐标是否正确**（圆点要落在 3D 网格该在的地方）。
// 用可手算的对称工况验证，避免“测了个恒真”。
//
// ⚠️ 负向验证过：把 y 翻转那行（(1 - ndc_y*0.5+0.5)）改成正向，屏幕上下方向
//    断言立即 FAIL。

#include "tools/jpov/soft_mesh_simulator/screen_projection.h"

#include "geom/common/vec.h"
#include "gtest/gtest.h"

namespace {

using jpov::soft_mesh_simulator::ProjectToScreen;
using jpov::soft_mesh_simulator::ProjectionCamera;

// 一台朝 -Z 看、位于 +Z 的相机（标准右手系）。
// 位置 (0,0,10)，目标 (0,0,0)，1280×720，fov=90（tan(45)=1，便于手算）。
ProjectionCamera MakeCamera() {
    ProjectionCamera cam;
    cam.position = geom::Vec3<float>(0.0f, 0.0f, 10.0f);
    cam.target   = geom::Vec3<float>(0.0f, 0.0f, 0.0f);
    cam.up       = geom::Vec3<float>(0.0f, 1.0f, 0.0f);
    cam.fov_deg  = 90.0f;
    cam.near     = 0.1f;
    cam.far      = 100.0f;
    cam.fbo_w    = 1280;
    cam.fbo_h    = 720;
    return cam;
}

// 投影中心点（原点，恰在相机视线轴上）→ 屏幕正中心 (640, 360)。
TEST(ScreenProjectionTest, CenterProjectsToScreenCenter) {
    const auto sp = ProjectToScreen(geom::Vec3<float>(0, 0, 0), MakeCamera());
    ASSERT_TRUE(sp.visible);
    EXPECT_NEAR(sp.x, 640.0f, 0.5f);
    EXPECT_NEAR(sp.y, 360.0f, 0.5f);
}

// 世界 +Y 方向（向上）的点必须投到屏幕**上方**（y 更小）。
// 这条锁住「NDC +y 上 → 屏幕 +y 下」的那次翻转；翻错方向会 FAIL。
TEST(ScreenProjectionTest, WorldUpMapsToScreenUp) {
    const auto center = ProjectToScreen(geom::Vec3<float>(0, 0, 0), MakeCamera());
    const auto above  = ProjectToScreen(geom::Vec3<float>(0, 1, 0), MakeCamera());
    ASSERT_TRUE(center.visible);
    ASSERT_TRUE(above.visible);
    EXPECT_LT(above.y, center.y) << "世界 +Y 的点应投到屏幕上方（y 更小）";
    EXPECT_NEAR(above.x, center.x, 0.5f) << "不应有水平偏移";
}

// 世界 +X（向右）的点必须投到屏幕**右侧**（x 更大）。
TEST(ScreenProjectionTest, WorldRightMapsToScreenRight) {
    const auto center = ProjectToScreen(geom::Vec3<float>(0, 0, 0), MakeCamera());
    const auto right  = ProjectToScreen(geom::Vec3<float>(1, 0, 0), MakeCamera());
    ASSERT_TRUE(center.visible);
    ASSERT_TRUE(right.visible);
    EXPECT_GT(right.x, center.x) << "世界 +X 的点应投到屏幕右侧（x 更大）";
    EXPECT_NEAR(right.y, center.y, 0.5f) << "不应有垂直偏移";
}

// 相机背后的点（z = 20，相机在 z=10 朝 -Z 看）必须 visible=false。
TEST(ScreenProjectionTest, PointBehindCameraIsInvisible) {
    const auto behind = ProjectToScreen(geom::Vec3<float>(0, 0, 20), MakeCamera());
    EXPECT_FALSE(behind.visible);
}

// 等距的两点，离相机越远的投影间距越小（透视缩小的定性验证）。
TEST(ScreenProjectionTest, PerspectiveShrinksWithDistance) {
    ProjectionCamera cam = MakeCamera();
    // 在 z=0 平面上取 x=0 与 x=1 两点 → 近；在 z=-5 平面上同样 x=0/1 → 远。
    const auto near_a = ProjectToScreen(geom::Vec3<float>(0, 0, 0), cam);
    const auto near_b = ProjectToScreen(geom::Vec3<float>(1, 0, 0), cam);
    const auto far_a  = ProjectToScreen(geom::Vec3<float>(0, 0, -5), cam);
    const auto far_b  = ProjectToScreen(geom::Vec3<float>(1, 0, -5), cam);
    ASSERT_TRUE(near_a.visible && near_b.visible && far_a.visible && far_b.visible);
    const float near_gap = near_b.x - near_a.x;
    const float far_gap  = far_b.x - far_a.x;
    EXPECT_GT(near_gap, far_gap) << "远的点投影间距应更小";
}

}  // namespace
