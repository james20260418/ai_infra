// JPOV SSR — 同面并集柱体实现。设计与判据见 ssr_slab_back.h 顶部。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/ssr/ssr_slab_back.h"

#include <algorithm>
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

#include "tools/jpov/src/shader_manager.h"
#include "tools/jpov/src/ssr/ssr_shader.h"

namespace jpov {
namespace {

// SSR 分辨率一次全屏 pass：输入 w（降采样后的 1/z，0 = 天空）+ 视空间法线，
// 输出每格的 back（w 空间，R32F）。
//
// 同面判定与融合层共用 kSsrSameSurfaceGlsl：用本格 (P, 法线) 造切平面，预测邻居格中心处
// 的 w，看残差。必须**双向判**（我判它 + 它判我）：单侧判会让"较近那一格"漏并，缝隙照样
// 留着。合并**单侧**：back = min(back, 邻居 front)，永远不动 front ⇒ 不会造假近面。
constexpr const char* kSsrSlabFs = R"glsl(
uniform sampler2D uW;
uniform sampler2D uNormal;
uniform ivec2 uRes;
uniform float uMinThicknessRel;
uniform float uGateRel;
uniform float uGateCos;
uniform float uTanHalf;
uniform float uAspectTan;   // = uAspect * uTanHalf（预算好，省一次乘法）
uniform vec2  uInvRes;      // 1/uRes（同上，把角射线的除法换成乘法）
out vec4 FragColor;

// 同面判定（2026-11-13 去重）：调用方预算好角射线，这里只做平面预测 + 判据。
// 复用 c_self/P_self 是本 pass 的主要优化点：原来每次比较都重算两条角射线（4 邻居 × 2 向）。
bool SameSurfacePre(vec3 n_self, vec3 P_self, vec3 c_self,
                    float w_nb, vec3 n_nb, vec3 c_nb) {
    if (!(w_nb > 0.0)) {
        return false;
    }
    // 法线门槛在共享判据里再查一次（这里先查是为了省掉整个平面预测）。
    if (dot(n_self, n_nb) < uGateCos) {
        return false;
    }
    float t = SsrPlaneViewDist(n_self, P_self, c_nb);
    if (!(t > 0.0)) {
        return false;
    }
    return SsrSameSurface(1.0 / t, w_nb, n_self, n_nb, uGateRel, uGateCos);
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float w = texelFetch(uW, p, 0).r;
    float base = w * (1.0 - uMinThicknessRel);
    if (!(w > 0.0)) {
        FragColor = vec4(0.0);
        return;
    }
    vec3 n = normalize(texelFetch(uNormal, p, 0).xyz);
    if (!(dot(n, n) > 0.5)) {          // 没有法线数据 ⇒ 只能给基线厚度
        FragColor = vec4(base, 0.0, 0.0, 1.0);
        return;
    }
    float back = base;
    // self 的角射线与平面点只算一次（原来每次比较都重算，4 邻居 × 2 向 = 8 次）
    vec3 c_self = SsrCellRay(p, uInvRes, uAspectTan, uTanHalf);
    vec3 P_self = c_self / w;
    ivec2 off[4] = ivec2[4](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1));
    for (int i = 0; i < 4; ++i) {
        ivec2 q = clamp(p + off[i], ivec2(0), uRes - 1);
        float w_nb = texelFetch(uW, q, 0).r;
        if (!(w_nb > 0.0)) {
            continue;
        }
        vec3 n_nb = normalize(texelFetch(uNormal, q, 0).xyz);
        vec3 c_nb = SsrCellRay(q, uInvRes, uAspectTan, uTanHalf);
        bool fwd = SameSurfacePre(n, P_self, c_self, w_nb, n_nb, c_nb);
        // 反向：用邻居的平面预测"我"的位置（单侧判会让较近那格漏并）
        bool rev = false;
        if (!fwd) {
            rev = SameSurfacePre(n_nb, c_nb / w_nb, c_nb, w, n, c_self);
        }
        if ((fwd || rev) && w_nb < back) {
            back = w_nb;
        }
    }
    FragColor = vec4(back, 0.0, 0.0, 1.0);
}
)glsl";

constexpr int kTexUnitW = 0;
constexpr int kTexUnitNormal = 1;
constexpr int kTexUnitCount = 2;

}  // namespace

SsrSlabBack::SsrSlabBack() = default;
SsrSlabBack::~SsrSlabBack() { Finalize(); }

