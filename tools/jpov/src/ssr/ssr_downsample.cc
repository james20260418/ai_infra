// JPOV SSR — 输入准备（降采样）实现。设计与选点规则见 ssr_downsample.h 顶部。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/ssr/ssr_downsample.h"

#include <string>
#include <vector>

#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include <glog/logging.h>

#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/ssr/ssr_shader.h"

namespace jpov {
namespace {

// 全分辨率 → SSR 分辨率。N×N 里挑**窗口深度最小**的 texel 代表整组 —— 本研究用到的
// 几张图必须取自同一个 texel。
//
// 两个变体只差"输出什么"：
//   kSource（无宏）：color + w + normal   （被反射的世界）
//   kQuery （SSR_DOWNSAMPLE_QUERY）：w + normal + rough（谁在问）
// 选点循环**逐字相同**（这正是两个 kind 必须共用同一份代码的理由：选点规则要是分叉了，
// 源与查询就会在不同 texel 上取到不同表面，症状是倒影整体偏一格）。
constexpr const char* kSsrDownsampleFs = R"glsl(
#version 330 core
uniform sampler2D uColor;     // 仅 kSource
uniform sampler2D uDepth;
uniform sampler2D uAttr;
uniform sampler2D uAttrMat;   // 仅 kQuery：rgb = F0，a = roughness
uniform ivec2 uFullRes;
uniform int   uScale;      // 1 = 直通；2 = 2×2 选一
uniform float uNear;
uniform float uFar;

#ifdef SSR_DOWNSAMPLE_QUERY
layout(location = 0) out float FragW;        // **w = 1/z_view**（0 = 没有几何）
layout(location = 1) out vec4  FragNormal;   // rgb = 三维视空间法线；a = 掩码编码
layout(location = 2) out float FragRough;    // 查询像素的 roughness（发射线前的截止用）
#else
layout(location = 0) out vec4  FragColor;    // 源颜色（命中取色）
layout(location = 1) out float FragW;        // **w = 1/z_view**（0 = 没有几何）
layout(location = 2) out vec4  FragNormal;   // rgb = 三维视空间法线；a = 掩码编码
#endif

// 窗口深度 → 视距（米）。与 ssr_math.h::DepthFromWindowDepth 逐字同式（含 NaN 守卫）。
float SsrZViewFromWindow(float depth, float zn, float zf) {
    float ndc = 2.0 * depth - 1.0;
    float denom = (zf + zn) - ndc * (zf - zn);
    if (!(abs(denom) > 1.0e-6)) {
        return max(zn, 1.0e-4);
    }
    return (2.0 * zn * zf) / denom;
}

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
    vec4 attr = texelFetch(uAttr, best, 0);
    FragNormal = attr;   // rgb = 三维视空间法线（不再重建 z），a = 掩码编码
    // 窗口深度 → w = 1/z_view（0 = 天空/远平面）。整条 SSR 链路内部只用 w：
    // 判据是"1/d 落在哪个区间"，而 1/d 对屏幕坐标仿射 ⇒ 每格判据零除法。
    // 每 SSR 像素只付一次除法（以前是每个被访问的格各付 3 次）。
    FragW = (best_z < 1.0) ? (1.0 / SsrZViewFromWindow(best_z, uNear, uFar)) : 0.0;
#ifdef SSR_DOWNSAMPLE_QUERY
    // 粗糙度必须与前三张**同源**（同一个 best texel）：跨表面的材质混进这条求交链，
    // 后果是"某块本该跳过的像素发了射线"，比颜色取错更难查。
    FragRough = texelFetch(uAttrMat, best, 0).a;
#else
    FragColor = texelFetch(uColor, best, 0);
#endif
}
)glsl";

// 两个变体的纹理单元分配（不同的输入组合 ⇒ 分开编号，互不覆盖）。
constexpr int kTexUnitSourceColor = 0;
constexpr int kTexUnitSourceDepth = 1;
constexpr int kTexUnitSourceNormal = 2;
constexpr int kTexUnitSourceCount = 3;
constexpr int kTexUnitQueryDepth = 0;
constexpr int kTexUnitQueryNormal = 1;
constexpr int kTexUnitQueryRough = 2;
constexpr int kTexUnitQueryCount = 3;

}  // namespace

