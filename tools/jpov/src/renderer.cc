// JPOV Renderer 实现
// FBO 动态调整，坐标以窗口坐标为空间。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "geom/common/common.h"

#include "tools/jpov/interface/gltf_object.h"
#include "tools/jpov/src/gltf_loader.h"
#include "tools/jpov/src/orm_unpack.h"

#include <GLFW/glfw3.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <glog/logging.h>

// stb_image_write — 轻量级 PNG 编码
#include "stb_image_write.h"
// stb_image — PNG/JPEG 解码（RenderGltf 拆 ORM 用）
#include "stb_image.h"

// Windows/MinGW: use function pointers loaded at runtime via wglGetProcAddress
// Linux/Mesa: use standard GL symbols (exported directly by libGL)
//
// 注意：MinGW 的 GL 库不导出 renderbuffer / MSAA 的 GL 3.x 核心函数，
// 因此 Windows 下 3D FBO 不使用 MSAA（带 #define JPOV_WITHOUT_MSAA）。
#ifdef _WIN32
#define JPOV_WITHOUT_MSAA
#include "third_party/gl_loader-mingw/gl_loader.h"
// glXxx→gl_Xxx 别名宏已集成在 gl_loader.h 中，本文件不再重复定义。

// MinGW 的 GL/gl.h 可能不定义 GL_CLAMP_TO_EDGE
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
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

// RGBA 纹理 Fragment Shader（用于 Image2D 全彩纹理）
const char* kImageFs = R"glsl(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uTexture;
uniform vec4 uTint;

void main() {
    vec4 tex_color = texture(uTexture, vTexCoord);
    FragColor = tex_color * uTint;
}
)glsl";

// 创建 GL atlas 纹理并上传 CPU 像素（初始全黑）

// 构建正交投影矩阵（列主序，OpenGL 右手系）。阴影 pass 用：太阳是平行光，视锥是正交盒。
void BuildOrthoProj(float left, float right, float bottom, float top,
                    float near_, float far_, float out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = 0.0f;
    out[0]  = 2.0f / (right - left);
    out[5]  = 2.0f / (top - bottom);
    out[10] = -2.0f / (far_ - near_);
    out[12] = -(right + left) / (right - left);
    out[13] = -(top + bottom) / (top - bottom);
    out[14] = -(far_ + near_) / (far_ - near_);
    out[15] = 1.0f;
}

// 构建 lookAt 视图矩阵。
//
// ⚠️ 必须与 Primitives3DRenderer::BuildMVP 里的 lookAt 布局完全一致：那处是
// 「行主序书写、列主序存储」的转置写法（view[0..3] 装 side/upv/-fwd 的 x 分量），
// 而非教科书 column-major。此处 copy 成同一种转置布局，否则阴影相机的 view 会
// 整体转置 → 影子 x/y 对调、depth 映射到错误轴（2026-08-17 踩坑实录）。
void BuildLookAt(const Vec3f& eye, const Vec3f& center, const Vec3f& up,
                 float out[16]) {
    Vec3f fwd = center - eye;
    float f_len = std::sqrt(fwd.x()*fwd.x() + fwd.y()*fwd.y() + fwd.z()*fwd.z());
    if (f_len < 1e-8f) { fwd = Vec3f(0.0f, 0.0f, -1.0f); }
    else { fwd = Vec3f(fwd.x()/f_len, fwd.y()/f_len, fwd.z()/f_len); }

    Vec3f side = Vec3f(fwd.y()*up.z() - fwd.z()*up.y(),
                       fwd.z()*up.x() - fwd.x()*up.z(),
                       fwd.x()*up.y() - fwd.y()*up.x());
    float s_len = std::sqrt(side.x()*side.x() + side.y()*side.y() + side.z()*side.z());
    if (s_len < 1e-8f) { side = Vec3f(1.0f, 0.0f, 0.0f); }
    else { side = Vec3f(side.x()/s_len, side.y()/s_len, side.z()/s_len); }

    Vec3f upv = Vec3f(side.y()*fwd.z() - side.z()*fwd.y(),
                      side.z()*fwd.x() - side.x()*fwd.z(),
                      side.x()*fwd.y() - side.y()*fwd.x());

    out[0] = side.x(); out[1] = upv.x(); out[2]  = -fwd.x(); out[3] = 0.0f;
    out[4] = side.y(); out[5] = upv.y(); out[6]  = -fwd.y(); out[7] = 0.0f;
    out[8] = side.z(); out[9] = upv.z(); out[10] = -fwd.z(); out[11] = 0.0f;
    out[12] = -(side.x()*eye.x() + side.y()*eye.y() + side.z()*eye.z());
    out[13] = -(upv.x()*eye.x() + upv.y()*eye.y() + upv.z()*eye.z());
    out[14] =  (fwd.x()*eye.x() + fwd.y()*eye.y() + fwd.z()*eye.z());
    out[15] = 1.0f;
}

// 手动矩阵乘（列主序），out = a * b。与 Object3DRenderer 匿名空间的 Mat4Mul 等价。
void RendererMat4Mul(const float a[16], const float b[16], float out[16]) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k*4 + r] * b[c*4 + k];
            }
            out[c*4 + r] = sum;
        }
    }
}

