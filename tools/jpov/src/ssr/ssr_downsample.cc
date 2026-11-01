// JPOV SSR — 输入准备（降采样）实现。设计与选点规则见 ssr_downsample.h 顶部。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/ssr/ssr_downsample.h"

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

// 全分辨率 (color, depth, attr) → SSR 分辨率 (color, depth, normal)。
// 2×2（或 N×N）里挑**窗口深度最小**的 texel 代表整组 —— 三张图必须取自同一个 texel。
// 法线附件整条 RGBA 带下去（rgb = 三维视空间法线，a = 掩码编码），朝向判据要用。
constexpr const char* kSsrDownsampleFs = R"glsl(
#version 330 core
uniform sampler2D uColor;
uniform sampler2D uDepth;
uniform sampler2D uAttr;
uniform ivec2 uFullRes;
uniform int   uScale;      // 1 = 直通；2 = 2×2 选一

layout(location = 0) out vec4  FragColor;
layout(location = 1) out float FragDepth;
layout(location = 2) out vec4  FragNormal;   // rgb = 三维视空间法线；a = 掩码编码

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
    FragNormal = attr;   // rgb = 三维视空间法线（不再重建 z），a = 掩码编码
}
)glsl";

constexpr int kTexUnitColor = 0;
constexpr int kTexUnitDepth = 1;
constexpr int kTexUnitAttr = 2;
constexpr int kTexUnitCount = 3;

}  // namespace

SsrDownsample::SsrDownsample() = default;
SsrDownsample::~SsrDownsample() { Finalize(); }

void SsrDownsample::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr);
    shader_mgr_ = shader_mgr;
    prog_ = shader_mgr_->GetOrCreate("ssr_downsample", {kSsrFullscreenVs, kSsrDownsampleFs, {}});
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
    if (depth_tex_ != 0) {
        glDeleteTextures(1, &depth_tex_);
        depth_tex_ = 0;
    }
    if (normal_tex_ != 0) {
        glDeleteTextures(1, &normal_tex_);
        normal_tex_ = 0;
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
    make_tex(&color_tex_, GL_RGBA16F, GL_RGBA, w, h);
    make_tex(&depth_tex_, GL_R32F, GL_RED, w, h);
    make_tex(&normal_tex_, GL_RGBA16F, GL_RGBA, w, h);

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, depth_tex_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, normal_tex_, 0);
    const GLenum bufs[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    glDrawBuffers(3, bufs);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
        << "SSR 降采样 FBO 不完整（" << w << "×" << h << "）";
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SsrDownsample::Run(unsigned int full_color_tex, unsigned int full_depth_tex,
                        unsigned int full_attr_tex, int full_w, int full_h, int scale) {
    CHECK(shader_mgr_ != nullptr) << "SsrDownsample::Run 前必须先 Init";
    CHECK_NE(full_color_tex, 0u);
    CHECK_NE(full_depth_tex, 0u);
    CHECK_NE(full_attr_tex, 0u);
    CHECK_GT(full_w, 0);
    CHECK_GT(full_h, 0);
    CHECK_GE(scale, 1);
    CHECK_LE(scale, 4) << "降采样倍率只支持 1..4（resolution_scale = 1/N）";

    int w = 0;
    int h = 0;
    DownsampleTargetSize(full_w, full_h, scale, &w, &h);

    GLint prev_fbo = 0;
    GLint prev_viewport[4] = {0, 0, 0, 0};
    GLint prev_active = 0;
    GLint prev_prog = 0;
    GLint prev_blend = 0;
    GLint prev_depth = 0;
    GLint prev_cull = 0;
    GLint prev_scissor = 0;
    GLint prev_bind[kTexUnitCount] = {0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    prev_blend = glIsEnabled(GL_BLEND);
    prev_depth = glIsEnabled(GL_DEPTH_TEST);
    prev_cull = glIsEnabled(GL_CULL_FACE);
    prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    for (int u = 0; u < kTexUnitCount; ++u) {
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

    glActiveTexture(GL_TEXTURE0 + kTexUnitColor);
    glBindTexture(GL_TEXTURE_2D, full_color_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitDepth);
    glBindTexture(GL_TEXTURE_2D, full_depth_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitAttr);
    glBindTexture(GL_TEXTURE_2D, full_attr_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uColor"), kTexUnitColor);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uDepth"), kTexUnitDepth);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uAttr"), kTexUnitAttr);
    glUniform2i(shader_mgr_->GetUniform(prog_, "uFullRes"), full_w, full_h);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uScale"), scale);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // 恢复调用方状态
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glUseProgram(static_cast<GLuint>(prev_prog));
    if (prev_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (prev_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prev_cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    for (int u = 0; u < kTexUnitCount; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_bind[u]));
    }
    glActiveTexture(static_cast<GLenum>(prev_active));
}

}  // namespace jpov
