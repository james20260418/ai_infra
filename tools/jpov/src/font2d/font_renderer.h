// JPOV FontRenderer — 字体 2D 渲染模块
//
// 管理字体的注册、字形光栅化、GL atlas 上传，以及 DrawText2D 渲染。
// 作为 Renderer 的内部组件，生命周期与 Renderer 相同。
//
// 每个 FontRenderer 实例管理一组字体，每种字体维护三层 GPU atlas
//（16/32/48px, GL_R8 单通道纹理）。字体通过 alias 引用。
//
// 用例：
//   FontRenderer fr;
//   fr.Init(user_fonts, builtin_fonts);
//   unsigned int prog = CompileProgram(kTextVs, kTextFs);
//   fr.DrawText2D(cmd, stream_vbo, fbo_w, fbo_h, prog);

#ifndef JPOV_FONT_RENDERER_H_
#define JPOV_FONT_RENDERER_H_

#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/src/font_manager.h"

namespace jpov {

class FontRenderer {
public:
    FontRenderer() = default;
    ~FontRenderer();

    FontRenderer(const FontRenderer&) = delete;
    FontRenderer& operator=(const FontRenderer&) = delete;

    // ---- 字体渲染所需的 GLSL shader 源码 ----
    //
    // 调用方用 ShaderManager 编译即可：
    //   shader_mgr.GetOrCreate("text2d", {kTextVs, kTextFs})
    // 编译得到的 program 传给 DrawText2D 的 text_prog 参数。
    //
    // kTextVs: 2D 顶点 shader
    //   输入 attribute: vec2 aPos (location=0), vec2 aTexCoord (location=1)
    //   输入 uniform:   vec2 uFboSize — FBO 像素尺寸（NDC 变换用）
    //   输出 varying:    vec2 vTexCoord
    static constexpr const char* kTextVs = R"glsl(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
uniform vec2 uFboSize;
out vec2 vTexCoord;
void main() {
    vec2 ndc = (aPos / uFboSize) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)glsl";

    // kTextFs: 2D 字体 fragment shader
    //   作用：从单通道（GL_R8）atlas 纹理读取字形 alpha，乘以 uniform 颜色
    //   输入 uniform: sampler2D uTexture (绑定到 TEXTURE0), vec4 uColor
    //   输出: FragColor = vec4(uColor.rgb, uColor.a * texture(uTexture, vTexCoord).r)
    static constexpr const char* kTextFs = R"glsl(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uTexture;
uniform vec4 uColor;
void main() {
    float alpha = texture(uTexture, vTexCoord).r;
    FragColor = vec4(uColor.rgb, uColor.a * alpha);
}
)glsl";

    // ---- Init ----
    //
    // 接收字体配置列表，加载 TTF/TTC 文件，创建三层 GPU atlas（16/32/48px）。
    //
    // 参数：
    //   font_entries:  用户字体列表，每项 = (path, ttc_index, alias)
    //      path:       字体文件路径（UTF-8）。为空 → crash。支持 .ttf / .ttc
    //      ttc_index:  .ttc 集合内索引（单 .ttf 填 0）
    //      alias:      别名，DrawText2D 通过 cmd.font_alias 匹配。为空 → crash。
    //                  最多 10 种用户字体，别名互不重复。
    //   default_fonts: 内置回退字体列表，格式同上。
    //      找不到文件 → 静默跳过；别名与用户字体冲突 → 跳过（用户优先）。
    //
    // Pre-condition: GL context 已激活（需要 glGenTextures 创建 atlas）
    // Pre-condition: font_entries.size() ≤ 10
    // Post-condition: 至少一种字体加载成功（否则 LOG(FATAL)）
    void Init(const std::vector<std::tuple<const char*, int, const char*>>& font_entries,
              const std::vector<std::tuple<const char*, int, const char*>>& default_fonts);

    // ---- DrawText2D ----
    //
    // 将 Text2DCommand 渲染到当前绑定的 FBO。
    //
    // 参数：
    //   cmd:          文字内容 + 位置 + 大小 + alias + 颜色 + 对齐方式。
    //                 cmd.font_size > 0，否则 crash。
    //   stream_vbo:   共享 GL_ARRAY_BUFFER（GLuint），调用前已创建。
    //                 容量需 ≥ kMaxStreamVertices × 2 × sizeof(float)。
    //                 本方法通过 glBufferData 写入一段顶点数据再 draw。
    //   fbo_w, fbo_h: 当前 FBO 的像素尺寸 (≥ 1)。
    //                 应与 glViewport(0, 0, fbo_w, fbo_h) 一致。
    //   text_prog:    由 kTextVs + kTextFs 编译的 GL program。
    //
    // GL 状态前置要求（调用方负责）：
    //   - FBO 已绑定（glBindFramebuffer）
    //   - glViewport(0, 0, fbo_w, fbo_h)
    //   - glEnable(GL_BLEND) + glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)
    //
    // 内部通过 glPushAttrib/glPopAttrib 恢复修改的 GL 状态。
    void DrawText2D(const Text2DCommand& cmd,
                    unsigned int stream_vbo,
                    int fbo_w, int fbo_h,
                    unsigned int text_prog);

    // stream_vbo 容量要求（顶点数上限）
    static constexpr int kMaxStreamVertices = 120000;

    // ---- MeasureTextWidth ----
    //
    // 测量文本以指定字号绘制时的宽度（像素）。
    // 委托 font_slots_ 内按 alias 找到的字体的 FontManager::MeasureTextWidth，
    // 语义与 DrawText2D 的布局推进完全一致（pen 水平终点 = 光标 X）。
    //
    // alias: 与 DrawText2D 的 cmd.font_alias 同规则——空串 → 首个注册字体；
    //       未知别名 → crash（与 DrawText2D 的 FindFontSlot 一致）。
    //
    // Pre-condition: font_size > 0
    float MeasureTextWidth(const std::string& alias,
                           std::string_view text, float font_size);

private:
    struct FontSlot {
        std::optional<FontManager> manager;
        unsigned int atlas_tex[3] = {0, 0, 0};  // [0]=16px, [1]=32px, [2]=48px
    };

    static void InitOneFontSlot(const char* alias,
                                const std::string& resolved_path,
                                int ttc_index,
                                FontSlot* slot);
    static void RegisterFont(const char* path,
                              int ttc_index,
                              const char* alias,
                              const char* source,
                              std::unordered_map<std::string, FontSlot>* font_slots,
                              std::vector<std::string>* font_order);
    FontSlot* FindFontSlot(const std::string& alias);
    void UploadAtlas(FontSlot& slot, int level);
    void UploadAllDirty(FontSlot& slot);

    std::unordered_map<std::string, FontSlot> font_slots_;
    std::vector<std::string> font_order_;
};

}  // namespace jpov

#endif  // JPOV_FONT_RENDERER_H_
