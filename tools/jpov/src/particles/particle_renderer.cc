// JPOV 广告牌粒子渲染器实现。见 particle_renderer.h。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/particles/particle_renderer.h"

#include <algorithm>
#include <cmath>
#include <vector>

// GL 头文件必须最先 include（在 MinGW #define 宏替换之前）。
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

// 粒子 flipbook 纹理单元：粒子是**独立 pass**（在不透明 3D 之后），此刻主链路的
// 材质/tile/shadow 单元都不活跃，故复用 unit 0（同后处理 pass 的惯例）。
constexpr int kParticleTexUnit = 0;

Vec3f Sub(const Vec3f& a, const Vec3f& b) {
    return Vec3f(a.x() - b.x(), a.y() - b.y(), a.z() - b.z());
}

Vec3f CrossVec(const Vec3f& a, const Vec3f& b) {
    return Vec3f(a.y() * b.z() - a.z() * b.y(),
                 a.z() * b.x() - a.x() * b.z(),
                 a.x() * b.y() - a.y() * b.x());
}

float DotVec(const Vec3f& a, const Vec3f& b) {
    return a.x() * b.x() + a.y() * b.y() + a.z() * b.z();
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

ParticleRenderer::~ParticleRenderer() {
    // GL 资源应经 Finalize 释放（析构可能在 GL 上下文已销毁之后）。
    // 这里只做断言提示：仍持有 GL 名字说明调用方漏了 Finalize。
    // （不在此 glDelete，避免无上下文时崩溃。）
}

void ParticleRenderer::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr) << "ParticleRenderer::Init: shader_mgr 不能为空";
    CHECK(!initialized_) << "ParticleRenderer::Init 重复调用";
    shader_mgr_ = shader_mgr;
    prog_ = shader_mgr_->GetOrCreate("particle_billboard", {kParticleVs, kParticleFs});
    glGenVertexArrays(1, &vao_);
    CHECK_NE(vao_, 0u) << "ParticleRenderer::Init: glGenVertexArrays 失败";
    initialized_ = true;
}

void ParticleRenderer::Finalize() {
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    initialized_ = false;
}

void ParticleRenderer::Draw(const ParticleRenderCommand& cmd,
                            TextureManager& texture_mgr,
                            const Camera& camera,
                            const float view_proj[16],
                            bool mrt) {
    CHECK(initialized_) << "ParticleRenderer::Draw: 未 Init";
    if (cmd.particles.empty()) {
        return;
    }

    // ---- 相机基向量（广告牌朝向用）----
    const Vec3f cam_pos = camera.position;
    const Vec3f world_up = Vec3f(0.0f, 1.0f, 0.0f);
    const Vec3f forward = NormalizeOr(Sub(camera.target, camera.position), Vec3f(0.0f, 0.0f, -1.0f));
    Vec3f right = CrossVec(forward, NormalizeOr(camera.up, world_up));
    right = NormalizeOr(right, Vec3f(1.0f, 0.0f, 0.0f));
    const Vec3f up = CrossVec(right, forward);

    // ---- 排序：半透明层按视深「背→前」（远者先画）----
    const size_t n = cmd.particles.size();
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i) {
        order[i] = i;
    }
    if (cmd.sort == SortMode::kByDepth) {
        std::vector<float> depth(n);
        for (size_t i = 0; i < n; ++i) {
            depth[i] = DotVec(Sub(cmd.particles[i].position, cam_pos), forward);
        }
        std::sort(order.begin(), order.end(),
                  [&depth](size_t a, size_t b) { return depth[a] > depth[b]; });
    }

    // ---- 组织 per-instance 数据 ----
    std::vector<float> pos_data;
    std::vector<float> misc_data;
    std::vector<float> color_data;
    std::vector<float> vel_data;
    pos_data.reserve(n * 3);
    misc_data.reserve(n * 4);
    color_data.reserve(n * 4);
    vel_data.reserve(n * 3);
    for (size_t k = 0; k < n; ++k) {
        const ParticleInstance& p = cmd.particles[order[k]];
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

    // ---- GL 状态 ----
    glPushAttrib(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LESS);
    glEnable(GL_BLEND);
    if (cmd.blend == BlendMode::kAdditive) {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    } else {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    // MRT 目标：只写颜色附件0，避免把透明粒子深度写进场景深度附件。
    if (mrt) {
        const GLenum only_color[1] = {GL_COLOR_ATTACHMENT0};
        glDrawBuffers(1, only_color);
    }

    glUseProgram(prog_);
    glUniformMatrix4fv(shader_mgr_->GetUniform(prog_, "uViewProj"), 1, GL_FALSE, view_proj);
    glUniform3f(shader_mgr_->GetUniform(prog_, "uCamRight"), right.x(), right.y(), right.z());
    glUniform3f(shader_mgr_->GetUniform(prog_, "uCamUp"), up.x(), up.y(), up.z());
    glUniform3f(shader_mgr_->GetUniform(prog_, "uCamPos"),
                cam_pos.x(), cam_pos.y(), cam_pos.z());
    glUniform3f(shader_mgr_->GetUniform(prog_, "uWorldUp"), world_up.x(), world_up.y(), world_up.z());
    glUniform1i(shader_mgr_->GetUniform(prog_, "uAlign"), static_cast<int>(cmd.align));
    glUniform1i(shader_mgr_->GetUniform(prog_, "uStretched"), cmd.stretched ? 1 : 0);
    glUniform1f(shader_mgr_->GetUniform(prog_, "uStretchScale"), cmd.stretch_scale);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uTilesX"), cmd.tiles_x);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uTilesY"), cmd.tiles_y);
    glUniform1i(shader_mgr_->GetUniform(prog_, "uFlipbook"), cmd.flipbook ? 1 : 0);

    if (cmd.texture_id != 0) {
        const unsigned int gl_tex = texture_mgr.GetGLTexture(cmd.texture_id);
        CHECK_NE(gl_tex, 0u) << "ParticleRenderer::Draw: texture_id " << cmd.texture_id
                             << " 未注册";
        glActiveTexture(GL_TEXTURE0 + kParticleTexUnit);
        glBindTexture(GL_TEXTURE_2D, gl_tex);
        glUniform1i(shader_mgr_->GetUniform(prog_, "uTex"), kParticleTexUnit);
        glUniform1i(shader_mgr_->GetUniform(prog_, "uHasTex"), 1);
    } else {
        glUniform1i(shader_mgr_->GetUniform(prog_, "uHasTex"), 0);
    }

    {
        InstanceBufferBinding bind(vao_, {&pos_buf_, &misc_buf_, &color_buf_, &vel_buf_});
        glBindVertexArray(vao_);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(n));
        glBindVertexArray(0);
    }

    // 恢复 draw buffers（若改过）。
    if (mrt) {
        const GLenum both[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(2, both);
    }
    glPopAttrib();
}

}  // namespace jpov