// 校验全局阴影配置（CSM）。在 Renderer::Init 时一次性调用，非法参数直接 LOG(FATAL)
// crash，避免运行期才发现（如级联段重叠/越界导致 FBO 创建崩溃）。
void ValidateShadowConfig(const jpov::ShadowConfig& s) {
    CHECK_GE(s.cascade_count, 1) << "cascade_count 至少 1，当前 " << s.cascade_count;
    CHECK_LE(s.cascade_count, jpov::ShadowConfig::kMaxCascades)
        << "cascade_count 至多 " << jpov::ShadowConfig::kMaxCascades
        << "，当前 " << s.cascade_count;
    for (int i = 0; i < s.cascade_count; ++i) {
        CHECK_GT(s.cascade_ranges[i], 0.0f)
            << "cascade_ranges[" << i << "] 必须 > 0";
        CHECK_GT(s.cascade_sizes[i], 0)
            << "cascade_sizes[" << i << "] 必须 > 0";
        CHECK_LE(s.cascade_sizes[i], jpov::Renderer::kMaxFboDim)
            << "shadow map 尺寸超限（kMaxFboDim="
            << jpov::Renderer::kMaxFboDim << "），cascade_sizes[" << i << "]="
            << s.cascade_sizes[i];
        if (i > 0) {
            CHECK_GT(s.cascade_ranges[i], s.cascade_ranges[i-1])
                << "cascade_ranges 必须严格递增（每段 far 单调），"
                << "cascade_ranges[" << i-1 << "]=" << s.cascade_ranges[i-1]
                << " >= cascade_ranges[" << i << "]=" << s.cascade_ranges[i];
        }
    }
    CHECK_GE(s.fade_end, 0.0f) << "fade_end >= 0";
    CHECK_GT(s.fade_end, s.fade_start)
        << "fade_end 必须 > fade_start，当前 fade_start=" << s.fade_start
        << " fade_end=" << s.fade_end;
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
    Destroy3DFBO();
    Destroy3DResolveFBO();
    DestroyShadowFBO();
    if (tile_index_tex_) { glDeleteTextures(1, &tile_index_tex_); tile_index_tex_ = 0; }
    // 注意：shader program 由 ShaderManager::~ShaderManager() 统一释放
    if (stream_vbo_)   glDeleteBuffers(1, &stream_vbo_);
    if (strip_vbo_)    glDeleteBuffers(1, &strip_vbo_);
    // font atlas 纹理由 FontRenderer 析构管理
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

void Renderer::Destroy3DFBO() {
    if (fbo_3d_) {
        glDeleteFramebuffers(1, &fbo_3d_);
        glDeleteTextures(1, &color_tex_3d_);
#ifndef JPOV_WITHOUT_MSAA
        if (depth_rb_3d_) {
            glDeleteRenderbuffers(1, &depth_rb_3d_);
        }
#else
        if (depth_tex_3d_) {
            glDeleteTextures(1, &depth_tex_3d_);
        }
#endif
        fbo_3d_ = 0;
        color_tex_3d_ = 0;
        depth_rb_3d_ = 0;
        depth_tex_3d_ = 0;
    }
    fbo_3d_w_ = 0;
    fbo_3d_h_ = 0;
}

void Renderer::Destroy3DResolveFBO() {
    if (resolve_fbo_3d_) {
        glDeleteFramebuffers(1, &resolve_fbo_3d_);
        glDeleteTextures(1, &resolve_tex_3d_);
        resolve_fbo_3d_ = 0;
        resolve_tex_3d_ = 0;
    }
    resolve_fbo_3d_w_ = 0;
    resolve_fbo_3d_h_ = 0;
}

void Renderer::DestroyShadowFBO() {
    for (auto& c : shadow_fbos_) {
        if (c.fbo) glDeleteFramebuffers(1, &c.fbo);
        if (c.tex) glDeleteTextures(1, &c.tex);
        if (c.rb) glDeleteRenderbuffers(1, &c.rb);
    }
    shadow_fbos_.clear();
}

void Renderer::EnsureShadowFBO(const ShadowConfig& cfg) {
    CHECK_GT(cfg.cascade_count, 0);
    CHECK_LE(cfg.cascade_count, ShadowConfig::kMaxCascades);

    // 若段数/尺寸与现状一致则复用（幂等）；否则整体重建。
    bool need_rebuild = shadow_fbos_.size() != static_cast<size_t>(cfg.cascade_count);
    if (!need_rebuild) {
        for (int i = 0; i < cfg.cascade_count; ++i) {
            if (shadow_fbos_[i].size != cfg.cascade_sizes[i]) {
                need_rebuild = true;
                break;
            }
        }
    }
    if (need_rebuild) DestroyShadowFBO();

    for (int i = static_cast<int>(shadow_fbos_.size()); i < cfg.cascade_count; ++i) {
        const int size = cfg.cascade_sizes[i];
        CHECK_GT(size, 0);
        CHECK_LE(size, kMaxFboDim);

        CascadeFBO c;
        c.size = size;

        // 阴影贴图用 RGBA32F 颜色纹理存光空间 ndc.z（而非 GL 深度缓冲）。
        // 原因（2026-08-17 踩坑）：llvmpipe（headless/Xvfb 软渲染）下 depth 纹理
        // 采样精度/格式不可靠 —— glReadPixels 读 FBO depth 正确，但 shader 里
        // texture(depthTex).r 采样同一张 depth texture 却得到错值。硬件 GPU 无此问题，
        // 但软渲染会。故改用手动写 ndc.z 到浮点颜色纹理的 .r 通道，采样即可靠。
        glGenTextures(1, &c.tex);
        glBindTexture(GL_TEXTURE_2D, c.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, size, size, 0,
                     GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &c.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, c.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, c.tex, 0);

        // 配套 depth renderbuffer：仅遮挡测试用（保证只写最近遮挡物深度），
        // 阴影深度数据本身写入颜色纹理的 .r 通道（见 kShadowFs）。
        glGenRenderbuffers(1, &c.rb);
        glBindRenderbuffer(GL_RENDERBUFFER, c.rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size, size);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, c.rb);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        CHECK_EQ(status, GL_FRAMEBUFFER_COMPLETE)
            << "Shadow FBO failed, status=" << status;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        shadow_fbos_.push_back(c);
    }
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

void Renderer::Ensure3DFBO(int width, int height) {
    if (fbo_3d_w_ == width && fbo_3d_h_ == height && fbo_3d_) return;

    CHECK_GT(width, 0);
    CHECK_GT(height, 0);
    CHECK_LE(width, kMaxFboDim);
    CHECK_LE(height, kMaxFboDim);

    Destroy3DFBO();
    Destroy3DResolveFBO();
    if (tile_index_tex_) { glDeleteTextures(1, &tile_index_tex_); tile_index_tex_ = 0; }

#ifdef JPOV_WITHOUT_MSAA
    // 非 MSAA 路径（Windows/MinGW 下 GL 不导出 MSAA 函数）
    // 直接用普通 2D 纹理 + 深度纹理
    glGenTextures(1, &color_tex_3d_);
    glBindTexture(GL_TEXTURE_2D, color_tex_3d_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenTextures(1, &depth_tex_3d_);
    glBindTexture(GL_TEXTURE_2D, depth_tex_3d_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0,
                 GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &fbo_3d_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_3d_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, color_tex_3d_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, depth_tex_3d_, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    CHECK_EQ(status, GL_FRAMEBUFFER_COMPLETE)
        << "3D FBO (non-MSAA) failed, status=" << status;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // 非 MSAA 路径不需要 separate resolve FBO（直接用 fbo_3d_ blit）
    resolve_fbo_3d_ = 0;
    resolve_tex_3d_ = 0;
    resolve_fbo_3d_w_ = 0;
    resolve_fbo_3d_h_ = 0;
#else
    // === 4x MSAA 颜色纹理 ===
    glGenTextures(1, &color_tex_3d_);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, color_tex_3d_);
    // 已知问题（2026-08-14 定位）: 在 llvmpipe（Mesa 软渲染，headless/Xvfb）下，
    // glTexImage2DMultisample 会产生一个非致命 GL_INVALID_OPERATION (1280)。
    // 它不破坏渲染（后续 resolve blit 仍能正确出图，gold 测试逐字节匹配通过），
    // 但因 glGetError 返回“第一个未清错误”，会一直残留到 DrawObject3D 后的
    // 检查才被捕获，表现为看似来自 draw 的 1280 警告。已用打桩验证：此错误
    // 精确源于本调用（深度 MSAA renderbuffer 探针干净），与 draw 无关。
    // 因 llvmpipe 不支持 MSAA 且当前 gold 图均基于 MSAA 路径生成，
    // 暂不改动（改非 MSAA 会改变输出字节）。详见 PR #50。
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA8,
                            width, height, GL_TRUE);
    glTexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);

    // === 深度 renderbuffer（MSAA 需要） ===
    glGenRenderbuffers(1, &depth_rb_3d_);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rb_3d_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_DEPTH_COMPONENT24,
                                     width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    // === FBO 对象 ===
    glGenFramebuffers(1, &fbo_3d_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_3d_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D_MULTISAMPLE, color_tex_3d_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, depth_rb_3d_);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    CHECK_EQ(status, GL_FRAMEBUFFER_COMPLETE)
        << "3D MSAA FBO failed, status=" << status;

    // === 中间 non-MSAA resolve FBO（同尺寸，用于 resolve + 缩放 blit） ===
    glGenTextures(1, &resolve_tex_3d_);
    glBindTexture(GL_TEXTURE_2D, resolve_tex_3d_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &resolve_fbo_3d_);
    glBindFramebuffer(GL_FRAMEBUFFER, resolve_fbo_3d_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, resolve_tex_3d_, 0);

    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    CHECK_EQ(status, GL_FRAMEBUFFER_COMPLETE)
        << "3D resolve FBO failed, status=" << status;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    resolve_fbo_3d_w_ = width;
    resolve_fbo_3d_h_ = height;
#endif

    fbo_3d_w_ = width;
    fbo_3d_h_ = height;
}

