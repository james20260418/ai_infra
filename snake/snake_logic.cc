#include "snake/snake_logic.h"

#include <cstdlib>
#include <ctime>

namespace snake {

Delta DirectionToDelta(Direction dir) {
  switch (dir) {
    case Direction::kUp:    return { 0, -1};
    case Direction::kDown:  return { 0,  1};
    case Direction::kLeft:  return {-1,  0};
    case Direction::kRight: return { 1,  0};
    default:                return { 0,  0};
  }
}

void MoveSnake(GameState* state) {
  if (state->game_over) return;

  // 应用缓冲方向（确保不反向）
  if (state->next_direction != Direction::kNone) {
    Delta cur = DirectionToDelta(state->direction);
    Delta nxt = DirectionToDelta(state->next_direction);
    // 允许 180 度反转（在蛇长 > 1 时由碰撞检测处理）
    if (!(nxt.dx == -cur.dx && nxt.dy == -cur.dy)) {
      state->direction = state->next_direction;
    }
    state->next_direction = Direction::kNone;
  }

  Delta delta = DirectionToDelta(state->direction);

  // 计算蛇头新位置
  SnakeSegment new_head(
      state->snake[0].x + delta.dx,
      state->snake[0].y + delta.dy);

  // 检查是否吃到食物
  bool ate = (new_head.x == state->food.x && new_head.y == state->food.y);

  // 插入新蛇头
  state->snake.insert(state->snake.begin(), new_head);

  if (!ate) {
    // 没吃到食物，移除蛇尾
    state->snake.pop_back();
  } else {
    // 吃到食物，蛇身增长（不删蛇尾），加分，生成新食物
    state->score += 10;
    SpawnFood(state);
  }

  // 碰撞检测
  if (CheckCollision(*state)) {
    state->game_over = true;
  }
}

bool CheckCollision(const GameState& state) {
  if (state.snake.empty()) return false;

  const SnakeSegment& head = state.snake[0];

  // 撞墙检测
  if (head.x < 0 || head.x >= state.grid_width ||
      head.y < 0 || head.y >= state.grid_height) {
    return true;
  }

  // 撞自身检测（从索引 1 开始）
  for (size_t i = 1; i < state.snake.size(); ++i) {
    if (head.x == state.snake[i].x && head.y == state.snake[i].y) {
      return true;
    }
  }

  return false;
}

bool CheckFoodCollision(const GameState& state) {
  if (state.snake.empty()) return false;
  const SnakeSegment& head = state.snake[0];
  return (head.x == state.food.x && head.y == state.food.y);
}

void SpawnFood(GameState* state) {
  // 如果蛇占满所有格子 → 获胜
  int total_cells = state->grid_width * state->grid_height;
  if (static_cast<int>(state->snake.size()) >= total_cells) {
    state->won = true;
    state->game_over = true;
    return;
  }

  // 收集空闲格子
  std::vector<SnakeSegment> free_cells;
  free_cells.reserve(total_cells - state->snake.size());

  // 构建占用集（小网格用 O(grid_size) 扫描即可）
  std::vector<std::vector<bool>> occupied(
      state->grid_height,
      std::vector<bool>(state->grid_width, false));
  for (const auto& seg : state->snake) {
    if (seg.x >= 0 && seg.x < state->grid_width &&
        seg.y >= 0 && seg.y < state->grid_height) {
      occupied[seg.y][seg.x] = true;
    }
  }

  for (int y = 0; y < state->grid_height; ++y) {
    for (int x = 0; x < state->grid_width; ++x) {
      if (!occupied[y][x]) {
        free_cells.push_back(SnakeSegment(x, y));
      }
    }
  }

  if (free_cells.empty()) {
    // 没有空闲格子 → 获胜
    state->won = true;
    state->game_over = true;
    return;
  }

  // 随机选一个空闲位置
  int idx = std::rand() % free_cells.size();
  state->food.x = free_cells[idx].x;
  state->food.y = free_cells[idx].y;
}

void ResetGame(GameState* state) {
  state->snake.clear();
  state->direction = Direction::kRight;
  state->next_direction = Direction::kNone;
  state->score = 0;
  state->game_over = false;
  state->won = false;

  // 初始蛇：长度 3，水平放置
  int start_x = state->grid_width / 2;
  int start_y = state->grid_height / 2;
  state->snake.push_back(SnakeSegment(start_x, start_y));       // 蛇头
  state->snake.push_back(SnakeSegment(start_x - 1, start_y));   // 身体
  state->snake.push_back(SnakeSegment(start_x - 2, start_y));   // 尾巴

  SpawnFood(state);
}

}  // namespace snake
