// JPOV SSR 接入层实现。设计/分工见 ssr_integration.h 顶部。
//
// 一趟序列（全部在 3D 不透明 pass 之后、半透明之前）：
//   ① blit  HDR COLOR0 → copy_tex（全分辨率副本，避免"边读边写"）
//   ② 降采样 全分辨率(color,depth,attr) → SSR 分辨率(color,depth,normal)，2×2 选点
//   ③ SSR    调 SsrRenderer（纯模块，半分辨率）
//   ④ 合成   全分辨率：A 预乘双边升采样 → 替换 3D pass 已加过的环境镜面项
//
// 已知限制（本版，写清楚免得当成 bug）：
//   · **反射体只支持"全场景一套材质"**（base_color/metallic/roughness/ao 是 uniform）。
//     属性 pass 只写视空间法线，不写逐像素材质。等要做"多个不同金属度的镜面"时，
//     把材质塞进属性纹理（.zw 放 roughness/metallic，base_color 再要一张）即可，接口不变。
//   · **AO 不逐像素**（用 uniform，默认 1）：3D pass 里 AO 乘在同一个权重上，这里没取到。
//   · **粗糙度不做 LOD**：只对 roughness ≈ 0 的反射体语义正确；composite 不判断，
//     由调用方（SsrConfig::roughness）自己保证。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/ssr/ssr_integration.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include <glog/logging.h>

#include "tools/jpov/src/env_specular.h"
#include "tools/jpov/src/ssr/ssr_shader.h"

