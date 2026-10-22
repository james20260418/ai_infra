// JPOV 坐标架小插件 —— 世界坐标轴（三根实体盒 + X/Y/Z 3D 标签）
//
// 用途：在模型编辑器 / 任意查看器里指示世界坐标朝向。三根轴杆都用
// `MeshData::MakeBox` 造的**单位矢量**（长 1 m）：
// 横截面 **0.02 m**、长 **1 m**，分别沿 +X / +Y / +Z 从 `origin` 向外伸；
// 颜色红 / 黄 / 蓝；杆顶各写一个 3D 文本 "X" / "Y" / "Z"（世界高度 0.2 m）。
//
// ═══════════════ 为什么只造一份网格 ═══════════════
//
// 三根杆的形状完全相同，只有朝向不同。用**一份** mesh（长轴 = 本地 +Y）
// 配 `DrawObject3D` 的 up/front 旋转三次即可，不必把旋转烘进顶点、也不
// 重复上传三份顶点数据。颜色是逐次 draw 的 `PBRMaterial::base_color`，
// 一个 mesh 装三种颜色没有问题（材质不进 mesh）。
//
// ═══════════════ 标签朝向 ═══════════════
//
// 每帧按相机做 billboard：三个标签共用**相机视线方向的反向**作为 quad 法线，
// 即三者都与屏幕平行（若各自「指向相机」，画面偏侧的字会被转成斜的、三个字
// 朝向不一致）。上方向取**相机 up**（= 屏幕上的“上”）；当视线与 up 共线
// （正俯视/正仰视）时 `DrawText3D` 无法定义 roll 会 crash —— 此时退到世界 +Z。
//
// ═══════════════ 与 interface/axis_gizmo.h 的关系 ═══════════════
//
// `axis_gizmo.h` 是 2026-09-14 的旧方案（1 m 长、5 cm 宽的**无厚度条带**，
// 半透明 alpha 0.2），供已删除的旧 model editor 使用，当前仓库内无调用方。
// 本文件按 2026-10-22 需求重做：**实体盒 + 3D 文字**，两者不复用。
// （旧文件保留不动，避免本 PR 顺带删除未在需求内的公共头。）
//
// ⚠️ 所有符号都在 `jpov::axis_frame` 子命名空间：本文件的常量/函数是坐标架
//    专用，不污染 `jpov` 顶层（避免与其它模块的 kAxisFrame* 之类重名）。

#ifndef JPOV_INTERFACE_AXIS_FRAME_H_
#define JPOV_INTERFACE_AXIS_FRAME_H_

#include <cmath>
#include <string>

#include <glog/logging.h>

#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/interface/pbr_material.h"
#include "tools/jpov/interface/render_command.h"