void SsrSlabBack::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr);
    if (prog_ != 0) {
        Finalize();
    }
    shader_mgr_ = shader_mgr;
    const std::string src = std::string(kSsrGlslVersion) + kSsrSameSurfaceGlsl + kSsrSlabFs;
    prog_ = shader_mgr_->GetOrCreate("ssr_slab_back", {kSsrFullscreenVs, src.c_str(), {}});
    CHECK_NE(prog_, 0u);
}

void SsrSlabBack::Finalize() {
    DestroyTarget();
    prog_ = 0;
    shader_mgr_ = nullptr;
}

void SsrSlabBack::DestroyTarget() {
    if (fbo_ != 0) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    if (tex_ != 0) {
        glDeleteTextures(1, &tex_);
        tex_ = 0;
    }
    width_ = 0;
    height_ = 0;
}

void SsrSlabBack::EnsureTarget(int w, int h) {
    if (w == width_ && h == height_ && fbo_ != 0) {
        return;
    }
    DestroyTarget();
    width_ = w;
    height_ = h;

    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    // R32F：只存 back（front 就是 w_tex 本身）。线性深度比 G16F 稳得多。
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, w, h, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_, 0);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
        << "SSR slab FBO 不完整";
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
}

unsigned int SsrSlabBack::Run(const Inputs& in) {
    CHECK(shader_mgr_ != nullptr) << "SsrSlabBack::Run 前必须先 Init";
    CHECK_NE(in.w_tex, 0u) << "SsrSlabBack::Inputs::w_tex 未设置";
    CHECK_NE(in.normal_tex, 0u) << "SsrSlabBack::Inputs::normal_tex 未设置";
    CHECK_GT(in.width, 0);
    CHECK_GT(in.height, 0);
    CHECK_GT(in.tan_half_fov_y, 0.0f);
    CHECK_GT(in.aspect, 0.0f);

    EnsureTarget(in.width, in.height);

    GLint prev_fbo = 0;
    GLint prev_viewport[4] = {0, 0, 0, 0};
    GLint prev_program = 0;
    GLint prev_active = 0;
    GLint prev_blend = 0;
    GLint prev_depth = 0;
    GLint prev_cull = 0;
    GLint prev_scissor = 0;
    GLint prev_bind[kTexUnitCount] = {0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    prev_blend = glIsEnabled(GL_BLEND);
    prev_depth = glIsEnabled(GL_DEPTH_TEST);
    prev_cull = glIsEnabled(GL_CULL_FACE);
    prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    for (int u = 0; u < kTexUnitCount; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_bind[u]);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, in.width, in.height);
    // 全屏三角形的四个开关必须自己关：本类的调用方是装置，装置对 GL 状态**不做任何假设**
    // （它可能刚被主渲染链路调用，depth/blend/scissor 都开着）。
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(prog_);
    auto set1f = [&](const char* name, float v) {
        const int loc = shader_mgr_->GetUniform(prog_, name);
        if (loc >= 0) {
            glUniform1f(loc, v);
        }
    };
    glActiveTexture(GL_TEXTURE0 + kTexUnitW);
    glBindTexture(GL_TEXTURE_2D, in.w_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uW"), kTexUnitW);
    glActiveTexture(GL_TEXTURE0 + kTexUnitNormal);
    glBindTexture(GL_TEXTURE_2D, in.normal_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uNormal"), kTexUnitNormal);
    glUniform2i(shader_mgr_->GetUniform(prog_, "uRes"), in.width, in.height);
    // 防御式钳位：配置来自面板/调用方，越界值会让柱体退化成 0 厚（全不命中）或误判。
    set1f("uMinThicknessRel", std::min(std::max(in.min_thickness_rel, 0.0f), 0.5f));
    set1f("uGateRel", std::min(std::max(in.gate_rel, 1.0e-4f), 1.0f));
    set1f("uGateCos", std::min(std::max(in.gate_cos, 0.0f), 1.0f));
    set1f("uTanHalf", in.tan_half_fov_y);
    set1f("uAspectTan", in.aspect * in.tan_half_fov_y);
    glUniform2f(shader_mgr_->GetUniform(prog_, "uInvRes"), 1.0f / static_cast<float>(in.width),
                1.0f / static_cast<float>(in.height));
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glUseProgram(static_cast<GLuint>(prev_program));
    for (int u = 0; u < kTexUnitCount; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_bind[u]));
    }
    glActiveTexture(static_cast<GLenum>(prev_active));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    if (prev_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (prev_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prev_cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    return tex_;
}

}  // namespace jpov
