#pragma once

#include "snake/snake_types.h"

namespace snake {

// === 游戏逻辑核心函数（纯函数，不依赖任何渲染/输入） ===

// 推进一帧：检查方向更新、移动蛇、吃食物、碰撞检测
// Pre-condition: state 非空
// Post-condition: state 被更新到下一帧状态
void MoveSnake(GameState* state);

// 碰撞检测：返回蛇头是否撞墙或撞自身
// Pre-condition: state 非空
// Returns: true if game over conditions are met
bool CheckCollision(const GameState& state);

// 在空闲网格上生成新的食物位置
// 保证不与蛇身重叠。如果网格已满（蛇占满所有格子），state.won = true
// Pre-condition: state 非空
void SpawnFood(GameState* state);

// 检查蛇头是否吃到了食物
// Pre-condition: state 非空
// Returns: true if snake head overlaps food
bool CheckFoodCollision(const GameState& state);

// 将 Direction 转换为坐标偏移量（dx, dy）
// 返回值符合 grid 坐标系统（x→右, y→下）
struct Delta {
  int dx = 0;
  int dy = 0;
};
Delta DirectionToDelta(Direction dir);

// 重置游戏状态到初始配置
// Pre-condition: state 非空
void ResetGame(GameState* state);

}  // namespace snake
