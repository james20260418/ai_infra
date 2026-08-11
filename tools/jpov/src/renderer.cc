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

// ==================== MVP 矩阵构建（纯 CPU，不碰 GL 矩阵栈）====================

// 4x4 矩阵乘法：out = a * b（列主序）
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

// 列主序透视投影矩阵（对应 GLM::perspective / glFrustum 语义）
// 构建右手系透视投影：fov_y, aspect, near, far
static void BuildPerspProj(float fov_y, float aspect, float near, float far,
                            float out[16]) {
    float f = 1.0f / std::tan(fov_y * 0.5f);
    float range_inv = 1.0f / (near - far);
    // 列主序
    out[0]  = f / aspect;  out[4]  = 0.0f; out[8]  = 0.0f;                 out[12] = 0.0f;
    out[1]  = 0.0f;        out[5]  = f;     out[9]  = 0.0f;                 out[13] = 0.0f;
    out[2]  = 0.0f;        out[6]  = 0.0f;  out[10] = (near + far) * range_inv; out[14] = 2.0f * near * far * range_inv;
    out[3]  = 0.0f;        out[7]  = 0.0f;  out[11] = -1.0f;                out[15] = 0.0f;
}

// 从 Camera 构建 MVP 矩阵（纯 CPU，不碰 GL 矩阵栈，不读回 GL 状态）
// MVP = Projection * View
// Projection: 透视投影（列主序）
// View: lookAt 矩阵（列主序）
static void BuildMVP(const jpov::Camera& cam, int fbo_w, int fbo_h, float mvp[16]) {
    float aspect = static_cast<float>(fbo_w) / static_cast<float>(fbo_h);
    float fov_rad = cam.fov * 3.14159265358979323846f / 180.0f;

    // === 投影矩阵 ===
    float proj[16];
    BuildPerspProj(fov_rad, aspect, cam.near, cam.far, proj);

    // === lookAt 视图矩阵 ===
    // 计算坐标基
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

    // 列主序 lookAt 矩阵 (OpenGL 右手系)
    float view[16] = {
        side.x(), upv.x(), -fwd.x(), 0.0f,
        side.y(), upv.y(), -fwd.y(), 0.0f,
        side.z(), upv.z(), -fwd.z(), 0.0f,
        -(side.x()*cam.position.x() + side.y()*cam.position.y() + side.z()*cam.position.z()),
        -(upv.x()*cam.position.x() + upv.y()*cam.position.y() + upv.z()*cam.position.z()),
         (fwd.x()*cam.position.x() + fwd.y()*cam.position.y() + fwd.z()*cam.position.z()),
         1.0f
    };

    // MVP = Proj * View
    Mat4Mul(proj, view, mvp);
}

// 从 center/up/front 构建 Model 矩阵（列主序，纯 CPU，不碰 GL 矩阵栈）
//
// 把局部空间（mesh 定义坐标）映射到世界空间：
//   - 局部 +Y → 世界 up
//   - 局部 +Z → 世界 front
//   - 局部 +X → normalize(cross(up, front))（保证右手系）
// 再平移 center。无缩放。
//
// Model = T(center) * R(right, up, front)
// Pre-condition: up、front 均非零且不平行（调用方 DrawObject3D 已校验）
static void BuildModelMatrix(const jpov::Vec3f& center,
                             const jpov::Vec3f& up,
                             const jpov::Vec3f& front,
                             float model[16]) {
    // 归一化 up 与 front
    float u_len = std::sqrt(up.x()*up.x() + up.y()*up.y() + up.z()*up.z());
    float f_len = std::sqrt(front.x()*front.x() + front.y()*front.y() + front.z()*front.z());
    jpov::Vec3f upn = {up.x()/u_len, up.y()/u_len, up.z()/u_len};
    jpov::Vec3f frn = {front.x()/f_len, front.y()/f_len, front.z()/f_len};

    // right = normalize(cross(up, front))——保证模型右手系
    jpov::Vec3f right = {upn.y()*frn.z() - upn.z()*frn.y(),
                         upn.z()*frn.x() - upn.x()*frn.z(),
                         upn.x()*frn.y() - upn.y()*frn.x()};
    float r_len = std::sqrt(right.x()*right.x() + right.y()*right.y() + right.z()*right.z());
    right = {right.x()/r_len, right.y()/r_len, right.z()/r_len};

    // 列主序旋转部分：
    //   model[0..2] = right 列（世界 X 轴方向）
    //   model[4..6] = upn   列（世界 Y 轴方向）
    //   model[8..10]= frn   列（世界 Z 轴方向）
    // 平移在最后一列（12..14）
    model[0] = right.x(); model[4] = upn.x(); model[8]  = frn.x(); model[12] = center.x();
    model[1] = right.y(); model[5] = upn.y(); model[9]  = frn.y(); model[13] = center.y();
    model[2] = right.z(); model[6] = upn.z(); model[10] = frn.z(); model[14] = center.z();
    model[3] = 0.0f;      model[7] = 0.0f;    model[11] = 0.0f;    model[15] = 1.0f;
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
    return shader_mgr_.GetOrCreate("solid3d", {kVs3d, kFs3d});
}

