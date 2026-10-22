// AxisFrame 单测 —— 坐标架小插件（三根 0.02m 见方 × 1m 的实体盒 + X/Y/Z 标签）
//
// 覆盖点（每条都对应一个「写错也看不出来」的失效模式）：
//   1. **杆的尺寸**：横截面 0.02、长 1（单位矢量），且长轴必须在**本地 +Y**
//      （MakeBox 三个半宽传错顺序 → 杆变扁、朝向歪，肉眼像"一块砖"）。
//   2. **颜色映射**：X=红 / Y=黄 / Z=蓝，逐分量锁死（不是 kColorGreen 那套约定）。
//   3. **摆位**：杆必须从 origin 往外伸（不是以 origin 为中心）—— 用
//      center − up×half_height == origin 断言，防"整根杆一半埋进地里"。
//   4. **朝向**：up 必须是该轴方向、front 必须与之垂直（不垂直会被
//      DrawObject3D 正交化，杆就会偏）。
//   5. **标签**：文本内容 / 锚点在杆顶上方 / 对齐方式 / 字号 / 颜色。
//   6. **billboard**：三个标签的法线都等于「相机视线反向」（用相机在 -Z 侧的
//      用例，若实现写死 +Z 就会失败）；正俯视时 up 不能与法线共线
//      （共线 = DrawText3D crash）。

#include "tools/jpov/interface/axis_frame.h"

#include <algorithm>
#include <cmath>

#include <gtest/gtest.h>

