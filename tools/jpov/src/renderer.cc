// JPOV Renderer 实现
// FBO 动态调整，坐标以窗口坐标为空间。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "geom/common/common.h"

#include <GLFW/glfw3.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <glog/logging.h>

// stb_image_write — 轻量级 PNG 编码
#include "stb_image_write.h"
// stb_truetype — 轻量级 TrueType 字体光栅化
// 实现由 //third_party/stb:stb_truetype 的 BUILD copts 提供
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

// Windows/MinGW: use function pointers loaded at runtime via wglGetProcAddress
// Linux/Mesa: use standard GL symbols (exported directly by libGL)
#ifdef _WIN32
#include "third_party/gl_loader-mingw/gl_loader.h"
#define glGenBuffers             gl_GenBuffers
#define glDeleteBuffers          gl_DeleteBuffers
#define glBindBuffer             gl_BindBuffer
#define glBufferData             gl_BufferData
#define glCreateShader           gl_CreateShader
#define glShaderSource           gl_ShaderSource
#define glCompileShader          gl_CompileShader
#define glGetShaderiv            gl_GetShaderiv
#define glGetShaderInfoLog       gl_GetShaderInfoLog
#define glCreateProgram          gl_CreateProgram
#define glAttachShader           gl_AttachShader
#define glLinkProgram            gl_LinkProgram
#define glGetProgramiv           gl_GetProgramiv
#define glGetProgramInfoLog      gl_GetProgramInfoLog
#define glDeleteShader           gl_DeleteShader
#define glDeleteProgram          gl_DeleteProgram
#define glUseProgram             gl_UseProgram
#define glGetUniformLocation     gl_GetUniformLocation
#define glUniform2f              gl_Uniform2f
#define glUniform4f              gl_Uniform4f
#define glGenFramebuffers        gl_GenFramebuffers
#define glDeleteFramebuffers     gl_DeleteFramebuffers
#define glBindFramebuffer        gl_BindFramebuffer
#define glFramebufferTexture2D   gl_FramebufferTexture2D
#define glCheckFramebufferStatus gl_CheckFramebufferStatus
#define glGenTextures            gl_GenTextures
#define glDeleteTextures         gl_DeleteTextures
#define glBindTexture            gl_BindTexture
#define glTexImage2D             gl_TexImage2D
#define glTexParameteri          gl_TexParameteri
#define glBlitFramebuffer        gl_BlitFramebuffer
#define glEnableVertexAttribArray  gl_EnableVertexAttribArray
#define glDisableVertexAttribArray gl_DisableVertexAttribArray
#define glVertexAttribPointer    gl_VertexAttribPointer
#endif

namespace {

// 窗口坐标 → NDC 标准化设备坐标
// 原点在窗口左上角，x→右，y→下
// 2D 坐标使用窗口尺寸做 NDC 变换（坐标超出 FBO 范围即裁剪）
const char* kVs = R"glsl(
#version 330 core
layout(location = 0) in vec2 aPos;
uniform vec2 uFboSize;

void main() {
    vec2 ndc = (aPos / uFboSize) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)glsl";

const char* kFs = R"glsl(
#version 330 core
out vec4 FragColor;
uniform vec4 uColor;

void main() {
    FragColor = uColor;
}
)glsl";

// 纹理+颜色混合 Fragment Shader
// 纹理采样（alpha 通道作为透明度）× uniform 颜色
const char* kTexVs = R"glsl(
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

const char* kTexFs = R"glsl(
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

unsigned int CompileShader(GLenum type, const char* source) {
    unsigned int shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        const char* tn = (type == GL_VERTEX_SHADER) ? "VS" : "FS";
        LOG(FATAL) << "Shader compile error [" << tn << "]: " << log;
    }
    return shader;
}

unsigned int LinkProgram(unsigned int vs, unsigned int fs) {
    unsigned int prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        LOG(FATAL) << "Program link error: " << log;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

}  // anonymous namespace

