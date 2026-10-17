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
    glGenVertexArrays(1, &vao_);
    CHECK_NE(vao_, 0u);
    initialized_ = true;
}

void ParticleRenderer::Finalize() {
    wbo_add_.Destroy();
    wbo_over_.Destroy();
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    initialized_ = false;
}

void ParticleRenderer::Begin(int full_w, int full_h) {
    CHECK(initialized_) << "ParticleRenderer::Begin: 未 Init";
    const int hw = std::max(1, static_cast<int>(full_w * kScale));
    const int hh = std::max(1, static_cast<int>(full_h * kScale));
    wbo_add_.Ensure(hw, hh);
    wbo_over_.Ensure(hw, hh);
    wbo_add_.BindForDraw();
    wbo_add_.Clear();
    wbo_over_.BindForDraw();
    wbo_over_.Clear();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    has_add_ = false;
    has_over_ = false;
}

void ParticleRenderer::Draw(const ParticleRenderCommand& cmd,
                            TextureManager& texture_mgr,
                            const Camera& camera,
                            const float view_proj[16],
                            unsigned int scene_depth_tex,
                            int full_w, int full_h,
                            const ParticleLighting& light) {
    CHECK(initialized_) << "ParticleRenderer::Draw: 未 Init";
    if (cmd.particles.empty()) {
        return;
    }
    CHECK_NE(scene_depth_tex, 0u) << "ParticleRenderer::Draw: scene_depth_tex 不能为 0";

    const int hw = std::max(1, static_cast<int>(full_w * kScale));
    const int hh = std::max(1, static_cast<int>(full_h * kScale));
    const bool additive = cmd.blend == BlendMode::kAdditive;
    WboBuffer& target = additive ? wbo_add_ : wbo_over_;
    target.Ensure(hw, hh);

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
    // 绑 FBO + viewport + 三 draw buffer + 对应 blend 语义（WboBuffer 统一负责）。
    if (additive) {
        target.BindForAdditiveDraw();   // reveal 不写：加性层不遮挡
    } else {
        target.BindForDraw();
    }

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
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uAdditive"), additive ? 1 : 0);
    glUniform2f(shader_mgr_->GetUniform(prog_draw_, "uViewport"), static_cast<float>(hw),
                static_cast<float>(hh));
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uNear"), camera.near);
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uFar"), camera.far);

    // ── 光照（只被遮挡类 = 烟消费；加性类在 FS 里提前 return，不会走到着色）──
    glUniform3f(shader_mgr_->GetUniform(prog_draw_, "uAmbientColor"),
                light.ambient_color.r, light.ambient_color.g, light.ambient_color.b);
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uAmbientIntensity"),
                light.ambient_intensity);
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uHasSun"),
                light.sun.has_value() ? 1 : 0);
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uForwardScatter"),
                light.forward_scatter);
    if (light.sun.has_value()) {
        // sun.direction = 光的**传播**方向；着色要的是**指向光源**的方向。
        glUniform3f(shader_mgr_->GetUniform(prog_draw_, "uSunDir"),
                    -light.sun->direction.x(), -light.sun->direction.y(),
                    -light.sun->direction.z());
        glUniform3f(shader_mgr_->GetUniform(prog_draw_, "uSunColor"),
                    light.sun->color.r, light.sun->color.g, light.sun->color.b);
        glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uSunIntensity"),
                    light.sun->intensity);
    }

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
    target.RestoreStandardBlend();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPopAttrib();

    if (additive) {
        has_add_ = true;
    } else {
        has_over_ = true;
    }
}

void ParticleRenderer::AppendLayers(TransparentLayerSet* layers /*output*/) const {
    CHECK(layers != nullptr);
    if (has_add_ && wbo_add_.valid()) {
        TransparentLayer add = wbo_add_.layer();
        add.scale = kScale;
        add.kind = TransparentLayerKind::kAdditive;
        layers->Add(add);
    }
    if (has_over_ && wbo_over_.valid()) {
        TransparentLayer over = wbo_over_.layer();
        over.scale = kScale;
        over.kind = TransparentLayerKind::kOver;
        layers->Add(over);
    }
}

}  // namespace jpov
