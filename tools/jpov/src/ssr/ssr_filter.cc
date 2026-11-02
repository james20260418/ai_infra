// JPOV SSR — 粗糙度预滤波实现。设计与三条细节见 ssr_filter.h 顶部。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/ssr/ssr_filter.h"

#include <algorithm>
#include <cmath>

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

// level 0：把 (radiance, type) 变成 (rgb, coverage)，并给空像素填一个"邻居的颜色"。
// 填充只改 rgb、不改 a ⇒ 覆盖率语义不变（空像素仍然不是命中）。
constexpr const char* kSsrFilterFillFs = R"glsl(
#version 330 core
uniform sampler2D  uRadiance;
uniform usampler2D uType;
uniform ivec2      uRes;

layout(location = 0) out vec4 FragColor;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 hi = uRes - 1;
    bool hit = (texelFetch(uType, p, 0).r == 1u);   // 只有 kHit 算"有答案"
    vec3 c = texelFetch(uRadiance, p, 0).rgb;
    float a = hit ? 1.0 : 0.0;
    if (!hit) {
        // **5×5** 里最近的命中（不是 3×3）：填充半径必须 ≥ 2，否则下一级 mip 的
        // "整块无覆盖"分支平均到的仍是"半亮"的填充色 —— 覆盖率边界上会留下一圈
        // 约 25% 的暗边（3×3 版本实测就是这样）。5×5 的 25 次取色只在 SSR 分辨率
        // 上每帧跑一次，成本可以忽略，而它刚好覆盖 2×2 降采样块的斜对角。
        // 距离相同时按 (j, i) 扫描顺序取第一份（确定性，无随机）。
        float best_d = 1.0e9;
        for (int j = -2; j <= 2; ++j) {
            for (int i = -2; i <= 2; ++i) {
                ivec2 q = clamp(p + ivec2(i, j), ivec2(0), hi);
                if (texelFetch(uType, q, 0).r != 1u) {
                    continue;
                }
                float d = float(i * i + j * j);
                if (d < best_d) {
                    best_d = d;
                    c = texelFetch(uRadiance, q, 0).rgb;
                }
            }
        }
    }
    FragColor = vec4(c, a);
}
)glsl";

// level n → n+1：2×2 box，颜色按覆盖率加权（细节 ①），覆盖率取平均（细节 ③）。
constexpr const char* kSsrFilterDownFs = R"glsl(
#version 330 core
uniform sampler2D uSrc;

layout(location = 0) out vec4 FragColor;

void main() {
    ivec2 src_res = textureSize(uSrc, 0);
    ivec2 q = ivec2(gl_FragCoord.xy) * 2;
    ivec2 hi = src_res - 1;
    vec4 s0 = texelFetch(uSrc, clamp(q + ivec2(0, 0), ivec2(0), hi), 0);
    vec4 s1 = texelFetch(uSrc, clamp(q + ivec2(1, 0), ivec2(0), hi), 0);
    vec4 s2 = texelFetch(uSrc, clamp(q + ivec2(0, 1), ivec2(0), hi), 0);
    vec4 s3 = texelFetch(uSrc, clamp(q + ivec2(1, 1), ivec2(0), hi), 0);

    float sum_a = s0.a + s1.a + s2.a + s3.a;
    vec3 sum_c = s0.rgb * s0.a + s1.rgb * s1.a + s2.rgb * s2.a + s3.rgb * s3.a;
    vec3 avg_c = 0.25 * (s0.rgb + s1.rgb + s2.rgb + s3.rgb);
    // sum_a > 0 ⇒ 只平均有答案的那几格（否则黑像素会把颜色拉暗）；
    // sum_a == 0 ⇒ 整块都没答案，但填充色仍要往下传（下一级双线性才不会插到黑）。
    vec3 rgb = (sum_a > 0.0) ? (sum_c / sum_a) : avg_c;
    FragColor = vec4(rgb, sum_a * 0.25);
}
)glsl";

constexpr int kTexUnitRadiance = 0;
constexpr int kTexUnitType = 1;
constexpr int kTexUnitSrc = 0;
constexpr int kTexUnitCount = 2;

int ComputeLevelCount(int w, int h) {
    int m = std::max(w, h);
    int levels = 1;
    while (m > 1) {
        m >>= 1;
        ++levels;
    }
    return levels;
}

int LevelSize(int base, int level) {
    return std::max(1, base >> level);
}

}  // namespace

SsrFilter::SsrFilter() = default;
SsrFilter::~SsrFilter() { Finalize(); }

void SsrFilter::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr);
    shader_mgr_ = shader_mgr;
    prog_fill_ = shader_mgr_->GetOrCreate(
        "ssr_filter_fill", {kSsrFullscreenVs, kSsrFilterFillFs, {}});
    prog_down_ = shader_mgr_->GetOrCreate(
        "ssr_filter_down", {kSsrFullscreenVs, kSsrFilterDownFs, {}});
    CHECK_NE(prog_fill_, 0u);
    CHECK_NE(prog_down_, 0u);
}

void SsrFilter::Finalize() {
    DestroyTargets();
    prog_fill_ = 0;
    prog_down_ = 0;
    shader_mgr_ = nullptr;
}

void SsrFilter::DestroyTargets() {
    for (unsigned int tex : level_tex_) {
        if (tex != 0) {
            glDeleteTextures(1, &tex);
        }
    }
    level_tex_.clear();
    if (fbo_src_ != 0) {
        glDeleteFramebuffers(1, &fbo_src_);
        fbo_src_ = 0;
    }
    if (fbo_dst_ != 0) {
        glDeleteFramebuffers(1, &fbo_dst_);
        fbo_dst_ = 0;
    }
    if (mip_tex_ != 0) {
        glDeleteTextures(1, &mip_tex_);
        mip_tex_ = 0;
    }
    levels_ = 0;
    width_ = 0;
    height_ = 0;
}