// ---- Shader program 访问器 ----
// 通过 ShaderManager 按名字获取。GetOrCreate 幂等：首次调用编译+缓存，
// 后续直接返回缓存的 program，因此可在 Init 和每次 draw 时安全调用。

unsigned int Renderer::SolidProg() {
    return shader_mgr_.GetOrCreate("solid", {kVs, kFs});
}

unsigned int Renderer::TextProg() {
    return shader_mgr_.GetOrCreate("text",
        {FontRenderer::kTextVs, FontRenderer::kTextFs});
}

unsigned int Renderer::ImageProg() {
    return shader_mgr_.GetOrCreate("image", {kTexVs, kImageFs});
}

unsigned int Renderer::Solid3DProg() {
    return shader_mgr_.GetOrCreate("solid3d", {Primitives3DRenderer::kVs3d, Primitives3DRenderer::kFs3d});
}

unsigned int Renderer::Text3DProg() {
    return shader_mgr_.GetOrCreate("text3d", {Primitives3DRenderer::kTexVs3d, kTexFs});
}

// DrawObject3D PBR shader — 无 UV 版本（mesh 不含 kUV 时使用）。
// vertex shader 只声明 location 0/1（aPos/aNormal），避免 VAO 中未绑定的
// location 2/5 导致部分 GL 实现异常。所有材质通道走 uHas*Tex=0 常值 fallback。
unsigned int Renderer::DrawObject3DProg() {
    return shader_mgr_.GetOrCreate("draw_object3d_pbr",
        {Object3DRenderer::kMeshVs3dPBR, Object3DRenderer::kMeshFs3dPBR});
}

// DrawObject3D PBR shader — 完整版（mesh 含 kUV+kTangent 时使用）。
// vertex shader 声明 location 0/1/2/5（aPos/aNormal/aTexCoord/aTangent），
// 支持纹理采样（baseColor/metallic/roughness/emissive/AO）和 TBN 法线映射。
unsigned int Renderer::DrawObject3DProgFull() {
    return shader_mgr_.GetOrCreate("draw_object3d_pbr_full",
        {Object3DRenderer::kMeshVs3dPBRFull, Object3DRenderer::kMeshFs3dPBR});
}

// 太阳阴影 pass 专用 shader（深度专用，见 kShadowVs/kShadowFs）。
unsigned int Renderer::ShadowProg() {
    return shader_mgr_.GetOrCreate("shadow",
        {Object3DRenderer::kShadowVs, Object3DRenderer::kShadowFs});
}

