// JPOV Renderer 实现
// FBO 动态调整，坐标以窗口坐标为空间。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <vector>

#include "geom/common/common.h"

#include <GLFW/glfw3.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <glog/logging.h>

// stb_image_write — 轻量级 PNG 编码
#include "stb_image_write.h"

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
#define glUniform1i              gl_Uniform1i
#define glActiveTexture          gl_ActiveTexture
#endif

namespace {

using jpov::Vec3f;

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

// ==================== 3D Shaders ====================

// 3D 顶点 shader：接受 vec3 世界坐标，通过 MVP 矩阵变换到 NDC
const char* kVs3d = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)glsl";

// 3D Fragment Shader（纯色）
const char* kFs3d = R"glsl(
#version 330 core
out vec4 FragColor;
uniform vec4 uColor;

void main() {
    FragColor = uColor;
}
)glsl";

// 3D 纹理顶点 shader（用于 Text3D）
const char* kTexVs3d = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;
uniform mat4 uMVP;
out vec2 vTexCoord;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vTexCoord = aTexCoord;
}
)glsl";

// ==================== MVP 矩阵构建 ====================

// 4x4 矩阵乘法：out = a * b（列主序）
// 仅用于组合 GL 矩阵栈读回的 proj × modelview
static void Mat4Mul(const float a[16], const float b[16], float out[16]) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = sum;
        }
    }
}

// 从 Camera 构建 MVP 矩阵
// 使用 GL 矩阵栈 API（glMatrixMode / glFrustum / glLoadMatrixf），
// 不自造轮子。最后通过 glGetFloatv 读出组合为 shader uniform。
static void BuildMVP(const jpov::Camera& cam, int fbo_w, int fbo_h, float mvp[16]) {
    // === 投影矩阵：用 glFrustum（GL 现成 API）===
    float aspect = static_cast<float>(fbo_w) / static_cast<float>(fbo_h);
    float fov_rad = cam.fov * 3.14159265358979323846f / 180.0f;
    float top = cam.near * std::tan(fov_rad * 0.5f);
    float bottom = -top;
    float right = top * aspect;
    float left = -right;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(left, right, bottom, top, cam.near, cam.far);

    // === 视图矩阵：用 glLoadMatrixf 加载 lookAt（GL 现成 API）===
    // 计算 lookAt 矩阵的坐标基
    Vec3f fwd = cam.target - cam.position;
    float f_len = std::sqrt(fwd.x()*fwd.x() + fwd.y()*fwd.y() + fwd.z()*fwd.z());
    if (f_len < 1e-8f) { fwd = {0.0f, 0.0f, -1.0f}; }
    else { fwd = {fwd.x()/f_len, fwd.y()/f_len, fwd.z()/f_len}; }

    Vec3f side = {fwd.y()*cam.up.z() - fwd.z()*cam.up.y(),
                  fwd.z()*cam.up.x() - fwd.x()*cam.up.z(),
                  fwd.x()*cam.up.y() - fwd.y()*cam.up.x()};
    float s_len = std::sqrt(side.x()*side.x() + side.y()*side.y() + side.z()*side.z());
    if (s_len < 1e-8f) { side = {1.0f, 0.0f, 0.0f}; }
    else { side = {side.x()/s_len, side.y()/s_len, side.z()/s_len}; }

    Vec3f upv = {side.y()*fwd.z() - side.z()*fwd.y(),
                 side.z()*fwd.x() - side.x()*fwd.z(),
                 side.x()*fwd.y() - side.y()*fwd.x()};

    // 列主序 lookAt 矩阵
    float view[16] = {
        side.x(), upv.x(), -fwd.x(), 0.0f,
        side.y(), upv.y(), -fwd.y(), 0.0f,
        side.z(), upv.z(), -fwd.z(), 0.0f,
        -(side.x()*cam.position.x() + side.y()*cam.position.y() + side.z()*cam.position.z()),
        -(upv.x()*cam.position.x() + upv.y()*cam.position.y() + upv.z()*cam.position.z()),
         (fwd.x()*cam.position.x() + fwd.y()*cam.position.y() + fwd.z()*cam.position.z()),
         1.0f
    };

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glLoadMatrixf(view);

    // 读回 GL 矩阵栈，组合 MVP = Proj * ModelView
    float proj[16], modelview[16];
    glGetFloatv(GL_PROJECTION_MATRIX, proj);
    glGetFloatv(GL_MODELVIEW_MATRIX, modelview);
    Mat4Mul(proj, modelview, mvp);
}

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

