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
// 本文件的放置状态 = 【先缩放，再旋转，最后平移】（PR 需求原文：
// "原glb 缩放后平移再旋转" 指滑条分组顺序；变换作用顺序仍是经典
//  scale → rotate → translate，与 DrawObject3D 既有语义一致）。
//
// 旋转用两个欧拉角表达，且**旋转轴固定在世界系**（不随模型自身姿态滚动，
// 避免万向节式"转着转着轴跑歪"的困惑）：
//   rx = 绕世界 X 轴俯仰（pitch）
//   ry = 绕世界 Y 轴偏航（yaw）
// 组合顺序：R = Ry(ry) · Rx(rx)（先俯仰后偏航——先让模型绕自身横轴低头，
// 再整体转到目标方位；对应"逐控件可复现"的直观手感）。
//
// 由于 BuildModelMatrix 用 (up,front) 重建正交基，本文件只需把 Rx/Ry 作用在
// 单位基 (0,1,0)/(0,0,1) 上即可得到 up/front，**不受 scale/center 影响**。

#ifndef JPOV_DEMO_MODEL_PLACEMENT_H_
#define JPOV_DEMO_MODEL_PLACEMENT_H_

#include <algorithm>
#include <cmath>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"

namespace jpov_viewer {

// 度数主值折叠到 [-180, 180)（等价角代表元）。
// 为什么不是 std::clamp：旋转交互是**连续累积**的（拖满一屏 = 360°），
// clamp 会让角度卡死在 ±180°（拖过头反而"弹不回"），违背"一屏一圈"直觉；
// 而角度的等价类本就是 mod 360°，故用"折叠"而非"夹断"。
// Pre-condition: 无（对任意有限值都收敛）。
inline float WrapDegToHalfOpen(float deg) {
    float d = std::fmod(deg, 360.0f);   // → (-360, 360)
    if (d >= 180.0f) d -= 360.0f;       // [180, 360) → [-180, 0)
    if (d < -180.0f) d += 360.0f;       // (-360, -180) → [0, 180)
    return d;
}

// 模型放置状态（状态外置：由调用方持有，UI/交互写回，本层只读）。
//
// 量纲约定：角度统一用**度**（与 UI 滑条/人手输入一致），在 ToDrawParams
// 边界处换算弧度；长度用米。
struct ModelPlacement {
    // 缩放 [0.1, 10]，默认 1.0（PR 需求定档）。
    float scale = 1.0f;
    // 平移（米），各轴 [-3, +3]，默认 0。
    float tx = 0.0f;
    float ty = 0.0f;
    float tz = 0.0f;
    // 旋转（度）：rx 绕世界 X 俯仰，ry 绕世界 Y 偏航；默认 0（= 恒等，坐标系等价）。
    // 存储恒为折叠后的主值 [-180, 180)。角度本质无界（可无限转），存储取主值
    // 只为了避免长时间拖动数值无限增长。
    float rx_deg = 0.0f;
    float ry_deg = 0.0f;

    // ---- 合法范围常量（滑条与交互共用，单点定义，杜绝两处 clamp 分叉）----
    static constexpr float kScaleMin = 0.1f;
    static constexpr float kScaleMax = 10.0f;
    static constexpr float kTransMin = -3.0f;
    static constexpr float kTransMax = 3.0f;
    // 旋转滑条的展示区间（角度本身无界，滑条需要一个有限区间；越界值会被折叠）。
    static constexpr float kRotMin = -180.0f;
    static constexpr float kRotMax = 180.0f;

