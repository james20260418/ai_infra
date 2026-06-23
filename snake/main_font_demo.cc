// main_font_demo.cc — JPOV 文字渲染方向测试 demo
// 用法: bazel run //snake:main_font_demo -- --headless=false
//
// 目的：在有 GL 环境的机器上验证 bitmap 字体渲染方向
// 显示字母 'ABCD' 和 'STAR'，用眼睛确认文字是否上下颠倒
//
// 如果字母看起来上下正常（A顶部分叉，B上半小下半大等）=> 渲染正确
// 如果字母看起来上下颠倒（底部收束等）=> 渲染仍然有问题

#include <cstdio>
#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"

// 5x7 点阵字体数据（与 snake_renderer.cc 相同）
static const uint8_t kFont5x7[] = {
    0x00, 0x00, 0x00, 0x00, 0x00,  // 0x20 Space
    0x00, 0x00, 0x5F, 0x00, 0x00,  // !
    0x00, 0x07, 0x00, 0x07, 0x00,  // "
    0x14, 0x7F, 0x14, 0x7F, 0x14,  // #
    0x24, 0x2A, 0x7F, 0x2A, 0x12,  // $
    0x23, 0x13, 0x08, 0x64, 0x62,  // %
    0x36, 0x49, 0x55, 0x22, 0x50,  // &
    0x00, 0x05, 0x03, 0x00, 0x00,  // '
    0x00, 0x1C, 0x22, 0x41, 0x00,  // (
    0x00, 0x41, 0x22, 0x1C, 0x00,  // )
    0x08, 0x2A, 0x1C, 0x2A, 0x08,  // *
    0x08, 0x08, 0x3E, 0x08, 0x08,  // +
    0x00, 0x50, 0x30, 0x00, 0x00,  // ,
    0x08, 0x08, 0x08, 0x08, 0x08,  // -
    0x00, 0x60, 0x60, 0x00, 0x00,  // .
    0x20, 0x10, 0x08, 0x04, 0x02,  // /
    0x3E, 0x51, 0x49, 0x45, 0x3E,  // 0
    0x00, 0x42, 0x7F, 0x40, 0x00,  // 1
    0x42, 0x61, 0x51, 0x49, 0x46,  // 2
    0x21, 0x41, 0x45, 0x4B, 0x31,  // 3
    0x18, 0x14, 0x12, 0x7F, 0x10,  // 4
    0x27, 0x45, 0x45, 0x45, 0x39,  // 5
    0x3C, 0x4A, 0x49, 0x49, 0x30,  // 6
    0x01, 0x71, 0x09, 0x05, 0x03,  // 7
    0x36, 0x49, 0x49, 0x49, 0x36,  // 8
    0x06, 0x49, 0x49, 0x29, 0x1E,  // 9
    0x00, 0x36, 0x36, 0x00, 0x00,  // :
    0x00, 0x56, 0x36, 0x00, 0x00,  // ;
    0x00, 0x08, 0x14, 0x22, 0x41,  // <
    0x14, 0x14, 0x14, 0x14, 0x14,  // =
    0x41, 0x22, 0x14, 0x08, 0x00,  // >
    0x02, 0x01, 0x51, 0x09, 0x06,  // ?
    0x32, 0x49, 0x79, 0x41, 0x3E,  // @
    0x7E, 0x11, 0x11, 0x11, 0x7E,  // A
    0x7F, 0x49, 0x49, 0x49, 0x36,  // B
    0x3E, 0x41, 0x41, 0x41, 0x22,  // C
    0x7F, 0x41, 0x41, 0x22, 0x1C,  // D
    0x7F, 0x49, 0x49, 0x49, 0x41,  // E
    0x7F, 0x09, 0x09, 0x01, 0x01,  // F
    0x3E, 0x41, 0x41, 0x51, 0x32,  // G
    0x7F, 0x08, 0x08, 0x08, 0x7F,  // H
    0x00, 0x41, 0x7F, 0x41, 0x00,  // I
    0x20, 0x40, 0x41, 0x3F, 0x01,  // J
    0x7F, 0x08, 0x14, 0x22, 0x41,  // K
    0x7F, 0x40, 0x40, 0x40, 0x40,  // L
    0x7F, 0x02, 0x04, 0x02, 0x7F,  // M
    0x7F, 0x04, 0x08, 0x10, 0x7F,  // N
    0x3E, 0x41, 0x41, 0x41, 0x3E,  // O
    0x7F, 0x09, 0x09, 0x09, 0x06,  // P
    0x3E, 0x41, 0x51, 0x21, 0x5E,  // Q
    0x7F, 0x09, 0x19, 0x29, 0x46,  // R
    0x46, 0x49, 0x49, 0x49, 0x31,  // S
    0x01, 0x01, 0x7F, 0x01, 0x01,  // T
    0x3F, 0x40, 0x40, 0x40, 0x3F,  // U
    0x1F, 0x20, 0x40, 0x20, 0x1F,  // V
    0x7F, 0x20, 0x18, 0x20, 0x7F,  // W
    0x63, 0x14, 0x08, 0x14, 0x63,  // X
    0x03, 0x04, 0x78, 0x04, 0x03,  // Y
    0x61, 0x51, 0x49, 0x45, 0x43,  // Z
};