namespace jpov {

const Color kColorRed         = {1.0f, 0.0f, 0.0f, 1.0f};
const Color kColorGreen       = {0.0f, 1.0f, 0.0f, 1.0f};
const Color kColorBlue        = {0.0f, 0.0f, 1.0f, 1.0f};
const Color kColorWhite       = {1.0f, 1.0f, 1.0f, 1.0f};
const Color kColorBlack       = {0.0f, 0.0f, 0.0f, 1.0f};
const Color kColorTransparent = {0.0f, 0.0f, 0.0f, 0.0f};

// === Renderer ===

Renderer::Renderer() = default;

Renderer::~Renderer() {
    DestroyFont();
    DestroyFBO();
    DestroyOutputFBO();
    if (prog_)       glDeleteProgram(prog_);
    if (stream_vbo_) glDeleteBuffers(1, &stream_vbo_);
    if (tex_prog_)   glDeleteProgram(tex_prog_);
}

void Renderer::DestroyFBO() {
    if (fbo_) {
        glDeleteFramebuffers(1, &fbo_);
        glDeleteTextures(1, &color_tex_);
        fbo_ = 0;
        color_tex_ = 0;
    }
    fbo_w_ = 0;
    fbo_h_ = 0;
}

void Renderer::DestroyOutputFBO() {
    if (out_fbo_) {
        glDeleteFramebuffers(1, &out_fbo_);
        glDeleteTextures(1, &out_color_tex_);
        out_fbo_ = 0;
        out_color_tex_ = 0;
    }
    out_w_ = 0;
    out_h_ = 0;
}

void Renderer::EnsureFBO(int width, int height) {
    if (fbo_w_ == width && fbo_h_ == height && fbo_) return;

    CHECK_GT(width, 0);
    CHECK_GT(height, 0);
    CHECK_LE(width, kMaxFboDim);
    CHECK_LE(height, kMaxFboDim);

    DestroyFBO();

    glGenTextures(1, &color_tex_);
    glBindTexture(GL_TEXTURE_2D, color_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, color_tex_, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    CHECK_EQ(status, GL_FRAMEBUFFER_COMPLETE)
        << "FBO failed, status=" << status;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    fbo_w_ = width;
    fbo_h_ = height;
    LOG(INFO) << "Renderer: FBO " << width << "x" << height;
}

void Renderer::CompileShaders() {
    unsigned int vs = CompileShader(GL_VERTEX_SHADER, kVs);
    unsigned int fs = CompileShader(GL_FRAGMENT_SHADER, kFs);
    prog_ = LinkProgram(vs, fs);
    CHECK_NE(prog_, 0u);

    // 纹理 shader
    unsigned int tvs = CompileShader(GL_VERTEX_SHADER, kTexVs);
    unsigned int tfs = CompileShader(GL_FRAGMENT_SHADER, kTexFs);
    tex_prog_ = LinkProgram(tvs, tfs);
    CHECK_NE(tex_prog_, 0u);
}

void Renderer::CreateStreamVBO() {
    size_t buf = static_cast<size_t>(kMaxStreamVertices) * 2 * sizeof(float);
    glGenBuffers(1, &stream_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER, buf, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::Init() {
#ifdef _WIN32
    CHECK_EQ(gl_loader_init(), 0) << "Failed to load OpenGL 3.x functions";
#endif
    CompileShaders();
    CreateStreamVBO();
    LoadFont();
}

void Renderer::DestroyFont() {
    if (font_atlas_tex_) {
        glDeleteTextures(1, &font_atlas_tex_);
        font_atlas_tex_ = 0;
    }
    for (auto& g : font_glyphs_) {
        if (g.pixels) {
            STBTT_free(g.pixels, nullptr);
            g.pixels = nullptr;
        }
    }
    if (font_info_) {
        STBTT_free(font_info_, nullptr);
        font_info_ = nullptr;
    }
    if (font_ttf_data_) {
        STBTT_free(font_ttf_data_, nullptr);
        font_ttf_data_ = nullptr;
    }
    font_loaded_ = false;
}

void Renderer::LoadFont() {
    // 先尝试 bazel run 的相对路径，再试 bazel test 的 runfiles
    const char* kFontPathCandidates[] = {
        "tools/jpov/fonts/DejaVuSans.ttf",
    };

    const char* font_path = nullptr;
    for (const char* c : kFontPathCandidates) {
        FILE* fp = std::fopen(c, "rb");
        if (fp) {
            font_path = c;
            std::fclose(fp);
            break;
        }
    }

    if (!font_path) {
        // Try TEST_SRCDIR for bazel test sandbox
        const char* srcdir = std::getenv("TEST_SRCDIR");
        if (srcdir) {
            std::string p = srcdir;
            if (!p.empty() && p.back() != '/') p.push_back('/');
            p += "__main__/tools/jpov/fonts/DejaVuSans.ttf";
            FILE* fp = std::fopen(p.c_str(), "rb");
            if (fp) {
                font_path = "__srcdir__";
                std::fclose(fp);
                // 重新打开到内存
                fp = std::fopen(p.c_str(), "rb");
                (void)font_path; // already found
                if (!fp) return;
                std::fseek(fp, 0, SEEK_END);
                long sz = std::ftell(fp);
                std::fseek(fp, 0, SEEK_SET);
                font_ttf_data_ = static_cast<unsigned char*>(STBTT_malloc(sz, nullptr));
                if (std::fread(font_ttf_data_, 1, sz, fp) != static_cast<size_t>(sz)) {
                    STBTT_free(font_ttf_data_, nullptr);
                    font_ttf_data_ = nullptr;
                    std::fclose(fp);
                    LOG(WARNING) << "Failed to read font: " << p;
                    return;
                }
                std::fclose(fp);
                goto parse_font;
            }
        }
        LOG(WARNING) << "Font file not found, text rendering disabled";
        return;
    }

    {
        FILE* fp = std::fopen(font_path, "rb");
        if (!fp) {
            LOG(WARNING) << "Failed to open font: " << font_path;
            return;
        }
        std::fseek(fp, 0, SEEK_END);
        long sz = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        font_ttf_data_ = static_cast<unsigned char*>(STBTT_malloc(sz, nullptr));
        if (std::fread(font_ttf_data_, 1, sz, fp) != static_cast<size_t>(sz)) {
            STBTT_free(font_ttf_data_, nullptr);
            font_ttf_data_ = nullptr;
            std::fclose(fp);
            LOG(WARNING) << "Failed to read font: " << font_path;
            return;
        }
        std::fclose(fp);
    }

parse_font:
    font_info_ = static_cast<stbtt_fontinfo*>(STBTT_malloc(sizeof(stbtt_fontinfo), nullptr));
    if (!stbtt_InitFont(font_info_, font_ttf_data_, 0)) {
        STBTT_free(font_info_, nullptr);
        font_info_ = nullptr;
        STBTT_free(font_ttf_data_, nullptr);
        font_ttf_data_ = nullptr;
        LOG(WARNING) << "Failed to init font";
        return;
    }

    // 获取度量信息
    float scale = stbtt_ScaleForPixelHeight(font_info_, 16.0f);  // 16px 基本字号
    int ascent, descent, linegap;
    stbtt_GetFontVMetrics(font_info_, &ascent, &descent, &linegap);
    font_ascent_ = static_cast<float>(ascent) * scale;
    font_descent_ = static_cast<float>(descent) * scale;
    font_linegap_ = static_cast<float>(linegap) * scale;

    // 生成字符图集（ASCII 32~126 可打印字符）
    // 每个字符独立 rasterize，布局到一个大的纹理图集中
    static constexpr int kFirstChar = 32;
    static constexpr int kLastChar = 126;
    static constexpr int kCharCount = kLastChar - kFirstChar + 1;  // 95 chars

    // 估算图集尺寸：假设每个字符最多 64x64，排成一行
    // 实际使用更紧凑的 8 行布局
    static constexpr int kAtlasCols = 16;
    static constexpr int kAtlasRows = 6;  // 16*6=96 >= 95
    static constexpr int kGlyphMaxW = 64;
    static constexpr int kGlyphMaxH = 64;
    static constexpr int kAtlasW = kAtlasCols * kGlyphMaxW;
    static constexpr int kAtlasH = kAtlasRows * kGlyphMaxH;

    // 临时图集 bitmap（灰度，1 byte/pixel）
    std::vector<unsigned char> atlas(kAtlasW * kAtlasH, 0);

    for (int i = 0; i < kCharCount; ++i) {
        int c = kFirstChar + i;
        int col = i % kAtlasCols;
        int row = i / kAtlasCols;
        int origin_x = col * kGlyphMaxW;
        int origin_y = row * kGlyphMaxH;

        GlyphBitmap& g = font_glyphs_[c];
        int pw, ph, xoff, yoff;
        g.pixels = stbtt_GetCodepointBitmap(font_info_,
                                             0, scale,
                                             c, &pw, &ph,
                                             &xoff, &yoff);
        g.w = pw;
        g.h = ph;
        g.xoff = static_cast<float>(xoff);
        g.yoff = static_cast<float>(yoff);

        int advance_width;
        stbtt_GetCodepointHMetrics(font_info_, c, &advance_width, nullptr);
        g.advance = static_cast<float>(advance_width) * scale;

        // 拷贝到纹理图集
        if (pw > 0 && ph > 0 && g.pixels) {
            int copy_w = std::min(pw, kGlyphMaxW);
            int copy_h = std::min(ph, kGlyphMaxH);
            for (int gy = 0; gy < copy_h; ++gy) {
                unsigned char* src = g.pixels + gy * pw;
                unsigned char* dst = atlas.data() + (origin_y + gy) * kAtlasW + origin_x;
                std::memcpy(dst, src, copy_w);
            }
        }
    }

    // 创建纹理
    glGenTextures(1, &font_atlas_tex_);
    glBindTexture(GL_TEXTURE_2D, font_atlas_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kAtlasW, kAtlasH, 0,
                 GL_RED, GL_UNSIGNED_BYTE, atlas.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    font_atlas_w_ = kAtlasW;
    font_atlas_h_ = kAtlasH;
    font_loaded_ = true;
    LOG(INFO) << "Font loaded: DejaVuSans.ttf, atlas " << kAtlasW << "x" << kAtlasH;
}

void Renderer::DrawText2D(const Text2DCommand& cmd) {
    if (!font_loaded_) {
        LOG_EVERY_N(WARNING, 60) << "Text2D: font not loaded, skipping";
        return;
    }

    CHECK_GT(cmd.font_size, 0.0f);

    // 计算缩放比例：目标字号 / 图集基本字号 (16px)
    float scale = cmd.font_size / 16.0f;

    // 收集所有可见字符的顶点数据
    // 每字符：4 顶点（位置+纹理坐标）× 2 三角形 = 6 组 (x, y, tx, ty)
    // interleaved vertex format: pos2f + tex2f = 4 floats/vertex
    struct TextVert { float x, y, tx, ty; };
    static constexpr int kMaxTextChars = 1024;
    TextVert verts_buf[kMaxTextChars * 6];
    int vert_count = 0;

    float cur_x = cmd.pos.x();
    float cur_y = cmd.pos.y();

    static constexpr int kFirstChar = 32;
    static constexpr int kLastChar = 126;
    static constexpr int kAtlasCols = 16;
    static constexpr int kGlyphMaxW = 64;
    static constexpr int kGlyphMaxH = 64;

    for (char ch : cmd.text) {
        int c = static_cast<unsigned char>(ch);
        if (c < kFirstChar || c > kLastChar) {
            if (c == '\n') {
                cur_x = cmd.pos.x();
                cur_y += (font_ascent_ - font_descent_ + font_linegap_) * scale;
            } else if (c == ' ') {
                cur_x += font_glyphs_[' '].advance * scale;
            }
            continue;
        }

        const GlyphBitmap& g = font_glyphs_[c];
        if (vert_count + 6 > kMaxTextChars * 6) break;

        int col = (c - kFirstChar) % kAtlasCols;
        int row = (c - kFirstChar) / kAtlasCols;

        float gx = cur_x + g.xoff * scale;
        float gy = cur_y + g.yoff * scale;
        float gw = static_cast<float>(g.w) * scale;
        float gh = static_cast<float>(g.h) * scale;

        // 纹理坐标 (图集空间)
        float tx0 = static_cast<float>(col * kGlyphMaxW) / static_cast<float>(font_atlas_w_);
        float ty0 = static_cast<float>(row * kGlyphMaxH) / static_cast<float>(font_atlas_h_);
        float tx1 = static_cast<float>(col * kGlyphMaxW + kGlyphMaxW) / static_cast<float>(font_atlas_w_);
        float ty1 = static_cast<float>(row * kGlyphMaxH + kGlyphMaxH) / static_cast<float>(font_atlas_h_);
        // 实际字符在图集中的像素范围
        float tx_adv = static_cast<float>(g.w) / static_cast<float>(kGlyphMaxW) * (tx1 - tx0);
        float ty_adv = static_cast<float>(g.h) / static_cast<float>(kGlyphMaxH) * (ty1 - ty0);

        // 两个三角形 (TRIANGLE_FAN 次序或两个独立三角形)
        // T1: (gx,gy)---(gx+gw,gy)---(gx+gw,gy+gh)
        // T2: (gx,gy)---(gx+gw,gy+gh)---(gx,gy+gh)
        TextVert* v = &verts_buf[vert_count];
        v[0] = {gx,      gy,      tx0, ty0};
        v[1] = {gx+gw,   gy,      tx0 + tx_adv, ty0};
        v[2] = {gx+gw,   gy+gh,   tx0 + tx_adv, ty0 + ty_adv};
        v[3] = {gx,      gy,      tx0, ty0};
        v[4] = {gx+gw,   gy+gh,   tx0 + tx_adv, ty0 + ty_adv};
        v[5] = {gx,      gy+gh,   tx0, ty0 + ty_adv};
        vert_count += 6;

        cur_x += g.advance * scale;
    }

    if (vert_count == 0) return;

    // 上传顶点数据到 VBO
    size_t buf_size = static_cast<size_t>(vert_count) * sizeof(TextVert);
    glUseProgram(tex_prog_);
    glUniform2f(glGetUniformLocation(tex_prog_, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(tex_prog_, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);
    glUniform1i(glGetUniformLocation(tex_prog_, "uTexture"), 0);

    // 绑定纹理
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, font_atlas_tex_);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER, buf_size, verts_buf, GL_DYNAMIC_DRAW);

    // 位置 (location 0)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(TextVert), (void*)0);
    // 纹理坐标 (location 1)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(TextVert), (void*)(sizeof(float) * 2));

    glDrawArrays(GL_TRIANGLES, 0, vert_count);

    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Renderer::BeginFrame(int render_w, int render_h) {
    EnsureFBO(render_w, render_h);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, render_w, render_h);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void Renderer::Render(const RenderCommandList& cmds, const Camera& camera,
                       const WindowInfo& winfo) {
    (void)camera;
    (void)winfo;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (const auto& [type, idx] : cmds.order) {
        switch (type) {
            case DrawCommandType::kRect2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.rect2d.size()));
                DrawRect2D(cmds.rect2d[idx]);
                break;
            }
            case DrawCommandType::kPolyline2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.polyline2d.size()));
                DrawPolyline2D(cmds.polyline2d[idx]);
                break;
            }
            case DrawCommandType::kCircle2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.circle2d.size()));
                DrawCircle2D(cmds.circle2d[idx]);
                break;
            }
            case DrawCommandType::kText2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.text2d.size()));
                DrawText2D(cmds.text2d[idx]);
                break;
            }
            default:
                break;
        }
    }
}

