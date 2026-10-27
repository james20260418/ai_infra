// JPOV ParticleRenderer 实现（WBO 分级纹理）。见 particle_renderer.h。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/particles/particle_renderer.h"

#include "tools/jpov/src/wbo/wbo_shader.h"

#include <algorithm>
#include <cmath>
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

#ifdef _WIN32
#undef near
#undef far
#endif

#include <glog/logging.h>

namespace jpov {

namespace {

// 粒子 WBO pass 是独立 pass（不透明 3D 之后），单元 0 空闲（与条带 / 体积火一致）。
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
    // 权重函数来自共享件（wbo_shader.h）——粒子与条带/体积火必须是同一份数学。
    const std::string fs_draw =
        std::string(kFsWboHead) + kWboWeightGlsl + kFsWboTail;
    prog_draw_ = shader_mgr_->GetOrCreate("particle_wbo_draw", {kVs, fs_draw.c_str()});
    prog_comp_ = shader_mgr_->GetOrCreate("particle_wbo_composite",
                                          {kWboCompositeVs, kWboCompositeFs});
    glGenVertexArrays(1, &vao_);
    CHECK_NE(vao_, 0u);
    initialized_ = true;
}

void ParticleRenderer::Finalize() {
    wbo_.Destroy();
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    initialized_ = false;
}

void ParticleRenderer::EnsureTarget(int w, int h) {
    wbo_.Ensure({w, h, /*with_additive=*/true, /*with_depth=*/false,
                 /*linear_filter=*/true});
}

void ParticleRenderer::Begin(int full_w, int full_h) {
    CHECK(initialized_) << "ParticleRenderer::Begin: 未 Init";
    const int hw = std::max(1, static_cast<int>(full_w * kScale));
    const int hh = std::max(1, static_cast<int>(full_h * kScale));
    EnsureTarget(hw, hh);
    wbo_.Clear();
    has_content_ = false;
    has_alpha_ = false;
    has_additive_ = false;
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
    EnsureTarget(hw, hh);

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
    wbo_.BindForDraw();
    glDisable(GL_CULL_FACE);

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

    // 软深度（soft particles）：nullopt = 关闭（旧 2cm 硬判据），有值 = 按视空间距离淡出。
    // 关闭时仍给 uSoftDepthM 一个合法值：FS 里虽不取，但避免读未初始化 uniform。
    if (cmd.soft_depth_m.has_value()) {
        CHECK_GT(*cmd.soft_depth_m, 0.0f)
            << "ParticleRenderCommand::soft_depth_m 若启用必须 > 0（想关闭请置 nullopt）";
        glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uSoftDepthEnabled"), 1);
        glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uSoftDepthM"),
                    *cmd.soft_depth_m);
    } else {
        glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uSoftDepthEnabled"), 0);
        glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uSoftDepthM"), 1.0f);
    }

    // 混合路径：0 = α 覆盖集（吸收介质），2 = 加法集（发光介质）。
    // 老的「1 = 亮度当覆盖的加法近似」已删：发光体就该求和，近似既不物理也不省什么。
    const int blend_path = cmd.emissive ? 2 : 0;
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uBlendPath"), blend_path);

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

    wbo_.EndDraw();                 // 还原 blend + 解绑（非常规混合不能泄漏给后续 pass）
    glActiveTexture(GL_TEXTURE0);   // 归位：调用方后续按「unit 0 已激活」写 pass
    glPopAttrib();

    has_content_ = true;
    if (blend_path == 2) {
        has_additive_ = true;
    } else {
        has_alpha_ = true;
    }
}

void ParticleRenderer::Composite(int full_w, int full_h) {
    if (!has_content_ || !wbo_.valid()) {
        return;
    }
    // 本 pass 会改 enable + blend func/equation：两者分属 GL_ENABLE_BIT 与
    // GL_COLOR_BUFFER_BIT，必须**一起**入栈，否则会把自定义混合泄漏给后续 pass。
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT);
    glViewport(0, 0, full_w, full_h);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);

    glUseProgram(prog_comp_);
    glBindVertexArray(vao_);

    // ① 覆盖集：C=accum.rgb/accum.a、cov=1−reveal，预乘 over 进目标。
    if (has_alpha_) {
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // over（预乘 src）
        glActiveTexture(GL_TEXTURE0 + 0);
        glBindTexture(GL_TEXTURE_2D, wbo_.accum_tex());
        glUniform1i(shader_mgr_->GetUniform(prog_comp_, "uAccum"), 0);
        glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, wbo_.reveal_tex());
        glUniform1i(shader_mgr_->GetUniform(prog_comp_, "uReveal"), 1);
        glUniform1i(shader_mgr_->GetUniform(prog_comp_, "uAdditive"), 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    // ② 加法集：直接求和到目标之上（加法光不被覆盖集衰减 —— 单层 OIT 的固有近似）。
    if (has_additive_) {
        glBlendFunc(GL_ONE, GL_ONE);   // 求和
        glActiveTexture(GL_TEXTURE0 + 0);
        glBindTexture(GL_TEXTURE_2D, wbo_.additive_tex());
        glUniform1i(shader_mgr_->GetUniform(prog_comp_, "uAdd"), 0);
        glUniform1i(shader_mgr_->GetUniform(prog_comp_, "uAdditive"), 1);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glPopAttrib();          // 还原 enable / blend（含 equation）
    has_content_ = false;   // 本帧消费掉
    has_alpha_ = false;
    has_additive_ = false;
}

}  // namespace jpov