    // 把本状态 clamp/折叠到全部合法范围（防御外部误赋值；幂等）。
    // 缩放/平移用 clamp（非周期量）；旋转用折叠（周期量，见 WrapDegToHalfOpen）。
    ModelPlacement Clamped() const {
        ModelPlacement p = *this;
        p.scale = std::clamp(p.scale, kScaleMin, kScaleMax);
        p.tx = std::clamp(p.tx, kTransMin, kTransMax);
        p.ty = std::clamp(p.ty, kTransMin, kTransMax);
        p.tz = std::clamp(p.tz, kTransMin, kTransMax);
        p.rx_deg = WrapDegToHalfOpen(p.rx_deg);
        p.ry_deg = WrapDegToHalfOpen(p.ry_deg);
        return p;
    }
};

// 度数 → 弧度。
inline constexpr double kPlacementDegToRad = 3.14159265358979323846 / 180.0;

// 绕世界 Y 轴偏航 ry（右手系：+Y 从下往上看逆时针 = CCW）。返回作用在基向量上的
// 3x3 旋转的行（即 R·v 的公式展开，避免额外矩阵类型）。
//
//   Ry = [ cos  0  sin ]
//        [  0   1   0  ]
//        [-sin  0  cos ]
inline jpov::Vec3f RotateWorldY(const jpov::Vec3f& v, float ry_deg) {
    const float a = static_cast<float>(ry_deg * kPlacementDegToRad);
    const float c = std::cos(a);
    const float s = std::sin(a);
    return {c * v.x() + s * v.z(), v.y(), -s * v.x() + c * v.z()};
}

// 绕世界 X 轴俯仰 rx（右手系）。
//
//   Rx = [ 1   0    0  ]
//        [ 0  cos -sin ]
//        [ 0  sin  cos ]
inline jpov::Vec3f RotateWorldX(const jpov::Vec3f& v, float rx_deg) {
    const float a = static_cast<float>(rx_deg * kPlacementDegToRad);
    const float c = std::cos(a);
    const float s = std::sin(a);
    return {v.x(), c * v.y() - s * v.z(), s * v.y() + c * v.z()};
}

// 放置状态 → DrawObject3D/DrawGltfObject 的 (center, up, front, scale) 四元组。
//
// 常量基向量：
//   up    = R · (0,1,0)     （R = Ry·Rx）
//   front = R · (0,0,1)
//   center = (tx, ty, tz)
// up/front 均为单位向量（旋转保长），故 BuildModelMatrix 内部归一化是恒等操作。
//
// Pre-condition: 无（内部先 Clamped()，非法输入被夹断而非 UB）。
struct DrawPlacement {
    jpov::Vec3f center;
    jpov::Vec3f up;
    jpov::Vec3f front;
    float scale;
};

inline DrawPlacement ToDrawParams(const ModelPlacement& in) {
    const ModelPlacement p = in.Clamped();
    const jpov::Vec3f up0 = {0.0f, 1.0f, 0.0f};
    const jpov::Vec3f fr0 = {0.0f, 0.0f, 1.0f};
    // R = Ry(ry) · Rx(rx)：先绕世界 X 俯仰，再绕世界 Y 偏航。
    const jpov::Vec3f up = RotateWorldY(RotateWorldX(up0, p.rx_deg), p.ry_deg);
    const jpov::Vec3f fr = RotateWorldY(RotateWorldX(fr0, p.rx_deg), p.ry_deg);
    DrawPlacement d;
    d.center = {p.tx, p.ty, p.tz};
    d.up = up;
    d.front = fr;
    d.scale = p.scale;
    return d;
}

// ---- 交互：左键横向 drag → 旋转 ----
//
// 需求（PR 原文）：
//   - 不按 Ctrl，横向拖动 = 模型绕【世界 X 轴】旋转，右滑逆时针；
//   - 按住左 Ctrl，横向拖动 = 模型绕【世界 Y 轴】旋转，右滑逆时针；
//   - 忽略左键纵向 drag 分量。
//
// "右滑逆时针"的判定（右手系，从被说到的**正轴一侧朝原点看**）：
//   绕 +X 看：+Y→+Z。右滑即 +X 转轴方向看是逆时针 → 绕 X 角速度 +dx·k
//             → rx += dx·k（正号）。
//   绕 +Y 看：世界 XZ 平面里 +Z→+X 才是逆时针（+Y 从上看：X 为右、Z 为朝观察者
//             *下方*的面内轴；朝 +Z 看是顺时针）→ 绕 Y 角速度 -dx·k
//             → ry -= dx·k（负号）。
//   本函数把该符号差异内聚在此，调用方只传"是否按 Ctrl"。
//
//   dx      — 本帧左键横向位移（像素，右为正）
//   ctrl    — 本帧左 Ctrl 是否按下（按下=绕世界 Y，否则绕世界 X）
//   window_w— 窗口宽（像素），映射用：横向拖满一屏 = 转 360°
// 返回 true = 本帧确实改变了旋转（供调用方做"有变化"日志/重绘判断）。
inline bool ApplyRotateDrag(ModelPlacement* p /*inout*/, float dx, bool ctrl,
                            int window_w) {
    CHECK_NOTNULL(p);
    CHECK_GT(window_w, 0);
    if (dx == 0.0f) return false;   // 纵向分量在此层已被调用方丢弃

    // 像素 → 角度：拖满一屏宽 = 360°（与查看器相机同样的一屏一圈手感）。
    const float kDegPerPx = 360.0f / static_cast<float>(window_w);
    const float delta = dx * kDegPerPx;

    if (ctrl) {
        p->ry_deg -= delta;   // 绕世界 Y：右滑逆时针
    } else {
        p->rx_deg += delta;   // 绕世界 X：右滑逆时针
    }
    // 收敛到 [-180,180)（等价角取主值，避免长时间拖动数值无限增长）。
    p->rx_deg = WrapDegToHalfOpen(p->rx_deg);
    p->ry_deg = WrapDegToHalfOpen(p->ry_deg);
    return true;
}

}  // namespace jpov_viewer

#endif  // JPOV_DEMO_MODEL_PLACEMENT_H_
