// JPOV 模型编辑器 — 模型放置（缩放 / 平移 / 旋转）纯函数层
//
// 定位（task #6，retarget 前置组件）：让人类用户用手把 glTF 微调到与目标骨架
// 对齐的姿态。本文件是"放置状态 → DrawObject3D 入参"的**唯一真相**，与渲染
// 后端零耦合：产出 center/up/front/scale 四个值，原样喂给现有
// RenderCommandList::DrawGltfObject（内部就是 DrawObject3D），**不改渲染器**。
//
// 变换语义（与 src/object3d/object3d_renderer.cc 的 BuildModelMatrix 严格一致）：
//
//   局部坐标轴映射： +X = left = normalize(cross(up,front))， +Y = up， +Z = front
//   顶点右乘：        v_world = T(center) · R(up,front) · (scale · v_local)
//
// 本文件的放置状态 = 【先缩放，再旋转，最后平移】。
//
// ═══════════════ 朝向状态为什么是 up/front 矢量，不是欧拉角 ═══════════════
//
// 朝向的**状态量就是 (up, front) 这对矢量本身**，不是任何角度参数。
// 理由（Danis 定调）：
//
//   1. 自由度对不上。两个 float 角度（固定轴序 Rx/Ry）只有 2 个自由度，且
//      第二次旋转的轴是"第一次转完之后的坐标系轴"；而需求要的是
//      「沿世界 Y 转，再沿世界 X 转」这种**任意顺序、任意次数的世界轴累积**，
//      可以扫出任意朝向。参数化做不到，直接累积矢量做得到。
//   2. 跨帧状态必须是朝向本身。角度是"参数化的中间量"——存它等于把连续
//      累积的姿态投影到 2 个标量上，中间信息丢失。存 (up, front) 才是
//      无损的跨帧姿态状态（每次拖动在新姿态上继续施加增量旋转）。
//
// 因此交互直接对 (up, front) 施加**世界轴增量旋转**（累积、不重新参数化）。
// 不复用欧拉角中间层，也就不存在"先 Y 后 X 顺序写错"这类隐患。
//
// ⚠️ 正交性：两次增量旋转都绕世界轴，且初始 (up, front) 正交归一，
//    故正交归一性在整个累积过程中保持（见 ApplyYawDelta/ApplyPitchDelta）。

#ifndef JPOV_DEMO_MODEL_PLACEMENT_H_
#define JPOV_DEMO_MODEL_PLACEMENT_H_

#include <algorithm>
#include <cmath>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"

namespace jpov_viewer {

// 度数 → 弧度。
inline constexpr double kPlacementDegToRad = 3.14159265358979323846 / 180.0;

// 绕世界 Y 轴旋转矢量 v，转角 angle_deg（右手系：+Y 朝上看逆时针）。
//
//   Ry = [ cos  0  sin ]
//        [  0   1   0  ]
//        [-sin  0  cos ]
inline jpov::Vec3f RotateWorldY(const jpov::Vec3f& v, float angle_deg) {
    const float a = static_cast<float>(angle_deg * kPlacementDegToRad);
    const float c = std::cos(a);
    const float s = std::sin(a);
    return {c * v.x() + s * v.z(), v.y(), -s * v.x() + c * v.z()};
}

// 绕世界 X 轴旋转矢量 v，转角 angle_deg（右手系：从 +X 朝原点看逆时针）。
//
//   Rx = [ 1   0    0  ]
//        [ 0  cos -sin ]
//        [ 0  sin  cos ]
inline jpov::Vec3f RotateWorldX(const jpov::Vec3f& v, float angle_deg) {
    const float a = static_cast<float>(angle_deg * kPlacementDegToRad);
    const float c = std::cos(a);
    const float s = std::sin(a);
    return {v.x(), c * v.y() - s * v.z(), s * v.y() + c * v.z()};
}

// 模型放置状态（状态外置：由调用方持有，UI/交互写回，本层只读）。
//
// 朝向状态 = (up, front) 两个单位矢量（见文件顶部"为什么不是欧拉角"）。
// 平移用米，缩放无量纲。
struct ModelPlacement {
    // 缩放 [0.1, 10]，默认 1.0（PR 需求定档）。
    float scale = 1.0f;
    // 平移（米），各轴 [-3, +3]，默认 0。
    float tx = 0.0f;
    float ty = 0.0f;
    float tz = 0.0f;

    // ---- 朝向状态（局部 +Y / +Z 在世界系中的指向）----
    // 默认 = 局部系 ≡ 世界系（坐标系等价：up=+Y, front=+Z）。
    // 恒为单位矢量且互相垂直（由增量旋转保持，见 ApplyYaw/PitchDelta）。
    jpov::Vec3f up    = {0.0f, 1.0f, 0.0f};   // 局部 +Y 的世界方向
    jpov::Vec3f front = {0.0f, 0.0f, 1.0f};   // 局部 +Z 的世界方向