void Renderer::Present(GLFWwindow* window, int window_width, int window_height) {
    int fb_w, fb_h;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    int src_w = std::min(window_width, fbo_w_);
    int src_h = std::min(window_height, fbo_h_);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, src_w, src_h,
                      0, 0, fb_w, fb_h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::EnsureOutputFBO(int win_w, int win_h) {
    if (out_w_ == win_w && out_h_ == win_h && out_fbo_) return;

    CHECK_GT(win_w, 0);
    CHECK_GT(win_h, 0);

    DestroyOutputFBO();

    glGenTextures(1, &out_color_tex_);
    glBindTexture(GL_TEXTURE_2D, out_color_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, win_w, win_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenFramebuffers(1, &out_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, out_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, out_color_tex_, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    CHECK_EQ(status, GL_FRAMEBUFFER_COMPLETE)
        << "Output FBO failed, status=" << status;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    out_w_ = win_w;
    out_h_ = win_h;
    LOG(INFO) << "Renderer: Output FBO " << win_w << "x" << win_h;
}

void Renderer::SaveScreenshot(int win_w, int win_h, const char* path) {
    geom::EnsurePathForFilename(path);

    std::vector<uint8_t> pixels;
    SaveScreenshotToBuffer(win_w, win_h, &pixels);

    // stb_image_write takes RGBA pixels with stride = 4*width
    // stbi_flip_vertically_on_write handles OpenGL's bottom-left origin
    stbi_flip_vertically_on_write(1);
    int ok = stbi_write_png(path, win_w, win_h, 4, pixels.data(), win_w * 4);
    CHECK_NE(ok, 0) << "Failed to write PNG: " << path;
    LOG(INFO) << "Screenshot saved: " << path
              << " (" << win_w << "x" << win_h << ")";
}

void Renderer::SaveScreenshotToBuffer(int win_w, int win_h,
                                        std::vector<uint8_t>* out_pixels) {
    CHECK_GT(win_w, 0);
    CHECK_GT(win_h, 0);
    CHECK_NE(fbo_, 0u);
    CHECK_NOTNULL(out_pixels);

    // 1. 拉伸到 output FBO 模拟 Present 到窗口的效果
    EnsureOutputFBO(win_w, win_h);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, out_fbo_);
    glViewport(0, 0, win_w, win_h);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    int src_w = std::min(win_w, fbo_w_);
    int src_h = std::min(win_h, fbo_h_);
    glBlitFramebuffer(0, 0, src_w, src_h,
                      0, 0, win_w, win_h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // 2. 从 output FBO 读回像素
    out_pixels->resize(static_cast<size_t>(win_w) * win_h * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, out_fbo_);
    glReadPixels(0, 0, win_w, win_h, GL_RGBA, GL_UNSIGNED_BYTE, out_pixels->data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    // 3. RGBA → BGRA（stb_image_write 期望 RGBA，不需要转换）
    // stb_image_write 接受 RGBA 数据，和 OpenGL 读出的 RGBA 一致。
    // Y 翻转由 stbi_flip_vertically_on_write(1) 在 SaveScreenshot 中处理。
    // 这里不做翻转，让调用者决定。
}

void Renderer::DrawPolyline2D(const Polyline2DCommand& cmd) {
    // Pre-condition:
    //   - vertices 至少 2 个点
    //   - edge_count (vertices.size()-1) ≤ kMaxPolylineEdges
    //   - line_width > 0（像素单位）
    int n = static_cast<int>(cmd.vertices.size());
    CHECK_GE(n, 2);
    int edge_count = n - 1;
    CHECK_LE(edge_count, kMaxPolylineEdges);
    CHECK_GT(cmd.line_width, 0.0f);

    // 每个 quad 6 顶点 + 每个 bridge 6 顶点（2 三角形）
    // 顶点格式：x, y, x, y, ...
    int total_verts = edge_count * 6 + (edge_count - 1) * 6;
    std::vector<float> verts;
    verts.reserve(static_cast<size_t>(total_verts) * 2);

    float half_w = cmd.line_width * 0.5f;

    for (int i = 0; i < edge_count; ++i) {
        const Vec2f& p0 = cmd.vertices[i];
        const Vec2f& p1 = cmd.vertices[i + 1];

        // 边向量
        Vec2f dir = p1 - p0;
        float len = std::sqrt(dir.x() * dir.x() + dir.y() * dir.y());

        // 垂直方向（归一化），len 过短时水平偏移
        Vec2f perp;
        static constexpr float kEpsilon = 1e-6f;
        if (len < kEpsilon) {
            perp = {1.0f, 0.0f};
        } else {
            perp = {-dir.y() / len, dir.x() / len};
        }

        // 四个角点
        Vec2f n0 = p0 + perp * half_w;
        Vec2f n1 = p0 - perp * half_w;
        Vec2f n2 = p1 + perp * half_w;
        Vec2f n3 = p1 - perp * half_w;

        // 两个三角形：n0-n1-n2, n2-n1-n3 (CW)
        // T1
        verts.push_back(n0.x()); verts.push_back(n0.y());
        verts.push_back(n1.x()); verts.push_back(n1.y());
        verts.push_back(n2.x()); verts.push_back(n2.y());
        // T2
        verts.push_back(n2.x()); verts.push_back(n2.y());
        verts.push_back(n1.x()); verts.push_back(n1.y());
        verts.push_back(n3.x()); verts.push_back(n3.y());
        // 每个连接处在 V 形间隙外侧补一个三角形
        if (i + 1 < edge_count) {
            const Vec2f& p2 = cmd.vertices[i + 2];
            Vec2f dn = p2 - p1;
            float ln = std::sqrt(dn.x()*dn.x()+dn.y()*dn.y());
            Vec2f perp_n;
            if (ln < kEpsilon) { perp_n = {1.0f, 0.0f}; }
            else { perp_n = {-dn.y()/ln, dn.x()/ln}; }

            // 用顶点和两段矩形外侧角点构成填充三角形
            // 内侧三角形 (p1, n3, n3_next) 和 (p1, n2_next, n2) 填充间隙
            verts.push_back(p1.x()); verts.push_back(p1.y());
            verts.push_back(n3.x()); verts.push_back(n3.y());
            verts.push_back((p1 - perp_n * half_w).x());
            verts.push_back((p1 - perp_n * half_w).y());

            verts.push_back(p1.x()); verts.push_back(p1.y());
            verts.push_back((p1 + perp_n * half_w).x());
            verts.push_back((p1 + perp_n * half_w).y());
            verts.push_back(n2.x()); verts.push_back(n2.y());
        }
    }

    CHECK_LE(total_verts, kMaxStreamVertices);

    glUseProgram(prog_);
    glUniform2f(glGetUniformLocation(prog_, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(prog_, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLES, 0, total_verts);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::DrawRect2D(const Rect2DCommand& cmd) {
    float verts[8];
    float x0 = cmd.pos.x();
    float y0 = cmd.pos.y();
    float x1 = x0 + cmd.size.x();
    float y1 = y0 + cmd.size.y();
    verts[0] = x0;
    verts[1] = y0;
    verts[2] = x1;
    verts[3] = y0;
    verts[4] = x1;
    verts[5] = y1;
    verts[6] = x0;
    verts[7] = y1;

    glUseProgram(prog_);
    // uFboSize = NDC 变换参照。必须用 FBO 尺寸（渲染分辨率），
    // 使 NDC 坐标空间与 glViewport 一致，避免 rect 偏移。
    glUniform2f(glGetUniformLocation(prog_, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(prog_, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::DrawCircle2D(const Circle2DCommand& cmd) {
    static constexpr int kSegments = 64;
    float verts[(kSegments + 2) * 2];  // fan center + kSegments perimeter points
    float cx = cmd.center.x();
    float cy = cmd.center.y();
    float r = cmd.radius;

    // Center of fan
    verts[0] = cx;
    verts[1] = cy;

    constexpr double kPi = 3.14159265358979323846;
    for (int i = 0; i <= kSegments; ++i) {
        double angle = 2.0 * kPi * static_cast<double>(i) /
                       static_cast<double>(kSegments);
        verts[(i + 1) * 2 + 0] = cx + r * static_cast<float>(cos(angle));
        verts[(i + 1) * 2 + 1] = cy + r * static_cast<float>(sin(angle));
    }

    glUseProgram(prog_);
    glUniform2f(glGetUniformLocation(prog_, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(prog_, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLE_FAN, 0, kSegments + 2);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

}  // namespace jpov
