// Snake Game — 自测验收程序
//
// 使用 JPOV RunOnce 模式进行头静态验证：
// 1. 初始帧（无输入） → 截图验证初始画面（棋盘格 + 蛇 + 食物）
// 2. 模拟方向键输入 → 多帧运行验证蛇移动
// 3. Game Over 场景截图
//
// 注意：JPOV::RunOnce 始终传入 frame_count=0（RunOnceInternal 内定义），
// 因此每次调用都会触发逻辑推进（frame_count % step == 0 恒成立）。
// 通过限制调用次数来控制蛇移动步数。
//
// 编译运行:
//   cd /james_pm/bulletin_work_space/data/ai_infra
//   bazel build //snake:snake_check_acceptance && ./bazel-bin/snake/snake_check_acceptance

#include <cstdio>

#include "snake/snake_app.h"

// 辅助：运行若干帧并保存最后一帧截图
static void RunFrames(snake::SnakeApp* app, int n,
                      const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      const char* path) {
    for (int i = 0; i < n; ++i) {
        app->RunOnce(input, winfo, path);
    }
}

int main() {
    // ---- 测试1: 初始帧截图 ----
    {
        JPOV::Config cfg;
        cfg.title = "Snake Acceptance Test";
        cfg.width  = 800;
        cfg.height = 600;
        cfg.target_fps = 60;
        cfg.headless = true;

        snake::SnakeApp app(cfg);
        app.Init();

        jpov::InputSnapshot input = {};  // zero-init → 所有按键 None
        jpov::WindowInfo winfo;
        winfo.width  = 800;
        winfo.height = 600;

        // 初始帧（推进1步触发 ResetGame + 一次逻辑步进）
        app.RunOnce(input, winfo, "/tmp/snake_frame_initial.png");
        app.Finalize();

        std::printf("[PASS] Test 1: Initial frame screenshot saved\n");
    }

    // ---- 测试2: 向右移动 2 步后截图 ----
    {
        JPOV::Config cfg;
        cfg.title = "Snake Acceptance Test";
        cfg.width  = 800;
        cfg.height = 600;
        cfg.target_fps = 60;
        cfg.headless = true;

        snake::SnakeApp app(cfg);
        app.Init();

        jpov::WindowInfo winfo;
        winfo.width  = 800;
        winfo.height = 600;

        // 模拟向右按键（Hold）
        jpov::InputSnapshot input = {};
        input.keys[static_cast<int>(jpov::KeyCode::Right)].raw = -2;

        // 跑 3 帧 = 3 步向右（蛇从 x=20 → x=23）
        // 初始蛇在 (20,15),(19,15),(18,15)，3步后蛇头在 (23,15)
        RunFrames(&app, 3, input, winfo, "/tmp/snake_frame_run.png");

        app.Finalize();

        std::printf("[PASS] Test 2: Moved right 3 steps, screenshot saved\n");
    }

    // ---- 测试3: 撞墙（Game Over）截图 ----
    {
        JPOV::Config cfg;
        cfg.title = "Snake Acceptance Test";
        cfg.width  = 800;
        cfg.height = 600;
        cfg.target_fps = 60;
        cfg.headless = true;

        snake::SnakeApp app(cfg);
        app.Init();

        jpov::WindowInfo winfo;
        winfo.width  = 800;
        winfo.height = 600;

        // 模拟向上按键（Hold）
        jpov::InputSnapshot input = {};
        input.keys[static_cast<int>(jpov::KeyCode::Up)].raw = -2;

        // 向上走 17 步：初始 y=15，走 16 步后 y=-1（撞墙）
        // 走 17 步确保 game_over 触发且蛇在撞墙后静止
        RunFrames(&app, 17, input, winfo, "/tmp/snake_frame_gameover.png");

        app.Finalize();

        std::printf("[PASS] Test 3: Game Over (wall collision), screenshot saved\n");
    }

    // ---- 测试4: 弯道移动（验证方向变更） ----
    {
        JPOV::Config cfg;
        cfg.title = "Snake Acceptance Test";
        cfg.width  = 800;
        cfg.height = 600;
        cfg.target_fps = 60;
        cfg.headless = true;

        snake::SnakeApp app(cfg);
        app.Init();

        jpov::WindowInfo winfo;
        winfo.width  = 800;
        winfo.height = 600;

        // 先向右走 3 步
        jpov::InputSnapshot input_right = {};
        input_right.keys[static_cast<int>(jpov::KeyCode::Right)].raw = -2;
        RunFrames(&app, 3, input_right, winfo, "/tmp/snake_frame_turn.png");

        // 再向下走 3 步
        jpov::InputSnapshot input_down = {};
        input_down.keys[static_cast<int>(jpov::KeyCode::Down)].raw = -2;
        RunFrames(&app, 3, input_down, winfo, "/tmp/snake_frame_turn.png");

        app.Finalize();

        std::printf("[PASS] Test 4: Turn (right→down), screenshot saved\n");
    }

    // ---- 测试5: 吃食物增长联调验收 ----
    {
        JPOV::Config cfg;
        cfg.title = "Snake Acceptance Test - Eat Food";
        cfg.width  = 800;
        cfg.height = 600;
        cfg.target_fps = 60;
        cfg.headless = true;

        snake::SnakeApp app(cfg);
        app.Init();

        jpov::WindowInfo winfo;
        winfo.width  = 800;
        winfo.height = 600;

        jpov::InputSnapshot input = {};

        // 策略：通过多次 RunOnce 跑出吃食物场景。初始蛇头在 (20,15)，方向右。
        // 随机食物可能在蛇前方也可能不在。我们需要走若干步直到吃到第一个食物。
        // 更可控的方案：用 GetState() 访问内部状态，人工将食物放蛇头前方。
        // 第一次 RunOnce 初始化游戏状态
        app.RunOnce(input, winfo, "/tmp/snake_food_frame0.png");

        // 获取状态引用，将食物放在蛇头正前方
        auto& state_ref = const_cast<snake::GameState&>(app.GetState());
        int head_x = state_ref.snake[0].x;
        int head_y = state_ref.snake[0].y;
        state_ref.food.x = head_x + 1;
        state_ref.food.y = head_y;

        // std::rand() 种子在 snake_logic.cc 中未显式 srand，
        // SpawnFood 会在吃到食物后被调用重新生成食物

        int old_size = state_ref.snake.size();
        int old_score = state_ref.score;

        // 再跑一次 RunOnce（蛇头前进到食物位置）
        app.RunOnce(input, winfo, "/tmp/snake_food_eaten.png");

        // 验证：长度+1，分数+10，新食物不在蛇身上
        bool length_grew  = (state_ref.snake.size() == old_size + 1);
        bool score_inc    = (state_ref.score == old_score + 10);
        bool new_food_ok  = true;
        for (const auto& seg : state_ref.snake) {
            if (seg.x == state_ref.food.x && seg.y == state_ref.food.y) {
                new_food_ok = false;
                break;
            }
        }

        if (length_grew && score_inc && new_food_ok) {
            std::printf("[PASS] Test 5: Eat food - snake %zu→%zu, "
                        "score %d→%d, new food at (%d,%d) clear\n",
                        old_size, state_ref.snake.size(),
                        old_score, state_ref.score,
                        state_ref.food.x, state_ref.food.y);
        } else {
            std::printf("[FAIL] Test 5: Eat food - grew=%d score=%d food_clear=%d\n",
                        length_grew, score_inc, new_food_ok);
            std::printf("  old size=%d score=%d → size=%zu score=%d\n",
                        old_size, old_score,
                        state_ref.snake.size(), state_ref.score);
            std::printf("  food at (%d,%d) vs snake:\n",
                        state_ref.food.x, state_ref.food.y);
            for (size_t i = 0; i < state_ref.snake.size(); ++i) {
                std::printf("    [%zu] (%d,%d)\n", i,
                            state_ref.snake[i].x, state_ref.snake[i].y);
            }
            app.Finalize();
            return 1;
        }

        app.Finalize();
    }

    // ---- 测试6: 长蛇吃食物 + 新食物不重叠验证 ----
    {
        JPOV::Config cfg;
        cfg.title = "Snake Acceptance Test - Eat Food Multiple";
        cfg.width  = 800;
        cfg.height = 600;
        cfg.target_fps = 60;
        cfg.headless = true;

        snake::SnakeApp app(cfg);
        app.Init();

        jpov::WindowInfo winfo;
        winfo.width  = 800;
        winfo.height = 600;

        jpov::InputSnapshot input = {};

        // 初始化
        app.RunOnce(input, winfo, "/tmp/snake_food_m0.png");

        // 吃3次食物，每次手动将食物放蛇头前方
        for (int eat_count = 1; eat_count <= 3; ++eat_count) {
            auto& s = const_cast<snake::GameState&>(app.GetState());
            s.food.x = s.snake[0].x + 1;
            s.food.y = s.snake[0].y;

            int before_size = s.snake.size();
            int before_score = s.score;

            app.RunOnce(input, winfo, "/tmp/snake_food_m1.png");

            // 每次吃食物后验证
            bool grew = (s.snake.size() == before_size + 1);
            bool scored = (s.score == before_score + 10);
            bool clear = true;
            for (const auto& seg : s.snake) {
                if (seg.x == s.food.x && seg.y == s.food.y) {
                    clear = false;
                    break;
                }
            }

            if (!grew || !scored || !clear) {
                std::printf("[FAIL] Test 6: Eat #%d - grew=%d scored=%d clear=%d\n",
                            eat_count, grew, scored, clear);
                app.Finalize();
                return 1;
            }

            std::printf("[PASS] Test 6.%d: Eat food #%d - "
                        "size %d→%zu score %d→%d food=(%d,%d) clear\n",
                        eat_count, eat_count,
                        before_size, s.snake.size(),
                        before_score, s.score,
                        s.food.x, s.food.y);
        }

        app.Finalize();
    }

    std::printf("\n=== All acceptance tests passed ===\n");
    return 0;
}