// 编译/注册全部 shader program。
// 通过 ShaderManager 统一管理（编译失败 → LOG(FATAL) crash）。
void Renderer::CompileShaders() {
    // 首次调用即触发编译+缓存；重复调用幂等。
    SolidProg();
    TextProg();
    ImageProg();
    Solid3DProg();
    Text3DProg();
    DrawObject3DProg();
    DrawObject3DProgFull();
    ShadowProg();
}

void Renderer::CreateStreamVBO() {
    size_t buf = static_cast<size_t>(kMaxStreamVertices) * 2 * sizeof(float);
    glGenBuffers(1, &stream_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER, buf, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Strip3D 专用 VBO：3000 顶点 × 3 floats × sizeof(float)
    size_t strip_buf = static_cast<size_t>(3000) * 3 * sizeof(float);  // Strip3D 上限
    glGenBuffers(1, &strip_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, strip_vbo_);
    glBufferData(GL_ARRAY_BUFFER, strip_buf, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::Init(
    const std::vector<std::tuple<const char*, int, const char*>>& font_entries,
    const std::vector<std::tuple<const char*, int, const char*>>& default_fonts,
    const ShadowConfig& shadow_cfg) {
#ifdef _WIN32
    CHECK_EQ(gl_loader_init(), 0) << "Failed to load OpenGL 3.x functions";
#endif
    ValidateShadowConfig(shadow_cfg);
    shadow_cfg_ = shadow_cfg;
    CompileShaders();
    CreateStreamVBO();
    font_renderer_.Init(font_entries, default_fonts);
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
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // ---- 补全 viewport 和 3D FBO 尺寸 ----
    // 如果 viewport 全 0，用窗口尺寸补全为全屏
    Camera cam = cmds.camera;
    if (cam.viewport_width == 0.0f && cam.viewport_height == 0.0f) {
        cam.viewport_x = 0.0f;
        cam.viewport_y = 0.0f;
        cam.viewport_width = winfo.width;
        cam.viewport_height = winfo.height;
    }
    // 如果 3D FBO 尺寸未设置，用主 FBO 尺寸
    int fbo_3d_w = static_cast<int>(cam.fbo_3d_width_);
    int fbo_3d_h = static_cast<int>(cam.fbo_3d_height_);
    if (fbo_3d_w <= 0 || fbo_3d_h <= 0) {
        fbo_3d_w = fbo_w_;
        fbo_3d_h = fbo_h_;
    }

    // ---- 检查是否有 3D 指令 ----
    bool has_3d = false;
    for (const auto& [type, idx] : cmds.order) {
        (void)idx;
        if (type == DrawCommandType::kTriangle3D ||
            type == DrawCommandType::kStrip3D ||
            type == DrawCommandType::kLine3D ||
            type == DrawCommandType::kText3D ||
            type == DrawCommandType::kObject3D) {
            has_3d = true;
            break;
        }
    }

    if (has_3d) {
        // ---- 保存并设置 3D 所需 GL 状态 ----
        // 用 glPushAttrib 批保存所有受影响的 GL 状态位。
        // GL_ENABLE_BIT:   GL_DEPTH_TEST, GL_CULL_FACE
        // GL_VIEWPORT_BIT:  glViewport
        // GL_SCISSOR_BIT:   视口相关
        // GL_CURRENT_BIT:   glMatrixMode 等（其实 3D 改的是 shader uniform，
        //                   不再改矩阵栈了，但保留 push/pop 习惯）
        glPushAttrib(GL_ENABLE_BIT | GL_VIEWPORT_BIT);

        // ---- 第 0 步：太阳阴影 pass（有 sun 时）----
        // 先渲染场景深度到阴影纹理，主 pass 的 PBR shader 才能采样做影子。
        // DrawShadowPass 内部会绑定 shadow FBO 并改变 GL 状态，放在 Ensure3DFBO
        // 之前：后续 Ensure3DFBO + bind + clear 会恢复正常 3D 状态。
        if (cmds.sun.has_value()) {
            EnsureShadowFBO(shadow_cfg_);
            DrawShadowPass(cmds);
        }

        // ---- 第一步：在离屏 MSAA FBO 上绘制所有 3D 指令 ----
        Ensure3DFBO(fbo_3d_w, fbo_3d_h);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_3d_);
        glViewport(0, 0, fbo_3d_w, fbo_3d_h);

        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);

        // Clear 3D FBO（颜色 + 深度）
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // 先用当前 Camera 计算 MVP（Proj * View，不含 Model）。
        // tile culling 投影光源需要它，必须先于 BuildTileLightIndices。
        Primitives3DRenderer::BuildMVP(cmds.camera, fbo_3d_w, fbo_3d_h, mvp_);

        // ---- Tile Forward 光照准备 ----
        // 若有 Object3D 需光照（!object_use_default_color）：
        //   - 总是上传光源 uniform + uTileCulling 开关（uLights[]/uTotalLights）
        //   - 仅当 tile_culling=true 才做 CPU 端 tile 索引构建（写入 tile 纹理）；
        //     tile_culling=false 时跳过，shader 走全光源遍历（无分界线）。
        if (!cmds.object_use_default_color && !cmds.object3d.empty()) {
            Object3DRenderer::UploadLightData(cmds, shader_mgr_,
                DrawObject3DProg(), DrawObject3DProgFull());
            if (cmds.tile_culling) {
                Object3DRenderer::EnsureTileLighting(fbo_3d_w, fbo_3d_h,
                    &tile_index_tex_, &tile_grid_w_, &tile_grid_h_,
                    &tile_tex_w_, &tile_tex_h_);
                Object3DRenderer::BuildTileLightIndices(cmds,
                    fbo_3d_w, fbo_3d_h, tile_index_tex_,
                    tile_grid_w_, tile_grid_h_, tile_tex_w_, tile_tex_h_, mvp_);
            }
        }

        // 太阳平行光 + 级联阴影贴图 uniform（有 sun 时）。
        // 绑各级联 shadow 深度纹理到 PBR shader，供直射光的 PCF 阴影采样。
        if (cmds.sun.has_value()) {
            Object3DRenderer::UploadSunData(cmds, shader_mgr_,
                DrawObject3DProg(), DrawObject3DProgFull(),
                shadow_fbos_, shadow_vp_, shadow_cfg_);
        }

        // 用 3D FBO 尺寸计算 MVP
        Draw3DCommands(cmds, fbo_3d_w, fbo_3d_h);

        // ---- 第二步：MSAA resolve 或直接 blit 到主 FBO ----
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);

#ifdef JPOV_WITHOUT_MSAA
        // 非 MSAA 路径：从 3D FBO 直接 blit 到主 FBO
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_3d_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
        glBlitFramebuffer(
            0, 0, fbo_3d_w, fbo_3d_h,
            static_cast<int>(cam.viewport_x),
            static_cast<int>(cam.viewport_y),
            static_cast<int>(cam.viewport_x + cam.viewport_width),
            static_cast<int>(cam.viewport_y + cam.viewport_height),
            GL_COLOR_BUFFER_BIT, GL_LINEAR);
#else
        // 先 MSAA resolve 到中间非 MSAA FBO（同尺寸 resolve）
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_3d_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo_3d_);
        glBlitFramebuffer(
            0, 0, fbo_3d_w, fbo_3d_h,
            0, 0, resolve_fbo_3d_w_, resolve_fbo_3d_h_,
            GL_COLOR_BUFFER_BIT, GL_LINEAR);

        // 再从中间 FBO 缩放 blit 到主 FBO（按 viewport）
        glBindFramebuffer(GL_READ_FRAMEBUFFER, resolve_fbo_3d_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
        glBlitFramebuffer(
            0, 0, resolve_fbo_3d_w_, resolve_fbo_3d_h_,
            static_cast<int>(cam.viewport_x),
            static_cast<int>(cam.viewport_y),
            static_cast<int>(cam.viewport_x + cam.viewport_width),
            static_cast<int>(cam.viewport_y + cam.viewport_height),
            GL_COLOR_BUFFER_BIT, GL_LINEAR);
#endif

        // ---- 恢复 GL 状态（回到 2D 绘制前的状态）----
        glPopAttrib();
    }

    // ---- 第四步：绑定主 FBO，绘制所有 2D 指令 ----
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, fbo_w_, fbo_h_);

    for (const auto& [type, idx] : cmds.order) {
        switch (type) {
            case DrawCommandType::kRect2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.rect2d.size()));
                Primitives2DRenderer::DrawRect2D(cmds.rect2d[idx], stream_vbo_, SolidProg(), fbo_w_, fbo_h_);
                break;
            }
            case DrawCommandType::kPolyline2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.polyline2d.size()));
                Primitives2DRenderer::DrawPolyline2D(cmds.polyline2d[idx], stream_vbo_, SolidProg(), fbo_w_, fbo_h_);
                break;
            }
            case DrawCommandType::kCircle2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.circle2d.size()));
                Primitives2DRenderer::DrawCircle2D(cmds.circle2d[idx], stream_vbo_, SolidProg(), fbo_w_, fbo_h_);
                break;
            }
            case DrawCommandType::kText2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.text2d.size()));
                font_renderer_.DrawText2D(cmds.text2d[idx],
                                          stream_vbo_, fbo_w_, fbo_h_,
                                          TextProg());
                break;
            }
            case DrawCommandType::kStrip2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.strip2d.size()));
                Primitives2DRenderer::DrawStrip2D(cmds.strip2d[idx], stream_vbo_, SolidProg(), fbo_w_, fbo_h_);
                break;
            }
            case DrawCommandType::kRoundRect2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.roundrect2d.size()));
                Primitives2DRenderer::DrawRoundRect2D(cmds.roundrect2d[idx], stream_vbo_, SolidProg(), fbo_w_, fbo_h_);
                break;
            }
            case DrawCommandType::kFillRect2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.fillrect2d.size()));
                Primitives2DRenderer::DrawFillRect2D(cmds.fillrect2d[idx], stream_vbo_, SolidProg(), fbo_w_, fbo_h_);
                break;
            }
            case DrawCommandType::kArc2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.arc2d.size()));
                Primitives2DRenderer::DrawArc2D(cmds.arc2d[idx], stream_vbo_, SolidProg(), fbo_w_, fbo_h_);
                break;
            }
            case DrawCommandType::kImage2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.image2d.size()));
                Primitives2DRenderer::DrawImage2D(cmds.image2d[idx], stream_vbo_, ImageProg(), texture_mgr_, fbo_w_, fbo_h_);
                break;
            }
            default:
                break;
        }
    }
}