namespace jpov {
namespace {

constexpr int kTexUnitIn0 = 0;   // 各趟自 unit 0 起绑自己的输入（pass-local）
constexpr int kTexUnitIn1 = 1;
constexpr int kTexUnitIn2 = 2;
constexpr int kTexUnitIn3 = 3;
constexpr int kTexUnitIn4 = 4;
constexpr int kTexUnitIn5 = 5;   // 低分法线
constexpr int kTexUnitCount = 6;

// 降采样：全分辨率 (color, depth, attr) → SSR 分辨率 (color, depth, normal)。
// 2×2 里挑**窗口深度最小**的那个 texel 代表整组 —— 三张图必须取自同一个 texel。
// 法线附件的 .z 必须一起带下去（= 1 表示该 texel 有法线数据，SSR 的命中面朝向判据用它）；
// 只传 .xy 的话，模块里只能把"没有法线"当成"平面朝相机" ⇒ 朝向判据在遮挡物上失效。
constexpr const char* kSsrDownsampleFs = R"glsl(
#version 330 core
uniform sampler2D uColor;
uniform sampler2D uDepth;
uniform sampler2D uAttr;
uniform ivec2 uFullRes;
uniform int   uScale;      // 1 = 直通；2 = 2×2 选一

layout(location = 0) out vec4  FragColor;
layout(location = 1) out float FragDepth;
layout(location = 2) out vec3  FragNormal;

void main() {
    ivec2 q = ivec2(gl_FragCoord.xy);
    ivec2 base = q * uScale;

    float best_z = 2.0;
    ivec2 best = clamp(base, ivec2(0), uFullRes - 1);
    for (int j = 0; j < uScale; ++j) {
        for (int i = 0; i < uScale; ++i) {
            ivec2 p = clamp(base + ivec2(i, j), ivec2(0), uFullRes - 1);
            float z = texelFetch(uDepth, p, 0).r;
            if (z < best_z) {
                best_z = z;
                best = p;
            }
        }
    }
    FragColor  = texelFetch(uColor, best, 0);
    FragDepth  = best_z;
    vec4 attr  = texelFetch(uAttr, best, 0);
    FragNormal = vec3(attr.xy, attr.z);   // .xy 视空间法线，.z = 有法线数据
}
)glsl";

// 合成：全分辨率。A 预乘的双边升采样 + 用 SSR 替换环境镜面项。
//
//   hdr_out = hdr_copy + mask · A · ao · (ssr_L · weight − env_term)
//
// A = 0 ⇒ 差值恒为 0 ⇒ 与"没接 SSR"时逐字节相同（单调改进）。
constexpr const char* kSsrCompositeFs = R"glsl(
uniform sampler2D uHdrCopy;     // 全分辨率 HDR 副本
uniform sampler2D uAttr;        // 全分辨率（.xy 视空间法线，.a mask）
uniform sampler2D uSceneDepth;  // 全分辨率 R32F
uniform sampler2D uSsr;         // SSR 分辨率 RGBA16F（rgb 辐亮度，a 覆盖率）
uniform sampler2D uDownDepth;   // SSR 分辨率 R32F
uniform sampler2D uDownNormal;  // SSR 分辨率 RG16F
uniform ivec2 uFullRes;
uniform vec2  uSsrRes;
uniform int   uScale;
uniform float uNear;
uniform float uFar;
uniform float uTanHalf;
uniform float uAspect;
uniform vec3  uBaseColor;
uniform float uMetallic;
uniform float uRoughness;
uniform float uAo;
uniform float uRangeDepthRatio;    // |Δ(1/z)| / (1/z) 的相对容差
uniform float uRangeNormalDot;     // 法线点积下限
uniform int   uCoverageDebug;      // 1 = 只输出覆盖率灰度

layout(location = 0) out vec4 FragColor;

// 窗口深度 → 视距（米）。与 ssr_math.h::DepthFromWindowDepth 同一份公式（含 NaN 守卫）。
float ZView(float depth) {
    float ndc = 2.0 * depth - 1.0;
    float denom = (uFar + uNear) - ndc * (uFar - uNear);
    if (!(abs(denom) > 1.0e-6)) {
        return max(uNear, 1.0e-4);
    }
    return (2.0 * uNear * uFar) / denom;
}

vec3 NormalFromAttr(vec4 attr) {
    return normalize(vec3(attr.xy, sqrt(max(0.0, 1.0 - dot(attr.xy, attr.xy)))));
}

void main() {
    ivec2 ip = ivec2(gl_FragCoord.xy);
    vec3 hdr = texelFetch(uHdrCopy, ip, 0).rgb;
    vec4 attr = texelFetch(uAttr, ip, 0);
    float mask = attr.a;

    // ── A 预乘双边上采样 ──
    // 空间：2×2 双线性权（低分辨率像素为单位）；值域：1/z 相对差 + 法线夹角。
    // **预乘**（w·A·rgb 与 w·A 分别累加）是必须的：否则 A=0 的邻居（rgb=0）会把结果
    // 拉暗，在 A 的边界形成一圈黑边（alpha bleeding）。
    vec2 lo = (vec2(ip) + 0.5) / (vec2(uSsrRes) * float(uScale));  // 低分辨率 uv
    vec2 f = lo * uSsrRes - 0.5;
    ivec2 q0 = ivec2(floor(f));
    vec2 t = f - vec2(q0);

    float z_hi = ZView(texelFetch(uSceneDepth, ip, 0).r);
    vec3 n_hi = NormalFromAttr(attr);
    float w_hi = 1.0 / max(z_hi, 1.0e-4);

    vec3 num = vec3(0.0);
    float den = 0.0;
    float wsum = 0.0;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            ivec2 q = clamp(q0 + ivec2(i, j), ivec2(0), ivec2(uSsrRes) - 1);
            float ws = (i == 0 ? 1.0 - t.x : t.x) * (j == 0 ? 1.0 - t.y : t.y);

            float z_lo = ZView(texelFetch(uDownDepth, q, 0).r);
            vec2 n2_lo = texelFetch(uDownNormal, q, 0).xy;
            vec3 n_lo = NormalFromAttr(vec4(n2_lo, 0.0, 0.0));
            float w_lo = 1.0 / max(z_lo, 1.0e-4);

            float wr = 1.0;
            if (abs(w_hi - w_lo) / w_hi > uRangeDepthRatio) {
                wr = 0.0;
            }
            if (dot(n_hi, n_lo) < uRangeNormalDot) {
                wr = 0.0;
            }

            float w = ws * wr;
            vec4 s = texelFetch(uSsr, q, 0);
            num += w * s.a * s.rgb;
            den += w * s.a;
            wsum += w;
        }
    }
    float A = (wsum > 0.0) ? (den / wsum) : 0.0;
    vec3 ssr_L = (den > 1.0e-6) ? (num / den) : vec3(0.0);

    if (uCoverageDebug == 1) {
        FragColor = vec4(vec3(A), 1.0);
        return;
    }
    if (mask < 0.5 || uCoverageDebug == 2) {
        FragColor = vec4(hdr, 1.0);
        return;
    }

    // ── 材质 / 环境镜面项（与 3D pass 同一份实现：kEnvSpecularGlsl）──
    vec2 ndc = (vec2(ip) + 0.5) / vec2(uFullRes) * 2.0 - 1.0;
    vec3 p_view = vec3(ndc.x * uAspect * uTanHalf, ndc.y * uTanHalf, -1.0) * z_hi;
    vec3 V = normalize(-p_view);
    float r = clamp(uRoughness, 0.02, 1.0);
    vec2 AB = EnvBrdfApprox(r, clamp(dot(n_hi, V), 1.0e-4, 1.0));
    vec3 F0 = mix(vec3(0.04), uBaseColor, uMetallic);
    vec3 weight = F0 * AB.x + AB.y;
    vec3 env_term = EnvSpecularTerm(n_hi, V, uRoughness, uMetallic, uBaseColor);

    vec3 out_rgb = hdr + mask * A * uAo * (ssr_L * weight - env_term);
    FragColor = vec4(out_rgb, 1.0);
}
)glsl";