namespace jpov {
namespace axis_frame {
namespace {

constexpr float kEps = 1e-5f;

struct Bounds {
    Vec3f min;
    Vec3f max;
};

Bounds MeshBoundsOf(const MeshData& mesh) {
    Bounds b{mesh.positions[0], mesh.positions[0]};
    for (const Vec3f& p : mesh.positions) {
        b.min = Vec3f(std::min(b.min.x(), p.x()), std::min(b.min.y(), p.y()),
                      std::min(b.min.z(), p.z()));
        b.max = Vec3f(std::max(b.max.x(), p.x()), std::max(b.max.y(), p.y()),
                      std::max(b.max.z(), p.z()));
    }
    return b;
}

Vec3f Unit(const Vec3f& v) {
    const float len = std::sqrt(v.Dot(v));
    return Vec3f(v.x() / len, v.y() / len, v.z() / len);
}

// ==================== 1. 杆的几何 ====================

TEST(AxisFrameMesh, IsUnitVectorBarWithPointZeroTwoWidth) {
    const MeshData mesh = MakeAxisFrameBarMesh();
    // MakeBox：24 顶点（6 面 × 4）/ 36 index（12 三角形）。
    EXPECT_EQ(mesh.positions.size(), 24u);
    EXPECT_EQ(mesh.normals.size(), 24u);
    EXPECT_EQ(mesh.indices.size(), 36u);

    const Bounds b = MeshBoundsOf(mesh);
    // 横截面 0.02（±0.01），长轴 1（±0.5）。
    EXPECT_NEAR(b.min.x(), -0.5f * kAxisFrameBarWidth, kEps);
    EXPECT_NEAR(b.max.x(), 0.5f * kAxisFrameBarWidth, kEps);
    EXPECT_NEAR(b.min.y(), -0.5f * kAxisFrameBarHeight, kEps);
    EXPECT_NEAR(b.max.y(), 0.5f * kAxisFrameBarHeight, kEps);
    EXPECT_NEAR(b.min.z(), -0.5f * kAxisFrameBarWidth, kEps);
    EXPECT_NEAR(b.max.z(), 0.5f * kAxisFrameBarWidth, kEps);

    // 长轴必须是 Y（长是宽的 50 倍）；若 MakeBox 的 front/up 参数传反，
    // 这里会变成 1/50 而不是 50。
    const float height_extent = b.max.y() - b.min.y();
    const float width_extent = b.max.x() - b.min.x();
    EXPECT_NEAR(height_extent / width_extent, 50.0f, kEps);

    // 法线全是单位向量（PBR 着色直接用，退化法线会整面发黑）。
    for (const Vec3f& n : mesh.normals) {
        EXPECT_NEAR(std::sqrt(n.Dot(n)), 1.0f, kEps);
    }
}

// ==================== 2. 颜色 / 材质 ====================

TEST(AxisFrameColor, RedYellowBlueInAxisOrder) {
    const Color red = AxisFrameColor(0);
    EXPECT_FLOAT_EQ(red.r, 1.0f);
    EXPECT_FLOAT_EQ(red.g, 0.0f);
    EXPECT_FLOAT_EQ(red.b, 0.0f);

    const Color yellow = AxisFrameColor(1);
    EXPECT_FLOAT_EQ(yellow.r, 1.0f);
    EXPECT_FLOAT_EQ(yellow.g, 1.0f);
    EXPECT_FLOAT_EQ(yellow.b, 0.0f);

    const Color blue = AxisFrameColor(2);
    EXPECT_FLOAT_EQ(blue.r, 0.0f);
    EXPECT_FLOAT_EQ(blue.g, 0.0f);
    EXPECT_FLOAT_EQ(blue.b, 1.0f);

    // 三色互不相同（防"复制粘贴时三根都是红"）。
    EXPECT_NE(red.g, yellow.g);
    EXPECT_NE(yellow.b, blue.b);
    EXPECT_NE(red.b, blue.b);
}

TEST(AxisFrameMaterial, BaseColorMatchesAxisAndEmissiveIsPresent) {
    for (int axis = 0; axis < 3; ++axis) {
        const PBRMaterial m = AxisFrameMaterial(axis);
        const Color c = AxisFrameColor(axis);
        EXPECT_FLOAT_EQ(m.base_color.r, c.r);
        EXPECT_FLOAT_EQ(m.base_color.g, c.g);
        EXPECT_FLOAT_EQ(m.base_color.b, c.b);
        EXPECT_EQ(m.alpha_mode, AlphaMode::kOpaque);
        // 背光面靠 emissive 保色：三通道必须按同一比例（= 0.35）跟随 base_color，
        // 写错通道顺序 / 漏掉某个通道都应当暴露。
        constexpr float kEmissiveFactor = 0.35f;
        EXPECT_NEAR(m.emissive.r, c.r * kEmissiveFactor, kEps);
        EXPECT_NEAR(m.emissive.g, c.g * kEmissiveFactor, kEps);
        EXPECT_NEAR(m.emissive.b, c.b * kEmissiveFactor, kEps);
        EXPECT_GT(m.emissive.r + m.emissive.g + m.emissive.b, 0.0f);
    }
}

// ==================== 3/4. 轴杆摆位与朝向 ====================

TEST(DrawAxisFrame, PlacesThreeBarsFromOriginAlongEachAxis) {
    RenderCommandList cmds;
    cmds.camera.position = Vec3f(-1.0f, 0.5f, -3.0f);  // 故意在 -Z 侧
    const Vec3f origin(0.25f, -0.5f, 0.75f);
    DrawAxisFrame(&cmds, /*bar_mesh_id=*/7u, origin, /*font_alias=*/"Latin");

    ASSERT_EQ(cmds.object3d.size(), 3u);
    ASSERT_EQ(cmds.text3d.size(), 3u);
    ASSERT_EQ(cmds.order.size(), 6u);

    for (int axis = 0; axis < 3; ++axis) {
        const Object3DCommand& cmd = cmds.object3d[static_cast<size_t>(axis)];
        const Vec3f dir = AxisFrameDirection(axis);
        EXPECT_EQ(cmd.mesh_id, 7u);

        // 朝向：up = 轴方向；front 与 up 垂直。
        EXPECT_NEAR(cmd.up.x(), dir.x(), kEps);
        EXPECT_NEAR(cmd.up.y(), dir.y(), kEps);
        EXPECT_NEAR(cmd.up.z(), dir.z(), kEps);
        EXPECT_NEAR(cmd.front.Dot(dir), 0.0f, kEps);

        // 杆的一端贴 origin：center − up × half_height == origin。
        const Vec3f tail(cmd.center.x() - cmd.up.x() * 0.5f * kAxisFrameBarHeight,
                         cmd.center.y() - cmd.up.y() * 0.5f * kAxisFrameBarHeight,
                         cmd.center.z() - cmd.up.z() * 0.5f * kAxisFrameBarHeight);
        EXPECT_NEAR(tail.x(), origin.x(), kEps);
        EXPECT_NEAR(tail.y(), origin.y(), kEps);
        EXPECT_NEAR(tail.z(), origin.z(), kEps);
    }

    // 命令顺序：三根杆在前、三个标签在后。
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(cmds.order[static_cast<size_t>(i)].first, DrawCommandType::kObject3D);
    }
    for (int i = 3; i < 6; ++i) {
        EXPECT_EQ(cmds.order[static_cast<size_t>(i)].first, DrawCommandType::kText3D);
    }
}

// ==================== 5/6. 标签内容与朝向 ====================

TEST(DrawAxisFrame, LabelsSitAboveBarTipsAndFaceCamera) {
    RenderCommandList cmds;
    const Vec3f camera_pos(-1.0f, 0.5f, -3.0f);  // 相机在 -Z 侧
    cmds.camera.position = camera_pos;
    const Vec3f origin(0.25f, -0.5f, 0.75f);
    DrawAxisFrame(&cmds, /*bar_mesh_id=*/3u, origin, /*font_alias=*/"Latin");

    ASSERT_EQ(cmds.text3d.size(), 3u);
    for (int axis = 0; axis < 3; ++axis) {
        const Text3DCommand& t = cmds.text3d[static_cast<size_t>(axis)];
        const Vec3f dir = AxisFrameDirection(axis);

        EXPECT_STREQ(t.text.c_str(), AxisFrameLabel(axis));
        EXPECT_EQ(t.alignment, TextAlignment::kMidBottom);
        EXPECT_FLOAT_EQ(t.font_height_world, kAxisFrameLabelHeight);
        EXPECT_EQ(t.font_alias, "Latin");

        const Color c = AxisFrameColor(axis);
        EXPECT_FLOAT_EQ(t.color.r, c.r);
        EXPECT_FLOAT_EQ(t.color.g, c.g);
        EXPECT_FLOAT_EQ(t.color.b, c.b);

        // 锚点在杆顶上方（+gap），不在杆内。
        const float d = kAxisFrameBarHeight + kAxisFrameLabelGap;
        EXPECT_NEAR(t.anchor.x(), origin.x() + dir.x() * d, kEps);
        EXPECT_NEAR(t.anchor.y(), origin.y() + dir.y() * d, kEps);
        EXPECT_NEAR(t.anchor.z(), origin.z() + dir.z() * d, kEps);

        // billboard：法线 = 相机视线反向（所有标签共用同一朝向，与屏幕平行）。
        // 相机在 -Z 侧 ⇒ 若实现写死 +Z，这里必然失败。
        const Vec3f view_dir = camera_pos;  // camera.target 默认在原点
        const Vec3f cross = Unit(t.face_direction).Cross(Unit(view_dir));
        EXPECT_NEAR(std::sqrt(cross.Dot(cross)), 0.0f, 1e-4f);
        const Vec3f to_cam(camera_pos.x() - t.anchor.x(),
                           camera_pos.y() - t.anchor.y(),
                           camera_pos.z() - t.anchor.z());
        EXPECT_GT(t.face_direction.Dot(to_cam), 0.0f);
    }
}

TEST(DrawAxisFrame, LabelUpFollowsCameraUp) {
    // 屏幕上的“上”= 相机 up。相机 up 设成 +X 时，标签的上方向必须跟着转，
    // 而不是写死世界 +Y。
    RenderCommandList cmds;
    cmds.camera.position = Vec3f(0.0f, 0.0f, 5.0f);
    cmds.camera.target = Vec3f(0.0f, 0.0f, 0.0f);
    cmds.camera.up = Vec3f(1.0f, 0.0f, 0.0f);
    DrawAxisFrame(&cmds, /*bar_mesh_id=*/1u, /*origin=*/Vec3f(0.0f, 0.0f, 0.0f),
                  /*font_alias=*/"Latin");

    ASSERT_EQ(cmds.text3d.size(), 3u);
    for (const Text3DCommand& t : cmds.text3d) {
        const Vec3f up = Unit(t.up_direction);
        EXPECT_NEAR(up.x(), 1.0f, kEps);
        EXPECT_NEAR(up.y(), 0.0f, kEps);
        EXPECT_NEAR(up.z(), 0.0f, kEps);
        // up 与法线必须垂直（否则 DrawText3D 会把它投影掉，滚动就丢了）。
        EXPECT_NEAR(up.Dot(Unit(t.face_direction)), 0.0f, kEps);
    }
}

TEST(DrawAxisFrame, TopDownCameraDoesNotMakeTextUpCollinearWithFace) {
    // 正俯视：视线方向就是 +Y，若 up 仍取 +Y 会共线 → DrawText3D 在渲染期
    // crash。这里验证实现退到了 +Z。
    RenderCommandList cmds;
    cmds.camera.position = Vec3f(0.0f, 5.0f, 0.0f);
    DrawAxisFrame(&cmds, /*bar_mesh_id=*/1u, /*origin=*/Vec3f(0.0f, 0.0f, 0.0f),
                  /*font_alias=*/"Latin");

    ASSERT_EQ(cmds.text3d.size(), 3u);
    const Text3DCommand& y_label = cmds.text3d[1];
    const Vec3f face = Unit(y_label.face_direction);
    const Vec3f up = Unit(y_label.up_direction);
    EXPECT_LT(std::fabs(up.Dot(face)), 0.99f);
}

TEST(DrawAxisFrame, DegenerateCameraTargetFallsBackToForwardFace) {
    // 相机与 target 重合：视线方向长度 0，必须有兜底（不能把零向量交给
    // DrawText3D，否则 CHECK 直接 crash）。
    RenderCommandList cmds;
    cmds.camera.position = Vec3f(1.02f, 0.0f, 0.0f);
    cmds.camera.target = Vec3f(1.02f, 0.0f, 0.0f);
    DrawAxisFrame(&cmds, /*bar_mesh_id=*/1u, /*origin=*/Vec3f(0.0f, 0.0f, 0.0f),
                  /*font_alias=*/"Latin");

    ASSERT_EQ(cmds.text3d.size(), 3u);
    for (const Text3DCommand& t : cmds.text3d) {
        EXPECT_NEAR(t.face_direction.x(), 0.0f, kEps);
        EXPECT_NEAR(t.face_direction.y(), 0.0f, kEps);
        EXPECT_NEAR(t.face_direction.z(), 1.0f, kEps);
        // up 仍取 +Y（与兜底面 +Z 垂直）。
        EXPECT_NEAR(
            std::fabs(Unit(t.up_direction).Dot(Unit(t.face_direction))), 0.0f,
            kEps);
    }
}

TEST(DrawAxisFrame, DegenerateFaceAndUpStillProducesNonCollinearUp) {
    // 相机与 target 重合（face 兜底 +Z）且相机 up 也是 +Z：若只退到 +Z，
    // up 与 face 仍共线 → DrawText3D 渲染期 crash。实现应再退到 +X。
    RenderCommandList cmds;
    cmds.camera.position = Vec3f(0.0f, 0.0f, 0.0f);
    cmds.camera.target = Vec3f(0.0f, 0.0f, 0.0f);
    cmds.camera.up = Vec3f(0.0f, 0.0f, 1.0f);
    DrawAxisFrame(&cmds, /*bar_mesh_id=*/1u, /*origin=*/Vec3f(0.0f, 0.0f, 0.0f),
                  /*font_alias=*/"Latin");

    ASSERT_EQ(cmds.text3d.size(), 3u);
    for (const Text3DCommand& t : cmds.text3d) {
        EXPECT_LT(std::fabs(Unit(t.up_direction).Dot(Unit(t.face_direction))),
                  0.99f);
    }
}

}  // namespace
}  // namespace axis_frame
}  // namespace jpov