// 创建 GL atlas 纹理并上传 CPU 像素（初始全黑）
unsigned int CreateGlAtlasTexture(int atlas_dim,
                                  const std::vector<uint8_t>& pixels) {
    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    // 用 GL_R8 单通道纹理，shader 中 .r 读取为 alpha
    // 先传全黑数据占位
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlas_dim, atlas_dim, 0,
                 GL_RED, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
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
    DestroyFBO();
    DestroyOutputFBO();
    if (prog_)         glDeleteProgram(prog_);
    if (prog_3d_)      glDeleteProgram(prog_3d_);
    if (tex_prog_3d_)  glDeleteProgram(tex_prog_3d_);
    if (stream_vbo_)   glDeleteBuffers(1, &stream_vbo_);
    if (strip_vbo_)    glDeleteBuffers(1, &strip_vbo_);
    if (tex_prog_)     glDeleteProgram(tex_prog_);
    // Font GL textures (所有注册字体的三层 atlas)
    for (auto& [alias, slot] : font_slots_) {
        (void)alias;
        for (int lv = 0; lv < 3; ++lv) {
            if (slot.atlas_tex[lv]) glDeleteTextures(1, &slot.atlas_tex[lv]);
        }
    }
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

    // 3D 纯色 shader
    unsigned int vs3d = CompileShader(GL_VERTEX_SHADER, kVs3d);
    unsigned int fs3d = CompileShader(GL_FRAGMENT_SHADER, kFs3d);
    prog_3d_ = LinkProgram(vs3d, fs3d);
    CHECK_NE(prog_3d_, 0u);

    // 3D 纹理 shader（用于 Text3D）
    unsigned int tvs3d = CompileShader(GL_VERTEX_SHADER, kTexVs3d);
    unsigned int tfs3d = CompileShader(GL_FRAGMENT_SHADER, kTexFs);
    tex_prog_3d_ = LinkProgram(tvs3d, tfs3d);
    CHECK_NE(tex_prog_3d_, 0u);
}

