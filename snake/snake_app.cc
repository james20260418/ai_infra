#include <algorithm>

#include "snake/snake_app.h"

namespace snake {

int SnakeApp::GetLogicStepFrames() const {
    int len = static_cast<int>(state_.snake.size());
    // 蛇越长度越快：长度3→8帧/步，长度25+→4帧/步
    int reduction = std::min((len - 3) / 6, 4);
    return 8 - reduction;
}

void SnakeApp::OneIteration(int64_t frame_count,
                             const jpov::InputSnapshot& input,
                             const jpov::WindowInfo& winfo,
                             jpov::RenderCommandList* cmds) {
    (void)winfo;

    // ---- 初始化游戏状态（首次帧） ----
    if (!initialized_) {
        ResetGame(&state_);
        initialized_ = true;
    }

    // ---- 游戏进行中才处理输入和逻辑更新 ----
    if (!state_.game_over) {
        // 1. 处理键盘输入
        HandleInput(input);

        // 2. 按逻辑步长推进（根据蛇身长度动态调整速度）
        //    每 cached_step_frames 渲染帧只推进一帧逻辑
        if (frame_count % cached_step_frames_ == 0) {
            // 每次逻辑推进前更新一次步长（蛇身长度变化后缓存更新）
            cached_step_frames_ = GetLogicStepFrames();
            // MoveSnake 内部做了：方向更新、蛇头插入、吃食物检查/增长/重生成、碰撞检测
            MoveSnake(&state_);
        }
    }

    // 3. 渲染画面（即使 game_over 也渲染，显示结束画面）
    renderer_.Render(state_, winfo, cmds);
}

void SnakeApp::HandleInput(const jpov::InputSnapshot& input) {
    // 游戏结束时，R 键重启
    if (state_.game_over) {
        if (input.GetKey(jpov::KeyCode::R).IsClick()) {
            ResetGame(&state_);
        }
        return;
    }

    // 读取方向键输入（Click 或 Hold 状态）
    // 优先级：最近的输入覆盖之前的方向缓冲
    if (input.GetKey(jpov::KeyCode::Up).IsClick() ||
        input.GetKey(jpov::KeyCode::Up).IsHold()) {
        state_.next_direction = Direction::kUp;
    } else if (input.GetKey(jpov::KeyCode::Down).IsClick() ||
               input.GetKey(jpov::KeyCode::Down).IsHold()) {
        state_.next_direction = Direction::kDown;
    } else if (input.GetKey(jpov::KeyCode::Left).IsClick() ||
               input.GetKey(jpov::KeyCode::Left).IsHold()) {
        state_.next_direction = Direction::kLeft;
    } else if (input.GetKey(jpov::KeyCode::Right).IsClick() ||
               input.GetKey(jpov::KeyCode::Right).IsHold()) {
        state_.next_direction = Direction::kRight;
    }
}

} // namespace snake