unsigned int Renderer::Text3DProg() {
    return shader_mgr_.GetOrCreate("text3d", {kTexVs3d, kTexFs});
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
        BuildMVP(cmds.camera, fbo_3d_w, fbo_3d_h, mvp_);

        // ---- Tile Forward 光照准备 ----
        // 若有 Object3D 需光照（!object_use_default_color），
        // 上传光源 uniform + CPU 端做 tile culling（写入 tile index 纹理）。
        if (!cmds.object_use_default_color && !cmds.object3d.empty()) {
            Object3DRenderer::EnsureTileLighting(fbo_3d_w, fbo_3d_h,
                &tile_index_tex_, &tile_grid_w_, &tile_grid_h_,
                &tile_tex_w_, &tile_tex_h_);
            Object3DRenderer::UploadLightData(cmds, shader_mgr_,
                DrawObject3DProg(), DrawObject3DProgFull());
            Object3DRenderer::BuildTileLightIndices(cmds,
                fbo_3d_w, fbo_3d_h, tile_index_tex_,
                tile_grid_w_, tile_grid_h_, tile_tex_w_, tile_tex_h_, mvp_);
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
                font_renderer_.DrawText2D(cmds.text2d[idx],
                                          stream_vbo_, fbo_w_, fbo_h_,
                                          TextProg());
                break;
            }
            case DrawCommandType::kStrip2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.strip2d.size()));
                DrawStrip2D(cmds.strip2d[idx]);
                break;
            }
            case DrawCommandType::kRoundRect2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.roundrect2d.size()));
                DrawRoundRect2D(cmds.roundrect2d[idx]);
                break;
            }
            case DrawCommandType::kFillRect2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.fillrect2d.size()));
                DrawFillRect2D(cmds.fillrect2d[idx]);
                break;
            }
            case DrawCommandType::kArc2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.arc2d.size()));
                DrawArc2D(cmds.arc2d[idx]);
                break;
            }
            case DrawCommandType::kImage2D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.image2d.size()));
                DrawImage2D(cmds.image2d[idx]);
                break;
            }
            default:
                break;
        }
    }
}