void Renderer::CreateStreamVBO() {
    size_t buf = static_cast<size_t>(kMaxStreamVertices) * 2 * sizeof(float);
    glGenBuffers(1, &stream_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER, buf, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Strip3D 专用 VBO：3000 顶点 × 3 floats × sizeof(float)
    size_t strip_buf = static_cast<size_t>(kMaxStripVertices) * 3 * sizeof(float);
    glGenBuffers(1, &strip_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, strip_vbo_);
    glBufferData(GL_ARRAY_BUFFER, strip_buf, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::Init(
    const std::vector<std::tuple<const char*, int, const char*>>& font_entries,
    const std::vector<std::tuple<const char*, int, const char*>>& default_fonts) {
#ifdef _WIN32
    CHECK_EQ(gl_loader_init(), 0) << "Failed to load OpenGL 3.x functions";
#endif
    CompileShaders();
    CreateStreamVBO();
    InitFonts(font_entries, default_fonts);
}


// ==================== 字体初始化 ====================

// 路径查找：先试原始路径，再试 bazel test 的 runfiles（TEST_SRCDIR）
static std::string ResolveFontPath(const char* raw_path) {
    FILE* fp = std::fopen(raw_path, "rb");
    if (fp) {
        std::fclose(fp);
        return raw_path;
    }
    // Try TEST_SRCDIR for bazel test sandbox
    const char* srcdir = std::getenv("TEST_SRCDIR");
    if (srcdir) {
        std::string p = srcdir;
        if (!p.empty() && p.back() != '/') p.push_back('/');
        p += "__main__/";
        p += raw_path;
        FILE* fp2 = std::fopen(p.c_str(), "rb");
        if (fp2) {
            std::fclose(fp2);
            return p;
        }
    }
    return "";
}

// ==================== InitOneFontSlot ====================

void Renderer::InitOneFontSlot(const char* alias,
                                const std::string& resolved_path,
                                int ttc_index,
                                FontSlot* slot /*output*/) {
    CHECK(slot != nullptr);
    CHECK(!slot->manager.has_value()) << "FontSlot already initialized for alias=" << alias;

    FontManagerConfig cfg;
    cfg.font_name = alias;
    cfg.font_path = resolved_path;
    cfg.ttc_font_index = ttc_index;

    std::optional<FontManager> mgr = FontManager::Create(cfg);
    CHECK(mgr.has_value())
        << "Failed to load font: alias=" << alias
        << " path=" << resolved_path
        << " ttc_index=" << ttc_index;

    slot->manager = std::move(mgr.value());
    for (int lv = 0; lv < 3; ++lv) {
        slot->atlas_tex[lv] = CreateGlAtlasTexture(
            FontManager::kAtlasDim,
            slot->manager->atlas_pixels(lv));
    }

    LOG(INFO) << "Font registered: alias=" << alias
              << " path=" << resolved_path
              << " ttc_index=" << ttc_index;
}

// ==================== RegisterFont ====================

void Renderer::RegisterFont(const char* path,
                              int ttc_index,
                              const char* alias,
                              const char* source,
                              std::unordered_map<std::string, FontSlot>* font_slots,
                              std::vector<std::string>* font_order) {
    CHECK(path != nullptr && path[0] != '\0')
        << "FontEntry path is null or empty (alias=" << alias << ")";
    CHECK(alias != nullptr && alias[0] != '\0')
        << "Font alias is null or empty (path=" << path << ")";

    std::string resolved = ResolveFontPath(path);
    if (resolved.empty()) {
        if (strcmp(source, "user") == 0) {
            LOG(FATAL) << "Font file not found: " << path
                       << " (alias=" << alias << ")";
        }
        // builtin 字体找不到就静默跳过
        LOG(INFO) << "Builtin font not found, skipping: " << path;
        return;
    }

    // 检查 alias 是否已注册
    CHECK(font_slots->find(alias) == font_slots->end())
        << "Duplicate font alias: \"" << alias << "\" from source=" << source
        << " path=" << path;

    FontSlot slot;
    InitOneFontSlot(alias, resolved, ttc_index, &slot);

    auto result = font_slots->emplace(alias, std::move(slot));
    CHECK(result.second) << "Duplicate font alias (internal): " << alias;
    font_order->push_back(alias);
}

void Renderer::InitFonts(
    const std::vector<std::tuple<const char*, int, const char*>>& font_entries,
    const std::vector<std::tuple<const char*, int, const char*>>& default_fonts) {
    // 用户字体最多 10 种
    CHECK_LE(static_cast<int>(font_entries.size()), 10)
        << "Too many fonts: " << font_entries.size()
        << " (max 10)";

    // 用户字体内部别名查重
    for (size_t i = 0; i < font_entries.size(); ++i) {
        for (size_t j = i + 1; j < font_entries.size(); ++j) {
            CHECK(strcmp(std::get<2>(font_entries[i]),
                         std::get<2>(font_entries[j])) != 0)
                << "Duplicate font alias: " << std::get<2>(font_entries[i]);
        }
    }

    // === 第一步：注册用户字体 ===
    for (const auto& fe : font_entries) {
        RegisterFont(std::get<0>(fe),
                      std::get<1>(fe),
                      std::get<2>(fe),
                      "user",
                      &font_slots_, &font_order_);
    }

    // === 第二步：注册内置默认字体（共享别名空间） ===
    for (const auto& de : default_fonts) {
        RegisterFont(std::get<0>(de),
                      std::get<1>(de),
                      std::get<2>(de),
                      "builtin",
                      &font_slots_, &font_order_);
    }

    // 至少一种字体可用
    CHECK(!font_slots_.empty())
        << "No fonts loaded (user nor built-in). "
        << "Provide at least one font via JPOV::Config::fonts.";
}

// ==================== FontSlot 查找 ====================

Renderer::FontSlot* Renderer::FindFontSlot(const std::string& alias) {
    if (!alias.empty()) {
        auto it = font_slots_.find(alias);
        if (it != font_slots_.end()) {
            return &it->second;
        }
        // 别名不存在 → crash（用户指定了不存在的字体别名）
        std::string registered;
        for (const auto& a : font_order_) {
            if (!registered.empty()) registered += ", ";
            registered += a;
        }
        LOG(FATAL) << "Unknown font alias: \"" << alias
                   << "\". Registered aliases: "
                   << (font_order_.empty() ? "(none)" : registered);
    }
    // 空别名 → 返回第一个
    if (font_order_.empty()) return nullptr;
    return &font_slots_.at(font_order_[0]);
}

// ==================== Atlas 上传 ====================

void Renderer::UploadAtlas(FontSlot& slot, int level) {
    if (!slot.manager.has_value() || !slot.manager->loaded()) return;
    if (!slot.manager->atlas_dirty(level) || !slot.atlas_tex[level]) return;
    // 全量更新 GL 纹理（4096x4096 不太大，全量上传即可）
    glBindTexture(GL_TEXTURE_2D, slot.atlas_tex[level]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    FontManager::kAtlasDim, FontManager::kAtlasDim,
                    GL_RED, GL_UNSIGNED_BYTE,
                    slot.manager->atlas_pixels(level).data());
    glBindTexture(GL_TEXTURE_2D, 0);
    slot.manager->mark_atlas_clean(level);
    LOG_EVERY_N(INFO, FontManager::kUploadLogInterval) << "UploadAtlas[" << level << "]: uploaded "
        << FontManager::kAtlasDim << "x" << FontManager::kAtlasDim;
}

void Renderer::UploadAllDirty(FontSlot& slot) {
    if (!slot.manager.has_value()) return;
    for (int lv = 0; lv < FontManager::kNumLevels; ++lv) {
        UploadAtlas(slot, lv);
    }
}

// ==================== DrawText2D ====================

void Renderer::DrawText2D(const Text2DCommand& cmd) {
    // 按别名查找字体
    FontSlot* slot = FindFontSlot(cmd.font_alias);
    if (!slot || !slot->manager.has_value() || !slot->manager->loaded()) {
        LOG_EVERY_N(WARNING, FontManager::kNotLoadedLogInterval)
            << "Text2D: font not loaded for alias=\"" << cmd.font_alias << "\", skipping";
        return;
    }

    CHECK_GT(cmd.font_size, 0.0f);

    // GenerateTextVertices 内部执行多级 atlas 选择 + 包围盒计算 + 顶点生成
    int selected_level = 0;
    std::vector<float> verts;
    bool ok = slot->manager->GenerateTextVertices(
        cmd.text, cmd.font_size,
        cmd.pos.x(), cmd.pos.y(),
        static_cast<int>(cmd.alignment),
        fbo_w_, fbo_h_,
        &selected_level,
        &verts);

    if (!ok || verts.empty()) {
        return;
    }

    // 上传新光栅化的字形到 GL atlas（所有脏层）
    UploadAllDirty(*slot);

    // 上传顶点数据到 VBO
    glUseProgram(tex_prog_);
    glUniform2f(glGetUniformLocation(tex_prog_, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(tex_prog_, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);
    glUniform1i(glGetUniformLocation(tex_prog_, "uTexture"), 0);

    // 绑定对应层级的纹理
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, slot->atlas_tex[selected_level]);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_DYNAMIC_DRAW);

    // 位置 (location 0)  | 纹理坐标 (location 1)
    // x,y,u,v interleaved, stride = 4 floats
    constexpr int kStride = 4 * static_cast<int>(sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, kStride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kStride,
                          (void*)(2 * sizeof(float)));

    int vert_count = static_cast<int>(verts.size()) / 4;
    glDrawArrays(GL_TRIANGLES, 0, vert_count);

    GLenum draw_err = glGetError();
    if (draw_err != GL_NO_ERROR) {
        LOG_FIRST_N(WARNING, 1) << "GL error after DrawText2D: " << draw_err;
    }

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

void Renderer::Render(const RenderCommandList& cmds,
                       const WindowInfo& winfo) {
    (void)winfo;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // ---- 第一步：绘制所有 3D 指令（开启深度测试）----
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glClear(GL_DEPTH_BUFFER_BIT);

    // 计算 MVP 矩阵
    float mvp[16];
    BuildMVP(cmds.camera, fbo_w_, fbo_h_, mvp);
    Draw3DCommands(cmds, fbo_w_, fbo_h_);

    // ---- 第二步：关闭深度测试，绘制所有 2D 指令 ----
    glDisable(GL_DEPTH_TEST);

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

void Renderer::Draw3DCommands(const RenderCommandList& cmds, int fbo_w, int fbo_h) {
    // 先用当前 Camera 计算 MVP
    BuildMVP(cmds.camera, fbo_w, fbo_h, mvp_);

    // 遍历 order，绘制 3D 指令
    for (const auto& [type, idx] : cmds.order) {
        switch (type) {
            case DrawCommandType::kTriangle3D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.triangle3d.size()));
                DrawTriangle3D(cmds.triangle3d[idx]);
                break;
            }
            case DrawCommandType::kStrip3D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.strip3d.size()));
                DrawStrip3D(cmds.strip3d[idx]);
                break;
            }
            case DrawCommandType::kLine3D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.line3d.size()));
                DrawLine3D(cmds.line3d[idx], mvp_);
                break;
            }
            case DrawCommandType::kText3D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.text3d.size()));
                DrawText3D(cmds.text3d[idx]);
                break;
            }
            default:
                break;
        }
    }
}

