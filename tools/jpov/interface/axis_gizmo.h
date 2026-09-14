// JPOV 坐标架辅助工具 —— 世界坐标轴可视化（三根 1m 条带）
//
// 用途：模型编辑器（model editor）在**模型处**摆一个 **1m × 1m × 1m** 的坐标架，
// 让用户肉眼对位——"这个模型朝向对不对、缩放量级合不合适"。三根条带分别沿世界
// +X / +Y / +Z 的正方向，红绿蓝三色（X=红、Y=绿、Z=蓝，图形学通行约定），
// alpha 0.2 近乎透明，避免遮挡被编辑的模型。
// （位置由调用方给 `origin`；编辑器传模型平移量，见 design doc §7.1b。）
//
// ═══════════════ 为什么是「条带」而不是「棱柱」 ═══════════════
//
// 需求原文说"用 make box 做直棱柱"，首版照做（`MeshData::MakeOrientedBox`）。
// 但那需要给每条轴一个**完整 3D 网格 + 材质 + DrawObject3D** —— 而
// `DrawObject3D` 的片元 shader 此前**写死 `FragColor.a = 1.0`**，没有任何
// alpha 混合路径，为了画三根半透明的辅助线就得去动 **Object3D 的着色通路**
// （加 uniform、加混合状态、改深度写入…）。
//
// Danis 定调（2026-09-14）：**不为辅助线动 Object3D 的 alpha 通路，减少影响面**。
// 改用既有的 **3D 条带**（`RenderCommandList::DrawStrip3D`）：
//   - 它的 FS 就是 `FragColor = uColor`（`primitives3d_renderer.h: kFs3d`），
//     **alpha 天然生效**，一行渲染代码都不用改；
//   - 无光照、无 PBR —— 正是辅助线要的"恒定鲜艳色"（首版棱柱方案用 Object3D 的
//     PBR 着色时，背光面发黑、乘 alpha 后肉眼是根"灰杆"，得靠 emissive 才能救；
//     改用条带后问题自然消失 —— 少一条复杂通路就少一类坑）。
//   - 代价：条带无厚度。对 1m 长的方向指示完全够用（用户只看朝向）。
//
// ═══════════════ 几何规格 ═══════════════
//
// 每条轴 = 一条**矩形带**（4 顶点，GL_TRIANGLE_STRIP → 2 三角形）：
//   - 从 `origin` 伸到 `origin + 轴方向 × 1m`（origin 由调用方给，编辑器传
//     "模型处"，见 design doc §7.1b）；
//   - 宽 5cm（需求"strip 宽度还是 5cm"）；
//   - **无厚度**（单面）。
//
// 朝向（法线，Danis 定档 2026-09-14 第二轮）：
//   - +X 条带：宽度沿 **Y** 展开（±2.5cm），**法线 +Z**
//   - +Y 条带：宽度沿 Z 展开（±2.5cm），**法线 +X**（竖立）
//   - +Z 条带：宽度沿 **Y** 展开（±2.5cm），**法线 +X**
//
// ⚠️ 条带**单面**且 `Draw3DCommands` 入口开着 `GL_CULL_FACE` → 法线背对相机
//    时整条被剔除。故（a）绕序必须逐带算对（见 `MakeAxisGizmoStrips`），
//    且（b）**编辑器对每条带正反各画一次**（`EditorApp::DrawAxisGizmo`）
//    → 实现"双面可见"，用户从任意方向看都能看到三条带。
//    这里**不**关剔除（Danis 定调：减少影响面，不动渲染状态）。

#ifndef JPOV_INTERFACE_AXIS_GIZMO_H_
#define JPOV_INTERFACE_AXIS_GIZMO_H_

#include <array>

#include <glog/logging.h>

#include "tools/jpov/interface/pbr_material.h"   // Color
#include "tools/jpov/interface/render_command.h" // Vec3f