void Renderer::Draw3DCommands(const RenderCommandList& cmds, int fbo_w, int fbo_h) {
    // 先用当前 Camera 计算 MVP（View * Proj，不含 Model）
    Primitives3DRenderer::BuildMVP(cmds.camera, fbo_w, fbo_h, mvp_);

    // 遍历 order，绘制 3D 指令
    for (const auto& [type, idx] : cmds.order) {
        switch (type) {
            case DrawCommandType::kTriangle3D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.triangle3d.size()));
                Primitives3DRenderer::DrawTriangle3D(
                    cmds.triangle3d[idx], stream_vbo_, Solid3DProg(), mvp_);
                break;
            }
            case DrawCommandType::kStrip3D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.strip3d.size()));
                Primitives3DRenderer::DrawStrip3D(
                    cmds.strip3d[idx], strip_vbo_, Solid3DProg(), mvp_);
                break;
            }
            case DrawCommandType::kLine3D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.line3d.size()));
                Primitives3DRenderer::DrawLine3D(
                    cmds.line3d[idx], stream_vbo_, Solid3DProg(), mvp_);
                break;
            }
            case DrawCommandType::kText3D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.text3d.size()));
                Primitives3DRenderer::DrawText3D(
                    cmds.text3d[idx], stream_vbo_, Text3DProg(), mvp_,
                    fbo_w, fbo_h);
                break;
            }
            case DrawCommandType::kObject3D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.object3d.size()));
                Object3DRenderer::DrawObject3D(cmds.object3d[idx], cmds,
                    mesh_mgr_, texture_mgr_, shader_mgr_, mvp_,
                    DrawObject3DProg(), DrawObject3DProgFull(),
                    tile_index_tex_);
                break;
            }
            default:
                break;
        }
    }
}