SsrDownsample::SsrDownsample() = default;
SsrDownsample::~SsrDownsample() { Finalize(); }

void SsrDownsample::Init(ShaderManager* shader_mgr, SsrDownsampleKind kind) {
    CHECK(shader_mgr != nullptr);
    if (prog_ != 0 || fbo_ != 0) {
        Finalize();
    }
    shader_mgr_ = shader_mgr;
    kind_ = kind;
    const bool query = (kind == SsrDownsampleKind::kQuery);
    prog_ = shader_mgr_->GetOrCreate(query ? "ssr_downsample_query" : "ssr_downsample_source",
                                     {kSsrFullscreenVs, kSsrDownsampleFs,
                                      query ? std::vector<std::string>{"SSR_DOWNSAMPLE_QUERY"}
                                            : std::vector<std::string>{}});
    CHECK_NE(prog_, 0u);
}

void SsrDownsample::Finalize() {
    DestroyTargets();
    prog_ = 0;
    shader_mgr_ = nullptr;
}

void SsrDownsample::DestroyTargets() {
    if (fbo_ != 0) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    if (color_tex_ != 0) {
        glDeleteTextures(1, &color_tex_);
        color_tex_ = 0;
    }
    if (w_tex_ != 0) {
        glDeleteTextures(1, &w_tex_);
        w_tex_ = 0;
    }
    if (normal_tex_ != 0) {
        glDeleteTextures(1, &normal_tex_);
        normal_tex_ = 0;
    }
    if (rough_tex_ != 0) {
        glDeleteTextures(1, &rough_tex_);
        rough_tex_ = 0;
    }
    width_ = 0;
    height_ = 0;
}