namespace jpov {

// ==================== 规格常量（需求定档，单点定义） ====================

// 每根条带的长度（米）。需求："1m, 1m, 1m 的座标架"。
inline constexpr float kAxisGizmoLength = 1.0f;

// 条带宽度（米）。需求："strip 宽度还是 5cm"。
// 即带宽 5cm → 半宽 2.5cm。
inline constexpr float kAxisGizmoWidth = 0.05f;

// 三轴颜色（需求："颜色红绿蓝"，与图形学通行约定一致：X=红 / Y=绿 / Z=蓝）。
// alpha = 0.2（"alpha 还是 0.2"）。
// ⚠️ 颜色语义 = 设计时肉眼期望的屏显色（纯红/纯绿/纯蓝）。条带 FS 是
//    `FragColor = uColor`（无光照、无 tone map 之前的编码变换以外的处理），
//    实测在编辑器浅灰地面上三色均清晰可辨。
inline constexpr Color kAxisGizmoColorX = {1.0f, 0.0f, 0.0f, 0.2f};  // +X 红
inline constexpr Color kAxisGizmoColorY = {0.0f, 1.0f, 0.0f, 0.2f};  // +Y 绿
inline constexpr Color kAxisGizmoColorZ = {0.0f, 0.0f, 1.0f, 0.2f};  // +Z 蓝

// ==================== 单条轴的条带顶点 ====================

// 造一条轴带：4 个世界空间顶点，按 GL_TRIANGLE_STRIP 顺序（p0,p1,p2,p3）。
//
// 条带沿 axis 方向从 origin 伸到 origin + axis × length；
// 在 `side` 方向上以 ±width/2 展开（`side` 须与 axis 垂直且为单位向量）。
//
// 🔑 **绕序（winding）决定法线朝向** —— 条带是单面几何，而 `Draw3DCommands`
//    入口开着 `GL_CULL_FACE` + CCW，所以**法线背对相机的那一面会被剔除、直接消失**。
//    故本函数显式给出两个等价绕序，由 `normal_toward_side` 选择：
//
//    winding A（p0=tail-, p1=tail+, p2=head-, p3=head+）→ 法线 = side × axis
//    winding B（交换两对）                                  → 法线 = axis × side
//
//    选择规则：谁的法线朝 `want_normal_side` 所在一侧就选谁。调用方
//    （`MakeAxisGizmoStrips`）按需求"法向 x/z 朝上、y 朝 +x"传入期望方向。
//
// Pre-condition: axis / side 均非零且**互相垂直**（本函数不自动正交化 ——
//   调用方的三种轴组合由 AxisGizmoStrips() 定死并已被单测覆盖）；
//   length > 0；width > 0。
inline std::array<Vec3f, 4> MakeAxisStripVertices(
    const Vec3f& origin, const Vec3f& axis, const Vec3f& side,
    float length = kAxisGizmoLength, float width = kAxisGizmoWidth,
    bool normal_toward_side = true) {
    CHECK_GT(length, 0.0f) << "MakeAxisStripVertices: length 必须 > 0";
    CHECK_GT(width, 0.0f) << "MakeAxisStripVertices: width 必须 > 0";
    CHECK(axis.x() != 0.0f || axis.y() != 0.0f || axis.z() != 0.0f)
        << "MakeAxisStripVertices: axis 不能为零向量";
    CHECK(side.x() != 0.0f || side.y() != 0.0f || side.z() != 0.0f)
        << "MakeAxisStripVertices: side 不能为零向量";

    const float half = 0.5f * width;
    // 轴的两个端点。
    const Vec3f tail(origin.x(), origin.y(), origin.z());
    const Vec3f head(origin.x() + axis.x() * length,
                     origin.y() + axis.y() * length,
                     origin.z() + axis.z() * length);

    auto offset = [&](const Vec3f& p, float sign) {
        return Vec3f(p.x() + side.x() * sign * half,
                     p.y() + side.y() * sign * half,
                     p.z() + side.z() * sign * half);
    };
    // winding A：法线 = side × axis；winding B：交换两对 → 法线反向。
    if (normal_toward_side) {
        return {offset(tail, -1.0f), offset(tail, +1.0f),
                offset(head, -1.0f), offset(head, +1.0f)};
    }
    return {offset(tail, +1.0f), offset(tail, -1.0f),
            offset(head, +1.0f), offset(head, -1.0f)};
}

// 三根轴带的顶点（索引 0/1/2 = X/Y/Z），已按需求定死朝向：
//   X 带：轴 +X，宽度沿 Z 展开 → 法线朝 ±Y（躺地面）
//   Y 带：轴 +Y，宽度沿 Z 展开 → 法线朝 ±X（竖立，面朝 X）
//   Z 带：轴 +Z，宽度沿 X 展开 → 法线朝 ±Y（躺地面）
struct AxisGizmoStrips {
    std::array<Vec3f, 4> x;
    std::array<Vec3f, 4> y;
    std::array<Vec3f, 4> z;
};

// 造三根轴带。origin 为坐标架原点（编辑器传"地面高度上的世界原点"）。
//
// 法线朝向（Danis 定档 2026-09-14，第二轮）：
//   X 带：轴 +X，宽度方向取 **+Y** → 法线 **+Z**
//   Y 带：轴 +Y，宽度方向取 +Z   → 法线 **+X**
//   Z 带：轴 +Z，宽度方向取 **+Y** → 法线 **+X**
//
// ⚠️ **绕序（winding）是功能性的，不是审美**：条带单面 + `Draw3DCommands` 入口
//   开着 GL_CULL_FACE，**法线背对相机就整条消失**（实测踩过）。
//   下面的 `normal_toward_side` 由 "法线 = side × axis" 逐带推出：
//     X: side × axis = Y × X = -Z → 需翻（flip=true）得 +Z
//     Y: Z × Y = -X             → 需翻（flip=true）得 +X
//     Z: Y × Z = +X             → 不翻（flip=false）
//   单测 `NormalsMatchRequirement` 断言**带符号**法线（不是 fabs）把三条锁死。
//
// ⚠️ 注意：即便绕序调对，从**背面**看仍会消失（单面几何的固有性质）。
//   故编辑器对每条带**正反各画一次**（见 EditorApp::DrawAxisGizmo）→ 双向可见，
//   不依赖调用方的观察方向。本函数只负责"正面法线朝向约定"。
// Pre-condition: length > 0 && width > 0
inline AxisGizmoStrips MakeAxisGizmoStrips(
    const Vec3f& origin, float length = kAxisGizmoLength,
    float width = kAxisGizmoWidth) {
    const Vec3f unit_x(1.0f, 0.0f, 0.0f);
    const Vec3f unit_y(0.0f, 1.0f, 0.0f);
    const Vec3f unit_z(0.0f, 0.0f, 1.0f);
    AxisGizmoStrips s;
    // X 带：宽度沿 Y（条带平面 = X-Y）→ 法线 ±Z，取 +Z 需 flip。
    s.x = MakeAxisStripVertices(origin, unit_x, unit_y, length, width,
                                /*normal_toward_side*/ false);
    // Y 带：宽度沿 Z（平面 = Y-Z）→ 法线 ±X，取 +X 需 flip。
    s.y = MakeAxisStripVertices(origin, unit_y, unit_z, length, width,
                                /*normal_toward_side*/ false);
    // Z 带：宽度沿 Y（平面 = Z-Y）→ 法线 ±X，side × axis = Y × Z = +X → 不翻。
    s.z = MakeAxisStripVertices(origin, unit_z, unit_y, length, width,
                                /*normal_toward_side*/ true);
    return s;
}

// 三轴颜色（索引 0/1/2 = X/Y/Z）。
inline Color AxisGizmoColor(int axis_index) {
    CHECK_GE(axis_index, 0);
    CHECK_LT(axis_index, 3);
    switch (axis_index) {
        case 0: return kAxisGizmoColorX;
        case 1: return kAxisGizmoColorY;
        default: return kAxisGizmoColorZ;
    }
}

// 轴名（索引 0/1/2 = X/Y/Z）—— 供编辑器在说明文字里标"什么颜色是什么轴"。
inline const char* AxisGizmoName(int axis_index) {
    CHECK_GE(axis_index, 0);
    CHECK_LT(axis_index, 3);
    switch (axis_index) {
        case 0: return "X";
        case 1: return "Y";
        default: return "Z";
    }
}

}  // namespace jpov

#endif  // JPOV_INTERFACE_AXIS_GIZMO_H_
