// JPOV ParticleRenderer 实现（WBO 分级纹理）。见 particle_renderer.h。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/particles/particle_renderer.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#ifdef _WIN32
#undef near
#undef far
#endif

#include <glog/logging.h>

namespace jpov {

namespace {

// 粒子 WBO pass 是独立 pass（不透明 3D 之后），单元 0 空闲（同 #176 的惯例）。
constexpr int kSceneDepthUnit = 0;
constexpr int kParticleTexUnit = 1;

Vec3f Sub(const Vec3f& a, const Vec3f& b) {
    return Vec3f(a.x() - b.x(), a.y() - b.y(), a.z() - b.z());
}
Vec3f CrossVec(const Vec3f& a, const Vec3f& b) {
    return Vec3f(a.y() * b.z() - a.z() * b.y(),
                 a.z() * b.x() - a.x() * b.z(),
                 a.x() * b.y() - a.y() * b.x());
}
Vec3f NormalizeOr(const Vec3f& v, const Vec3f& fallback) {
    const float n = v.Norm();
    if (n < 1e-6f) {
        return fallback;
    }
    return Vec3f(v.x() / n, v.y() / n, v.z() / n);
}

}  // namespace

ParticleRenderer::ParticleRenderer() = default;
ParticleRenderer::~ParticleRenderer() = default;

void ParticleRenderer::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr) << "ParticleRenderer::Init: shader_mgr 不能为空";
    CHECK(!initialized_) << "ParticleRenderer::Init 重复调用";
    shader_mgr_ = shader_mgr;
    prog_draw_ = shader_mgr_->GetOrCreate("particle_wbo_draw", {kVs, kFsWbo});
    prog_comp_ = shader_mgr_->GetOrCreate("particle_wbo_composite", {kVsComp, kFsComp});
    glGenVertexArrays(1, &vao_);
    CHECK_NE(vao_, 0u);
    initialized_ = true;
}

void ParticleRenderer::Finalize() {
    DestroyWbo();
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    initialized_ = false;
}

void ParticleRenderer::EnsureWbo(int w, int h) {
    if (wbo_fbo_ != 0 && wbo_w_ == w && wbo_h_ == h) {
        return;
    }
    CHECK_GT(w, 0);
    CHECK_GT(h, 0);
    DestroyWbo();

    glGenTextures(1, &accum_tex_);
    glBindTexture(GL_TEXTURE_2D, accum_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);   // 低分→高分上采样
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &reveal_tex_);
    glBindTexture(GL_TEXTURE_2D, reveal_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, w, h, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &depth_tex_);
    glBindTexture(GL_TEXTURE_2D, depth_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, w, h, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &wbo_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, wbo_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           accum_tex_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                           reveal_tex_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D,
                           depth_tex_, 0);
    const GLenum bufs[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
                            GL_COLOR_ATTACHMENT2};
    glDrawBuffers(3, bufs);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE)
        << "ParticleRenderer WBO FBO incomplete";
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    wbo_w_ = w;
    wbo_h_ = h;
}

void ParticleRenderer::DestroyWbo() {
    if (wbo_fbo_) { glDeleteFramebuffers(1, &wbo_fbo_); wbo_fbo_ = 0; }
    if (accum_tex_) { glDeleteTextures(1, &accum_tex_); accum_tex_ = 0; }
    if (reveal_tex_) { glDeleteTextures(1, &reveal_tex_); reveal_tex_ = 0; }
    if (depth_tex_) { glDeleteTextures(1, &depth_tex_); depth_tex_ = 0; }
    wbo_w_ = 0;
    wbo_h_ = 0;
}

