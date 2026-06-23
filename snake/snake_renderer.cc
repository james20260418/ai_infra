#include "snake/snake_renderer.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace snake {

// 5×7 点阵字体：每个字符 5 字节，每个字节的 bit0-bit6 对应第0-6行
// 字节中 bit7 未使用，bit0=最下一行，bit6=最上一行
// 空格(0x20)到 ~(0x7E) 可打印字符
const uint8_t GameRenderer::kFont5x7[] = {
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
    0x00, 0x00, 0x7F, 0x41, 0x41,  // [
    0x02, 0x04, 0x08, 0x10, 0x20,  // \
    0x41, 0x41, 0x7F, 0x00, 0x00,  // ]
    0x04, 0x02, 0x01, 0x02, 0x04,  // ^
    0x40, 0x40, 0x40, 0x40, 0x40,  // _
    0x00, 0x01, 0x02, 0x04, 0x00,  // `
    0x20, 0x54, 0x54, 0x54, 0x78,  // a
    0x7F, 0x48, 0x44, 0x44, 0x38,  // b
    0x38, 0x44, 0x44, 0x44, 0x20,  // c
    0x38, 0x44, 0x44, 0x48, 0x7F,  // d
    0x38, 0x54, 0x54, 0x54, 0x18,  // e
    0x08, 0x7E, 0x09, 0x01, 0x02,  // f
    0x08, 0x14, 0x54, 0x54, 0x3C,  // g
    0x7F, 0x08, 0x04, 0x04, 0x78,  // h
    0x00, 0x44, 0x7D, 0x40, 0x00,  // i
    0x20, 0x40, 0x44, 0x3D, 0x00,  // j
    0x00, 0x7F, 0x10, 0x28, 0x44,  // k
    0x00, 0x41, 0x7F, 0x40, 0x00,  // l
    0x7C, 0x04, 0x18, 0x04, 0x78,  // m
    0x7C, 0x08, 0x04, 0x04, 0x78,  // n
    0x38, 0x44, 0x44, 0x44, 0x38,  // o
    0x7C, 0x14, 0x14, 0x14, 0x08,  // p
    0x08, 0x14, 0x14, 0x18, 0x7C,  // q
    0x7C, 0x08, 0x04, 0x04, 0x08,  // r
    0x48, 0x54, 0x54, 0x54, 0x20,  // s
    0x04, 0x3F, 0x44, 0x40, 0x20,  // t
    0x3C, 0x40, 0x40, 0x20, 0x7C,  // u
    0x1C, 0x20, 0x40, 0x20, 0x1C,  // v
    0x3C, 0x40, 0x30, 0x40, 0x3C,  // w
    0x44, 0x28, 0x10, 0x28, 0x44,  // x
    0x0C, 0x50, 0x50, 0x50, 0x3C,  // y
    0x44, 0x64, 0x54, 0x4C, 0x44,  // z
    0x00, 0x08, 0x36, 0x41, 0x00,  // {
    0x00, 0x00, 0x7F, 0x00, 0x00,  // |
    0x00, 0x41, 0x36, 0x08, 0x00,  // }
    0x08, 0x08, 0x2A, 0x1C, 0x08,  // ~
};

void GameRenderer::DrawChar(char ch, float x, float y, float cell_size,
                            const jpov::Color& color,
                            jpov::RenderCommandList* cmds) const {
    int idx = static_cast<int>(ch) - 0x20;
    if (idx < 0 || idx > 94) return;
    const uint8_t* glyph = &kFont5x7[idx * 5];
    for (int col = 0; col < 5; ++col) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; ++row) {
            if (bits & (1 << row)) {
                float px = x + col * cell_size;
                float py = y + (6 - row) * cell_size;  // 行0=底，行6=顶
                cmds->DrawRect({px, py}, {cell_size, cell_size}, color);
            }
        }
    }
}

void GameRenderer::DrawBitmapText(const char* text, float x, float y,
                                   float cell_size, const jpov::Color& color,
                                   jpov::RenderCommandList* cmds) const {
    float cursor_x = x;
    for (const char* p = text; *p; ++p) {
        if (*p == ' ') {
            cursor_x += 4 * cell_size;  // 空格宽度
            continue;
        }
        DrawChar(*p, cursor_x, y, cell_size, color, cmds);
        cursor_x += 6 * cell_size;  // 字符间距
    }
}

