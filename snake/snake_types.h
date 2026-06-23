#pragma once

#include <vector>
#include <cstdint>

namespace snake {

// 方向枚举
enum class Direction : uint8_t {
  kUp,
  kDown,
  kLeft,
  kRight,
  kNone,
};

// 蛇身的一个节段（网格坐标）
struct SnakeSegment {
  int x = 0;
  int y = 0;

  SnakeSegment() = default;
  SnakeSegment(int x, int y) : x(x), y(y) {}
};

// 食物
struct Food {
  int x = 0;
  int y = 0;
};

// 游戏主状态
struct GameState {
  // 网格尺寸（逻辑格数）
  int grid_width = 40;
  int grid_height = 30;

  // 蛇身（索引 0 为蛇头）
  std::vector<SnakeSegment> snake;

  // 当前移动方向（下一帧蛇头将沿此方向前进）
  Direction direction = Direction::kRight;
  // 缓冲的下一方向（单帧内只允许改变一次方向）
  Direction next_direction = Direction::kNone;

  // 食物
  Food food;

  // 分数
  int score = 0;

  // 游戏是否结束
  bool game_over = false;
  bool won = false;

  // 玩家名（用于显示）
  const char* player_name = "Player";
};

// 检查两个网格坐标是否相等
inline bool operator==(const SnakeSegment& a, const SnakeSegment& b) {
  return a.x == b.x && a.y == b.y;
}

inline bool operator==(const Food& a, const Food& b) {
  return a.x == b.x && a.y == b.y;
}

}  // namespace snake