void ParticleRenderer::Begin(int full_w, int full_h) {
    CHECK(initialized_) << "ParticleRenderer::Begin: 未 Init";
    const int hw = std::max(1, static_cast<int>(full_w * kScale));
    const int hh = std::max(1, static_cast<int>(full_h * kScale));
    EnsureWbo(hw, hh);
    glBindFramebuffer(GL_FRAMEBUFFER, wbo_fbo_);
    glViewport(0, 0, hw, hh);
    const GLfloat clr_accum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLfloat clr_reveal[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    const GLfloat clr_depth[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glClearBufferfv(GL_COLOR, 0, clr_accum);
    glClearBufferfv(GL_COLOR, 1, clr_reveal);
    glClearBufferfv(GL_COLOR, 2, clr_depth);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    has_content_ = false;
}

void ParticleRenderer::Draw(const ParticleRenderCommand& cmd,
                            TextureManager& texture_mgr,
                            const Camera& camera,
                            const float view_proj[16],
                            unsigned int scene_depth_tex,
                            int full_w, int full_h) {
    CHECK(initialized_) << "ParticleRenderer::Draw: 未 Init";
    if (cmd.particles.empty()) {
        return;
    }
    CHECK_NE(scene_depth_tex, 0u) << "ParticleRenderer::Draw: scene_depth_tex 不能为 0";

    const int hw = std::max(1, static_cast<int>(full_w * kScale));
    const int hh = std::max(1, static_cast<int>(full_h * kScale));
    EnsureWbo(hw, hh);

    // 相机基向量
    const Vec3f cam_pos = camera.position;
    const Vec3f world_up = Vec3f(0.0f, 1.0f, 0.0f);
    const Vec3f forward = NormalizeOr(Sub(camera.target, camera.position),
                                      Vec3f(0.0f, 0.0f, -1.0f));
    Vec3f right = NormalizeOr(CrossVec(forward, NormalizeOr(camera.up, world_up)),
                              Vec3f(1.0f, 0.0f, 0.0f));
    const Vec3f up = CrossVec(right, forward);

    // per-instance 数据（WBO 顺序无关 → 不排序）
    const size_t n = cmd.particles.size();
    std::vector<float> pos_data, misc_data, color_data, vel_data;
    pos_data.reserve(n * 3);
    misc_data.reserve(n * 4);
    color_data.reserve(n * 4);
    vel_data.reserve(n * 3);
    for (size_t k = 0; k < n; ++k) {
        const ParticleInstance& p = cmd.particles[k];
        pos_data.push_back(p.position.x());
        pos_data.push_back(p.position.y());
        pos_data.push_back(p.position.z());
        misc_data.push_back(p.width);
        misc_data.push_back(p.height);
        misc_data.push_back(p.rotation);
        misc_data.push_back(p.frame);
        color_data.push_back(p.color.r);
        color_data.push_back(p.color.g);
        color_data.push_back(p.color.b);
        color_data.push_back(p.color.a);
        vel_data.push_back(p.velocity.x());
        vel_data.push_back(p.velocity.y());
        vel_data.push_back(p.velocity.z());
    }
    pos_buf_.Upload(pos_data);
    misc_buf_.Upload(misc_data);
    color_buf_.Upload(color_data);
    vel_buf_.Upload(vel_data);

    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, wbo_fbo_);
    glViewport(0, 0, hw, hh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunci(0, GL_ONE, GL_ONE);                    // accum += 预乘色×w
    glBlendFunci(1, GL_ZERO, GL_ONE_MINUS_SRC_COLOR);   // reveal *= (1−α)
    glBlendFunci(2, GL_ONE, GL_ONE);                    // depth += z·α·w

    glUseProgram(prog_draw_);
    glUniformMatrix4fv(shader_mgr_->GetUniform(prog_draw_, "uViewProj"), 1, GL_FALSE,
                       view_proj);
    glUniform3f(shader_mgr_->GetUniform(prog_draw_, "uCamRight"), right.x(), right.y(), right.z());
    glUniform3f(shader_mgr_->GetUniform(prog_draw_, "uCamUp"), up.x(), up.y(), up.z());
    glUniform3f(shader_mgr_->GetUniform(prog_draw_, "uCamPos"), cam_pos.x(), cam_pos.y(), cam_pos.z());
    glUniform3f(shader_mgr_->GetUniform(prog_draw_, "uWorldUp"), world_up.x(), world_up.y(), world_up.z());
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uAlign"), static_cast<int>(cmd.align));
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uStretched"), cmd.stretched ? 1 : 0);
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uStretchScale"), cmd.stretch_scale);
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uTilesX"), cmd.tiles_x);
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uTilesY"), cmd.tiles_y);
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uFlipbook"), cmd.flipbook ? 1 : 0);
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uFrameBlend"), cmd.frame_blend ? 1 : 0);
    glUniform2f(shader_mgr_->GetUniform(prog_draw_, "uViewport"), static_cast<float>(hw),
                static_cast<float>(hh));
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uNear"), camera.near);
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uFar"), camera.far);

    glActiveTexture(GL_TEXTURE0 + kSceneDepthUnit);
    glBindTexture(GL_TEXTURE_2D, scene_depth_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uSceneDepth"), kSceneDepthUnit);

    if (cmd.texture_id != 0) {
        const unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.texture_id);
        CHECK_NE(gl_tex, 0u) << "ParticleRenderer::Draw: texture_id " << cmd.texture_id
                             << " 未注册";
        glActiveTexture(GL_TEXTURE0 + kParticleTexUnit);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uTex"), kParticleTexUnit);
        glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uHasTex"), 1);
    } else {
        glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uHasTex"), 0);
    }

    {
        InstanceBufferBinding bind(vao_, {&pos_buf_, &misc_buf_, &color_buf_, &vel_buf_});
        glBindVertexArray(vao_);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(n));
        glBindVertexArray(0);
    }

    // 复原 blend（非常规混合必须显式回写；GL_COLOR_BUFFER_BIT 的 pop 不一定覆盖 blend func）。
    glBlendFunci(0, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendFunci(1, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendFunci(2, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPopAttrib();

    has_content_ = true;
}

void ParticleRenderer::Composite(int full_w, int full_h) {
    if (!has_content_ || wbo_fbo_ == 0) {
        return;
    }
    glPushAttrib(GL_ENABLE_BIT);
    glViewport(0, 0, full_w, full_h);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // over（预乘 src）

    glUseProgram(prog_comp_);
    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, accum_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_comp_, "uAccum"), 0);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, reveal_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_comp_, "uReveal"), 1);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glPopAttrib();
    has_content_ = false;   // 本帧消费掉
}

}  // namespace jpov