void SsrFilter::EnsureTargets(int w, int h) {
    if (mip_tex_ != 0 && width_ == w && height_ == h) {
        return;
    }
    DestroyTargets();
    width_ = w;
    height_ = h;
    levels_ = ComputeLevelCount(w, h);

    glGenTextures(1, &mip_tex_);
    glBindTexture(GL_TEXTURE_2D, mip_tex_);
    for (int l = 0; l < levels_; ++l) {
        glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA16F, LevelSize(w, l), LevelSize(h, l), 0,
                     GL_RGBA, GL_FLOAT, nullptr);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // 手工定义每一级 ⇒ 必须显式声明最大值，否则"未定义完整 mip 链"的纹理是不完整的：
    // 采样返回黑且**不报错**（这条坑在天空 cubemap 那边也写过一次）。
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels_ - 1);
    glBindTexture(GL_TEXTURE_2D, 0);

    level_tex_.resize(levels_, 0);
    for (int l = 0; l < levels_; ++l) {
        glGenTextures(1, &level_tex_[l]);
        glBindTexture(GL_TEXTURE_2D, level_tex_[l]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, LevelSize(w, l), LevelSize(h, l), 0,
                     GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &fbo_src_);
    glGenFramebuffers(1, &fbo_dst_);
}

void SsrFilter::Run(unsigned int ssr_radiance_tex, unsigned int ssr_type_tex, int w, int h) {
    CHECK(shader_mgr_ != nullptr) << "SsrFilter::Run 前必须先 Init";
    CHECK_NE(ssr_radiance_tex, 0u);
    CHECK_NE(ssr_type_tex, 0u);
    CHECK_GT(w, 0);
    CHECK_GT(h, 0);

    GLint prev_read_fbo = 0;
    GLint prev_draw_fbo = 0;
    GLint prev_viewport[4] = {0, 0, 0, 0};
    GLint prev_prog = 0;
    GLint prev_active = 0;
    GLint prev_blend = 0;
    GLint prev_depth = 0;
    GLint prev_cull = 0;
    GLint prev_scissor = 0;
    GLint prev_bind[kTexUnitCount] = {0, 0};
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    prev_blend = glIsEnabled(GL_BLEND);
    prev_depth = glIsEnabled(GL_DEPTH_TEST);
    prev_cull = glIsEnabled(GL_CULL_FACE);
    prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    for (int u = 0; u < kTexUnitCount; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_bind[u]);
    }

    EnsureTargets(w, h);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_src_);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    // ── level 0 ──
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, level_tex_[0], 0);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
        << "SSR 预滤波 FBO 不完整（level 0，" << w << "×" << h << "）";
    glViewport(0, 0, w, h);
    glUseProgram(prog_fill_);
    glActiveTexture(GL_TEXTURE0 + kTexUnitRadiance);
    glBindTexture(GL_TEXTURE_2D, ssr_radiance_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_fill_, "uRadiance"), kTexUnitRadiance);
    // 类型图是 usampler2D：单独占单元 + 单独 glUniform1i（"基址+下标"的坑）。
    glActiveTexture(GL_TEXTURE0 + kTexUnitType);
    glBindTexture(GL_TEXTURE_2D, ssr_type_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_fill_, "uType"), kTexUnitType);
    glUniform2i(shader_mgr_->GetUniform(prog_fill_, "uRes"), w, h);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE0 + kTexUnitType);
    glBindTexture(GL_TEXTURE_2D, 0);

    // ── level 1..n ──（读写不同纹理，无反馈环）
    glUseProgram(prog_down_);
    glUniform1i(shader_mgr_->GetUniform(prog_down_, "uSrc"), kTexUnitSrc);
    for (int l = 1; l < levels_; ++l) {
        const int lw = LevelSize(w, l);
        const int lh = LevelSize(h, l);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               level_tex_[l], 0);
        CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER),
                 static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
            << "SSR 预滤波 FBO 不完整（level " << l << "，" << lw << "×" << lh << "）";
        glViewport(0, 0, lw, lh);
        glActiveTexture(GL_TEXTURE0 + kTexUnitSrc);
        glBindTexture(GL_TEXTURE_2D, level_tex_[l - 1]);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // ── 拼进 mip_tex_ 的各 level（blit：读端写端是两个不同 FBO，无反馈环）──
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_src_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_dst_);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    for (int l = 0; l < levels_; ++l) {
        const int lw = LevelSize(w, l);
        const int lh = LevelSize(h, l);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_src_);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               level_tex_[l], 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_dst_);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               mip_tex_, l);
        CHECK_EQ(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER),
                 static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
            << "SSR 预滤波的 mip 目标 FBO 不完整（level " << l << "）";
        glBlitFramebuffer(0, 0, lw, lh, 0, 0, lw, lh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    // 拼完把 dst 上的挂载摘掉，避免下一帧误用（句柄还在，只是不再挂在 FBO 上）。
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_read_fbo));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prev_draw_fbo));

    // 恢复调用方状态
    for (int u = 0; u < kTexUnitCount; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_bind[u]));
    }
    glActiveTexture(static_cast<GLenum>(prev_active));
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glUseProgram(static_cast<GLuint>(prev_prog));
    if (prev_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (prev_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prev_cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
}

}  // namespace jpov
