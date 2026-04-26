// JPOV InputSnapshot — 帧级输入抽象
//
// 窗口层在每帧结束时将原始输入事件队列结算为 InputSnapshot，
// 传给 OneIteration::Step()。OneIteration 内部不需要维护跨帧状态。
//
// 设计原则：
// - 事件由框架层基于原始升降沿时序计算，OneIteration 只消费结果
// - 鼠标/键盘事件互斥：同一输入源的每个通道同一帧只能有一种事件
// - 低帧率（1fps）下仍保持语义正确，降采样不会丢失 Click 判断

#ifndef JPOV_INPUT_SNAPSHOT_H_
#define JPOV_INPUT_SNAPSHOT_H_

#include <cstdint>
#include <cstddef>

namespace jpov {

// ==================== 类型定义 ====================

// 鼠标事件：每帧每个按键（左/中/右）互斥
enum class MouseEvent : uint8_t {
    None  = 0,  // 悬空：没有交互
    Click = 1,  // 单击：按下+释放在 CLICK_DELTA 内
    Drag  = 2,  // 拖拽：整帧始终按下（可能位移为零）
};

// 一次单击的详细信息
struct ClickEvent {
    float x;           // 推算的点击位置 X（mouse_pos.x - mouse_delta.x * (1 - time_ratio)）
    float y;           // 推算的点击位置 Y
    float time_ratio;  // 帧内时刻 [0, 1]
};

// 鼠标单键状态（左/中/右共享此结构）
// 内部存储：用一个 int8_t 编码
//   -1  → Drag（整帧按下）
//    0  → None
//    1~8 → Click 事件，数值 = 本帧单击次数
struct MouseState {
    // 公开接口
    MouseEvent event() const;
    int click_count() const;       // 仅 Click 时有意义
    const ClickEvent* clicks() const;   // 仅 Click 时有意义
    int clicks_capacity() const;        // clicks 数组容量（固定 8）

    // 内部访问（窗口层设置用）
    int8_t raw;  // 编码值
};

static_assert(sizeof(MouseState) == 1, "MouseState must be 1 byte");

// 键盘单键状态
// 内部存储：用一个 int8_t 编码
//   -1  → Hold（全程按下）
//    0  → None
//    1~8 → Click 事件，数值 = 本帧单击次数
struct KeyState {
    // 公开接口
    bool IsDown() const;     // true if Click or Hold
    bool IsClick() const;    // true if Click
    bool IsHold() const;     // true if Hold
    int click_count() const; // 仅 Click 时有意义

    // 内部访问（窗口层设置用）
    int8_t raw;  // 编码值
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

// ==================== InputSnapshot ====================

constexpr int kMaxClicksPerFrame = 8;  // 1fps × 250ms CLICK_DELTA = 4，给余量

struct InputSnapshot {
    // ---- 鼠标 ----
    float mouse_x;      // 本帧帧末鼠标位置 X
    float mouse_y;      // 本帧帧末鼠标位置 Y
    float mouse_dx;     // 本帧相对上一帧的位移 X
    float mouse_dy;     // 本帧相对上一帧的位移 Y
    float scroll_delta; // 本帧滚轮增量（向上为正）

    MouseState left;    // 左键
    MouseState right;   // 右键
    MouseState middle;  // 中键

    // 单击详情（仅 left/middle/right 中任一为 Click 时有效）
    // 所有 Click 事件共用此池，窗口层按点击顺序填入
    ClickEvent click_pool[kMaxClicksPerFrame * 3];  // 最多 3 键 × 8 次

    // ---- 键盘 ----
    KeyState keys[kMaxKeyCode];  // 索引 = KeyCode 数值

    // ---- 辅助方法 ----
    const KeyState& GetKey(KeyCode key) const {
        return keys[static_cast<int>(key)];
    }

    bool IsCtrlDown() const {
        return GetKey(KeyCode::LeftCtrl).IsDown() ||
               GetKey(KeyCode::RightCtrl).IsDown();
    }
    bool IsShiftDown() const {
        return GetKey(KeyCode::LeftShift).IsDown() ||
               GetKey(KeyCode::RightShift).IsDown();
    }
    bool IsAltDown() const {
        return GetKey(KeyCode::LeftAlt).IsDown() ||
               GetKey(KeyCode::RightAlt).IsDown();
    }
};

// ==================== 实现 ====================

// --- MouseState ---
inline MouseEvent MouseState::event() const {
    if (raw < 0) return MouseEvent::Drag;
    if (raw > 0) return MouseEvent::Click;
    return MouseEvent::None;
}

inline int MouseState::click_count() const {
    return (raw > 0) ? static_cast<int>(raw) : 0;
}

// 注意：clicks() 和 clicks_capacity() 需要外部传入 click_pool 和偏移量
// 不在 MouseState 内部维护，因为 click_pool 是 InputSnapshot 级别的资源。
// 使用方式见下面 InputSnapshot 的辅助方法。

inline int MouseState::clicks_capacity() const {
    return jpov::kMaxClicksPerFrame;
}

// 在 InputSnapshot 上添加鼠标辅助方法
inline const ClickEvent* GetLeftClicks(const InputSnapshot& snap) {
    return snap.left.event() == MouseEvent::Click ? snap.click_pool : nullptr;
}

inline const ClickEvent* GetRightClicks(const InputSnapshot& snap) {
    return snap.right.event() == MouseEvent::Click ? snap.click_pool + kMaxClicksPerFrame : nullptr;
}

inline const ClickEvent* GetMiddleClicks(const InputSnapshot& snap) {
    return snap.right.event() == MouseEvent::Click ? snap.click_pool + kMaxClicksPerFrame * 2 : nullptr;
}

// --- KeyState ---
inline bool KeyState::IsDown()  const { return raw != 0; }
inline bool KeyState::IsClick() const { return raw > 0; }
inline bool KeyState::IsHold()  const { return raw < 0; }

inline int KeyState::click_count() const {
    return (raw > 0) ? static_cast<int>(raw) : 0;
}

}  // namespace jpov

#endif  // JPOV_INPUT_SNAPSHOT_H_
