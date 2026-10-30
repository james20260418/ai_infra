// JPOV DebugOverlayRenderer 实现 —— 内置 8x8 点阵字体绘制

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/debug_overlay/debug_overlay_renderer.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// GL 头文件必须最先 include（MinGW 那边靠 gl_loader 提供 2.0+ 原型）
// GL 头文件必须最先 include。**两个平台都要 <GL/glext.h>**：MinGW 的 <GL/gl.h>
// 只到 1.1，GL_R8 / GL_RED / GL_CLAMP_TO_EDGE 都在 glext 里（仿 volume_manager.cc）。
#include <GL/gl.h>
#include <GL/glext.h>
#ifdef _WIN32
// MinGW: windows.h 定义 ERROR 宏与 glog 冲突，必须在 glog 之前 suppress
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include <glog/logging.h>

#include "tools/jpov/src/debug_overlay/debug_font8x8.h"

namespace jpov {
namespace {

constexpr int kGlyphCount =
    kDebugFontLastChar - kDebugFontFirstChar + 1;   // ASCII 32..126 = 95

// 每条顶点 4 个 float：(x, y, u, v)，与 FontRenderer 的 text VS 布局一致。
constexpr int kFloatsPerVertex = 4;
constexpr int kVerticesPerGlyph = 6;   // 两个三角形

}  // namespace

DebugOverlayRenderer::~DebugOverlayRenderer() {
    Finalize();
}

void DebugOverlayRenderer::Init() {
    // 必须**严格**小于：最后一格（kSolidCellIndex）要留下来当纯色填充块。
    CHECK_LT(kGlyphCount, kAtlasCols * kAtlasRows)
        << "点阵字形数占满图集，没有格子留给黑底填充";

    Finalize();   // 允许重复 Init

    // ---- 把点阵表铺进一张单通道图集（行主序：字形 i 在 (i%16, i/16) 格）----
    std::vector<uint8_t> pixels(static_cast<std::size_t>(kAtlasW) * kAtlasH, 0);
    for (int gi = 0; gi < kGlyphCount; ++gi) {
        const int cell_x = (gi % kAtlasCols) * kDebugFontGlyphW;
        const int cell_y = (gi / kAtlasCols) * kDebugFontGlyphH;
        for (int row = 0; row < kDebugFontGlyphH; ++row) {
            const uint8_t bits = kDebugFont8x8[gi][row];
            for (int col = 0; col < kDebugFontGlyphW; ++col) {
                // bit0 = 该行最左像素（VGA 传统顺序）
                if (((bits >> col) & 1u) != 0u) {
                    const int px = cell_x + col;
                    const int py = cell_y + row;
                    pixels[static_cast<std::size_t>(py) * kAtlasW + px] = 255;
                }
            }
        }
    }

    // 最后一格填满 255：Draw() 铺黑底时按格中心采样，等价于一个纯色 texel。
    {
        const int cell_x = (kSolidCellIndex % kAtlasCols) * kDebugFontGlyphW;
        const int cell_y = (kSolidCellIndex / kAtlasCols) * kDebugFontGlyphH;
        for (int row = 0; row < kDebugFontGlyphH; ++row) {
            for (int col = 0; col < kDebugFontGlyphW; ++col) {
                const int px = cell_x + col;
                const int py = cell_y + row;
                pixels[static_cast<std::size_t>(py) * kAtlasW + px] = 255;
            }
        }
    }

    glGenTextures(1, &atlas_tex_);
    glBindTexture(GL_TEXTURE_2D, atlas_tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kAtlasW, kAtlasH, 0, GL_RED,
                 GL_UNSIGNED_BYTE, pixels.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);   // 还原 GL 默认值，不污染调用方
    // 点阵必须用 NEAREST：线性过滤会把 8x8 的点阵糊成一团灰。
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void DebugOverlayRenderer::Finalize() {
    if (atlas_tex_ != 0) {
        glDeleteTextures(1, &atlas_tex_);
        atlas_tex_ = 0;
    }
}

void DebugOverlayRenderer::Draw(std::string_view text,
                                unsigned int stream_vbo,
                                unsigned int text_prog,
                                int fbo_w, int fbo_h,
                                int glyph_scale) {
    CHECK_NE(atlas_tex_, 0u) << "DebugOverlayRenderer::Init() 未调用";
    CHECK_GT(fbo_w, 0);
    CHECK_GT(fbo_h, 0);
    CHECK_GE(glyph_scale, kGlyphScaleMin);
    CHECK_LE(glyph_scale, kGlyphScaleMax);
    if (text.empty()) {
        return;
    }
    // ⚠️ 内容的主要来源是 GL 驱动字符串（`GL_RENDERER` 之类），长度**不由我们决定**。
    // 超长时**截断后照画**（+ 一次告警），不是 CHECK 崩掉：外部数据不该让引擎 abort。
    if (text.size() > static_cast<std::size_t>(kMaxTextChars)) {
        LOG_FIRST_N(WARNING, 1) << "DrawDebugOverlay: 文本 " << text.size()
                                << " 字符超过上限 " << kMaxTextChars
                                << "，只画前 " << kMaxTextChars << " 个";
        text = text.substr(0, static_cast<std::size_t>(kMaxTextChars));
    }

    const int glyph_w = kDebugFontGlyphW * glyph_scale;
    const int glyph_h = kDebugFontGlyphH * glyph_scale;
    const int advance = glyph_w;
    const int line_h = (kDebugFontGlyphH + kLineGapPx) * glyph_scale;

    // ---- 第 1 遍：量内容（黑底必须贴内容，所以不能边画字边定尺寸）----
    const OverlayExtent extent = MeasureOverlayExtent(text);
    const int max_cols = extent.max_cols;
    const int line_count = extent.line_count;

    // 全是换行/空行：没有内容也就没有面板。
    if (extent.IsEmpty()) {
        return;
    }

    // 黑底：左 kPadPx、右/下 kBackdropPadPx。宽度由最长行决定，**不**按窗口宽。
    const float panel_w =
        static_cast<float>(kPadPx + max_cols * advance + kBackdropPadPx);
    const float panel_h = static_cast<float>(kPadPx + (line_count - 1) * line_h +
                                             glyph_h + kBackdropPadPx);

    // ---- 第 2 遍：生成顶点 ----
    // 黑底和文字用同一个 program，但 uColor 不同（黑 0.55 / 白 1.0），
    // 所以分两次上传、两次 draw。两次都走 shared stream VBO，开销可忽略。
    std::vector<float> backdrop_verts;
    backdrop_verts.reserve(kVerticesPerGlyph * kFloatsPerVertex);
    {
        // 图集最后一格是实心白（Init() 填的）：按格中心采样 = 纯色 texel。
        const float u =
            (static_cast<float>((kSolidCellIndex % kAtlasCols) * kDebugFontGlyphW) +
             static_cast<float>(kDebugFontGlyphW) * 0.5f) /
            static_cast<float>(kAtlasW);
        const float v =
            (static_cast<float>((kSolidCellIndex / kAtlasCols) * kDebugFontGlyphH) +
             static_cast<float>(kDebugFontGlyphH) * 0.5f) /
            static_cast<float>(kAtlasH);
        const float quad[kVerticesPerGlyph][kFloatsPerVertex] = {
            {0.0f, 0.0f, u, v},
            {panel_w, 0.0f, u, v},
            {panel_w, panel_h, u, v},
            {0.0f, 0.0f, u, v},
            {panel_w, panel_h, u, v},
            {0.0f, panel_h, u, v},
        };
        for (int vtx = 0; vtx < kVerticesPerGlyph; ++vtx) {
            for (int f = 0; f < kFloatsPerVertex; ++f) {
                backdrop_verts.push_back(quad[vtx][f]);
            }
        }
    }

    std::vector<float> text_verts;
    text_verts.reserve(text.size() * kVerticesPerGlyph * kFloatsPerVertex);

    int pen_x = kPadPx;
    int pen_y = kPadPx;
    for (const char ch : text) {
        if (ch == '\n') {
            pen_x = kPadPx;
            pen_y += line_h;
            continue;
        }
        const int code = static_cast<unsigned char>(ch);
        if (code < kDebugFontFirstChar || code > kDebugFontLastChar) {
            // 区间外字符（含 UTF-8 多字节的每个字节）：占一个空位，不画。
            pen_x += advance;
            continue;
        }

        const int gi = code - kDebugFontFirstChar;
        const float u0 = static_cast<float>((gi % kAtlasCols) * kDebugFontGlyphW) /
                         static_cast<float>(kAtlasW);
        const float v0 = static_cast<float>((gi / kAtlasCols) * kDebugFontGlyphH) /
                         static_cast<float>(kAtlasH);
        const float u1 = u0 + static_cast<float>(kDebugFontGlyphW) /
                                  static_cast<float>(kAtlasW);
        const float v1 = v0 + static_cast<float>(kDebugFontGlyphH) /
                                  static_cast<float>(kAtlasH);

        const float x0 = static_cast<float>(pen_x);
        const float y0 = static_cast<float>(pen_y);
        const float x1 = x0 + static_cast<float>(glyph_w);
        const float y1 = y0 + static_cast<float>(glyph_h);

        // 两个三角形（左上 → 右上 → 右下 / 左上 → 右下 → 左下）
        const float quad[kVerticesPerGlyph][kFloatsPerVertex] = {
            {x0, y0, u0, v0},
            {x1, y0, u1, v0},
            {x1, y1, u1, v1},
            {x0, y0, u0, v0},
            {x1, y1, u1, v1},
            {x0, y1, u0, v1},
        };
        for (int vtx = 0; vtx < kVerticesPerGlyph; ++vtx) {
            for (int f = 0; f < kFloatsPerVertex; ++f) {
                text_verts.push_back(quad[vtx][f]);
            }
        }

        pen_x += advance;
    }

    // ---- 上传 + 绘制 ----
    //
    // 三样状态我们自己改、自己还原（glPushAttrib(GL_ENABLE_BIT) 只管 enable 位，
    // **管不到混合因子和 active texture unit**）：
    //   * enable 位（混合/深度测试）—— push/pop(GL_ENABLE_BIT)
    //   * glBlendFunc 因子            —— push/pop(GL_COLOR_BUFFER_BIT)
    //   * glActiveTexture 单元        —— 手动查/还原（attrib 栈没有这一项）
    // 为什么 blend func 不自己查/还原：要精确还原 RGB 与 alpha 可能不同的因子就得
    // glBlendFuncSeparate，而 MinGW 的 gl_loader 里没有它（--config=windows 会链接失败）。
    // GL_COLOR_BUFFER_BIT 这一段 GL 1.1 就有，两个平台都稳。
    // HUD 是每帧最后一个绘制动作，漏还原今天不会立刻出问题（后面的 pass 都自带
    // 状态设置），但"上游状态我保证不动"这条契约必须是真的，不能靠下游侥幸。
    GLint prev_active_tex = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active_tex);

    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    glUseProgram(text_prog);
    glUniform2f(glGetUniformLocation(text_prog, "uFboSize"),
                static_cast<float>(fbo_w), static_cast<float>(fbo_h));
    glUniform1i(glGetUniformLocation(text_prog, "uTexture"), 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas_tex_);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
    constexpr int kStride = kFloatsPerVertex * static_cast<int>(sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, kStride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kStride,
                          (void*)(2 * sizeof(float)));

    // ---- (1) 黑底：纯黑、alpha = kBackdropAlpha ----
    glUniform4f(glGetUniformLocation(text_prog, "uColor"), 0.0f, 0.0f, 0.0f,
                kBackdropAlpha);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(backdrop_verts.size() * sizeof(float)),
                 backdrop_verts.data(), GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0,
                 static_cast<GLsizei>(backdrop_verts.size()) / kFloatsPerVertex);

    // ---- (2) 文字：白色、全不透明（alpha 交给点阵覆盖度决定）----
    if (!text_verts.empty()) {
        glUniform4f(glGetUniformLocation(text_prog, "uColor"), 1.0f, 1.0f, 1.0f, 1.0f);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(text_verts.size() * sizeof(float)),
                     text_verts.data(), GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0,
                     static_cast<GLsizei>(text_verts.size()) / kFloatsPerVertex);
    }

    const GLenum draw_err = glGetError();
    if (draw_err != GL_NO_ERROR) {
        LOG_FIRST_N(WARNING, 1) << "GL error after DrawDebugOverlay: " << draw_err;
    }

    glPopAttrib();
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(static_cast<GLenum>(prev_active_tex));
}

}  // namespace jpov