// ---- DrawShadowPass ----
// 太阳阴影 pass（CSM）：当 cmds.sun 有值时，把场景里所有 Object3D 从太阳正交视角
// 按级联渲染进各段阴影纹理（只写光空间 ndc.z），供主 pass 的 PBR shader 采样做 PCF。
//
// 每段：取相机在该级联 near~far 之间的视锥 8 角点 → 变换到光源 view 空间 → 求 AABB
// → 用 AABB 定该段正交投影范围。这样每段 shadow map 只覆盖 "这一段视锥在光源空间
// 的包围盒"，而非全场景 —— 近段高分辨率、远段低分辨率（CSM 核心）。
//
// world_up 固定为 y 轴正向 (0,1,0)；若用户给的太阳方向近乎正上方（direction 与
// -y 几乎平行），lookAt 会退化，故把方向偏置到非正上方（世界 x 方向略偏）。
void Renderer::DrawShadowPass(const RenderCommandList& cmds) {
    if (cmds.object3d.empty()) return;

    const int cascade_count = shadow_cfg_.cascade_count;
    CHECK_GT(cascade_count, 0);
    CHECK_EQ(shadow_fbos_.size(), static_cast<size_t>(cascade_count));

    const DirectionalLight& sun = *cmds.sun;

    // 光传播方向归一化；若近乎正上方（direction 平行 -y 到接近 1），偏置到非正上方，
    // 避免 lookAt 的 cross(fwd, world_up) 退化（fwd 与 up 平行 → side=0）。
    Vec3f dir(sun.direction.x(), sun.direction.y(), sun.direction.z());
    float dlen = std::sqrt(dir.x()*dir.x() + dir.y()*dir.y() + dir.z()*dir.z());
    if (dlen < 1e-8f) { dir = Vec3f(0.0f, -1.0f, 0.0f); dlen = 1.0f; }
    dir = Vec3f(dir.x()/dlen, dir.y()/dlen, dir.z()/dlen);
    if (std::fabs(dir.y()) > 0.98f) {
        // 近乎正上方：沿 x 略偏，避免与 world_up=(0,1,0) 平行。
        dir = Vec3f(0.1f, (dir.y() < 0.0f ? -1.0f : 1.0f), dir.z());
        float nd = std::sqrt(dir.x()*dir.x() + dir.y()*dir.y() + dir.z()*dir.z());
        dir = Vec3f(dir.x()/nd, dir.y()/nd, dir.z()/nd);
    }

    const Vec3f world_up(0.0f, 1.0f, 0.0f);  // y-up 世界

    // 光源 view 矩阵：正交投影下成像只由方向决定，眼睛沿反向退固定距离。
    // eye 取相机位置沿光反向退 kLightDist（足够覆盖所有级联范围的视锥）。
    const float kLightDist = 200.0f;
    const Vec3f camera_pos = {
        cmds.camera.position.x(), cmds.camera.position.y(), cmds.camera.position.z()
    };
    Vec3f eye = camera_pos - Vec3f(dir.x(), dir.y(), dir.z()) * kLightDist;
    float view[16];
    BuildLookAt(eye, camera_pos, world_up, view);

    const unsigned int shadow_prog = ShadowProg();

    // 相机透视投影参数：fov + aspect（用于展开视锥角点）、near（首段）、
    // 各级联 far（cascade_ranges）。
    const float aspect = static_cast<float>(cmds.camera.fbo_3d_width_ > 0
            ? cmds.camera.fbo_3d_width_
            : 1280.0f)
        / static_cast<float>(cmds.camera.fbo_3d_height_ > 0
            ? cmds.camera.fbo_3d_height_
            : 720.0f);
    const float fov_rad = cmds.camera.fov * 3.14159265358979323846f / 180.0f;

    float prev_far = cmds.camera.near;
    for (int c = 0; c < cascade_count; ++c) {
        const float near_i = prev_far;
        const float far_i = shadow_cfg_.cascade_ranges[c];

        // 该级联相机视锥 8 角点：相机朝向 +fwd，frustum 从 near 平面延伸到 far 平面。
        // 用相机 lookAt 的基向量展开：
        //   fwd = normalize(target - position)
        //   side, upv 由 RendererMat4Mul 同套 BuildLookAt 逻辑推导
        Vec3f fwd = {
            cmds.camera.target.x() - cmds.camera.position.x(),
            cmds.camera.target.y() - cmds.camera.position.y(),
            cmds.camera.target.z() - cmds.camera.position.z()
        };
        float fl = std::sqrt(fwd.x()*fwd.x() + fwd.y()*fwd.y() + fwd.z()*fwd.z());
        if (fl < 1e-8f) fwd = Vec3f(0.0f, 0.0f, -1.0f); else fwd = Vec3f(fwd.x()/fl, fwd.y()/fl, fwd.z()/fl);
        const Vec3f cu = { cmds.camera.up.x(), cmds.camera.up.y(), cmds.camera.up.z() };
        Vec3f side = Vec3f(fwd.y()*cu.z() - fwd.z()*cu.y(),
                           fwd.z()*cu.x() - fwd.x()*cu.z(),
                           fwd.x()*cu.y() - fwd.y()*cu.x());
        float sl = std::sqrt(side.x()*side.x() + side.y()*side.y() + side.z()*side.z());
        if (sl < 1e-8f) side = Vec3f(1.0f, 0.0f, 0.0f); else side = Vec3f(side.x()/sl, side.y()/sl, side.z()/sl);
        Vec3f upv = Vec3f(side.y()*fwd.z() - side.z()*fwd.y(),
                          side.z()*fwd.x() - side.x()*fwd.z(),
                          side.x()*fwd.y() - side.y()*fwd.x());

        // 半高度 h = tan(fov/2) * dist；半宽 w = h * aspect。
        const float tan_half = std::tan(fov_rad * 0.5f);
        const float h_near = tan_half * near_i;
        const float h_far  = tan_half * far_i;
        const float w_near = h_near * aspect;
        const float w_far  = h_far * aspect;
        const Vec3f c_near = camera_pos + fwd * near_i;   // near 平面中心
        const Vec3f c_far  = camera_pos + fwd * far_i;    // far 平面中心

        // 8 角点（世界空间）：near 四角 + far 四角。
        Vec3f corners[8] = {
            c_near - side*w_near - upv*h_near, c_near + side*w_near - upv*h_near,
            c_near - side*w_near + upv*h_near, c_near + side*w_near + upv*h_near,
            c_far  - side*w_far  - upv*h_far,  c_far  + side*w_far  - upv*h_far,
            c_far  - side*w_far  + upv*h_far,  c_far  + side*w_far  + upv*h_far,
        };

        // 变换到光源 view 空间，求 AABB（光空间 x/y/z 范围）。
        float min_x = 1e30f, max_x = -1e30f;
        float min_y = 1e30f, max_y = -1e30f;
        float min_z = 1e30f, max_z = -1e30f;
        for (const Vec3f& p : corners) {
            const float lx = view[0]*p.x() + view[4]*p.y() + view[8]*p.z()  + view[12];
            const float ly = view[1]*p.x() + view[5]*p.y() + view[9]*p.z()  + view[13];
            const float lz = view[2]*p.x() + view[6]*p.y() + view[10]*p.z() + view[14];
            min_x = std::min(min_x, lx); max_x = std::max(max_x, lx);
            min_y = std::min(min_y, ly); max_y = std::max(max_y, ly);
            min_z = std::min(min_z, lz); max_z = std::max(max_z, lz);
        }

        // 正交投影：光照沿 -z（光 view 空间），xy 覆盖 AABB；z 覆盖 AABB。
        // 加少量边距避免边缘裁剪。
        // ⚠️ BuildOrthoProj 的 near/far 参数是沿 -z 的**正距离**（映射 z_eye=-near→-1、
        // z_eye=-far→+1），不是光空间 z 本身。光空间里物体在眼前方 z 为负，
        // 最近处（近平面）是最小负数 → near 距离 = -max_z，far 距离 = -min_z。
        const float pad = 1.0f;
        const float left = min_x - pad, right = max_x + pad;
        const float bottom = min_y - pad, top = max_y + pad;
        const float near_dist = -max_z + pad;   // 近平面距离（正）
        const float far_dist  = -min_z + pad;   // 远平面距离（正）
        if (right - left < 1e-5f || top - bottom < 1e-5f || far_dist - near_dist < 1e-5f) {
            // 退化的级联（如首段 near 极近导致视锥近似点），给最小范围。
            float vp[16]; BuildOrthoProj(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 2.0f, vp);
            RendererMat4Mul(vp, view, shadow_vp_[c]);
            continue;
        }

        float proj[16];
        BuildOrthoProj(left, right, bottom, top, near_dist, far_dist, proj);
        RendererMat4Mul(proj, view, shadow_vp_[c]);

        // 渲第 c 段：绑定对应 FBO + viewport，清屏，画所有投射物体。
        const CascadeFBO& fb = shadow_fbos_[c];
        glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
        glViewport(0, 0, fb.size, fb.size);

        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);

        // 清颜色为 1.0（ndc.z=1.0 = 最远 = 无遮挡），深度清为最远。
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        for (const auto& o : cmds.object3d) {
            Object3DRenderer::DrawObject3DShadow(o, mesh_mgr_, shader_mgr_,
                                                 shadow_vp_[c], shadow_prog);
        }

        prev_far = far_i;
    }

    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
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

