// JPOV InputSnapshot — 帧级输入抽象
//
// 窗口层在每帧结束时将原始输入事件队列结算为 InputSnapshot，
// 传给 OneIteration::Step()。OneIteration 内部不需要维护跨帧状态。
//
// 设计原则：
// - 事件由框架层基于原始升降沿时序计算，OneIteration 只消费结果
// - 鼠标/键盘事件互斥：同一输入源的每个通道同一帧只能有一种事件
// - 低帧率（1fps）下仍保持语义正确，降采样不会丢失 Click 判断
//
// 坐标系统一约定：
// - 屏幕坐标：原点在窗口左上角，x 向右为正，y 向下为正
//   右上角 = (width, 0)，左下角 = (0, height)，右下角 = (width, height)
// - 值类型（float）允许子像素精度
// - 3D 世界坐标属于渲染层，不在 InputSnapshot 中定义

#ifndef JPOV_INPUT_SNAPSHOT_H_
#define JPOV_INPUT_SNAPSHOT_H_

#include <cstdint>
#include <cstddef>
#include <glog/logging.h>

namespace jpov {

// ==================== 类型定义 ====================

// 鼠标事件：每帧每个按键（左/中/右）互斥
//
// None  — 悬空：该键在本帧没有任何交互。
//
// Click — 单击：本帧有释放事件，且按下期间鼠标无移动。
//         每次 Click 记录释放时刻的位置。
//         多个 Click 可出现在同一帧（低帧率场景）。
//         典型使用：按钮点击、选项选择、单次操作触发。
//
// Hold  — 按住不放：键处于按下状态，且按下期间鼠标无移动。
//         是 Click 的按下态延续。
//         典型使用：等待用户决定是 click 还是 drag。
//
// Drag  — 拖拽：键处于按下状态，且按下期间鼠标有过移动。
//         一旦移动过就永远不算 Click。
//         典型使用：窗口拖动、选区框选、视角旋转。
enum class MouseEvent : uint8_t {
    None  = 0,
    Click = 1,
    Hold  = 2,
    Drag  = 3,
};

// 一次单击的详细信息
// x/y — click 释放时鼠标的窗口位置（像素坐标，左上角为原点）
// time_ratio — 帧内时刻 [0, 1]，0=帧开始，1=帧结束
struct ClickEvent {
    float x;
    float y;
    float time_ratio;
};

// 鼠标单键状态（左/中/右各一个）
// 内部存储：int8_t 编码
//   -2  → Hold（按下但未移动）
//   -1  → Drag（按下且移动过）
//    0  → None（默认构造）
//    1~8 → Click 事件，数值 = 本帧单击次数
struct MouseState {
    // 公开接口
    bool IsNone() const;
    bool IsClick() const;
    bool IsHold() const;
    bool IsDrag() const;
    int click_count() const;       // 仅 Click 时有意义

    // 内部访问（窗口层设置用）
    int8_t raw;  // 编码值，默认 0（None）
};

static_assert(sizeof(MouseState) == 1, "MouseState must be 1 byte");

// 键盘单键状态
// 内部存储：int8_t 编码
//   -1  → Hold（全程按下）
//    0  → None（默认构造）
//    1~8 → Click 事件，数值 = 本帧单击次数
struct KeyState {
    // 公开接口
    bool IsNone() const;
    bool IsClick() const;
    bool IsHold() const;
    int click_count() const; // 仅 Click 时有意义

    // 内部访问（窗口层设置用）
    int8_t raw;  // 编码值，默认 0（None）
};

static_assert(sizeof(KeyState) == 1, "KeyState must be 1 byte");

// ==================== KeyCode 枚举 ====================

enum class KeyCode : uint16_t {
    // GLFW key codes 的子集，只包含常用键
    // 完整列表对应 GLFW_KEY_* 的数值
    Unknown = 0,