void Renderer::Draw3DCommands(const RenderCommandList& cmds, int fbo_w, int fbo_h) {
    // 先用当前 Camera 计算 MVP（View * Proj，不含 Model）
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
                DrawLine3D(cmds.line3d[idx]);
                break;
            }
            case DrawCommandType::kText3D: {
                CHECK_GE(idx, 0);
                CHECK_LT(idx, static_cast<int>(cmds.text3d.size()));
                DrawText3D(cmds.text3d[idx]);
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

// ==================== 3D 绘制方法 ====================

void Renderer::DrawTriangle3D(const Triangle3DCommand& cmd) {
    // 3 个顶点 × xyz = 9 floats
    float verts[9] = {
        cmd.p1.x(), cmd.p1.y(), cmd.p1.z(),
        cmd.p2.x(), cmd.p2.y(), cmd.p2.z(),
        cmd.p3.x(), cmd.p3.y(), cmd.p3.z(),
    };

    unsigned int prog = Solid3DProg();
    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"),
                       1, GL_FALSE, mvp_);
    glUniform4f(glGetUniformLocation(prog, "uColor"),
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

    // 使用真正的 GL_TRIANGLE_STRIP，直接上传原始顶点序列。
    // GL_TRIANGLE_STRIP 的卷绕顺序为：三角形 i 由顶点 (i, i+1, i+2) 构成，
    // 每个三角形的卷绕方向取决于顶点索引的奇偶性——奇数三角形保持 CCW，
    // 偶数三角形自动反转卷绕以维持面朝向的一致性。
    // 因此无需 save/restore CULL_FACE。

    int total_floats = capped_n * 3;  // capped_n 个顶点 × 3 floats

    unsigned int prog = Solid3DProg();
    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"),
                       1, GL_FALSE, mvp_);
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    // 上传到专用 VBO
    glBindBuffer(GL_ARRAY_BUFFER, strip_vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(total_floats * sizeof(float)),
                 cmd.vertices.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, capped_n);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::DrawStrip2D(const Strip2DCommand& cmd) {
    int n = static_cast<int>(cmd.vertices.size());
    if (n < 3) return;

    int capped_n = (n > kMaxStrip2DVertices) ? kMaxStrip2DVertices : n;
    int total_floats = capped_n * 2;  // Vec2f = 2 floats per vertex

    unsigned int prog = SolidProg();
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(total_floats * sizeof(float)),
                 cmd.vertices.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, capped_n);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// ==================== 圆角矩形填充三角化（共享方法） ====================
//
// 将圆角矩形分成 9 个区域（4 个角 + 4 个边 + 1 个中心矩形），
// 每个 90° 圆角用 kRoundCornerSegments 个扇形三角形逼近。
// 返回的顶点数据用于 GL_TRIANGLES（非 fan）。
// radius=0 时退化返回空（调用方用 GL_TRIANGLE_FAN 处理）。
//
// 三角化拓扑（半径 r）：
//   圆角区域：以圆角内切矩形边界为锚点，计算圆弧上的点
//   边/中心区域：直接用矩形对三角形
std::vector<float> Renderer::TriangulateRoundRectFill(
    const Vec2f& pos, const Vec2f& size, float radius) {
    if (radius <= 0.0f) {
        return {};
    }

    float x0 = pos.x();
    float y0 = pos.y();
    float x1 = x0 + size.x();
    float y1 = y0 + size.y();
    float r = radius;

    // 圆角边界辅助点（内切边界坐标）
    // 左上角: (x0+r, y0+r) 右上角: (x1-r, y0+r)
    // 左下角: (x0+r, y1-r) 右下角: (x1-r, y1-r)
    float cx_inner = x0 + r;
    float cy_inner = y0 + r;
    float cx_outer = x1 - r;
    float cy_outer = y1 - r;

    int max_verts = 4 * (kRoundCornerSegments + 1) * 2  // 4 corners
                  + 4 * 6                                 // 4 edge rects (2 tris each)
                  + 6;                                    // center rect (2 tris)
    std::vector<float> verts;
    verts.reserve(max_verts);

    // ===== 中心矩形 (x0+r, y0+r) ~ (x1-r, y1-r) =====
    float center_x0 = cx_inner;
    float center_y0 = cy_inner;
    float center_x1 = cx_outer;
    float center_y1 = cy_outer;
    if (center_x1 > center_x0 && center_y1 > center_y0) {
        verts.push_back(center_x0); verts.push_back(center_y0);
        verts.push_back(center_x1); verts.push_back(center_y0);
        verts.push_back(center_x1); verts.push_back(center_y1);
        verts.push_back(center_x1); verts.push_back(center_y1);
        verts.push_back(center_x0); verts.push_back(center_y1);
        verts.push_back(center_x0); verts.push_back(center_y0);
    }

    // ===== 4 个边矩形（上、下、左、右） =====
    // 上边: (x0+r, y0) ~ (x1-r, y0+r)
    if (x1 - r > x0 + r) {
        verts.push_back(cx_inner);  verts.push_back(y0);
        verts.push_back(cx_outer);  verts.push_back(y0);
        verts.push_back(cx_outer);  verts.push_back(cy_inner);
        verts.push_back(cx_outer);  verts.push_back(cy_inner);
        verts.push_back(cx_inner);  verts.push_back(cy_inner);
        verts.push_back(cx_inner);  verts.push_back(y0);
    }
    // 下边: (x0+r, y1-r) ~ (x1-r, y1)
    if (x1 - r > x0 + r) {
        verts.push_back(cx_inner);  verts.push_back(cy_outer);
        verts.push_back(cx_outer);  verts.push_back(cy_outer);
        verts.push_back(cx_outer);  verts.push_back(y1);
        verts.push_back(cx_outer);  verts.push_back(y1);
        verts.push_back(cx_inner);  verts.push_back(y1);
        verts.push_back(cx_inner);  verts.push_back(cy_outer);
    }
    // 左边: (x0, y0+r) ~ (x0+r, y1-r)
    if (y1 - r > y0 + r) {
        verts.push_back(x0);       verts.push_back(cy_inner);
        verts.push_back(cx_inner); verts.push_back(cy_inner);
        verts.push_back(cx_inner); verts.push_back(cy_outer);
        verts.push_back(cx_inner); verts.push_back(cy_outer);
        verts.push_back(x0);       verts.push_back(cy_outer);
        verts.push_back(x0);       verts.push_back(cy_inner);
    }
    // 右边: (x1-r, y0+r) ~ (x1, y1-r)
    if (y1 - r > y0 + r) {
        verts.push_back(cx_outer); verts.push_back(cy_inner);
        verts.push_back(x1);       verts.push_back(cy_inner);
        verts.push_back(x1);       verts.push_back(cy_outer);
        verts.push_back(x1);       verts.push_back(cy_outer);
        verts.push_back(cx_outer); verts.push_back(cy_outer);
        verts.push_back(cx_outer); verts.push_back(cy_inner);
    }

    // ===== 4 个圆角区域（扇形三角形） =====
    // 左上角：圆心 (x0+r, y0+r)，从 180° 到 270°
    for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
        double a0 = 3.14159265358979323846 * (180.0 + 90.0 * seg / kRoundCornerSegments) / 180.0;
        double a1 = 3.14159265358979323846 * (180.0 + 90.0 * (seg + 1) / kRoundCornerSegments) / 180.0;
        float px0 = cx_inner + r * std::cos(a0);
        float py0 = cy_inner + r * std::sin(a0);
        float px1 = cx_inner + r * std::cos(a1);
        float py1 = cy_inner + r * std::sin(a1);
        verts.push_back(cx_inner); verts.push_back(cy_inner);
        verts.push_back(px0);      verts.push_back(py0);
        verts.push_back(px1);      verts.push_back(py1);
    }
    // 右上角：圆心 (x1-r, y0+r)，从 270° 到 360°
    for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
        double a0 = 3.14159265358979323846 * (270.0 + 90.0 * seg / kRoundCornerSegments) / 180.0;
        double a1 = 3.14159265358979323846 * (270.0 + 90.0 * (seg + 1) / kRoundCornerSegments) / 180.0;
        float px0 = cx_outer + r * std::cos(a0);
        float py0 = cy_inner + r * std::sin(a0);
        float px1 = cx_outer + r * std::cos(a1);
        float py1 = cy_inner + r * std::sin(a1);
        verts.push_back(cx_outer); verts.push_back(cy_inner);
        verts.push_back(px0);      verts.push_back(py0);
        verts.push_back(px1);      verts.push_back(py1);
    }
    // 右下角：圆心 (x1-r, y1-r)，从 0° 到 90°
    for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
        double a0 = 3.14159265358979323846 * (90.0 * seg / kRoundCornerSegments) / 180.0;
        double a1 = 3.14159265358979323846 * (90.0 * (seg + 1) / kRoundCornerSegments) / 180.0;
        float px0 = cx_outer + r * std::cos(a0);
        float py0 = cy_outer + r * std::sin(a0);
        float px1 = cx_outer + r * std::cos(a1);
        float py1 = cy_outer + r * std::sin(a1);
        verts.push_back(cx_outer); verts.push_back(cy_outer);
        verts.push_back(px0);      verts.push_back(py0);
        verts.push_back(px1);      verts.push_back(py1);
    }
    // 左下角：圆心 (x0+r, y1-r)，从 90° 到 180°
    for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
        double a0 = 3.14159265358979323846 * (90.0 + 90.0 * seg / kRoundCornerSegments) / 180.0;
        double a1 = 3.14159265358979323846 * (90.0 + 90.0 * (seg + 1) / kRoundCornerSegments) / 180.0;
        float px0 = cx_inner + r * std::cos(a0);
        float py0 = cy_outer + r * std::sin(a0);
        float px1 = cx_inner + r * std::cos(a1);
        float py1 = cy_outer + r * std::sin(a1);
        verts.push_back(cx_inner); verts.push_back(cy_outer);
        verts.push_back(px0);      verts.push_back(py0);
        verts.push_back(px1);      verts.push_back(py1);
    }

    return verts;
}

