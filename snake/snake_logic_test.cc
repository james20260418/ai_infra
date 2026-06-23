#include "snake/snake_types.h"
#include "snake/snake_logic.h"

#include <cstdlib>
#include <ctime>

#include "glog/logging.h"
#include "gtest/gtest.h"

namespace snake {
namespace {

// ==================== 测试辅助 ====================

// 创建一个初始游戏状态（蛇位于中央水平3节，方向右）
GameState MakeDefaultState() {
  GameState state;
  ResetGame(&state);
  return state;
}

// ==================== DirectionToDelta ====================

TEST(SnakeLogicTest, DirectionToDelta_Up) {
  auto d = DirectionToDelta(Direction::kUp);
  EXPECT_EQ(d.dx, 0);
  EXPECT_EQ(d.dy, -1);
}

TEST(SnakeLogicTest, DirectionToDelta_Down) {
  auto d = DirectionToDelta(Direction::kDown);
  EXPECT_EQ(d.dx, 0);
  EXPECT_EQ(d.dy, 1);
}

TEST(SnakeLogicTest, DirectionToDelta_Left) {
  auto d = DirectionToDelta(Direction::kLeft);
  EXPECT_EQ(d.dx, -1);
  EXPECT_EQ(d.dy, 0);
}

TEST(SnakeLogicTest, DirectionToDelta_Right) {
  auto d = DirectionToDelta(Direction::kRight);
  EXPECT_EQ(d.dx, 1);
  EXPECT_EQ(d.dy, 0);
}

TEST(SnakeLogicTest, DirectionToDelta_None) {
  auto d = DirectionToDelta(Direction::kNone);
  EXPECT_EQ(d.dx, 0);
  EXPECT_EQ(d.dy, 0);
}

// ==================== ResetGame ====================

TEST(SnakeLogicTest, ResetGame_InitialState) {
  GameState state;
  ResetGame(&state);

  // 默认网格尺寸
  EXPECT_EQ(state.grid_width, 40);
  EXPECT_EQ(state.grid_height, 30);

  // 蛇初始长度3
  ASSERT_EQ(state.snake.size(), 3u);

  // 蛇头在中央
  int expect_head_x = 40 / 2;
  int expect_head_y = 30 / 2;
  EXPECT_EQ(state.snake[0].x, expect_head_x);
  EXPECT_EQ(state.snake[0].y, expect_head_y);

  // 蛇身水平排列
  EXPECT_EQ(state.snake[1].x, expect_head_x - 1);
  EXPECT_EQ(state.snake[1].y, expect_head_y);
  EXPECT_EQ(state.snake[2].x, expect_head_x - 2);
  EXPECT_EQ(state.snake[2].y, expect_head_y);

  // 方向
  EXPECT_EQ(state.direction, Direction::kRight);
  EXPECT_EQ(state.next_direction, Direction::kNone);
  EXPECT_EQ(state.score, 0);
  EXPECT_FALSE(state.game_over);
  EXPECT_FALSE(state.won);

  // 食物应该在某处，且不与蛇重叠
  Food f = state.food;
  for (const auto& seg : state.snake) {
    EXPECT_FALSE(seg.x == f.x && seg.y == f.y)
        << "Food should not overlap snake at (" << f.x << "," << f.y << ")";
  }
}

// ==================== MoveSnake - 基本移动 ====================

TEST(SnakeLogicTest, MoveSnake_ForwardOneStep) {
  auto state = MakeDefaultState();
  int old_head_x = state.snake[0].x;
  int old_head_y = state.snake[0].y;

  MoveSnake(&state);

  // 蛇头向右移动一格
  EXPECT_EQ(state.snake[0].x, old_head_x + 1);
  EXPECT_EQ(state.snake[0].y, old_head_y);

  // 蛇身跟进（长度不变，因为没吃到食物）
  EXPECT_EQ(state.snake.size(), 3u);
  EXPECT_FALSE(state.game_over);
}

TEST(SnakeLogicTest, MoveSnake_ChangeDirection) {
  auto state = MakeDefaultState();
  state.next_direction = Direction::kUp;

  MoveSnake(&state);

  // 蛇头向上移一格
  int head_x = 40 / 2;
  int head_y = 30 / 2;
  EXPECT_EQ(state.snake[0].x, head_x);
  EXPECT_EQ(state.snake[0].y, head_y - 1);

  // 方向已应用
  EXPECT_EQ(state.direction, Direction::kUp);
}

TEST(SnakeLogicTest, MoveSnake_RejectReverseDirection) {
  auto state = MakeDefaultState();
  // 当前方向右，尝试向左（反向）
  state.next_direction = Direction::kLeft;

  MoveSnake(&state);

  // 反向被忽略，蛇仍然向右
  EXPECT_EQ(state.direction, Direction::kRight);
  EXPECT_EQ(state.snake[0].x, 40 / 2 + 1);
}

// ==================== MoveSnake - 吃食物 ====================

TEST(SnakeLogicTest, EatFood_GrowsAndScores) {
  auto state = MakeDefaultState();

  // 把食物放在蛇头正前方一格
  int head_x = state.snake[0].x;
  int head_y = state.snake[0].y;
  state.food.x = head_x + 1;
  state.food.y = head_y;

  int old_size = state.snake.size();
  MoveSnake(&state);

  // 蛇变长
  EXPECT_EQ(state.snake.size(), old_size + 1);
  // 加分
  EXPECT_EQ(state.score, 10);
  // 食物被重新生成
  EXPECT_FALSE(state.food.x == head_x + 1 && state.food.y == head_y)
      << "Food should have been respawned";
}

// ==================== CheckCollision - 撞墙 ====================

TEST(SnakeLogicTest, Collision_WallRight) {
  auto state = MakeDefaultState();
  // 把蛇头移到右边界
  state.snake[0].x = state.grid_width - 1;
  state.snake[0].y = 0;
  state.direction = Direction::kRight;

  MoveSnake(&state);

  EXPECT_TRUE(state.game_over);
}

TEST(SnakeLogicTest, Collision_WallLeft) {
  auto state = MakeDefaultState();
  state.snake[0].x = 0;
  state.snake[0].y = 0;
  state.direction = Direction::kLeft;

  MoveSnake(&state);

  EXPECT_TRUE(state.game_over);
}

TEST(SnakeLogicTest, Collision_WallTop) {
  auto state = MakeDefaultState();
  state.snake[0].x = 0;
  state.snake[0].y = 0;
  state.direction = Direction::kUp;

  MoveSnake(&state);

  EXPECT_TRUE(state.game_over);
}

TEST(SnakeLogicTest, Collision_WallBottom) {
  auto state = MakeDefaultState();
  state.snake[0].x = 0;
  state.snake[0].y = state.grid_height - 1;
  state.direction = Direction::kDown;

  MoveSnake(&state);

  EXPECT_TRUE(state.game_over);
}

// ==================== CheckCollision - 撞自身 ====================

TEST(SnakeLogicTest, Collision_Self) {
  auto state = MakeDefaultState();
  // 制造撞自身场景：让蛇头向右一格后，立刻向上，再向左（蛇身在后续路径上）
  // 简单做法：设蛇身成一个 U 形
  state.snake.clear();
  state.snake.push_back(SnakeSegment(5, 5));   // 蛇头
  state.snake.push_back(SnakeSegment(5, 6));   // 正下方
  state.snake.push_back(SnakeSegment(4, 6));   // 左下方
  state.snake.push_back(SnakeSegment(4, 5));   // 正左

  state.direction = Direction::kDown;

  MoveSnake(&state);

  // 蛇头从(5,5)向下到(5,6)，与身体(5,6)重叠
  EXPECT_TRUE(state.game_over);
}

// ==================== SpawnFood ====================

TEST(SnakeLogicTest, SpawnFood_NotOnSnake) {
  auto state = MakeDefaultState();
  // 重置食物位置在蛇上
  state.food.x = state.snake[0].x;
  state.food.y = state.snake[0].y;

  SpawnFood(&state);

  // 新食物不在蛇身上
  for (const auto& seg : state.snake) {
    EXPECT_FALSE(seg.x == state.food.x && seg.y == state.food.y)
        << "Spawned food at (" << state.food.x << "," << state.food.y
        << ") overlaps snake";
  }
}

TEST(SnakeLogicTest, SpawnFood_FullGrid_Wins) {
  auto state = MakeDefaultState();
  // 把网格设成 2x2，塞满蛇
  state.grid_width = 2;
  state.grid_height = 2;
  state.snake.clear();
  state.snake.push_back(SnakeSegment(0, 0));
  state.snake.push_back(SnakeSegment(1, 0));
  state.snake.push_back(SnakeSegment(1, 1));
  state.snake.push_back(SnakeSegment(0, 1));

  SpawnFood(&state);

  EXPECT_TRUE(state.won);
  EXPECT_TRUE(state.game_over);
}

// ==================== CheckFoodCollision ====================

TEST(SnakeLogicTest, CheckFoodCollision_OnFood) {
  auto state = MakeDefaultState();
  state.food.x = state.snake[0].x;
  state.food.y = state.snake[0].y;

  EXPECT_TRUE(CheckFoodCollision(state));
}

TEST(SnakeLogicTest, CheckFoodCollision_NotOnFood) {
  auto state = MakeDefaultState();
  // 确保食物不在蛇头
  state.food.x = state.snake[0].x + 100;
  state.food.y = state.snake[0].y;

  EXPECT_FALSE(CheckFoodCollision(state));
}

// ==================== 综合场景：移动 + 转向 + 吃食物 + 撞墙 ====================

TEST(SnakeLogicTest, FullSequence_MoveEatAndDie) {
  // 模拟完整时序：前进两步 → 转向向上 → 吃食物 → 继续向上 → 撞墙
  auto state = MakeDefaultState();
  int head_x = state.snake[0].x;
  int head_y = state.snake[0].y;
  int food_x = head_x + 1;
  int food_y = head_y;

  // 第一步：向前一格（没吃到食物）
  MoveSnake(&state);
  EXPECT_EQ(state.snake[0].x, head_x + 1);
  EXPECT_EQ(state.snake.size(), 3u);
  EXPECT_FALSE(state.game_over);

  // 把食物放在蛇头前方
  state.food.x = state.snake[0].x + 1;
  state.food.y = state.snake[0].y;

  // 第二步：前进，吃到食物
  MoveSnake(&state);
  EXPECT_EQ(state.snake.size(), 4u);
  EXPECT_EQ(state.score, 10);

  // 第三步：转向向上
  state.next_direction = Direction::kUp;
  MoveSnake(&state);
  EXPECT_EQ(state.direction, Direction::kUp);

  // 不断向上直到撞墙
  int steps = 0;
  while (!state.game_over && steps < 100) {
    MoveSnake(&state);
    steps++;
  }
  EXPECT_TRUE(state.game_over) << "Should have hit the top wall";
  EXPECT_GT(steps, 0);
  EXPECT_LT(steps, 50);  // 网格高30，从中央出发最多15步撞上墙
}

}  // namespace
}  // namespace snake