    // ---- 合法范围常量（滑条与交互共用，单点定义，杜绝两处 clamp 分叉）----
    static constexpr float kScaleMin = 0.1f;
    static constexpr float kScaleMax = 10.0f;
    static constexpr float kTransMin = -3.0f;
    static constexpr float kTransMax = 3.0f;

    // 把非旋转分量 clamp 到合法范围（防御外部误赋值；幂等）。
    // 朝向不做 clamp（它是单位矢量，无"越界"概念，见头文件顶部理由）。
    ModelPlacement Clamped() const {
        ModelPlacement p = *this;
        p.scale = std::clamp(p.scale, kScaleMin, kScaleMax);
        p.tx = std::clamp(p.tx, kTransMin, kTransMax);
        p.ty = std::clamp(p.ty, kTransMin, kTransMax);
        p.tz = std::clamp(p.tz, kTransMin, kTransMax);
        return p;
    }
};

// ---- 朝向增量旋转（交互的唯一入口）----
//
// 绕世界 Y 轴偏航 delta_deg：up 的 Y 分量不变（在 Y 轴上转不动），front 在
// XZ 平面内转。两个矢量同施同一旋转 → 正交归一保持。
inline void ApplyYawDelta(ModelPlacement* p /*inout*/, float delta_deg) {
    CHECK_NOTNULL(p);
    p->up    = RotateWorldY(p->up, delta_deg);
    p->front = RotateWorldY(p->front, delta_deg);
}

// 绕世界 X 轴俯仰 delta_deg：up 与 front 都绕世界 X 转（X 分量不变）。
inline void ApplyPitchDelta(ModelPlacement* p /*inout*/, float delta_deg) {
    CHECK_NOTNULL(p);
    p->up    = RotateWorldX(p->up, delta_deg);
    p->front = RotateWorldX(p->front, delta_deg);
}

// ---- 交互：左键横向 drag → 旋转 ----
//
// 需求（PR 原文）：
//   - 不按 Ctrl，横向拖动 = 模型绕【世界 X 轴】旋转，右滑逆时针；
//   - 按住左 Ctrl，横向拖动 = 模型绕【世界 Y 轴】旋转，右滑逆时针；
//   - 忽略左键纵向 drag 分量。
//
// "右滑逆时针"的符号推导（右手系，从被说到的**正轴一侧朝原点看**）：
//   绕 +X 看：+Y→+Z。右滑即沿 +X 转轴方向看是逆时针 → 角速度 +dx·k
//             → 俯仰角增量取正号。
//   绕 +Y 看：世界 XZ 平面里 +Z→+X 才是逆时针（+Y 从上看时 X 为右、Z 为
//             朝观察者*下方*的面内轴；直接朝 +Z 看是顺时针）→ 角速度 -dx·k
//             → 偏航角增量取负号。
// 该符号差异内聚在此，调用方只传"是否按 Ctrl"。
//
//   dx      — 本帧左键横向位移（像素，右为正）
//   ctrl    — 本帧左 Ctrl 是否按下（按下=绕世界 Y，否则绕世界 X）
//   window_w— 窗口宽（像素），映射用：横向拖满一屏 = 转 360°
// 返回 true = 本帧确实改变了朝向（供调用方做"有变化"日志/重绘判断）。
inline bool ApplyRotateDrag(ModelPlacement* p /*inout*/, float dx, bool ctrl,
                            int window_w) {
    CHECK_NOTNULL(p);
    CHECK_GT(window_w, 0);
    if (dx == 0.0f) return false;   // 纵向分量在此层已被调用方丢弃

    // 像素 → 角度：拖满一屏宽 = 360°（与查看器相机同样的一屏一圈手感）。
    const float kDegPerPx = 360.0f / static_cast<float>(window_w);
    const float delta = dx * kDegPerPx;

    if (ctrl) {
        ApplyYawDelta(p, -delta);     // 绕世界 Y：右滑逆时针
    } else {
        ApplyPitchDelta(p, delta);    // 绕世界 X：右滑逆时针
    }
    return true;
}

// 放置状态 → DrawObject3D/DrawGltfObject 的 (center, up, front, scale) 四元组。
// 朝向直接透传状态里的 up/front（它们本就是绘制要的量），无需重新参数化。
//
// Pre-condition: 无（内部先 Clamped() 处理缩放/平移；朝向按状态原样透传）。
struct DrawPlacement {
    jpov::Vec3f center;
    jpov::Vec3f up;
    jpov::Vec3f front;
    float scale;
};

inline DrawPlacement ToDrawParams(const ModelPlacement& in) {
    const ModelPlacement p = in.Clamped();
    DrawPlacement d;
    d.center = {p.tx, p.ty, p.tz};
    d.up = p.up;
    d.front = p.front;
    d.scale = p.scale;
    return d;
}

}  // namespace jpov_viewer

#endif  // JPOV_DEMO_MODEL_PLACEMENT_H_