bool IsValidScale(float s) { return s == 0.25f || s == 0.5f || s == 1.0f; }

}  // namespace

SsrIntegration::SsrIntegration() = default;

SsrIntegration::~SsrIntegration() { Finalize(); }

void SsrIntegration::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr) << "SsrIntegration::Init 需要非空的 ShaderManager";
    if (initialized_) {
        Finalize();
    }
    shader_mgr_ = shader_mgr;

    prog_attr_ = shader_mgr_->GetOrCreate("ssr_attr",
                                          {kSsrAttrVs, kSsrAttrFs, {}});
    prog_downsample_ = shader_mgr_->GetOrCreate(
        "ssr_downsample", {kSsrFullscreenVs, kSsrDownsampleFs, {}});

    // 合成的 FS = 环境镜面（GLSL 只有一份，见 env_specular.h）+ 本文件的主体。
    const std::string composite_src =
        std::string(kSsrGlslVersion) + kEnvSpecularGlsl + kSsrCompositeFs;
    prog_composite_ = shader_mgr_->GetOrCreate(
        "ssr_composite", {kSsrFullscreenVs, composite_src.c_str(), {}});

    ssr_.Init(shader_mgr_);
    initialized_ = true;
}

void SsrIntegration::Finalize() {
    DestroyTargets();
    ssr_.Finalize();
    prog_attr_ = 0;
    prog_downsample_ = 0;
    prog_composite_ = 0;
    shader_mgr_ = nullptr;
    initialized_ = false;
}

void SsrIntegration::DestroyTargets() {
    if (copy_fbo_ != 0) {
        glDeleteFramebuffers(1, &copy_fbo_);
        copy_fbo_ = 0;
    }
    if (copy_tex_ != 0) {
        glDeleteTextures(1, &copy_tex_);
        copy_tex_ = 0;
    }
    if (down_fbo_ != 0) {
        glDeleteFramebuffers(1, &down_fbo_);
        down_fbo_ = 0;
    }
    if (down_color_tex_ != 0) {
        glDeleteTextures(1, &down_color_tex_);
        down_color_tex_ = 0;
    }
    if (down_depth_tex_ != 0) {
        glDeleteTextures(1, &down_depth_tex_);
        down_depth_tex_ = 0;
    }
    if (down_normal_tex_ != 0) {
        glDeleteTextures(1, &down_normal_tex_);
        down_normal_tex_ = 0;
    }
    width_ = 0;
    height_ = 0;
    ssr_w_ = 0;
    ssr_h_ = 0;
}

