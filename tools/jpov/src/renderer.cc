// JPOV Renderer 实现
// FBO 动态调整，坐标以窗口坐标为空间。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/renderer.h"

#include <algorithm>
#include <array>
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

// 统一后处理 tone map：全屏三角形 vertex shader（覆盖 NDC）。
// 消费 HDR FBO 的浮点纹理，做 ACES filmic tone map 压缩到 LDR。
const char* kTonemapVs = R"glsl(
#version 330 core
out vec2 vTexCoord;
void main() {
    vec2 pos;
    if (gl_VertexID == 0) pos = vec2(-1.0, -1.0);
    else if (gl_VertexID == 1) pos = vec2( 3.0, -1.0);
    else pos = vec2(-1.0,  3.0);
    vTexCoord = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
)glsl";

// ACES filmic tone map（Narkowicz 拟合）—— luminance-only（色度保持）版本。
//
// 背景：完整版 ACES（含 ACES_IN/ACES_OUT 色彩矩阵）会把颜色往广色域走再映射回来，
// 这一来一回是“有损近似”，会在暗部/低饱和区引入系统性色偏（尤其暗部发绿）。
// JPOV 要的是“只压缩亮度、不偏移色调”的鲁棒 tone map，故采用 luminance-only：
//   1. 先求线性亮度 lum = dot(color, Rec.709 权重)；
//   2. 只对 lum 套 ACES filmic 曲线（RRTAndODTFit）→ 高光柔和滚落；
//   3. 颜色按 (tone/lum) 比例缩回 —— 色向/饱和度 100% 保持，只改明暗。
// 这样 night 的极暗部不会被翻成绿色，sunset 的红霞物体也不会被染色。
const char* kTonemapFs = R"glsl(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uHdrTexture;

// ACES filmic 曲线（Narkowicz / Stephen Hill RRTAndODTFit 拟合）。
// 对标量亮度工作，输出 [0,1] 附近，高光有柔和 S 型肩。
float aces_curve(float x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 aces_tonemap(vec3 color) {
    // 线性亮度（Rec.709 绿色权重 > 蓝），用于只压亮度不动色相。
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float tone = aces_curve(lum);
    // 按比例缩回：色相/饱和度不变，仅整体明暗被 ACES 曲线压缩。
    // lum≈0 时避免除零（直接返回黑）。
    return color * (tone / max(lum, 1e-5));
}

void main() {
    vec3 hdr = texture(uHdrTexture, vTexCoord).rgb;
    vec3 ldr = aces_tonemap(hdr);
    FragColor = vec4(ldr, 1.0);
}
)glsl";

// 拾取（color-ID）pass 的 vertex shader：与 Object3D PBR 的顶点语义一致
//（aPos loc=0 + aNormal loc=1），只需把物体摆到正确世界位置（MVP×Model）。
const char* kPickVs = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
uniform mat4 uMVP;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)glsl";

// 拾取（color-ID）pass 的 fragment shader：把 picking_id 编码成 RGB 三字节。
//   r = id & 0xFF；g = (id>>8) & 0xFF；b = (id>>16) & 0xFF
// fragment 写回 RGBA8，由 CPU glReadPixels 解码回整数 id。
// 深度测试在拾取 pass 照常开启，resolve 最前面的物体（前景优先）。
const char* kPickFs = R"glsl(
#version 330 core
out vec4 FragColor;
uniform int uPickingId;
void main() {
    int id = uPickingId;
    int r = id & 0xFF;
    int g = (id >> 8) & 0xFF;
    int b = (id >> 16) & 0xFF;
    FragColor = vec4(float(r) / 255.0, float(g) / 255.0, float(b) / 255.0, 1.0);
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
    DestroyHDRFBO();
    DestroyHDRResolveFBO();
    DestroyShadowFBO();
    DestroyHighlightFBO();
    if (tile_index_tex_) { glDeleteTextures(1, &tile_index_tex_); tile_index_tex_ = 0; }
    if (pick_fbo_) {
        glDeleteFramebuffers(1, &pick_fbo_);
        glDeleteTextures(1, &pick_tex_);
        glDeleteRenderbuffers(1, &pick_depth_rb_);
        pick_fbo_ = 0; pick_tex_ = 0; pick_depth_rb_ = 0;
    }
    // 注意：shader program 由 ShaderManager::~ShaderManager() 统一释放
    if (stream_vbo_)   glDeleteBuffers(1, &stream_vbo_);
    if (stream_vao_)   glDeleteVertexArrays(1, &stream_vao_);
    if (strip_vbo_)    glDeleteBuffers(1, &strip_vbo_);
    // font atlas 纹理由 FontRenderer 析构管理
}

