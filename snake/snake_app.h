#pragma once

#include <cstdint>

#include "tools/jpov/include/jpov/jpov.h"
#include "snake/snake_types.h"
#include "snake/snake_logic.h"
#include "snake/snake_renderer.h"

namespace snake {

// SnakeApp — 贪吃蛇游戏主应用
//
// 继承 JPOV，在每帧 OneIteration 中处理输入、更新游戏状态、产出渲染指令。
//
// 生命周期:
//   SnakeApp app(cfg);
//   app.Init();
//   app.Run();   // 或 app.RunOnce(...)
//   app.Finalize();
class SnakeApp : public JPOV {
public:
    using JPOV::JPOV;

    // 每帧渲染逻辑（纯虚函数实现）
    // 1. 从 InputSnapshot 读取方向键 → 更新 GameState.next_direction
    // 2. 以固定步长推进游戏逻辑
    // 3. 生成渲染指令（网格、蛇、食物、分数、GameOver 提示）
    void OneIteration(int64_t frame_count,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override;

    // 获取当前游戏状态（供外部读取，如用于自测验收）
    const GameState& GetState() const { return state_; }

    // 游戏逻辑帧率（每多少渲染帧推进一次游戏逻辑）
    // 根据蛇身长度动态调整：蛇越长速度越快
    // 初始长度 3 → 8帧/步；长度 25+ → 4帧/步（最快）
    // 计算公式：8 - min(length / 8, 4)，范围 [4, 8]
    int GetLogicStepFrames() const;

private:
    // 处理键盘输入 → 更新方向
    void HandleInput(const jpov::InputSnapshot& input);

    // 游戏渲染器
    GameRenderer renderer_;

    // 当前生效的逻辑步长（缓存避免每帧计算）
    int cached_step_frames_ = 8;

    // 游戏状态
    GameState state_;
    bool initialized_ = false;
};

}  // namespace snake