void Renderer::DrawRoundRect2D(const RoundRect2DCommand& cmd) {
    // Render a round-topped rectangle using shared CPU-side triangulation
    // via TriangulateRoundRectFill().

    if (cmd.radius <= 0.0f) {
        // Degenerate to plain rectangle via GL_TRIANGLE_FAN
        float x0 = cmd.pos.x();
        float y0 = cmd.pos.y();
        float x1 = x0 + cmd.size.x();
        float y1 = y0 + cmd.size.y();
        float verts[8] = {x0, y0, x1, y0, x1, y1, x0, y1};
        unsigned int prog = SolidProg();
        glUseProgram(prog);
        glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                    static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
        glUniform4f(glGetUniformLocation(prog, "uColor"),
                    cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);
        glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glDisableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        return;
    }

    std::vector<float> verts = TriangulateRoundRectFill(
        cmd.pos, cmd.size, cmd.radius);

    // Render
    int total_verts = static_cast<int>(verts.size()) / 2;
    unsigned int prog = SolidProg();
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
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

void Renderer::DrawFillRect2D(const FillRect2DCommand& cmd) {
    // FillRect2D 的实现策略：
    // 1. 填充部分：直接复用 TriangulateRoundRectFill() 三角化逻辑（用 fill_color）
    // 2. 边框部分：将圆角矩形边框环三角化为 GL_TRIANGLES（用 border_color）
    //
    // 边框环的几何定义：
    //   外圆角矩形 = (pos, size, radius)
    //   内圆角矩形 = (pos + border_width, size - 2*border_width, inner_radius)
    //   其中 inner_radius = max(0, radius - border_width)
    //
    // 边框环三角化：
    //   每个角区域：内外圆弧之间的扇形环
    //   每条边区域：内外矩形边之间的矩形带

    static constexpr int kMaxBorderVerts = 4 * kRoundCornerSegments * 6  // 4 corner rings, 2 tris/seg
                                          + 4 * 6;                      // 4 edge strips

    float x0 = cmd.pos.x();
    float y0 = cmd.pos.y();
    float x1 = x0 + cmd.size.x();
    float y1 = y0 + cmd.size.y();
    float r = cmd.radius;
    float bw = cmd.border_width;

    // ===== 第一步：填充部分 =====
    // 复用 TriangulateRoundRectFill() 产生三角形顶点
    unsigned int prog = SolidProg();
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.fill_color.r, cmd.fill_color.g,
                cmd.fill_color.b, cmd.fill_color.a);

    if (r <= 0.0f) {
        float verts[8] = {x0, y0, x1, y0, x1, y1, x0, y1};
        glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glDisableVertexAttribArray(0);
    } else {
        std::vector<float> fill_verts = TriangulateRoundRectFill(
            cmd.pos, cmd.size, cmd.radius);
        int total_verts = static_cast<int>(fill_verts.size()) / 2;
        glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(fill_verts.size() * sizeof(float)),
                     fill_verts.data(), GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glDrawArrays(GL_TRIANGLES, 0, total_verts);
        glDisableVertexAttribArray(0);
    }

    // ===== 第二步：边框环三角化 =====
    if (bw <= 0.0f) {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        return;
    }

    float in_bw = std::min(bw, std::min(cmd.size.x(), cmd.size.y()) * 0.5f);
    float inner_r = std::max(0.0f, r - in_bw);

    float ix0 = x0 + in_bw;
    float iy0 = y0 + in_bw;
    float ix1 = x1 - in_bw;
    float iy1 = y1 - in_bw;

    // 外圆角边界辅助点
    float cx_inner = x0 + r;
    float cy_inner = y0 + r;
    float cx_outer = x1 - r;
    float cy_outer = y1 - r;

    // 内圆角边界辅助点
    float icx_inner = ix0 + inner_r;
    float icy_inner = iy0 + inner_r;
    float icx_outer = ix1 - inner_r;
    float icy_outer = iy1 - inner_r;

    std::vector<float> border_verts;
    border_verts.reserve(kMaxBorderVerts);

    // ===== 边框边区域（矩形带） =====
    // 上边：外 (x0+r, y0) ~ (x1-r, y0) vs 内 (ix0+inner_r, iy0) ~ (ix1-inner_r, iy0)
    float outer_top_left = x0 + r;
    float outer_top_right = x1 - r;
    float inner_top_left = ix0 + inner_r;
    float inner_top_right = ix1 - inner_r;
    if (outer_top_right > outer_top_left && inner_top_right > inner_top_left) {
        border_verts.push_back(outer_top_left);  border_verts.push_back(y0);
        border_verts.push_back(outer_top_right); border_verts.push_back(y0);
        border_verts.push_back(inner_top_right); border_verts.push_back(iy0);
        border_verts.push_back(inner_top_right); border_verts.push_back(iy0);
        border_verts.push_back(inner_top_left);  border_verts.push_back(iy0);
        border_verts.push_back(outer_top_left);  border_verts.push_back(y0);
    }
    // 下边
    float outer_bot_left = x0 + r;
    float outer_bot_right = x1 - r;
    float inner_bot_left = ix0 + inner_r;
    float inner_bot_right = ix1 - inner_r;
    if (outer_bot_right > outer_bot_left && inner_bot_right > inner_bot_left) {
        border_verts.push_back(outer_bot_left);  border_verts.push_back(y1);
        border_verts.push_back(inner_bot_left);  border_verts.push_back(iy1);
        border_verts.push_back(inner_bot_right); border_verts.push_back(iy1);
        border_verts.push_back(inner_bot_right); border_verts.push_back(iy1);
        border_verts.push_back(outer_bot_right); border_verts.push_back(y1);
        border_verts.push_back(outer_bot_left);  border_verts.push_back(y1);
    }
    // 左边
    float outer_left_top = y0 + r;
    float outer_left_bot = y1 - r;
    float inner_left_top = iy0 + inner_r;
    float inner_left_bot = iy1 - inner_r;
    if (outer_left_bot > outer_left_top && inner_left_bot > inner_left_top) {
        border_verts.push_back(x0);       border_verts.push_back(outer_left_top);
        border_verts.push_back(ix0);      border_verts.push_back(inner_left_top);
        border_verts.push_back(ix0);      border_verts.push_back(inner_left_bot);
        border_verts.push_back(ix0);      border_verts.push_back(inner_left_bot);
        border_verts.push_back(x0);       border_verts.push_back(outer_left_bot);
        border_verts.push_back(x0);       border_verts.push_back(outer_left_top);
    }
    // 右边
    float outer_right_top = y0 + r;
    float outer_right_bot = y1 - r;
    float inner_right_top = iy0 + inner_r;
    float inner_right_bot = iy1 - inner_r;
    if (outer_right_bot > outer_right_top && inner_right_bot > inner_right_top) {
        border_verts.push_back(x1);       border_verts.push_back(outer_right_top);
        border_verts.push_back(x1);       border_verts.push_back(outer_right_bot);
        border_verts.push_back(ix1);      border_verts.push_back(inner_right_bot);
        border_verts.push_back(ix1);      border_verts.push_back(inner_right_bot);
        border_verts.push_back(ix1);      border_verts.push_back(inner_right_top);
        border_verts.push_back(x1);       border_verts.push_back(outer_right_top);
    }

    // ===== 边框圆角区域（扇形环） =====
    auto add_ring_segment = [&](float ocx, float ocy, float icx, float icy,
                                double start_deg, double end_deg) {
        double a0 = 3.14159265358979323846 * start_deg / 180.0;
        double a1 = 3.14159265358979323846 * end_deg / 180.0;
        float ox0 = ocx + r * std::cos(a0);
        float oy0 = ocy + r * std::sin(a0);
        float ox1 = ocx + r * std::cos(a1);
        float oy1 = ocy + r * std::sin(a1);
        float ix0_ = icx + inner_r * std::cos(a0);
        float iy0_ = icy + inner_r * std::sin(a0);
        float ix1_ = icx + inner_r * std::cos(a1);
        float iy1_ = icy + inner_r * std::sin(a1);
        border_verts.push_back(ox0);  border_verts.push_back(oy0);
        border_verts.push_back(ox1);  border_verts.push_back(oy1);
        border_verts.push_back(ix0_); border_verts.push_back(iy0_);
        border_verts.push_back(ix0_); border_verts.push_back(iy0_);
        border_verts.push_back(ox1);  border_verts.push_back(oy1);
        border_verts.push_back(ix1_); border_verts.push_back(iy1_);
    };

    if (inner_r > 0.0f) {
        // 左上角
        for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
            add_ring_segment(
                cx_inner, cy_inner, icx_inner, icy_inner,
                180.0 + 90.0 * seg / kRoundCornerSegments,
                180.0 + 90.0 * (seg + 1) / kRoundCornerSegments);
        }
        // 右上角
        for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
            add_ring_segment(
                cx_outer, cy_inner, icx_outer, icy_inner,
                270.0 + 90.0 * seg / kRoundCornerSegments,
                270.0 + 90.0 * (seg + 1) / kRoundCornerSegments);
        }
        // 右下角
        for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
            add_ring_segment(
                cx_outer, cy_outer, icx_outer, icy_outer,
                90.0 * seg / kRoundCornerSegments,
                90.0 * (seg + 1) / kRoundCornerSegments);
        }
        // 左下角
        for (int seg = 0; seg < kRoundCornerSegments; ++seg) {
            add_ring_segment(
                cx_inner, cy_outer, icx_inner, icy_outer,
                90.0 + 90.0 * seg / kRoundCornerSegments,
                90.0 + 90.0 * (seg + 1) / kRoundCornerSegments);
        }
    }

    // Render border
    if (!border_verts.empty()) {
        int total_border = static_cast<int>(border_verts.size()) / 2;
        unsigned int bprog = SolidProg();
        glUseProgram(bprog);
        glUniform2f(glGetUniformLocation(bprog, "uFboSize"),
                    static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
        glUniform4f(glGetUniformLocation(bprog, "uColor"),
                    cmd.border_color.r, cmd.border_color.g,
                    cmd.border_color.b, cmd.border_color.a);
        glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(border_verts.size() * sizeof(float)),
                     border_verts.data(), GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glDrawArrays(GL_TRIANGLES, 0, total_border);
        glDisableVertexAttribArray(0);
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::DrawArc2D(const Arc2DCommand& cmd) {
    // CPU 端三角化圆弧/扇形：
    // 扇形近似：圆心 + 圆弧上 N 个扇形三角形（GL_TRIANGLES）。
    // 跨度角度绝对值 >= 360 时绘制完整圆形。
    // 角度为负时绘制顺时针方向。

    static constexpr int kArcSegs = kArcFullCircleSegments;  // 完整圆的三角形数

    // 计算实际跨度（归一化到 360 度内，支持多圈）
    float abs_span = std::fabs(cmd.span_angle);
    if (abs_span < 1e-6f) return;  // 零跨度，不绘制

    // 如果是完整圆或超过 360 度，绘制完整圆
    if (abs_span >= 360.0f - 1e-6f) {
        // 完整圆：圆形 + N 个三角形
        int tri_count = kArcSegs;
        std::vector<float> verts;
        verts.reserve(static_cast<size_t>(tri_count) * 3 * 2);

        float cx = cmd.center.x();
        float cy = cmd.center.y();
        float r = cmd.radius;

        double start_rad = 0.0;
        double step = 2.0 * 3.14159265358979323846 / kArcSegs;
        for (int i = 0; i < kArcSegs; ++i) {
            double a0 = start_rad + i * step;
            double a1 = start_rad + (i + 1) * step;
            float px0 = cx + r * std::cos(a0);
            float py0 = cy + r * std::sin(a0);
            float px1 = cx + r * std::cos(a1);
            float py1 = cy + r * std::sin(a1);
            verts.push_back(cx);  verts.push_back(cy);
            verts.push_back(px0); verts.push_back(py0);
            verts.push_back(px1); verts.push_back(py1);
        }

        int total_verts = static_cast<int>(verts.size()) / 2;
        unsigned int prog = SolidProg();
        glUseProgram(prog);
        glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                    static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
        glUniform4f(glGetUniformLocation(prog, "uColor"),
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
        return;
    }

    // 非完整圆：扇形近似
    // 三角形数 = ceil(abs_span / 360 * kArcSegs)，至少 3
    float ratio = abs_span / 360.0f;
    int tri_count = std::max(3, static_cast<int>(kArcSegs * ratio + 0.5f));

    float cx = cmd.center.x();
    float cy = cmd.center.y();
    float r = cmd.radius;

    double start_rad = 3.14159265358979323846 * cmd.start_angle / 180.0;
    double span_rad = 3.14159265358979323846 * cmd.span_angle / 180.0;
    double step = span_rad / tri_count;

    std::vector<float> verts;
    verts.reserve(static_cast<size_t>(tri_count) * 3 * 2);

    for (int i = 0; i < tri_count; ++i) {
        double a0 = start_rad + i * step;
        double a1 = start_rad + (i + 1) * step;
        float px0 = cx + r * std::cos(a0);
        float py0 = cy + r * std::sin(a0);
        float px1 = cx + r * std::cos(a1);
        float py1 = cy + r * std::sin(a1);
        verts.push_back(cx);  verts.push_back(cy);
        verts.push_back(px0); verts.push_back(py0);
        verts.push_back(px1); verts.push_back(py1);
    }

    int total_verts = static_cast<int>(verts.size()) / 2;
    unsigned int prog = SolidProg();
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
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

void Renderer::DrawLine3D(const Line3DCommand& cmd) {
    float verts[6] = {
        cmd.p1.x(), cmd.p1.y(), cmd.p1.z(),
        cmd.p2.x(), cmd.p2.y(), cmd.p2.z(),
    };

    unsigned int prog = Solid3DProg();
    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"),
                       1, GL_FALSE, mvp_);
    glUniform4f(glGetUniformLocation(prog, "uColor"),
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
    LOG_FIRST_N(WARNING, 1) << "DrawText3D: not yet implemented, skipping";
}

// 上传全部点光源数据到 lighting shader 的 flat uniform 数组。
//
// 每帧在 BuildTileLightIndices 之前调用一次（非逐 object 热路径，可接受
// 每光源 3 次 snprintf + glGetUniformLocation）。ShaderManager 会缓存
// uniform location，避免重复字符串查找。
//
// 只上传前 kMaxTotalLights_ 个光源；超限提示由 BuildTileLightIndices 负责
//（这是唯一 warning 触发点）。
//
// Pre-condition: MeshLighting3DProg() 已注册（首次调用会触发编译）。

// CPU 端 tile culling：
//   遍历 cmds.point_lights（用户已按优先级排好序），把每个光源投影到
//   NDC → 屏幕空间覆盖矩形 → 转换成 tile 坐标范围 → 向每个覆盖 tile 写入
//   light index（先到先得，每 tile 最多 kMaxLightsPerTile_ 个，后到的丢弃）。
//
// 输出到 tile_index_tex_（GL_RGBA8），fragment shader 按 gl_FragCoord 查表。
//
// 覆盖是保守近似：投影光源球的 6 个轴向边界点取屏幕 x/y 的 min/max，保证
// 装下球体；光源 radius（线性衰减边界）跨越相机时投影会失真，此时让该光源
// 覆盖整个屏幕，确保不漏光（宁多勿少）。这是近似而非精确锥体裁剪。
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

    unsigned int prog = SolidProg();
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
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

    unsigned int prog = SolidProg();
    glUseProgram(prog);
    // uFboSize = NDC 变换参照。必须用 FBO 尺寸（渲染分辨率），
    // 使 NDC 坐标空间与 glViewport 一致，避免 rect 偏移。
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
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
    static constexpr int kSegments = kCircleFanSegments;
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

    unsigned int prog = SolidProg();
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(prog, "uColor"),
                cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLE_FAN, 0, kSegments + 2);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::DrawImage2D(const Image2DCommand& cmd) {
    // 查找纹理
    int tex_w = 0;
    int tex_h = 0;
    bool found = texture_mgr_.GetSize(cmd.texture_id, &tex_w, &tex_h);
    CHECK(found) << "DrawImage2D: texture_id=" << cmd.texture_id
                 << " not registered";
    CHECK_GT(tex_w, 0);
    CHECK_GT(tex_h, 0);

    uint32_t gl_tex = texture_mgr_.GetGLTexture(cmd.texture_id);
    CHECK_NE(gl_tex, 0u) << "DrawImage2D: texture_id=" << cmd.texture_id
                          << " has no GL texture";

    // 构建矩形面片：pos → pos+size，UV (0,0)→(1,1)
    // 顶点格式：[x, y, u, v] interleaved，6 顶点（2 三角形）
    float x0 = cmd.pos.x();
    float y0 = cmd.pos.y();
    float x1 = x0 + cmd.size.x();
    float y1 = y0 + cmd.size.y();

    // T1: (x0,y0)-(x1,y0)-(x1,y1), T2: (x0,y0)-(x1,y1)-(x0,y1)
    // 屏幕坐标 y0=上 y1=下。shader 设 ndc.y = -ndc.y 使 y0→NDC 上方。
    // stb_image 像素第一行=图片顶部, glTexImage2D 把它放在 v=0 (GL 纹理底部),
    // 因此图片顶部对应 v=0, 图片底部对应 v=1。
    // 要把图片顶部映射到屏幕上方 (y0), 底部映射到屏幕下方 (y1):
    //   y0 → v=0, y1 → v=1
    float verts[24] = {
        x0, y0, 0.0f, 0.0f,  // T1: top-left
        x1, y0, 1.0f, 0.0f,  // T1: top-right
        x1, y1, 1.0f, 1.0f,  // T1: bottom-right
        x0, y0, 0.0f, 0.0f,  // T2: top-left
        x1, y1, 1.0f, 1.0f,  // T2: bottom-right
        x0, y1, 0.0f, 1.0f,  // T2: bottom-left
    };

    unsigned int prog = ImageProg();
    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uFboSize"),
                static_cast<float>(fbo_w_), static_cast<float>(fbo_h_));
    glUniform4f(glGetUniformLocation(prog, "uTint"),
                cmd.tint.r, cmd.tint.g, cmd.tint.b, cmd.tint.a);
    glUniform1i(glGetUniformLocation(prog, "uTexture"), 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl_tex);

    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);

    constexpr int kStride = 4 * static_cast<int>(sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, kStride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kStride,
                          (void*)(2 * sizeof(float)));

    glDrawArrays(GL_TRIANGLES, 0, 6);

    GLenum draw_err = glGetError();
    if (draw_err != GL_NO_ERROR) {
        LOG_FIRST_N(WARNING, 1) << "GL error after DrawImage2D: " << draw_err;
    }

    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace jpov