float Renderer::MeasureTextWidth(const std::string& alias,
                                  const std::string& text,
                                  float font_size) {
    // 委托 FontRenderer：按 alias 查找字体并测量（空别名 → 首个字体）。
    return font_renderer_.MeasureTextWidth(alias, text, font_size);
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

void Renderer::DestroyHDRFBO() {
    if (fbo_hdr_) {
        glDeleteFramebuffers(1, &fbo_hdr_);
        glDeleteTextures(1, &color_tex_hdr_);
#ifndef JPOV_WITHOUT_MSAA
        if (depth_rb_hdr_) {
            glDeleteRenderbuffers(1, &depth_rb_hdr_);
        }
#else
        if (depth_tex_hdr_) {
            glDeleteTextures(1, &depth_tex_hdr_);
        }
#endif
        fbo_hdr_ = 0;
        color_tex_hdr_ = 0;
        depth_rb_hdr_ = 0;
        depth_tex_hdr_ = 0;
    }
    fbo_hdr_w_ = 0;
    fbo_hdr_h_ = 0;
}

void Renderer::DestroyHDRResolveFBO() {
    if (resolve_fbo_hdr_) {
        glDeleteFramebuffers(1, &resolve_fbo_hdr_);
        glDeleteTextures(1, &resolve_tex_hdr_);
        resolve_fbo_hdr_ = 0;
        resolve_tex_hdr_ = 0;
    }
    resolve_fbo_hdr_w_ = 0;
    resolve_fbo_hdr_h_ = 0;
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

// HDR 3D FBO（RGBA16F 颜色 + depth）：3D 内容（天空 + object3d + primitives3d）的
// 统一渲染目标。与 RGBA8 LDR FBO 分开 —— RGBA8 阶段不承载 HDR（>1 会 clamp），
// 而 HDR FBO 用 16F 浮点颜色存下 >1 的亮度，交给后处理 tone map pass 压缩。
// 深度在本 FBO 内置（与 LDR FBO 的“共享一张深度”语义预留：后续 primitives3d 转
// LDR 时，可把本 FBO 用完的 depth 直接 attach 给 LDR FBO 复用，见规划）。
void Renderer::EnsureHDRFBO(int width, int height) {
    if (fbo_hdr_w_ == width && fbo_hdr_h_ == height && fbo_hdr_) return;

    CHECK_GT(width, 0);
    CHECK_GT(height, 0);
    CHECK_LE(width, kMaxFboDim);
    CHECK_LE(height, kMaxFboDim);

    DestroyHDRFBO();
    DestroyHDRResolveFBO();

#ifdef JPOV_WITHOUT_MSAA
    // 非 MSAA 路径（Windows/MinGW）：普通 2D 浮点纹理 + 深度纹理
    glGenTextures(1, &color_tex_hdr_);
    glBindTexture(GL_TEXTURE_2D, color_tex_hdr_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0,
                 GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenTextures(1, &depth_tex_hdr_);
    glBindTexture(GL_TEXTURE_2D, depth_tex_hdr_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0,
                 GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &fbo_hdr_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_hdr_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, color_tex_hdr_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, depth_tex_hdr_, 0);

    // 注意：HDR FBO 不再带 stencil。高亮（方法 B）由独立的 DrawHighlightPass
    // 在单采样 hl FBO（含组合 depth+stencil）上统一执行（见 DrawHighlightPass），
    // 因此这里不再为 fbo_hdr_ 附加 stencil renderbuffer。

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    CHECK_EQ(status, GL_FRAMEBUFFER_COMPLETE)
        << "HDR FBO (non-MSAA) failed, status=" << status;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    resolve_fbo_hdr_ = 0;
    resolve_tex_hdr_ = 0;
    resolve_fbo_hdr_w_ = 0;
    resolve_fbo_hdr_h_ = 0;
#else
    // === 4x MSAA 浮点颜色纹理（RGBA16F） ===
    // 注意：浮点 MSAA renderbuffer 在某些软渲染（llvmpipe）下可能不支持；
    // 若 gold 测试在 headless 下 resolve 异常，需回退非 MSAA 路径（见 PR 讨论）。
    glGenTextures(1, &color_tex_hdr_);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, color_tex_hdr_);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA16F,
                            width, height, GL_TRUE);
    glTexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);

    glGenRenderbuffers(1, &depth_rb_hdr_);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rb_hdr_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_DEPTH_COMPONENT24,
                                     width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glGenFramebuffers(1, &fbo_hdr_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_hdr_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D_MULTISAMPLE, color_tex_hdr_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, depth_rb_hdr_);
    // 注意：不在 MSAA HDR FBO 上附加 stencil —— MSAA stencil renderbuffer 在
    //   llvmpipe（headless 软渲染）下导致 FBO 不完整（GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT）。
    //   高亮描边在 MSAA 路径走另一条路：resolve 到非 MSAA resolve FBO 后，
    //   在 resolve FBO 上做单采样 stencil（见 DrawHighlightPass）。

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    CHECK_EQ(status, GL_FRAMEBUFFER_COMPLETE)
        << "HDR MSAA FBO failed, status=" << status;

    // === 中间 non-MSAA resolve FBO（RGBA16F，同尺寸） ===
    glGenTextures(1, &resolve_tex_hdr_);
    glBindTexture(GL_TEXTURE_2D, resolve_tex_hdr_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0,
                 GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &resolve_fbo_hdr_);
    glBindFramebuffer(GL_FRAMEBUFFER, resolve_fbo_hdr_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, resolve_tex_hdr_, 0);

    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    CHECK_EQ(status, GL_FRAMEBUFFER_COMPLETE)
        << "HDR resolve FBO failed, status=" << status;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    resolve_fbo_hdr_w_ = width;
    resolve_fbo_hdr_h_ = height;
#endif

    fbo_hdr_w_ = width;
    fbo_hdr_h_ = height;
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

// 统一后处理 tone map shader（ACES filmic，见 kTonemapVs/kTonemapFs）。
unsigned int Renderer::TonemapProg() {
    return shader_mgr_.GetOrCreate("tonemap", {kTonemapVs, kTonemapFs});
}

// 拾取（color-ID）pass shader（见 kPickVs/kPickFs）。
unsigned int Renderer::PickProg() {
    return shader_mgr_.GetOrCreate("pick", {kPickVs, kPickFs});
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
    TonemapProg();
    PickProg();
}

void Renderer::CreateStreamVBO() {
    size_t buf = static_cast<size_t>(kMaxStreamVertices) * 2 * sizeof(float);
    glGenBuffers(1, &stream_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glBufferData(GL_ARRAY_BUFFER, buf, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // 流式绘制 VAO：core profile 下顶点属性需绑定 VAO 才合法
    //（GL_POINTS 高亮回填等用）。
    glGenVertexArrays(1, &stream_vao_);
    glBindVertexArray(stream_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glBindVertexArray(0);
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

        // ---- 太阳平行光与环境光：直接取用户手配值，不从 DaySkyCommand 推导 -------
        // （2026-08-19 回归最小可行：DaySkyCommand 只定义“天色”，不再 Derive 方向光/
        // 环境光。eff_sun=用户配的 cmds.sun（可为空）；eff_ambient=用户配的
        // cmds.ambient，未配则用默认值）
        const std::optional<DirectionalLight> eff_sun = cmds.sun;
        std::optional<AmbientLight> eff_ambient = cmds.ambient;

        // ---- 第 0 步：太阳阴影 pass（有 sun 时）----
        // 先渲染场景深度到阴影纹理，主 pass 的 PBR shader 才能采样做影子。
        // DrawShadowPass 内部会绑定 shadow FBO 并改变 GL 状态，放在 Ensure3DFBO/
        // EnsureHDRFBO 之前：后续 Ensure+FBO + bind + clear 会恢复正常 3D 状态。
        if (eff_sun.has_value()) {
            EnsureShadowFBO(shadow_cfg_);
            DrawShadowPass(cmds, *eff_sun);
        }

        // ---- 第一步：选渲染目标 FBO 并绘制所有 3D 指令 ----
        // tone_mapping=false：走原 RGBA8 3D FBO（旧行为，字节兼容，零回归）。
        // tone_mapping=true ：走 HDR FBO（RGBA16F 浮点），存下 >1 的 HDR 亮度，
        //                     后续由统一 tone map pass 压缩到 LDR。
        const bool use_hdr = cmds.tone_mapping;
        unsigned int fbo_3d_target = 0;
        if (use_hdr) {
            EnsureHDRFBO(fbo_3d_w, fbo_3d_h);
            fbo_3d_target = fbo_hdr_;
        } else {
            Ensure3DFBO(fbo_3d_w, fbo_3d_h);
            fbo_3d_target = fbo_3d_;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_3d_target);
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

        // ---- 第 0.5 步：天光背景（有 sky 指令时）----
        // 先画程序化天光垫底（不写深度、不参与深度测试），3D 物体随后用深度覆盖。
        // 独立 shader program（sky），一帧一次 draw，与 object3d PBR 完全分开。
        // 纯 Preetham 程序化天空 + 日月圆盘，不依赖 HDRI/纹理。
        if (cmds.sky.has_value()) {
            // 天空垫底：关深度测试（背景永远最远），关面剔除（全屏三角形）
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);

            SkyRenderer::DrawSky(*cmds.sky, cmds.camera, fbo_3d_w, fbo_3d_h,
                                 shader_mgr_);

            // 恢复 3D 物体所需的深度状态
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_TRUE);
            glDepthFunc(GL_LESS);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glFrontFace(GL_CCW);
        }

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

        // 太阳平行光 + 级联阴影贴图 uniform（总是上传，无 sun 时置 uHasSun=0）。
        // 绑各级联 shadow 深度纹理到 PBR shader，供直射光的 PCF 阴影采样。
        if (!cmds.object3d.empty()) {
            Object3DRenderer::UploadSunData(shader_mgr_,
                DrawObject3DProg(), DrawObject3DProgFull(),
                shadow_fbos_, shadow_vp_, shadow_cfg_, eff_sun);
        }

        // 全局环境光 uniform（总是上传，未设置则用默认值）。
        // 环境光无方向、无影子，与 sun/点光源并列，照亮物体背阳面。
        if (!cmds.object3d.empty()) {
            // 未显式配 ambient 时，用默认值（中性灰白 × 0.4，= 旧的硬编码 AMBIENT）。
            const AmbientLight ambient = eff_ambient.value_or(AmbientLight{});
            Object3DRenderer::UploadAmbient(shader_mgr_,
                DrawObject3DProg(), DrawObject3DProgFull(), ambient);
        }

        // 拾取（color-ID）pass：仅在用户发起了 pick 查询时才跑。
        // 会切换到自己 FBO；跑完必须恢复主 3D FBO + 深度/剔除状态供 Draw3DCommands。
        if (cmds.pick.enabled) {
            // 传生效的 viewport（cam 已被 Render() 回退为全窗口）。
            DrawPickingPass(cmds, fbo_3d_w, fbo_3d_h,
                            cam.viewport_x, cam.viewport_y,
                            cam.viewport_width, cam.viewport_height);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo_3d_target);
            glViewport(0, 0, fbo_3d_w, fbo_3d_h);
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_TRUE);
            glDepthFunc(GL_LESS);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glFrontFace(GL_CCW);
            glEnable(GL_BLEND);
        }

        // 用 3D FBO 尺寸计算 MVP
        Draw3DCommands(cmds, fbo_3d_w, fbo_3d_h);

        // ---- 第二步：MSAA resolve + 按 flag 决定 tone map 或直接 blit 到主 FBO ----
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);

        if (use_hdr) {
            // ---- HDR 路径：先把 HDR 内容 resolve/blit 到一张同尺寸 non-MSAA 浮点
            //      纹理（resolve_tex_hdr_），作为 tone map pass 的输入采样纹理。
#ifdef JPOV_WITHOUT_MSAA
            // 非 MSAA 路径：3D FBO 本身即单采样浮点纹理，直接作为 tone map 输入。
            unsigned int hdr_input_tex = color_tex_hdr_;
#else
            // MSAA 路径：先把 MSAA HDR FBO resolve 到单采样浮点纹理。
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_hdr_);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo_hdr_);
            glBlitFramebuffer(
                0, 0, fbo_3d_w, fbo_3d_h,
                0, 0, resolve_fbo_hdr_w_, resolve_fbo_hdr_h_,
                GL_COLOR_BUFFER_BIT, GL_LINEAR);

            unsigned int hdr_input_tex = resolve_tex_hdr_;