// ==================== glTF 模型加载 ====================

namespace {

// 把 ORM（metallicRoughnessTexture，R=AO/G=Roughness/B=Metallic）拆成
// 3 张独立灰度 PNG 写入临时文件，并逐个加载为 GPU 纹理。
//
// 返回 {ao_id, roughness_id, metallic_id}。任何一步失败返回全 0。
// 临时文件写到 <orm_path 所在目录>/<basename>_ao.png 等，保证
// TextureManager 按绝对路径去重（同一 ORM 只拆/传一次）。
struct OrmTextureIds {
    uint32_t ao = 0;
    uint32_t roughness = 0;
    uint32_t metallic = 0;
    bool ok = false;
};

OrmTextureIds LoadOrmTextures(TextureManager& tex_mgr,
                              const std::string& orm_path) {
    OrmTextureIds out;

    int ow = 0, oh = 0, oc = 0;
    unsigned char* orm_pixels = stbi_load(orm_path.c_str(), &ow, &oh, &oc, 4);
    if (!orm_pixels) {
        LOG(ERROR) << "LoadGltf: 无法加载 ORM 贴图 " << orm_path
                   << " (" << stbi_failure_reason() << ")";
        return out;
    }

    // 临时 ORM 文件写到统一 scratch 目录，避免污染资源目录/仓库。
    // 路径按 ORM 源 basename 稳定生成，保证 TextureManager 按绝对路径
    // 去重（同一 ORM 只拆/传一次）。
    const std::string scratch_dir = "/tmp/jpov_gltf_orm/";
    std::system(("mkdir -p " + scratch_dir).c_str());
    const size_t last_slash = orm_path.find_last_of("/\\");
    const std::string base_name = (last_slash == std::string::npos)
        ? orm_path : orm_path.substr(last_slash + 1);
    const size_t dot = base_name.find_last_of('.');
    const std::string stem = (dot == std::string::npos)
        ? base_name : base_name.substr(0, dot);

    const struct { int channel; const char* suffix; } kChannels[] = {
        {0, "_ao.png"},      // R = AO
        {1, "_rough.png"},   // G = Roughness
        {2, "_metal.png"},   // B = Metallic
    };
    uint32_t ids[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        std::vector<unsigned char> png =
            ExtractChannelToPng(orm_pixels, ow, oh, kChannels[i].channel);
        if (png.empty()) {
            LOG(ERROR) << "LoadGltf: ORM 通道 " << i << " 拆包失败";
            stbi_image_free(orm_pixels);
            return out;
        }
        const std::string tmp = scratch_dir + stem + kChannels[i].suffix;
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) {
            LOG(ERROR) << "LoadGltf: 无法写临时 ORM 文件 " << tmp;
            stbi_image_free(orm_pixels);
            return out;
        }
        std::fwrite(png.data(), 1, png.size(), f);
        std::fclose(f);
        ids[i] = tex_mgr.LoadFromFile(tmp);
    }
    stbi_image_free(orm_pixels);