void SsrIntegration::EnsureTargets(int w, int h, int ssr_w, int ssr_h, bool coverage_debug,
                                   bool attr_valid) {
    if (w == width_ && h == height_ && ssr_w == ssr_w_ && ssr_h == ssr_h_ &&
        coverage_debug == coverage_debug_ && attr_valid == attr_valid_ && copy_fbo_ != 0) {
        return;
    }
    DestroyTargets();
    width_ = w;
    height_ = h;
    ssr_w_ = ssr_w;
    ssr_h_ = ssr_h;
    coverage_debug_ = coverage_debug;
    attr_valid_ = attr_valid;

    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);

    // HDR 副本（只读副本 → 避免"读自己正在写的颜色"）
    glGenTextures(1, &copy_tex_);
    glBindTexture(GL_TEXTURE_2D, copy_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &copy_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, copy_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, copy_tex_, 0);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
        << "SSR copy FBO 不完整";

    // 降采样目标：color / depth / normal 三附件（必须同尺寸）
    auto make_tex = [](unsigned int* tex, GLint internal, GLenum format, int tw, int th) {
        glGenTextures(1, tex);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexImage2D(GL_TEXTURE_2D, 0, internal, tw, th, 0, format, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    };
    make_tex(&down_color_tex_, GL_RGBA16F, GL_RGBA, ssr_w, ssr_h);
    make_tex(&down_depth_tex_, GL_R32F, GL_RED, ssr_w, ssr_h);
    // RGBA（不是 RG）：.z = 该 texel 有法线数据，朝向判据要用
    make_tex(&down_normal_tex_, GL_RGBA16F, GL_RGBA, ssr_w, ssr_h);

    glGenFramebuffers(1, &down_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, down_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, down_color_tex_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, down_depth_tex_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, down_normal_tex_,
                           0);
    const GLenum bufs[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    glDrawBuffers(3, bufs);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
        << "SSR 降采样 FBO 不完整（" << ssr_w << "×" << ssr_h << "）";

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
}

void SsrIntegration::Render(const SsrPassInputs& in, unsigned int dst_fbo) {
    CHECK(initialized_) << "SsrIntegration::Render 前必须先 Init";
    CHECK(in.cfg.enabled) << "SsrIntegration::Render 只在 enabled 时调用";
    CHECK_NE(in.hdr_color_tex, 0u);
    CHECK_NE(in.scene_depth_tex, 0u);
    CHECK_NE(in.attr_tex, 0u);
    CHECK_NE(in.env_cube_tex, 0u) << "composite 需要环境 cubemap（与 3D pass 同一份）";
    CHECK_GT(in.full_w, 0);
    CHECK_GT(in.full_h, 0);
    CHECK_GT(in.near_plane, 0.0f);
    CHECK_GT(in.far_plane, in.near_plane);
    CHECK_GT(in.tan_half_fov_y, 0.0f);
    CHECK_GT(in.aspect, 0.0f);
    CHECK(IsValidScale(in.cfg.resolution_scale))
        << "SsrConfig::resolution_scale 只支持 1.0 / 0.5 / 0.25，得到 "
        << in.cfg.resolution_scale;

    const int scale = static_cast<int>(std::lround(1.0f / in.cfg.resolution_scale));
    const int ssr_w = std::max(1, (in.full_w + scale - 1) / scale);
    const int ssr_h = std::max(1, (in.full_h + scale - 1) / scale);

    // ── 保存调用方状态（返回前原样恢复）──
    GLint prev_fbo = 0;
    GLint prev_read = 0;
    GLint prev_viewport[4] = {0, 0, 0, 0};
    GLint prev_active = 0;
    GLint prev_blend = 0;
    GLint prev_depth = 0;
    GLint prev_cull = 0;
    GLint prev_scissor = 0;
    GLint prev_program = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    prev_blend = glIsEnabled(GL_BLEND);
    prev_depth = glIsEnabled(GL_DEPTH_TEST);
    prev_cull = glIsEnabled(GL_CULL_FACE);
    prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint prev_bind[kTexUnitCount] = {0, 0, 0, 0, 0, 0};
    for (int u = 0; u < kTexUnitCount; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_bind[u]);
    }

    EnsureTargets(in.full_w, in.full_h, ssr_w, ssr_h, in.cfg.debug_coverage != 0,
                  in.attr_tex != 0);

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    // ── ① HDR 副本：blit（不采样、不重建，纯拷贝）──
    glBindFramebuffer(GL_READ_FRAMEBUFFER, dst_fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, copy_fbo_);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glBlitFramebuffer(0, 0, in.full_w, in.full_h, 0, 0, in.full_w, in.full_h,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);

    // ── ② 降采样（选点）──
    glBindFramebuffer(GL_FRAMEBUFFER, down_fbo_);
    glViewport(0, 0, ssr_w, ssr_h);
    glUseProgram(prog_downsample_);
    glActiveTexture(GL_TEXTURE0 + kTexUnitIn0);
    glBindTexture(GL_TEXTURE_2D, in.hdr_color_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitIn1);
    glBindTexture(GL_TEXTURE_2D, in.scene_depth_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitIn2);
    glBindTexture(GL_TEXTURE_2D, in.attr_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_downsample_, "uColor"), kTexUnitIn0);
    glUniform1i(shader_mgr_->GetUniform(prog_downsample_, "uDepth"), kTexUnitIn1);
    glUniform1i(shader_mgr_->GetUniform(prog_downsample_, "uAttr"), kTexUnitIn2);
    glUniform2i(shader_mgr_->GetUniform(prog_downsample_, "uFullRes"), in.full_w, in.full_h);
    glUniform1i(shader_mgr_->GetUniform(prog_downsample_, "uScale"), scale);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // ── ③ SSR（模块自己管 FBO 与状态）──
    SsrInputs si;
    si.color_tex = down_color_tex_;
    si.depth_tex = down_depth_tex_;
    si.normal_tex = down_normal_tex_;
    si.width = ssr_w;
    si.height = ssr_h;
    si.near_plane = in.near_plane;
    si.far_plane = in.far_plane;
    si.tan_half_fov_y = in.tan_half_fov_y;
    si.aspect = in.aspect;
    si.thickness_pixels = in.cfg.thickness_pixels;
    si.min_thickness_m = in.cfg.min_thickness_m;
    si.max_distance_m = in.cfg.max_distance_m;
    si.max_ray_pixels = in.cfg.max_ray_pixels;
    si.tile_size = in.cfg.tile_size;
    si.debug_reason = false;
    const SsrOutputs& outs = ssr_.Render(si);

    // ── ④ 合成 ──
    glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
    glViewport(0, 0, in.full_w, in.full_h);
    glUseProgram(prog_composite_);
    glActiveTexture(GL_TEXTURE0 + kTexUnitIn0);
    glBindTexture(GL_TEXTURE_2D, copy_tex_);
    glActiveTexture(GL_TEXTURE0 + kTexUnitIn1);
    glBindTexture(GL_TEXTURE_2D, in.attr_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitIn2);
    glBindTexture(GL_TEXTURE_2D, in.scene_depth_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitIn3);
    glBindTexture(GL_TEXTURE_2D, outs.radiance_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitIn4);
    glBindTexture(GL_TEXTURE_2D, down_depth_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uHdrCopy"), kTexUnitIn0);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uAttr"), kTexUnitIn1);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uSceneDepth"), kTexUnitIn2);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uSsr"), kTexUnitIn3);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uDownDepth"), kTexUnitIn4);
    // 低分法线也要绑一个单元（合成里要用它做法线值域项）
    {
        const int u = kTexUnitIn5;
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, down_normal_tex_);
        glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uDownNormal"), u);
    }
    glUniform2i(shader_mgr_->GetUniform(prog_composite_, "uFullRes"), in.full_w, in.full_h);
    glUniform2f(shader_mgr_->GetUniform(prog_composite_, "uSsrRes"),
                static_cast<float>(ssr_w), static_cast<float>(ssr_h));
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uScale"), scale);
    glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uNear"), in.near_plane);
    glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uFar"), in.far_plane);
    glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uTanHalf"), in.tan_half_fov_y);
    glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uAspect"), in.aspect);
    glUniform3f(shader_mgr_->GetUniform(prog_composite_, "uBaseColor"), in.cfg.base_color.x(),
                in.cfg.base_color.y(), in.cfg.base_color.z());
    glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uMetallic"), in.cfg.metallic);
    glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uRoughness"), in.cfg.roughness);
    glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uAo"), in.cfg.ao);
    glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uRangeDepthRatio"),
                in.cfg.upsample_depth_tolerance);
    glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uRangeNormalDot"),
                in.cfg.upsample_normal_dot);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uCoverageDebug"),
                in.cfg.debug_coverage ? 1 : 0);
    // 环境镜面：**每帧必须重传**（uniform 是 program 状态，上一帧开着本帧不传就还用旧的）。
    UploadEnvSpecular(*shader_mgr_, prog_composite_, in.env_cube_tex, in.env_max_lod,
                      in.env_sky_scale, in.env_enabled);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // ── 恢复 ──
    glUseProgram(static_cast<GLuint>(prev_program));
    for (int u = 0; u < kTexUnitCount; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_bind[u]));
    }
    glActiveTexture(static_cast<GLenum>(prev_active));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_read));
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    if (prev_blend != 0) {
        glEnable(GL_BLEND);
    }
    if (prev_depth != 0) {
        glEnable(GL_DEPTH_TEST);
    }
    if (prev_cull != 0) {
        glEnable(GL_CULL_FACE);
    }
    if (prev_scissor != 0) {
        glEnable(GL_SCISSOR_TEST);
    }
}

}  // namespace jpov
