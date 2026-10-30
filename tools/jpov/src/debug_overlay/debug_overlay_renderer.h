// JPOV DebugOverlayRenderer —— 内置点阵字体绘制（左上角 debug HUD 的最后一公里）
//
// 为什么是自己一套 GL 路径，而不是走 FontRenderer/DrawText2D：
//   DrawText2D 需要 app 通过 `Config::fonts` 显式注册 TTF，而 debug HUD 必须对
//   **任何** app 都可用（包括零字体注册的 app —— `kDefaultFonts` 恒为空）。
//   所以这里自带一张 8x8 ASCII 点阵图集（见 debug_font8x8.h），零外部文件依赖。
//
// 复用而不是重写的东西：着色器沿用 FontRenderer::kTextVs / kTextFs（同一份
// 「pos + uv → 单通道 atlas 采样 × 颜色」的 shader），顶点布局也一致
// （x, y, u, v 交错，4 float/顶点，attrib 0 = 位置、attrib 1 = uv），
// 所以可以直接吃 Renderer 的共享 stream VBO 与 text2d program。
//
// 生命周期：Init() → Draw()… → Finalize()，与 Renderer 同寿命（对称）。

#ifndef JPOV_DEBUG_OVERLAY_RENDERER_H_
#define JPOV_DEBUG_OVERLAY_RENDERER_H_

#include <string_view>

namespace jpov {

class DebugOverlayRenderer {
public:
    DebugOverlayRenderer() = default;
    ~DebugOverlayRenderer();

    DebugOverlayRenderer(const DebugOverlayRenderer&) = delete;
    DebugOverlayRenderer& operator=(const DebugOverlayRenderer&) = delete;

    // 建立点阵图集纹理（GL_R8，128x48 = 16 列 x 6 行 x 8x8 格，95 个字形）。
    //
    // Pre-condition: GL context 已激活（要 glGenTextures）。
    // Post-condition: 可重复调用（先释放旧的）。
    void Init();

    // 释放图集纹理。可重复调用（未 Init 也安全）。与 Init() 对称。
    void Finalize();

    // 把多行文本画到**当前绑定的 FBO** 的左上角：先铺一层半透明黑底（见
    // kBackdropAlpha），再画纯白色文字。黑底是必要的 —— 纯白字直接压在
    // 天空/浅地板上根本看不清，而 HUD 可能在任意场景里被 F3 叫出来。
    //
    // 黑底尺寸**贴内容**（= 8px 左留白 + 最长行宽 + 4px 右边距，行数决定高度），
    // 不是固定 500px：固定宽会在左上角常驻一块大黑砖，挡住 app 自己的面板。
    // 宽度上限由 DebugInfo::kMaxLineChars 保证（61 字符 x 8px + 8 + 4 = 500px）。
    //
    // 参数：
    //   text        —— ASCII 文本，'\n' 换行。区间外的字符（非 32..126）画成空白，
    //                  不会崩。空串 = 什么都不画。
    //   stream_vbo  —— 共享 GL_ARRAY_BUFFER（Renderer 的），容量需覆盖本次顶点数。
    //   text_prog   —— 由 FontRenderer::kTextVs + kTextFs 编译的 program。
    //   fbo_w/h     —— 当前 FBO 像素尺寸（≥ 1）；同时决定 uFboSize。
    //   glyph_scale —— 字形放大倍数（整数字节缩放，1 = 8x8 原生像素）。
    //
    // GL 状态前置要求（调用方负责）：FBO 已绑定、glViewport(0,0,fbo_w,fbo_h)。
    // 内部会自己开混合（SRC_ALPHA / ONE_MINUS_SRC_ALPHA）、关深度测试，
    // 并用 glPushAttrib(GL_ENABLE_BIT) 恢复，不污染调用方的状态。
    //
    // Pre-condition: Init() 已调用（否则 CHECK 失败）
    // Pre-condition: fbo_w > 0 && fbo_h > 0
    // Pre-condition: glyph_scale ∈ [kGlyphScaleMin, kGlyphScaleMax]
    // Pre-condition: text.size() <= kMaxTextChars
    void Draw(std::string_view text,
              unsigned int stream_vbo,
              unsigned int text_prog,
              int fbo_w, int fbo_h,
              int glyph_scale);

    // 字形放大倍数范围。上限 8：再大就没有 8x8 点阵的意义了（一格 64 像素）。
    static constexpr int kGlyphScaleMin = 1;
    static constexpr int kGlyphScaleMax = 8;

    // 文本长度上限（字符数）。5000 字符 ≈ 30000 顶点，远小于共享 stream VBO 容量，
    // 也远超任何 debug HUD 的实际需要；设上限是为了不越界写共享 VBO。
    static constexpr int kMaxTextChars = 5000;

private:
    // 图集布局：16 列 x 6 行（16*6 = 96 ≥ 95 个字形）。
    static constexpr int kAtlasCols = 16;
    static constexpr int kAtlasRows = 6;
    static constexpr int kAtlasW = kAtlasCols * 8;   // 128
    static constexpr int kAtlasH = kAtlasRows * 8;   // 48

    // 左上角留白与行距（未缩放像素；行距 = 字高 + 3 像素空隙）。
    static constexpr int kPadPx = 8;
    static constexpr int kLineGapPx = 3;

    // 黑底在文字右侧/下方的额外留白（像素，不随 scale 变，与 kPadPx 同理）。
    // 加上左边的 kPadPx 后，最长行 61 字符时黑底正好 8 + 488 + 4 = 500px。
    static constexpr int kBackdropPadPx = 4;

    // 黑底不透明度。0.55 = 白字在上面仍是 255 纯白（文字 alpha=1），
    // 而场景色被压到 45%，足够压住天空/浅色地板。调它的人只能在一个地方改。
    static constexpr float kBackdropAlpha = 0.55f;

    // 图集里保留给「纯色填充」的那一格（不需要真实字形：8x8 全 255）。
    // 95 个字形占 0..94，96 格图集剩最后一格 —— Init() 把它填满白色，
    // Draw() 用它的格中心 uv 铺黑底。这样黑底不用第二套 shader。
    static constexpr int kSolidCellIndex = kAtlasCols * kAtlasRows - 1;

    unsigned int atlas_tex_ = 0;
};

}  // namespace jpov

#endif  // JPOV_DEBUG_OVERLAY_RENDERER_H_