    // 字母
    A = 65, B = 66, C = 67, D = 68, E = 69, F = 70,
    G = 71, H = 72, I = 73, J = 74, K = 75, L = 76,
    M = 77, N = 78, O = 79, P = 80, Q = 81, R = 82,
    S = 83, T = 84, U = 85, V = 86, W = 87, X = 88,
    Y = 89, Z = 90,

    // 数字
    _0 = 48, _1 = 49, _2 = 50, _3 = 51, _4 = 52,
    _5 = 53, _6 = 54, _7 = 55, _8 = 56, _9 = 57,

    // 功能键
    F1 = 290, F2 = 291, F3 = 292, F4 = 293,
    F5 = 294, F6 = 295, F7 = 296, F8 = 297,
    F9 = 298, F10 = 299, F11 = 300, F12 = 301,

    // 控制键
    Space      = 32,
    Enter      = 257,
    Escape     = 256,
    Tab        = 258,
    Backspace  = 259,
    Delete     = 261,
    LeftShift  = 340, RightShift  = 344,
    LeftCtrl   = 341, RightCtrl   = 345,
    LeftAlt    = 342, RightAlt    = 346,

    // 方向键
    Left    = 263,
    Right   = 262,
    Up      = 265,
    Down    = 264,

    // 范围上限
    MaxKey = 348,  // GLFW_KEY_LAST
};

constexpr int kMaxKeyCode = static_cast<int>(KeyCode::MaxKey) + 1;

// ==================== 常量 ====================

// 每键每帧最大单击次数
// 1fps = 1000ms/帧，CLICK_DELTA = 250ms → 最多 4 次/帧
// 取 8 给足余量
constexpr int kMaxClicksPerFrame = 8;

// ==================== InputSnapshot ====================

struct InputSnapshot {
    // ---- 鼠标 ----
    float mouse_x;      // 本帧帧末鼠标位置 X（像素坐标，左上角为原点）
    float mouse_y;      // 本帧帧末鼠标位置 Y
    float mouse_dx;     // 本帧相对上一帧的位移 X
    float mouse_dy;     // 本帧相对上一帧的位移 Y
    float scroll_delta; // 本帧滚轮增量（1.0 = 滚一格 / 一个 notch，正=向上）

    MouseState left;    // 左键
    MouseState right;   // 右键
    MouseState middle;  // 中键

    // 单击详情（每个键独立池）
    // 仅该键为 Click 时有效，窗口层按时间顺序填入
    ClickEvent left_clicks[kMaxClicksPerFrame];
    ClickEvent right_clicks[kMaxClicksPerFrame];
    ClickEvent middle_clicks[kMaxClicksPerFrame];

    // ---- 键盘 ----
    KeyState keys[kMaxKeyCode];  // 索引 = KeyCode 数值

    // ---- 辅助方法 ----

    // Pre-condition: key != KeyCode::Unknown && key <= KeyCode::MaxKey
    // 传入 Unknown 或超出 MaxKey 的非法值会导致 CHECK crash
    const KeyState& GetKey(KeyCode key) const {
        int idx = static_cast<int>(key);
        CHECK_GE(idx, 1);  // Unknown(0) 不允许访问
        CHECK_LT(idx, kMaxKeyCode);
        return keys[idx];
    }
};

// ==================== 实现 ====================

// --- MouseState ---
inline bool MouseState::IsNone()  const { return raw == 0; }
inline bool MouseState::IsClick() const { return raw > 0; }
inline bool MouseState::IsHold()  const { return raw == -2; }
inline bool MouseState::IsDrag()  const { return raw == -1; }

inline int MouseState::click_count() const {
    return (raw > 0) ? static_cast<int>(raw) : 0;
}

// --- KeyState ---
inline bool KeyState::IsNone()  const { return raw == 0; }
inline bool KeyState::IsClick() const { return raw > 0; }
inline bool KeyState::IsHold()  const { return raw < 0; }

inline int KeyState::click_count() const {
    return (raw > 0) ? static_cast<int>(raw) : 0;
}

}  // namespace jpov

#endif  // JPOV_INPUT_SNAPSHOT_H_
