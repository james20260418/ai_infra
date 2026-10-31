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

// 文本的版面尺寸（**GL-free 纯计算**，可以在不建 GL context 的测试里直接验证）。
//
// 为什么要单独拎出来：Draw() 里"量内容 → 定黑底 → 生成顶点"三步，第一步错了
// （例如把"空内容就不画"的判断插进了 for 循环的分支里）在绝大多数输入上照样
// 出图，只有"首行为空、后面有字"时整块不画 —— 这种 bug 只有纯函数单测抓得住。
struct OverlayExtent {
    int max_cols = 0;     // 最宽一行的字符数（非 ASCII 的每个字节也算一格）
    int line_count = 1;   // 物理行数（'\n' 分隔）

    // 没有任何要画的字符（空串、"\n"、"\n\n" 这种）。
    bool IsEmpty() const { return max_cols == 0; }
};

// 数行宽/行数。与 Draw() 的第 2 遍用**同一套前进量规则**：每个字节占一格。
inline OverlayExtent MeasureOverlayExtent(std::string_view text) {
    OverlayExtent e;
    int cols = 0;
    for (const char ch : text) {
        if (ch == '\n') {
            if (cols > e.max_cols) {
                e.max_cols = cols;
            }
            cols = 0;
            ++e.line_count;
            continue;
        }
        ++cols;
    }
    if (cols > e.max_cols) {
        e.max_cols = cols;
    }
    return e;
}

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
    // GL 状态：调用方负责「FBO 已绑定 + glViewport(0,0,fbo_w,fbo_h)」。
    // 内部自己开混合（SRC_ALPHA / ONE_MINUS_SRC_ALPHA）、关深度测试，并**还原**：
    //   * enable 位 / 混合因子 —— glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT)
    //   * glActiveTexture 单元 —— 手动查/还原（attrib 栈里没有这一项）
    //   * GL_ARRAY_BUFFER、unit 0 的 GL_TEXTURE_2D 绑定 —— 解绑
    // **不还原**（与 FontRenderer::DrawText2D 同一约定）：当前 program、顶点属性
    // 数组的启用位/指针（attrib 0/1 指向共享 stream VBO）—— 逐项查/还原不值当。
    // 所以本方法必须是**本帧最后一个绘制动作**（JPOV 里正是 Render() 之后）。
    //
    // Pre-condition: Init() 已调用（否则 CHECK 失败）
    // Pre-condition: fbo_w > 0 && fbo_h > 0
    // Pre-condition: glyph_scale ∈ [kGlyphScaleMin, kGlyphScaleMax]
    // text 长度**不是**前置条件：超过 kMaxTextChars 时截断后照画（见实现）。
    void Draw(std::string_view text,
              unsigned int stream_vbo,
              unsigned int text_prog,
              int fbo_w, int fbo_h,
              int glyph_scale);

    // 字形放大倍数范围。上限 8：再大就没有 8x8 点阵的意义了（一格 64 像素）。
    static constexpr int kGlyphScaleMin = 1;
    static constexpr int kGlyphScaleMax = 8;

    // 文本长度上限（按字节算的字符数）。
    //
    // **实际只可能被 GL 驱动字符串碰到**：DebugInfo 里除 renderer / vendor /
    // version / glsl 四条外部串之外全是定长格式（分辨率/帧号/fps + 相机 6 个数），
    // 最坏情况也就几百字符 —— 相机/帧率这些我们自己的字段不可能把文本撑到 5000。
    //
    // 存在的理由只是给"长度不由我们决定"的数据兜一个内存/耗时上界：
    // 5000 字符 ≈ 30000 顶点 ≈ 0.5 MB 顶点数据。注意共享 stream VBO 是每次
    // glBufferData 现传现扩容的，**没有"越界写"这回事**（初始容量不是上限）。
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