#endif

            // ── 高亮 pass（方法 B stencil）—— 3D 内容全部画完后统一叠加。
            // 高亮作为 3D 渲染管线里的一个独立子步骤（与 shadow / tone map 并列）：
            // 无论 MSAA 有无，都从 fbo_hdr_（MSAA 时自动 resolve）blit color+depth
            // 到单采样 hl FBO，在其上做“写 stencil=1 → 放大副本描边”两步式，
            // 输出叠加了高亮的颜色纹理，作为 tone map 的唯一输入。
            // 有高亮 → 用 hl_color_tex_；无高亮 → 用 resolve/color 原始纹理（零开销）。
            if (cmds.highlight_style.has_value()) {
                hdr_input_tex = DrawHighlightPass(cmds,
                                                  fbo_3d_w, fbo_3d_h);
            }

            // ---- 统一 tone map pass：ACES filmic 把 HDR 压缩到 LDR 主 FBO ----
            unsigned int prog = TonemapProg();
            glUseProgram(prog);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, hdr_input_tex);
            glUniform1i(shader_mgr_.GetUniform(prog, "uHdrTexture"), 0);

            glViewport(
                static_cast<int>(cam.viewport_x),
                static_cast<int>(cam.viewport_y),
                static_cast<int>(cam.viewport_width),
                static_cast<int>(cam.viewport_height));
            glBindFramebuffer(GL_FRAMEBUFFER, fbo_);

            // 全屏三角形（无 VAO/VBO，用 gl_VertexID）
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glUseProgram(0);
            glBindTexture(GL_TEXTURE_2D, 0);
        } else {
            // ---- LDR 路径（旧行为，字节兼容） ----
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
        }

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
                const Object3DCommand& obj = cmds.object3d[idx];

                // 3D 内容绘制保持纯净：高亮（方法 B stencil）由独立的
                // DrawHighlightPass 在 3D 内容全部画完后统一叠加（见 Render()）。
                // 此处不掺任何 stencil 状态，DrawObject3D 只负责本体着色。
                Object3DRenderer::DrawObject3D(obj, cmds,
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
void Renderer::DrawShadowPass(const RenderCommandList& cmds, const DirectionalLight& sun) {
    if (cmds.object3d.empty()) return;

    const int cascade_count = shadow_cfg_.cascade_count;
    CHECK_GT(cascade_count, 0);
    CHECK_EQ(shadow_fbos_.size(), static_cast<size_t>(cascade_count));

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

    // 预计算每个 Object3D 的光空间 AABB（用其 CPU 包围盒 bounds_min/max + 摆放变换）。
    // 目标：让每级联 shadow 覆盖能包含“可能往该区域投影”的所有物体，即使其相机
    // 视锥之外 —— 否则物体在视锥外就不产生阴影（原实现的致命缺陷）。
    // 结构：[min_x,max_x,min_y,max_y,min_z,max_z] 光空间 AABB。
    std::vector<std::array<float, 6>> obj_light_aabb;
    obj_light_aabb.reserve(cmds.object3d.size());
    for (const auto& o : cmds.object3d) {
        const GPUMesh* mesh = mesh_mgr_.GetMesh(o.mesh_id);
        if (!mesh) continue;
        // 本物体世界 AABB：把局部 bounds_min/max 8 角点经（center,up,front）变换到世界。
        const Vec3f center{o.center.x(), o.center.y(), o.center.z()};
        const Vec3f up{o.up.x(), o.up.y(), o.up.z()};
        const Vec3f fr{o.front.x(), o.front.y(), o.front.z()};
        float u_len = std::sqrt(up.x()*up.x()+up.y()*up.y()+up.z()*up.z());
        float f_len = std::sqrt(fr.x()*fr.x()+fr.y()*fr.y()+fr.z()*fr.z());
        if (u_len < 1e-8f || f_len < 1e-8f) continue;
        const Vec3f un = up * (1.0f/u_len);
        const Vec3f fn = fr * (1.0f/f_len);
        // 局部坐标轴：+X=left=cross(up,front)，+Y=up，+Z=front（与 BuildModelMatrix 一致）。
        Vec3f left{un.y()*fn.z()-un.z()*fn.y(), un.z()*fn.x()-un.x()*fn.z(),
                   un.x()*fn.y()-un.y()*fn.x()};
        float l_len = std::sqrt(left.x()*left.x()+left.y()*left.y()+left.z()*left.z());
        if (l_len < 1e-8f) continue;
        left = left * (1.0f/l_len);
        // 局部 AABB 8 角点 → 光空间，累计本物体光空间 AABB。
        std::array<float,6> b{1e30f,-1e30f,1e30f,-1e30f,1e30f,-1e30f};
        for (int ix=0; ix<=1; ++ix) {
          const float lx = (ix==0) ? mesh->bounds_min[0] : mesh->bounds_max[0];
          for (int iy=0; iy<=1; ++iy) {
            const float ly = (iy==0) ? mesh->bounds_min[1] : mesh->bounds_max[1];
            for (int iz=0; iz<=1; ++iz) {
              const float lz = (iz==0) ? mesh->bounds_min[2] : mesh->bounds_max[2];
              // 局部 → 世界（center + 轴缩放）：world = center + left*lx + up*ly + fn*lz
              const Vec3f wp = center + left*lx + un*ly + fn*lz;
              const float lx2 = view[0]*wp.x()+view[4]*wp.y()+view[8]*wp.z()+view[12];
              const float ly2 = view[1]*wp.x()+view[5]*wp.y()+view[9]*wp.z()+view[13];
              const float lz2 = view[2]*wp.x()+view[6]*wp.y()+view[10]*wp.z()+view[14];
              b[0]=std::min(b[0],lx2); b[1]=std::max(b[1],lx2);
              b[2]=std::min(b[2],ly2); b[3]=std::max(b[3],ly2);
              b[4]=std::min(b[4],lz2); b[5]=std::max(b[5],lz2);
            }
          }
        }
        if (b[1] > -1e30f) {  // 有效（有顶点）
            obj_light_aabb.push_back(b);
        }
    }

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
        // 并把场景所有物体的光空间 AABB 并入 —— 保证该级联 shadow map 一定包含
        // 所有可能投到该区域的物体（即使其在相机视锥外）。这是“相机不对着墙也有
        // 墙影”的关键修复（原实现只 fit 相机视锥切片，视锥外物体被正交裁剪出不了影）。
        float min_x = 1e30f, max_x = -1e30f;
        float min_y = 1e30f, max_y = -1e30f;
        float min_z = 1e30f, max_z = -1e30f;
        const auto acc = [&](float x, float y, float z) {
            min_x = std::min(min_x, x); max_x = std::max(max_x, x);
            min_y = std::min(min_y, y); max_y = std::max(max_y, y);
            min_z = std::min(min_z, z); max_z = std::max(max_z, z);
        };
        for (const Vec3f& p : corners) {
            const float lx = view[0]*p.x() + view[4]*p.y() + view[8]*p.z()  + view[12];
            const float ly = view[1]*p.x() + view[5]*p.y() + view[9]*p.z()  + view[13];
            const float lz = view[2]*p.x() + view[6]*p.y() + view[10]*p.z() + view[14];
            acc(lx, ly, lz);
        }
        // 并入“光程与该级联相机切片有重叠”的物体（按光空间 z 筛选）。
        // 只并入重叠物体：远处物体不撑大近级联覆盖，保持近密远疏的精度分布。
        // 重叠容差 = 该级联切片 z 跨度的一半（把光路径上的投影体都纳入）；
        // 物体若与切片 z 有交集（或落在容差内）都可能投影到这片区域 → 并入。
        if (obj_light_aabb.size() > 0u) {
            const float slice_max_z = max_z;
            const float slice_min_z = min_z;
            const float z_tol = 0.5f * (slice_max_z - slice_min_z) + 10.0f;
            for (int k = 0; k < (int)obj_light_aabb.size(); ++k) {
                const auto& b = obj_light_aabb[k];
                // 物体光空间 z [b[4],b[5]] 与切片 [min_tol,max_tol] 若有重叠则并入。
                if (b[5] >= slice_min_z - z_tol && b[4] <= slice_max_z + z_tol) {
                    acc(b[0], b[2], b[4]);
                    acc(b[1], b[3], b[5]);
                }
            }
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

// 拾取（color-ID）pass。cmds.pick.enabled 时调用：把所有 picking_id>0 的物体
// 用纯色 ID shader 画进一个自管理的小 RGBA8 FBO（带 depth，resolve 前景），
// 读回光标像素解码成 picking_id → last_pick_。
void Renderer::DrawPickingPass(const RenderCommandList& cmds, int fbo_w, int fbo_h,
                               float vp_x, float vp_y, float vp_w, float vp_h) {
    // 没有任何可拾取物体 → 直接判未命中，不建 FBO 不画（零成本）。
    bool has_pickable = false;
    for (const auto& o : cmds.object3d) {
        if (o.picking_id > 0) { has_pickable = true; break; }
    }
    if (!has_pickable) {
        last_pick_.hit = false;
        last_pick_.picking_id = 0;
        return;
    }

    // ---- 自管理 pick FBO（RGBA8 颜色 + depth renderbuffer），尺寸 = 3D FBO ----
    if (pick_fbo_w_ != fbo_w || pick_fbo_h_ != fbo_h || pick_fbo_ == 0) {
        if (pick_fbo_) {
            glDeleteFramebuffers(1, &pick_fbo_);
            glDeleteTextures(1, &pick_tex_);
            glDeleteRenderbuffers(1, &pick_depth_rb_);
        }
        glGenTextures(1, &pick_tex_);
        glBindTexture(GL_TEXTURE_2D, pick_tex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fbo_w, fbo_h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);

        glGenRenderbuffers(1, &pick_depth_rb_);
        glBindRenderbuffer(GL_RENDERBUFFER, pick_depth_rb_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, fbo_w, fbo_h);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        glGenFramebuffers(1, &pick_fbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, pick_fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, pick_tex_, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, pick_depth_rb_);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        CHECK_EQ(status, GL_FRAMEBUFFER_COMPLETE) << "pick FBO failed, status=" << status;
        pick_fbo_w_ = fbo_w;
        pick_fbo_h_ = fbo_h;
    }

    // ---- 画 color-ID pass ----
    glBindFramebuffer(GL_FRAMEBUFFER, pick_fbo_);
    glViewport(0, 0, fbo_w, fbo_h);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glDisable(GL_BLEND);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);   // 黑 = id 0 = 背景/未命中
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    float mvp[16];
    Primitives3DRenderer::BuildMVP(cmds.camera, fbo_w, fbo_h, mvp);
    const unsigned int pick_prog = PickProg();
    glUseProgram(pick_prog);

    for (const auto& o : cmds.object3d) {
        if (o.picking_id == 0) continue;   // 不可拾取物体不参与
        const GPUMesh* mesh = mesh_mgr_.GetMesh(o.mesh_id);
        CHECK(mesh != nullptr) << "DrawPickingPass: mesh_id " << o.mesh_id << " 未注册";
        CHECK_GT(mesh->vao, 0u);

        float model[16], final_mvp[16];
        Primitives3DRenderer::BuildModelMatrix(o.center, o.up, o.front, model);
        Primitives3DRenderer::Mat4Mul(mvp, model, final_mvp);

        glUniformMatrix4fv(glGetUniformLocation(pick_prog, "uMVP"),
                           1, GL_FALSE, final_mvp);
        glUniform1i(glGetUniformLocation(pick_prog, "uPickingId"),
                    static_cast<int>(o.picking_id));

        glBindVertexArray(mesh->vao);
        if (mesh->index_count > 0)
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh->index_count),
                           GL_UNSIGNED_INT, nullptr);
        else
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(mesh->vertex_count));
        glBindVertexArray(0);
    }

    // ---- 窗口像素坐标 → 3D FBO 像素（GL 左下原点）----
    // 语义：screen_x/y 是窗口像素（左上原点，与 InputSnapshot 一致，由调用方
    // 传入生效的 viewport（vp_x/y/w/h，含“viewport 全 0 时默认全窗口”的回退）。
    // 先把窗口坐标投影到 viewport，再映射到 3D FBO 分辨率。
    const float u = (vp_w > 0.0f) ? (cmds.pick.screen_x - vp_x) / vp_w : 0.0f;
    const float v = (vp_h > 0.0f) ? (cmds.pick.screen_y - vp_y) / vp_h : 0.0f;
    // clamp 到 [0,1]，越界命中背景
    int px = static_cast<int>(std::clamp(u, 0.0f, 1.0f) * static_cast<float>(fbo_w));
    int py_top = static_cast<int>(std::clamp(v, 0.0f, 1.0f) * static_cast<float>(fbo_h));
    px = std::clamp(px, 0, fbo_w - 1);
    py_top = std::clamp(py_top, 0, fbo_h - 1);
    const int py = fbo_h - 1 - py_top;   // 翻转到 GL 左下原点

    unsigned char pix[4] = {0, 0, 0, 0};
    glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pix);

    const unsigned int id =
        static_cast<unsigned int>(pix[0]) |
        (static_cast<unsigned int>(pix[1]) << 8) |
        (static_cast<unsigned int>(pix[2]) << 16);

    last_pick_.hit = (id != 0);
    last_pick_.picking_id = id;

    // 恢复 GL 状态
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
}