class FontDemoApp : public JPOV {
public:
    using JPOV::JPOV;

    void OneIteration(int64_t /*frame_count*/,
                      const jpov::InputSnapshot& /*input*/,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        cmds->render_width  = static_cast<int>(winfo.width);
        cmds->render_height = static_cast<int>(winfo.height);

        // 黑色背景
        cmds->DrawRect({0, 0},
                       {static_cast<float>(winfo.width), static_cast<float>(winfo.height)},
                       {0.0f, 0.0f, 0.0f, 1.0f});

        float cell = 12.0f;

        // 第一行: 'A' 'B' 'C' 'D'
        float y = 50.0f;
        float x = 50.0f;
        DrawLetter('A', x, y, cell, {1.0f, 0.3f, 0.3f, 1.0f}, cmds);
        x += 7 * cell;
        DrawLetter('B', x, y, cell, {0.3f, 1.0f, 0.3f, 1.0f}, cmds);
        x += 7 * cell;
        DrawLetter('C', x, y, cell, {0.3f, 0.3f, 1.0f, 1.0f}, cmds);
        x += 7 * cell;
        DrawLetter('D', x, y, cell, {1.0f, 1.0f, 0.3f, 1.0f}, cmds);

        // 说明: "ABCD"
        DrawLetter('A', 50.0f, y - 30.0f, 6.0f, {0.5f, 0.5f, 0.5f, 1.0f}, cmds);
        DrawLetter('B', 92.0f, y - 30.0f, 6.0f, {0.5f, 0.5f, 0.5f, 1.0f}, cmds);
        DrawLetter('C', 134.0f, y - 30.0f, 6.0f, {0.5f, 0.5f, 0.5f, 1.0f}, cmds);
        DrawLetter('D', 176.0f, y - 30.0f, 6.0f, {0.5f, 0.5f, 0.5f, 1.0f}, cmds);

        // 第二行: 'S' 'T' 'A' 'R'
        float y2 = y + 9 * cell;
        float x2 = 50.0f;
        DrawLetter('S', x2, y2, cell, {0.3f, 1.0f, 1.0f, 1.0f}, cmds);
        x2 += 7 * cell;
        DrawLetter('T', x2, y2, cell, {1.0f, 0.3f, 1.0f, 1.0f}, cmds);
        x2 += 7 * cell;
        DrawLetter('A', x2, y2, cell, {1.0f, 1.0f, 0.3f, 1.0f}, cmds);
        x2 += 7 * cell;
        DrawLetter('R', x2, y2, cell, {0.8f, 0.8f, 0.8f, 1.0f}, cmds);

        // 说明: "STAR"
        DrawLetter('S', 50.0f, y2 - 30.0f, 6.0f, {0.5f, 0.5f, 0.5f, 1.0f}, cmds);
        DrawLetter('T', 92.0f, y2 - 30.0f, 6.0f, {0.5f, 0.5f, 0.5f, 1.0f}, cmds);
        DrawLetter('A', 134.0f, y2 - 30.0f, 6.0f, {0.5f, 0.5f, 0.5f, 1.0f}, cmds);
        DrawLetter('R', 176.0f, y2 - 30.0f, 6.0f, {0.5f, 0.5f, 0.5f, 1.0f}, cmds);
    }

private:
    static void DrawLetter(char ch, float x, float y, float cell_size,
                           const jpov::Color& color,
                           jpov::RenderCommandList* cmds) {
        int idx = static_cast<int>(ch) - 0x20;
        if (idx < 0 || idx > 94) return;
        const uint8_t* glyph = &kFont5x7[idx * 5];
        for (int col = 0; col < 5; ++col) {
            uint8_t bits = glyph[col];
            for (int row = 0; row < 7; ++row) {
                if (bits & (1 << row)) {
                    float px = x + col * cell_size;
                    float py = y + row * cell_size;  // 修正后的算法 bit0=顶
                    cmds->DrawRect({px, py}, {cell_size, cell_size}, color);
                }
            }
        }
    }
};

int main() {
    JPOV::Config cfg;
    cfg.title = "Font Direction Demo — ABCD STAR";
    cfg.width = 640;
    cfg.height = 480;
    cfg.target_fps = 30;
    FontDemoApp app(cfg);
    app.Init();
    app.Run();
    app.Finalize();
    return 0;
}