namespace jpov {
namespace axis_frame {

// ==================== 规格常量（需求定档，单点定义） ====================

// 轴杆横截面边长（米）。需求："底面 0.02 width"。
inline constexpr float kAxisFrameBarWidth = 0.02f;

// 轴杆长度（米）。需求："用 MakeBox 做单位矢量" —— 单位矢量长度为 1。
inline constexpr float kAxisFrameBarHeight = 1.0f;

// 标签文字的世界高度（米）。需求："3D 文字高度 0.2 米"。
inline constexpr float kAxisFrameLabelHeight = 0.2f;

// 杆顶到标签包围盒底部的空隙（米），避免文字贴住杆顶。
inline constexpr float kAxisFrameLabelGap = 0.02f;

// 三轴颜色（需求："颜色红黄蓝"）：X=红 / Y=黄 / Z=蓝。
inline constexpr Color kAxisFrameColorX = {1.0f, 0.0f, 0.0f, 1.0f};
inline constexpr Color kAxisFrameColorY = {1.0f, 1.0f, 0.0f, 1.0f};
inline constexpr Color kAxisFrameColorZ = {0.0f, 0.0f, 1.0f, 1.0f};

// ==================== 轴向 / 颜色 / 标签 ====================

// 单位轴向（索引 0/1/2 = X/Y/Z）。
inline Vec3f AxisFrameDirection(int axis_index) {
    CHECK_GE(axis_index, 0);
    CHECK_LT(axis_index, 3);
    switch (axis_index) {
        case 0: return Vec3f(1.0f, 0.0f, 0.0f);
        case 1: return Vec3f(0.0f, 1.0f, 0.0f);
        default: return Vec3f(0.0f, 0.0f, 1.0f);
    }
}

// 三轴颜色（索引 0/1/2 = X/Y/Z）。
inline Color AxisFrameColor(int axis_index) {
    CHECK_GE(axis_index, 0);
    CHECK_LT(axis_index, 3);
    switch (axis_index) {
        case 0: return kAxisFrameColorX;
        case 1: return kAxisFrameColorY;
        default: return kAxisFrameColorZ;
    }
}

// 三轴标签文本（索引 0/1/2 = X/Y/Z）。
inline const char* AxisFrameLabel(int axis_index) {
    CHECK_GE(axis_index, 0);
    CHECK_LT(axis_index, 3);
    switch (axis_index) {
        case 0: return "X";
        case 1: return "Y";
        default: return "Z";
    }
}

// 三轴材质（索引 0/1/2 = X/Y/Z）。
//
// base_color = 该轴颜色；emissive 取同色的 35%，让背光面也保持可辨认的
// 红/黄/蓝（纯漫反射在无直射光的一侧只剩 ambient，颜色会被压灰 —— 旧
// axis_gizmo 的实测结论）。
inline PBRMaterial AxisFrameMaterial(int axis_index) {
    const Color c = AxisFrameColor(axis_index);
    PBRMaterial m = PBRMaterial::SolidColor(c);
    m.emissive = Color{c.r * 0.35f, c.g * 0.35f, c.b * 0.35f, 1.0f};
    return m;
}

// ==================== 几何 ====================

// 三根轴共用的轴杆网格：本地 +Y 为长轴（长 kAxisFrameBarHeight），
// 横截面 kAxisFrameBarWidth × kAxisFrameBarWidth，**中心在本地原点**。
//
// 摆放由 DrawAxisFrame 用 center/up/front 完成：杆的起点贴 origin、
// 终点在 origin + 轴方向 × kAxisFrameBarHeight。
inline MeshData MakeAxisFrameBarMesh() {
    return MeshData::MakeBox(
        /*front_half_width=*/0.5f * kAxisFrameBarWidth,
        /*up_half_width=*/0.5f * kAxisFrameBarHeight,
        /*left_half_width=*/0.5f * kAxisFrameBarWidth);
}

// ==================== 绘制 ====================

// 追加「一个坐标架」到本帧命令列表：三根轴杆 + 三个 3D 标签。
//
// 参数：
//   cmds:         本帧命令列表（非空）
//   bar_mesh_id:  已注册的轴杆网格，必须是 MakeAxisFrameBarMesh() 的产物
//                 （杆长/粗细与标签锚点位置按本文件规格计算）
//   origin:       坐标架原点（三根杆都从这里出发）
//   font_alias:   标签字体别名（须已在 JPOV::Config::fonts 注册；
//                 未知别名在渲染期 crash，与 DrawText3D 同规则）
//
// 标签朝向按**当帧相机**做 billboard，故调用前必须已设好 cmds->camera
// （与其它 3D 命令同帧构建、顺序无关，只要写入命令列表前 camera 已就位）。
//
// Pre-condition: cmds != nullptr；bar_mesh_id > 0。
inline void DrawAxisFrame(RenderCommandList* cmds, uint32_t bar_mesh_id,
                          const Vec3f& origin, const std::string& font_alias) {
    CHECK(cmds != nullptr) << "DrawAxisFrame: cmds 不能为空";
    CHECK_GT(bar_mesh_id, 0u) << "DrawAxisFrame: bar_mesh_id 必须 > 0";

    const float half_h = 0.5f * kAxisFrameBarHeight;

    // ---- 三根轴杆 ----
    for (int axis = 0; axis < 3; ++axis) {
        const Vec3f dir = AxisFrameDirection(axis);
        // 盒中心 = 起点 + 轴方向 × 半长（让杆的一端贴 origin）。
        const Vec3f center(origin.x() + dir.x() * half_h,
                           origin.y() + dir.y() * half_h,
                           origin.z() + dir.z() * half_h);
        // local +Y → 轴方向；front 取一个与该轴垂直的世界轴（三种组合都不平行）。
        const Vec3f up = dir;
        const Vec3f front = (axis == 2) ? Vec3f(0.0f, 1.0f, 0.0f)
                                        : Vec3f(0.0f, 0.0f, 1.0f);
        cmds->DrawObject3D(bar_mesh_id, AxisFrameMaterial(axis), center, up, front,
                           /*scale=*/1.0f, /*highlight=*/false,
                           /*picking_id=*/0);
    }

    // ---- 三个 3D 标签（杆顶上方，朝向相机） ----
    for (int axis = 0; axis < 3; ++axis) {
        const Vec3f dir = AxisFrameDirection(axis);
        const float anchor_d = kAxisFrameBarHeight + kAxisFrameLabelGap;
        const Vec3f anchor(origin.x() + dir.x() * anchor_d,
                           origin.y() + dir.y() * anchor_d,
                           origin.z() + dir.z() * anchor_d);

        // 法线 = 相机视线方向的反向（billboard）。三个标签共用同一个朝向：
        // 若改成「每个标签各自指向相机」，画面偏侧的标签会被绕 Y 轴转成斜的
        // （正交化后仍可读，但三个字朝向不一致）。共用视线方向 = 三者都与屏幕
        // 平行，读起来最稳。相机与 target 重合时退 +Z（避免零向量 CHECK）。
        Vec3f face(cmds->camera.position.x() - cmds->camera.target.x(),
                   cmds->camera.position.y() - cmds->camera.target.y(),
                   cmds->camera.position.z() - cmds->camera.target.z());
        float face_len = std::sqrt(face.Dot(face));
        if (face_len < 1e-6f) {
            face = Vec3f(0.0f, 0.0f, 1.0f);
            face_len = 1.0f;
        }
        const float inv_face_len = 1.0f / face_len;

        // 上方向取相机 up（屏幕上的“上”）。与法线共线时（正俯视/正仰视）
        // 退到世界 +Z；相机 up 未设（零向量）时退到世界 +Y。
        // 若连 +Z 都与法线共线（法线自己就是 ±Z，如相机与 target 重合的兜底），
        // 再退到 +X —— 保证交给 DrawText3D 的 up 永远不共线。
        Vec3f up = cmds->camera.up;
        float up_len = std::sqrt(up.Dot(up));
        if (up_len < 1e-6f) {
            up = Vec3f(0.0f, 1.0f, 0.0f);
            up_len = 1.0f;
        }
        const float up_dot_face =
            (face.x() * up.x() + face.y() * up.y() + face.z() * up.z()) *
            inv_face_len / up_len;
        if (std::fabs(up_dot_face) > 0.9999f) {
            const float face_z = face.z() * inv_face_len;
            up = (std::fabs(face_z) > 0.9999f) ? Vec3f(1.0f, 0.0f, 0.0f)
                                               : Vec3f(0.0f, 0.0f, 1.0f);
        }

        cmds->DrawText3D(AxisFrameLabel(axis), anchor, face, up,
                         kAxisFrameLabelHeight, AxisFrameColor(axis), font_alias,
                         TextAlignment::kMidBottom);
    }
}

}  // namespace axis_frame
}  // namespace jpov

#endif  // JPOV_INTERFACE_AXIS_FRAME_H_