// 高亮（CPU 屏幕空间回填）：给已画进剪影 mask 的高亮物体，读回 mask 做
// outline_px 次像素膨胀，求恒定像素宽边缘环。详见 DrawHighlightPass。
void Renderer::DestroyHighlightFBO() {
    if (hl_fbo_) {
        glDeleteFramebuffers(1, &hl_fbo_);
        glDeleteTextures(1, &hl_color_tex_);
        glDeleteRenderbuffers(1, &hl_depth_stencil_rb_);
        hl_fbo_ = 0; hl_color_tex_ = 0; hl_depth_stencil_rb_ = 0;
    }
    hl_fbo_w_ = 0; hl_fbo_h_ = 0;
    DestroyHighlightMaskFBO();
}

// 高亮剪影 mask FBO：单色（R8）纹理 + 独立 FBO，尺寸与 3D FBO 一致。
// 存被高亮物体的不扩张剪影（白=物体，黑=背景），供 CPU 读回做像素膨胀。
void Renderer::DestroyHighlightMaskFBO() {
    if (hl_mask_fbo_) {
        glDeleteFramebuffers(1, &hl_mask_fbo_);
        glDeleteTextures(1, &hl_mask_tex_);
        glDeleteRenderbuffers(1, &hl_mask_depth_rb_);
        hl_mask_fbo_ = 0; hl_mask_tex_ = 0; hl_mask_depth_rb_ = 0;
    }
    hl_mask_w_ = 0; hl_mask_h_ = 0;
}