void SsrDownsample::EnsureTargets(int w, int h) {
    if (w == width_ && h == height_ && fbo_ != 0) {
        return;
    }
    DestroyTargets();
    width_ = w;
    height_ = h;

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

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    GLenum bufs[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    if (kind_ == SsrDownsampleKind::kSource) {
        make_tex(&color_tex_, GL_RGBA16F, GL_RGBA, w, h);
        make_tex(&w_tex_, GL_R32F, GL_RED, w, h);
        make_tex(&normal_tex_, GL_RGBA16F, GL_RGBA, w, h);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex_, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, w_tex_, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, normal_tex_,
                               0);
        // ⚠️ 命中取色走**双线性**（resolve 里 texture(uColor, pos/uRes)）⇒ 必须开 LINEAR。
        //    其它几张被 texelFetch 精确读取，保持 NEAREST（避免任何意外的插值）。
        glBindTexture(GL_TEXTURE_2D, color_tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
    } else {
        make_tex(&w_tex_, GL_R32F, GL_RED, w, h);
        make_tex(&normal_tex_, GL_RGBA16F, GL_RGBA, w, h);
        make_tex(&rough_tex_, GL_R16F, GL_RED, w, h);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, w_tex_, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, normal_tex_,
                               0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, rough_tex_, 0);
    }
    glDrawBuffers(3, bufs);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
        << "SSR 降采样 FBO 不完整（" << w << "×" << h << "，kind="
        << (kind_ == SsrDownsampleKind::kQuery ? "query" : "source") << "）";
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SsrDownsample::Run(const Inputs& in) {
    CHECK(shader_mgr_ != nullptr) << "SsrDownsample::Run 前必须先 Init";
    const bool query = (kind_ == SsrDownsampleKind::kQuery);
    CHECK_NE(in.depth_tex, 0u) << "SsrDownsample::Inputs::depth_tex 未设置";
    CHECK_NE(in.normal_tex, 0u) << "SsrDownsample::Inputs::normal_tex 未设置";
    if (query) {
        CHECK_NE(in.rough_tex, 0u) << "kQuery 需要 rough_tex（RGBA16F，只读 .a）";
    } else {
        CHECK_NE(in.color_tex, 0u) << "kSource 需要 color_tex";
    }
    CHECK_GT(in.full_w, 0);
    CHECK_GT(in.full_h, 0);
    CHECK_GE(in.scale, 1);
    CHECK_LE(in.scale, 4) << "降采样倍率只支持 1..4（resolution_scale = 1/N）";
    CHECK_GT(in.near_plane, 0.0f);
    CHECK_GT(in.far_plane, in.near_plane) << "窗口深度 → w 的换算需要合法相机";

    int w = 0;
    int h = 0;
    DownsampleTargetSize(in.full_w, in.full_h, in.scale, &w, &h);

    const int unit_count = query ? kTexUnitQueryCount : kTexUnitSourceCount;
    GLint prev_fbo = 0;
    GLint prev_viewport[4] = {0, 0, 0, 0};
    GLint prev_active = 0;
    GLint prev_prog = 0;
    GLint prev_blend = 0;
    GLint prev_depth = 0;
    GLint prev_cull = 0;
    GLint prev_scissor = 0;
    GLint prev_bind[kTexUnitQueryCount] = {0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    prev_blend = glIsEnabled(GL_BLEND);
    prev_depth = glIsEnabled(GL_DEPTH_TEST);
    prev_cull = glIsEnabled(GL_CULL_FACE);
    prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    for (int u = 0; u < unit_count; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_bind[u]);
    }

    EnsureTargets(w, h);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);     // 全屏三角形：不能被剔除
    glDisable(GL_SCISSOR_TEST);  // 调用方开着 scissor 会把全屏 pass 裁成一小块
    glUseProgram(prog_);

    if (query) {
        glActiveTexture(GL_TEXTURE0 + kTexUnitQueryDepth);
        glBindTexture(GL_TEXTURE_2D, in.depth_tex);
        glActiveTexture(GL_TEXTURE0 + kTexUnitQueryNormal);
        glBindTexture(GL_TEXTURE_2D, in.normal_tex);
        glActiveTexture(GL_TEXTURE0 + kTexUnitQueryRough);
        glBindTexture(GL_TEXTURE_2D, in.rough_tex);
        glUniform1i(shader_mgr_->GetUniform(prog_, "uDepth"), kTexUnitQueryDepth);
        glUniform1i(shader_mgr_->GetUniform(prog_, "uAttr"), kTexUnitQueryNormal);
        glUniform1i(shader_mgr_->GetUniform(prog_, "uAttrMat"), kTexUnitQueryRough);
    } else {
        glActiveTexture(GL_TEXTURE0 + kTexUnitSourceColor);
        glBindTexture(GL_TEXTURE_2D, in.color_tex);
        glActiveTexture(GL_TEXTURE0 + kTexUnitSourceDepth);
        glBindTexture(GL_TEXTURE_2D, in.depth_tex);
        glActiveTexture(GL_TEXTURE0 + kTexUnitSourceNormal);
        glBindTexture(GL_TEXTURE_2D, in.normal_tex);
        glUniform1i(shader_mgr_->GetUniform(prog_, "uColor"), kTexUnitSourceColor);
        glUniform1i(shader_mgr_->GetUniform(prog_, "uDepth"), kTexUnitSourceDepth);
        glUniform1i(shader_mgr_->GetUniform(prog_, "uAttr"), kTexUnitSourceNormal);
    }
    glUniform2i(shader_mgr_->GetUniform(prog_, "uFullRes"), in.full_w, in.full_h);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uScale"), in.scale);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uNear"), in.near_plane);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uFar"), in.far_plane);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // 恢复调用方状态
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glUseProgram(static_cast<GLuint>(prev_prog));
    if (prev_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (prev_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prev_cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    for (int u = 0; u < unit_count; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_bind[u]));
    }
    glActiveTexture(static_cast<GLenum>(prev_active));
}

}  // namespace jpov