void GameRenderer::DrawDigit(int digit, float x, float y, float cell_size,
                              const jpov::Color& color,
                              jpov::RenderCommandList* cmds) const {
    DrawChar('0' + digit, x, y, cell_size, color, cmds);
}

void GameRenderer::DrawInt(int value, float x, float y, float cell_size,
                            const jpov::Color& color,
                            jpov::RenderCommandList* cmds) const {
    if (value < 0) {
        DrawChar('-', x, y, cell_size, color, cmds);
        x += 6 * cell_size;
        value = -value;
    }
    // 转换为数字字符串
    char buf[16];
    int len = 0;
    if (value == 0) {
        buf[len++] = '0';
    } else {
        char tmp[16];
        int tmp_len = 0;
        while (value > 0) {
            tmp[tmp_len++] = '0' + (value % 10);
            value /= 10;
        }
        for (int i = tmp_len - 1; i >= 0; --i) {
            buf[len++] = tmp[i];
        }
    }
    buf[len] = '\0';
    DrawBitmapText(buf, x, y, cell_size, color, cmds);
}

void GameRenderer::Render(const GameState& state,
                          const jpov::WindowInfo& winfo,
                          jpov::RenderCommandList* cmds) {
    // 设定渲染分辨率 = 窗口尺寸（这样网格对齐不拉伸）
    int res_w = static_cast<int>(winfo.width);
    int res_h = static_cast<int>(winfo.height);
    cmds->render_width  = res_w;
    cmds->render_height = res_h;

    // 每个网格的像素尺寸
    const float cell_w = static_cast<float>(res_w) / static_cast<float>(state.grid_width);
    const float cell_h = static_cast<float>(res_h) / static_cast<float>(state.grid_height);
    const float cell_min = (cell_w < cell_h) ? cell_w : cell_h;

    // 游戏区域在窗口中的偏移（居中网格，保持正方形网格单元格）
    const float grid_pixel_w = cell_min * state.grid_width;
    const float grid_pixel_h = cell_min * state.grid_height;
    const float offset_x = (static_cast<float>(res_w) - grid_pixel_w) * 0.5f;
    const float offset_y = (static_cast<float>(res_h) - grid_pixel_h) * 0.5f;

    // ---- 棋盘格背景 ----
    const jpov::Color kBgLight = {0.15f, 0.15f, 0.15f, 1.0f};
    const jpov::Color kBgDark  = {0.10f, 0.10f, 0.10f, 1.0f};
    for (int gy = 0; gy < state.grid_height; ++gy) {
        for (int gx = 0; gx < state.grid_width; ++gx) {
            bool is_dark = ((gx + gy) % 2 == 0);
            cmds->DrawRect({offset_x + gx * cell_min, offset_y + gy * cell_min},
                           {cell_min, cell_min},
                           is_dark ? kBgDark : kBgLight);
        }
    }

    // ---- 食物（红色方块） ----
    float food_x = offset_x + state.food.x * cell_min;
    float food_y = offset_y + state.food.y * cell_min;
    float food_inset = cell_min * 0.1f;
    cmds->DrawRect({food_x + food_inset, food_y + food_inset},
                   {cell_min - 2.0f * food_inset, cell_min - 2.0f * food_inset},
                   {1.0f, 0.2f, 0.1f, 1.0f});

    // ---- 蛇 ----
    const int snake_len = static_cast<int>(state.snake.size());
    for (int i = 0; i < snake_len; ++i) {
        float sx = offset_x + state.snake[i].x * cell_min;
        float sy = offset_y + state.snake[i].y * cell_min;
        float inset = 1.0f;
        if (i == 0) {
            cmds->DrawRect({sx + inset, sy + inset},
                           {cell_min - 2 * inset, cell_min - 2 * inset},
                           {0.0f, 0.8f, 0.2f, 1.0f});
        } else {
            float t = static_cast<float>(i) / static_cast<float>(snake_len);
            jpov::Color body_color = {0.0f, 0.6f * (1.0f - t * 0.5f), 0.1f, 1.0f};
            cmds->DrawRect({sx + inset, sy + inset},
                           {cell_min - 2 * inset, cell_min - 2 * inset},
                           body_color);
        }
    }

    // ---- 状态显示（左上角 HUD） ----
    float hud_font_size = std::max(2.0f, std::min(16.0f,
        static_cast<float>(res_w) / 200.0f));
    float hud_x = 10.0f;
    float hud_y = 10.0f;

    if (state.game_over) {
        // ---- Game Over 横幅 ----
        float banner_h = 50.0f;
        cmds->DrawRect({0.0f, 0.0f},
                       {static_cast<float>(res_w), banner_h},
                       {0.8f, 0.1f, 0.1f, 0.7f});

        // 点阵显示 GAME OVER / WIN
        float center_x = static_cast<float>(res_w) * 0.5f;
        const char* status_msg = state.won ? "WIN" : "GAMEOVER";
        float msg_w = std::strlen(status_msg) * 6.0f * hud_font_size;
        DrawBitmapText(status_msg,
                       center_x - msg_w * 0.5f, 12.0f,
                       hud_font_size, {1.0f, 1.0f, 1.0f, 1.0f}, cmds);

        // 分数和长度
        char score_buf[64];
        std::snprintf(score_buf, sizeof(score_buf), "SCORE:%d LEN:%d",
                      state.score, static_cast<int>(state.snake.size()));
        float score_w = std::strlen(score_buf) * 6.0f * (hud_font_size * 0.7f);
        DrawBitmapText(score_buf,
                       center_x - score_w * 0.5f, 32.0f,
                       hud_font_size * 0.7f, {1.0f, 1.0f, 0.6f, 1.0f}, cmds);

        // ---- 中央 Game Over 大提示 ----
        float go_center_y = static_cast<float>(res_h) * 0.5f - 50.0f;
        float big_font = std::max(3.0f, std::min(24.0f,
            static_cast<float>(res_w) / 150.0f));
        const char* game_over_msg = state.won ? "YOU WIN!" : "GAME OVER";
        float go_w = std::strlen(game_over_msg) * 6.0f * big_font;
        DrawBitmapText(game_over_msg,
                       center_x - go_w * 0.5f, go_center_y,
                       big_font, {1.0f, 0.2f, 0.1f, 1.0f}, cmds);

        // ---- R 键重启提示 ----
        float press_r_y = go_center_y + big_font * 14.0f;
        const char* restart_msg = "PRESS R TO RESTART";
        float restart_w = std::strlen(restart_msg) * 6.0f * (big_font * 0.5f);
        DrawBitmapText(restart_msg,
                       center_x - restart_w * 0.5f, press_r_y,
                       big_font * 0.5f, {0.8f, 0.8f, 1.0f, 0.8f}, cmds);

        // ---- 最终分数 ----
        char final_buf[64];
        std::snprintf(final_buf, sizeof(final_buf),
                      "FINAL SCORE: %d   SNAKE LENGTH: %d",
                      state.score, static_cast<int>(state.snake.size()));
        float final_w = std::strlen(final_buf) * 6.0f * (big_font * 0.35f);
        DrawBitmapText(final_buf,
                       center_x - final_w * 0.5f, press_r_y + big_font * 7.0f,
                       big_font * 0.35f, {1.0f, 1.0f, 1.0f, 0.7f}, cmds);

        // ---- 半透明遮罩 ----
        cmds->DrawRect({0.0f, 0.0f},
                       {static_cast<float>(res_w), static_cast<float>(res_h)},
                       {0.0f, 0.0f, 0.0f, 0.15f});
    } else {
        // ---- 正常游戏 HUD：左上角分数和蛇长度 ----
        char hud_buf[64];
        std::snprintf(hud_buf, sizeof(hud_buf),
                      "SCORE %d  LEN %d",
                      state.score, static_cast<int>(state.snake.size()));
        DrawBitmapText(hud_buf, hud_x, hud_y, hud_font_size,
                       {0.2f, 1.0f, 0.2f, 0.9f}, cmds);

        // ---- 移动方向提示 ----
        DrawBitmapText("ARROW KEYS:MOVE",
                       hud_x, hud_y + hud_font_size * 8.0f,
                       hud_font_size * 0.6f, {0.6f, 0.6f, 0.6f, 0.6f}, cmds);
    }
}

} // namespace snake