void Renderer::EnsureHighlightMaskFBO(int w, int h) {
    if (hl_mask_w_ == w && hl_mask_h_ == h && hl_mask_fbo_) return;
    if (w <= 0 || h <= 0 || w > kMaxFboDim || h > kMaxFboDim) return;
    DestroyHighlightMaskFBO();

    glGenTextures(1, &hl_mask_tex_);
    glBindTexture(GL_TEXTURE_2D, hl_mask_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    // mask 需要 depth 做遮挡剔除（画剪影时只画高亮物体可见部分）。
    glGenRenderbuffers(1, &hl_mask_depth_rb_);
    glBindRenderbuffer(GL_RENDERBUFFER, hl_mask_depth_rb_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glGenFramebuffers(1, &hl_mask_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, hl_mask_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, hl_mask_tex_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, hl_mask_depth_rb_);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    CHECK_EQ(status, GL_FRAMEBUFFER_COMPLETE) << "highlight mask FBO failed, status=" << status;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    hl_mask_w_ = w; hl_mask_h_ = h;
}

// MSAA 路径的高亮单采样 FBO：RGBA16F 颜色 + 组合 depth+stencil renderbuffer（无 MSAA）。
void Renderer::EnsureHighlightFBO(int w, int h) {
    if (hl_fbo_w_ == w && hl_fbo_h_ == h && hl_fbo_) return;
    if (w <= 0 || h <= 0 || w > kMaxFboDim || h > kMaxFboDim) return;
    DestroyHighlightFBO();

    glGenTextures(1, &hl_color_tex_);
    glBindTexture(GL_TEXTURE_2D, hl_color_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    // 组合 depth+stencil（GL_DEPTH24_STENCIL8）：部分实现（llvmpipe）要求
    // depth 与 stencil 共存于同一 renderbuffer，用 GL_DEPTH_STENCIL_ATTACHMENT，
    // 分离的两个 renderbuffer 会 FBO 不完整（status=0x8CD7）。
    glGenRenderbuffers(1, &hl_depth_stencil_rb_);
    glBindRenderbuffer(GL_RENDERBUFFER, hl_depth_stencil_rb_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glGenFramebuffers(1, &hl_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, hl_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, hl_color_tex_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, hl_depth_stencil_rb_);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    CHECK_EQ(status, GL_FRAMEBUFFER_COMPLETE) << "highlight FBO failed, status=" << status;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    hl_fbo_w_ = w; hl_fbo_h_ = h;

    // 同时确保剪影 mask FBO 尺寸匹配（CPU 回填需要它）。
    EnsureHighlightMaskFBO(w, h);
}

// 高亮 pass（CPU 屏幕空间回填）：3D 内容全部画完后执行的一个独立子步骤。
// 入参 fbo_w/fbo_h 为 3D FBO 尺寸。流程：
//   1) blit 场景 color 从 fbo_hdr_（MSAA 时自动 resolve）到单采样 hl FBO
//      —— 得到含 PBR 本色的场景底色；
//   2) 把被高亮物体画成不扩张的单色剪影到独立 mask 纹理（hl_mask_tex_）；
//   3) CPU 读回 mask，做 outline_px 次像素膨胀，膨胀图与原剪影相减得
//      恒定像素宽的边缘环；
//   4) 用 GL_POINTS 把边缘环屏幕像素以边框色回填叠加到 hl_color_tex_；
//   5) 返回叠加了高亮的颜色纹理，供 tone map pass 作输入。
// 取代旧的“模型空间放大副本 + stencil”方案（旧式边框为相对缩放、近大远小；
// 此方案边框恒定像素宽，且不依赖深度匹配防溢出——剪影边缘天然不溢出）。
//
// 纯 CPU 的“剪影膨胀求边缘环”：读回 mask（w*h，白=物体/非0），对物体像素
// 做 outline_px 次十字膨胀（上下左右±1），像素(膨胀后 && !原剪影)即边缘环。
// 返回边缘环像素的 NDC 坐标（x,y in [-1,1]，z=0），供 GL_POINTS 直接绘制。
static void ComputeHighlightEdgeNdc(const std::vector<uint8_t>& mask,
                                     int w, int h, int outline_px,
                                     std::vector<float>* out_ndc /*output*/) {
    CHECK(out_ndc != nullptr);
    const int np = w * h;
    // occupied 逐像素：先标记物体像素（膨胀源），再迭代十字扩张。
    std::vector<uint8_t> obj(np, 0);       // 原物体剪影
    std::vector<uint8_t> dil(np, 0);       // 膨胀后的物体区域
    for (int i = 0; i < np; ++i) {
        obj[i] = (mask[i] != 0) ? 1 : 0;
        dil[i] = obj[i];
    }
    // 迭代 outline_px 次十字膨胀（每次向上下左右各扩张 1 像素）。
    for (int it = 0; it < outline_px; ++it) {
        std::vector<uint8_t> next = dil;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int i = y * w + x;
                if (dil[i] == 0) continue;
                if (x > 0)     next[y * w + (x - 1)] = 1;
                if (x < w - 1) next[y * w + (x + 1)] = 1;
                if (y > 0)     next[(y - 1) * w + x] = 1;
                if (y < h - 1) next[(y + 1) * w + x] = 1;
            }
        }
        dil.swap(next);
    }
    // 边缘环 = 膨胀后 && !原剪影。收集其 NDC 坐标（y 翻转到 GL 左下原点）。
    const float inv_hw = 2.0f / static_cast<float>(w);
    const float inv_hh = 2.0f / static_cast<float>(h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int i = y * w + x;
            if (dil[i] && !obj[i]) {
                // 像素中心 (x+0.5, y+0.5) 映射到 NDC；像素系左上原点 → GL 左下原点。
                const float ndc_x = (x + 0.5f) * inv_hw - 1.0f;
                const float ndc_y = 1.0f - (y + 0.5f) * inv_hh;
                out_ndc->push_back(ndc_x);
                out_ndc->push_back(ndc_y);
                out_ndc->push_back(0.0f);
            }
        }
    }
}

unsigned int Renderer::DrawHighlightPass(const RenderCommandList& cmds,
                                         int fbo_w, int fbo_h) {
    if (!cmds.highlight_style.has_value()) return 0;
    CHECK_GT(cmds.highlight_style->outline_px, 0) << "outline_px 必须 > 0";
    EnsureHighlightFBO(fbo_w, fbo_h);

    // 1) 场景 color+depth 从 HDR FBO blit 到 hl FBO（得到含 PBR 本色的底色；
    //    depth 进 hl_depth_stencil_rb_，供剪影遮挡剔除）。
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_hdr_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, hl_fbo_);
    glBlitFramebuffer(
        0, 0, fbo_w, fbo_h,
        0, 0, hl_fbo_w_, hl_fbo_h_,
        GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);

    // 2) 把被高亮物体画成单色剪影到独立 mask 纹理。
    //    先把场景 depth 从 HDR FBO resolve/blit 到 mask FBO 的 depth，
    //    再画剪影用 LEQUAL depth test：仅高亮物体可见（未被遮挡）部分写 1
    //    —— 被前景遮挡处不写，边框不会透过遮挡露出。
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_hdr_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, hl_mask_fbo_);
    glBlitFramebuffer(
        0, 0, fbo_w, fbo_h,
        0, 0, hl_mask_w_, hl_mask_h_,
        GL_DEPTH_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, hl_mask_fbo_);
    glViewport(0, 0, fbo_w, fbo_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);   // 背景=黑=非物体
    glClear(GL_COLOR_BUFFER_BIT);   // 只清颜色，depth 已被 blit 填充
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);   // 深度已 resolve，只读不重写
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    float mvp[16];
    Primitives3DRenderer::BuildMVP(cmds.camera, fbo_w, fbo_h, mvp);
    const unsigned int pick_prog = PickProg();
    glUseProgram(pick_prog);
    // PickProg 的 fragment shader 把 id 编码到 RGB —— 用 id=1 画剪影，
    // 背景已被 clear 为黑（非0/0 区分剪影即可）。
    glUniform1i(glGetUniformLocation(pick_prog, "uPickingId"), 1);
    for (const auto& o : cmds.object3d) {
        if (!o.highlight) continue;
        const GPUMesh* mesh = mesh_mgr_.GetMesh(o.mesh_id);
        if (!mesh || mesh->vao == 0) continue;
        float model[16], final_mvp[16];
        Primitives3DRenderer::BuildModelMatrix(o.center, o.up, o.front, model);
        Primitives3DRenderer::Mat4Mul(mvp, model, final_mvp);
        glUniformMatrix4fv(glGetUniformLocation(pick_prog, "uMVP"),
                           1, GL_FALSE, final_mvp);
        glBindVertexArray(mesh->vao);
        if (mesh->index_count > 0)
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh->index_count),
                           GL_UNSIGNED_INT, nullptr);
        else
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(mesh->vertex_count));
        glBindVertexArray(0);
    }

    // 3) CPU 读回 mask（R8 → 每像素 1 字节，此刻读 buffer 为 mask FBO 的颜色），
    //    算恒定像素宽边缘环的 NDC。
    std::vector<uint8_t> mask(static_cast<size_t>(fbo_w) * fbo_h, 0);
    glReadPixels(0, 0, fbo_w, fbo_h, GL_RED, GL_UNSIGNED_BYTE, mask.data());
    std::vector<float> edge_ndc;
    ComputeHighlightEdgeNdc(mask, fbo_w, fbo_h,
                            cmds.highlight_style->outline_px, &edge_ndc);

    // 切回 hl FBO：随后 GL_POINTS 把边框色回填叠加到场景底色上。
    glBindFramebuffer(GL_FRAMEBUFFER, hl_fbo_);
    glViewport(0, 0, fbo_w, fbo_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDepthMask(GL_FALSE);

    // 4) GL_POINTS 回填边框色到 hl_color_tex_（叠加在场景底色上）。
    if (!edge_ndc.empty()) {
        const unsigned int solid3d = Solid3DProg();
        glUseProgram(solid3d);
        // 单位矩阵：顶点坐标即 NDC。
        float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        glUniformMatrix4fv(glGetUniformLocation(solid3d, "uMVP"),
                           1, GL_FALSE, identity);
        const jpov::Color& c = cmds.highlight_style->color;
        glUniform4f(glGetUniformLocation(solid3d, "uColor"),
                    c.r, c.g, c.b, c.a);

        // 上传点序列到 stream VBO（3 floats/vertex：x,y,z）后，绑定流式 VAO
        //（attribute 0 = vec3，已在 CreateStreamVBO 配好）直接画点。
        const GLsizei vcount = static_cast<GLsizei>(edge_ndc.size() / 3);
        CHECK_LE(vcount, kMaxStreamVertices)
            << "highlight 边缘像素点过多，超出 stream VBO 容量";
        glBindBuffer(GL_ARRAY_BUFFER, stream_vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(edge_ndc.size() * sizeof(float)),
                     edge_ndc.data(), GL_STREAM_DRAW);
        glEnable(GL_PROGRAM_POINT_SIZE);
        glPointSize(1.0f);
        glBindVertexArray(stream_vao_);
        glDrawArrays(GL_POINTS, 0, vcount);
        glBindVertexArray(0);
        glDisable(GL_PROGRAM_POINT_SIZE);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);

    // 返回叠加了高亮的颜色纹理，作为 tone map 的输入。
    return hl_color_tex_;
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

        // 合并本 primitive 顶点到位包围盒（loader 输出坐标 = 渲染时模型局部坐标）。
        // 注意：RegisterMesh 把几何上传 GPU 后 CPU positions 不再保留，
        // 因此必须在收集阶段（entry->mesh 还在）顺手算好 bbox。
        for (const Vec3f& p : entry->mesh.positions) {
            if (!obj->bounds_valid) {
                obj->bounds_min[0] = obj->bounds_max[0] = p.x();
                obj->bounds_min[1] = obj->bounds_max[1] = p.y();
                obj->bounds_min[2] = obj->bounds_max[2] = p.z();
                obj->bounds_valid = true;
            } else {
                obj->bounds_min[0] = std::min(obj->bounds_min[0], p.x());
                obj->bounds_min[1] = std::min(obj->bounds_min[1], p.y());
                obj->bounds_min[2] = std::min(obj->bounds_min[2], p.z());
                obj->bounds_max[0] = std::max(obj->bounds_max[0], p.x());
                obj->bounds_max[1] = std::max(obj->bounds_max[1], p.y());
                obj->bounds_max[2] = std::max(obj->bounds_max[2], p.z());
            }
        }

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