// ==================== 3D 绘制方法 ====================

void Renderer::DrawTriangle3D(const Triangle3DCommand& cmd) {
    // 3 个顶点 × xyz = 9 floats
    float verts[9] = {
        cmd.p1.x(), cmd.p1.y(), cmd.p1.z(),
        cmd.p2.x(), cmd.p2.y(), cmd.p2.z(),
        cmd.p3.x(), cmd.p3.y(), cmd.p3.z(),
    };

    glUseProgram(prog_3d_);
    glUniformMatrix4fv(glGetUniformLocation(prog_3d_, "uMVP"),
                       1, GL_FALSE, mvp_);
    glUniform4f(glGetUniformLocation(prog_3d_, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::DrawStrip3D(const Strip3DCommand& cmd) {
    int n = static_cast<int>(cmd.vertices.size());
    if (n < 3) return;

    // 截断到 3000 顶点上限
    int capped_n = (n > kMaxStripVertices) ? kMaxStripVertices : n;

    // 条带化三角形数 = capped_n - 2
    int tri_count = capped_n - 2;
    // 每个三角形 3 个顶点，共 tri_count * 3 个顶点 × 3 floats
    // 先把所有须绘制的顶点平铺展开到 local buffer
    //（因为条带化共享顶点，直接用 GPU 的 GL_TRIANGLE_STRIP 更简单！）

    // 用 GL_TRIANGLES 模式展开条带化顶点
    // strip 顶点布局：[p0,p1,p2,  p1,p2,p3,  p2,p3,p4, ...]
    // 用 local buffer 写入后一次性上传 VBO
    int total_floats = tri_count * 3 * 3;  // tri_count 个三角形 × 3 顶点 × 3 分量
    std::vector<float> verts;
    verts.reserve(total_floats);
    for (int i = 0; i < tri_count; ++i) {
        const Vec3f& v0 = cmd.vertices[i];
        const Vec3f& v1 = cmd.vertices[i + 1];
        const Vec3f& v2 = cmd.vertices[i + 2];
        verts.push_back(v0.x()); verts.push_back(v0.y()); verts.push_back(v0.z());
        verts.push_back(v1.x()); verts.push_back(v1.y()); verts.push_back(v1.z());
        verts.push_back(v2.x()); verts.push_back(v2.y()); verts.push_back(v2.z());
    }

    glUseProgram(prog_3d_);
    glUniformMatrix4fv(glGetUniformLocation(prog_3d_, "uMVP"),
                       1, GL_FALSE, mvp_);
    glUniform4f(glGetUniformLocation(prog_3d_, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    // 上传到专用 VBO（3000 顶点缓存，跨帧共享）
    glBindBuffer(GL_ARRAY_BUFFER, strip_vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(total_floats * sizeof(float)),
                 verts.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLES, 0, tri_count * 3);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::DrawLine3D(const Line3DCommand& cmd,
                           const float mvp[16]) {
    (void)cmd;
    (void)mvp;
    // 简单的线框支持：绘制 3D 线段
    float verts[6] = {
        cmd.p1.x(), cmd.p1.y(), cmd.p1.z(),
        cmd.p2.x(), cmd.p2.y(), cmd.p2.z(),
    };

    glUseProgram(prog_3d_);
    glUniformMatrix4fv(glGetUniformLocation(prog_3d_, "uMVP"),
                       1, GL_FALSE, mvp);
    glUniform4f(glGetUniformLocation(prog_3d_, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glDrawArrays(GL_LINES, 0, 2);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::DrawText3D(const Text3DCommand& cmd) {
    (void)cmd;
    // Text3D 尚未实现，跳过
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
