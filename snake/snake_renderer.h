#pragma once

#include "tools/jpov/include/jpov/jpov.h"
#include "snake/snake_types.h"

namespace snake {

// GameRenderer — 贪吃蛇渲染器
//
// 职责：将 GameState 渲染为 JPOV RenderCommandList。
// 不包含窗口管理、不包含输入处理、不包含逻辑更新。
//
// 用法:
//   GameRenderer renderer;
//   renderer.Render(state, winfo, &cmds);
class GameRenderer {
public:
    // 渲染一帧贪吃蛇游戏
    //
    // state — input: 当前游戏状态
    // winfo — input: 窗口尺寸信息（用于计算网格像素尺寸）
    // cmds  — output: 帧渲染指令，由调用者在帧末提交给框架
    //
    // Pre-condition: cmds != nullptr
    void Render(const GameState& state,
                const jpov::WindowInfo& winfo,
                jpov::RenderCommandList* cmds);

private:
    // 用 5×7 点阵块绘制一个数字字符（左上角位置 + 像素块大小）
    void DrawDigit(int digit, float x, float y, float cell_size,
                   const jpov::Color& color,
                   jpov::RenderCommandList* cmds) const;

    // 用 5×7 点阵块绘制一个字母
    void DrawChar(char ch, float x, float y, float cell_size,
                  const jpov::Color& color,
                  jpov::RenderCommandList* cmds) const;

    // 绘制整数（用 5×7 点阵数字逐位显示）
    void DrawInt(int value, float x, float y, float cell_size,
                 const jpov::Color& color,
                 jpov::RenderCommandList* cmds) const;

    // 单行文本，用 5×7 点阵拼出
    void DrawBitmapText(const char* text, float x, float y, float cell_size,
                        const jpov::Color& color,
                        jpov::RenderCommandList* cmds) const;

    // 5×7 点阵字体查找表（每字符 5 字节低位图）
    static const uint8_t kFont5x7[];
};

}  // namespace snake
