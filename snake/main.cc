// Snake Game — 入口文件
//
// 编译运行:
//   bazel run //snake:snake_game
#include "snake/snake_app.h"

int main() {
    JPOV::Config cfg;
    cfg.title = "Snake Game — JPOV";
    cfg.width  = 800;
    cfg.height = 600;
    cfg.target_fps = 60;

    snake::SnakeApp app(cfg);
    app.Init();
    app.Run();
    app.Finalize();
    return 0;
}