    out.ao = ids[0];
    out.roughness = ids[1];
    out.metallic = ids[2];
    out.ok = true;
    return out;
}

}  // namespace

GltfObject Renderer::LoadGltf(const std::string& path) {
    GltfObject obj;

    // ORM 贴图按源路径去重缓存（多 primitive 共享同一 arm 图时只拆一次）
    std::unordered_map<std::string, OrmTextureIds> orm_cache;

    // 逐 primitive 交付：注册 mesh/贴图，填充 GltfObject。
    struct CollectCtx {
        Renderer* self;
        GltfObject* obj;
        std::unordered_map<std::string, OrmTextureIds>* orm_cache;
    };

    auto collect = [](const GltfMeshEntry* entry, void* data) {
        CollectCtx* ctx = static_cast<CollectCtx*>(data);
        Renderer* self = ctx->self;
        GltfObject* obj = ctx->obj;
        std::unordered_map<std::string, OrmTextureIds>* orm_cache =
            ctx->orm_cache;

        const GltfMaterialInfo& mi = entry->material;
        GltfPrimitive prim;
        prim.mesh_id = self->mesh_mgr_.RegisterMesh(entry->mesh);

        PBRMaterial& mat = prim.material;

        // baseColor: 有纹理用纹理（白 fallback），否则用常值 baseColorFactor
        if (!mi.base_color_tex.empty()) {
            mat.base_color_tex =
                self->texture_mgr_.LoadFromFile(mi.base_color_tex);
            mat.base_color = {1.0f, 1.0f, 1.0f, 1.0f};
        } else {
            mat.base_color = {mi.base_color[0], mi.base_color[1],
                              mi.base_color[2], mi.base_color[3]};
        }

        // emissive: 常值自发光色
        mat.emissive = {mi.emissive_factor[0], mi.emissive_factor[1],
                        mi.emissive_factor[2], 1.0f};
        // normal
        if (!mi.normal_tex.empty()) {
            mat.normal_tex =
                self->texture_mgr_.LoadFromFile(mi.normal_tex);
            mat.normal_scale = mi.normal_scale;
        }
        // ORM → metallic / roughness / ao
        if (!mi.metallic_roughness_tex.empty()) {
            const std::string& orm = mi.metallic_roughness_tex;
            OrmTextureIds oid;
            auto it = orm_cache->find(orm);
            if (it != orm_cache->end()) {
                oid = it->second;
            } else {
                oid = LoadOrmTextures(self->texture_mgr_, orm);
                (*orm_cache)[orm] = oid;
            }
            if (oid.ok) {
                mat.metallic = mi.metallic_factor;
                mat.has_metallic_tex = true;
                mat.metallic_tex = oid.metallic;
                mat.roughness = mi.roughness_factor;
                mat.has_roughness_tex = true;
                mat.roughness_tex = oid.roughness;
                mat.ao_tex = oid.ao;
            } else {
                LOG(ERROR) << "LoadGltf: ORM 拆包失败，退回常值材质";
                mat.metallic = mi.metallic_factor;
                mat.roughness = mi.roughness_factor;
            }
        } else {
            mat.metallic = mi.metallic_factor;
            mat.roughness = mi.roughness_factor;
        }

        obj->primitives.push_back(std::move(prim));
    };

    CollectCtx ctx{this, &obj, &orm_cache};
    if (!jpov::LoadGltfScene(path, collect, &ctx) || obj.primitives.empty()) {
        // 失败或空：释放已注册的资源
        ReleaseGltf(obj);
        LOG(ERROR) << "Renderer::LoadGltf: 加载失败 " << path;
        return {};
    }

    LOG(INFO) << "Renderer::LoadGltf: " << path << " → "
              << obj.primitives.size() << " primitives";
    return obj;
}

void Renderer::ReleaseGltf(const GltfObject& gltf) {
    // 收集本 gltf 的所有 mesh / texture id，去重后释放。
    std::vector<uint32_t> mesh_ids;
    std::vector<uint32_t> tex_ids;

    auto push_tex = [&tex_ids](uint32_t id) {
        if (id == 0) return;
        if (std::find(tex_ids.begin(), tex_ids.end(), id) == tex_ids.end()) {
            tex_ids.push_back(id);
        }
    };

    for (const GltfPrimitive& prim : gltf.primitives) {
        if (prim.mesh_id != 0) {
            mesh_ids.push_back(prim.mesh_id);
        }
        const PBRMaterial& m = prim.material;
        push_tex(m.base_color_tex);
        push_tex(m.normal_tex);
        push_tex(m.metallic_tex);
        push_tex(m.roughness_tex);
        push_tex(m.ao_tex);
        push_tex(m.emissive_tex);
    }

    // mesh_id 去重释放（一个 mesh 只属于一个 primitive）
    for (uint32_t id : mesh_ids) {
        mesh_mgr_.ReleaseMesh(id);
    }
    for (uint32_t id : tex_ids) {
        texture_mgr_.Release(id);
    }
}

}  // namespace jpov
