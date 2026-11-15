// JPOV WaterSkinRenderer 实现。见 water_skin_renderer.h。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/water/water_skin_renderer.h"

#include <cmath>

#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#ifdef _WIN32
// windows.h 把 near / far 定义成空宏，会炸 cam.near / cam.far（同 particle_renderer.cc）。
#undef near
#undef far
#endif

#include <glog/logging.h>

#include "tools/jpov/src/wbo/wbo_shader.h"
#include "tools/jpov/src/water/water_skin_shader.h"

namespace jpov {

namespace {

// 水面 pass 是独立的 1× 后处理 pass（不透明 3D 之后），纹理单元 0/1 空闲
//（与粒子 / 条带 / 体积火一致：这三个 pass 不同时活跃）。
constexpr int kSceneDepthUnit = 0;
constexpr int kEnvCubeUnit = 1;

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
    if (n < 1.0e-8f) {
        return fallback;
    }
    return Vec3f(v.x() / n, v.y() / n, v.z() / n);
}

}  // namespace

WaterSkinRenderer::WaterSkinRenderer() = default;
WaterSkinRenderer::~WaterSkinRenderer() = default;

void WaterSkinRenderer::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr) << "WaterSkinRenderer::Init: shader_mgr 不能为空";
    CHECK(!initialized_) << "WaterSkinRenderer::Init 重复调用";
    shader_mgr_ = shader_mgr;
    prog_draw_ = shader_mgr_->GetOrCreate("water_skin_draw",
                                          {water::kWaterSkinVs, water::kWaterSkinFs});
    prog_comp_ = shader_mgr_->GetOrCreate("water_skin_composite",
                                          {kWboCompositeVs, kWboCompositeFs});
    glGenVertexArrays(1, &vao_);
    CHECK_NE(vao_, 0u) << "WaterSkinRenderer::Init: glGenVertexArrays 失败";
    initialized_ = true;
}

void WaterSkinRenderer::Finalize() {
    wbo_.Destroy();
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    has_content_ = false;
    initialized_ = false;
}

void WaterSkinRenderer::EnsureTarget(int w, int h) {
    // 1× 层、不含加法集；**申请 depth 附件**（att3）—— 这是「水皮是一层」的全部凭据，
    // 下一步体积雾要按它做「雾段 + 层」的交替积分。线性过滤关掉：1× 上插值没有意义。
    wbo_.Ensure({w, h, /*with_additive=*/false, /*with_depth=*/true,
                 /*linear_filter=*/false});
}

void WaterSkinRenderer::Begin(int full_w, int full_h) {
    CHECK(initialized_) << "WaterSkinRenderer::Begin: 未 Init";
    CHECK_GT(full_w, 0) << "WaterSkinRenderer::Begin: full_w 必须 > 0";
    CHECK_GT(full_h, 0) << "WaterSkinRenderer::Begin: full_h 必须 > 0";
    EnsureTarget(full_w, full_h);
    wbo_.Clear();
    has_content_ = false;
}

void WaterSkinRenderer::UploadCommonUniforms(unsigned int prog, const Inputs& in) {
    const Camera& cam = in.camera;
    const Vec3f fwd = NormalizeOr(Sub(cam.target, cam.position), Vec3f(0.0f, 0.0f, -1.0f));
    const Vec3f side = NormalizeOr(CrossVec(fwd, cam.up), Vec3f(1.0f, 0.0f, 0.0f));
    const Vec3f upv = CrossVec(side, fwd);
    const float tan_half_fov = std::tan(0.5f * cam.fov * 3.14159265358979323846f / 180.0f);
    const float aspect = static_cast<float>(in.full_w) / static_cast<float>(in.full_h);

    glUniform2f(shader_mgr_->GetUniform(prog, "uViewport"),
                static_cast<float>(in.full_w), static_cast<float>(in.full_h));
    glUniform3f(shader_mgr_->GetUniform(prog, "uCamPos"),
                cam.position.x(), cam.position.y(), cam.position.z());
    glUniform3f(shader_mgr_->GetUniform(prog, "uCamFwd"), fwd.x(), fwd.y(), fwd.z());
    glUniform3f(shader_mgr_->GetUniform(prog, "uCamRight"), side.x(), side.y(), side.z());
    glUniform3f(shader_mgr_->GetUniform(prog, "uCamUp"), upv.x(), upv.y(), upv.z());
    glUniform1f(shader_mgr_->GetUniform(prog, "uTanHalfFov"), tan_half_fov);
    glUniform1f(shader_mgr_->GetUniform(prog, "uAspect"), aspect);
    glUniform1f(shader_mgr_->GetUniform(prog, "uNear"), cam.near);
    glUniform1f(shader_mgr_->GetUniform(prog, "uFar"), cam.far);
    glUniform1f(shader_mgr_->GetUniform(prog, "uLevelY"), in.level_y);
    glUniform1f(shader_mgr_->GetUniform(prog, "uIor"), in.ior);
    glUniform1f(shader_mgr_->GetUniform(prog, "uEnvScale"), in.env_scale);
    glUniform3f(shader_mgr_->GetUniform(prog, "uTirColor"),
                in.tir_color.x(), in.tir_color.y(), in.tir_color.z());
    glUniform1f(shader_mgr_->GetUniform(prog, "uHorizonClampSin"), water::kHorizonClampSin);
    glUniform1f(shader_mgr_->GetUniform(prog, "uDepthBiasM"), water::kDepthBiasM);
}

void WaterSkinRenderer::Draw(const Inputs& in) {
    CHECK(initialized_) << "WaterSkinRenderer::Draw: 未 Init";
    CHECK_NE(in.scene_depth_tex, 0u) << "WaterSkinRenderer::Draw: scene_depth_tex 不能为 0";
    CHECK_NE(in.env_cube_tex, 0u) << "WaterSkinRenderer::Draw: env_cube_tex 不能为 0";
    CHECK_GT(in.ior, 0.0f) << "WaterSkinRenderer::Draw: ior 必须 > 0";
    CHECK_GT(in.full_w, 0);
    CHECK_GT(in.full_h, 0);
    CHECK_EQ(in.full_w, wbo_.width()) << "WaterSkinRenderer::Draw: 与 Begin 的宽度不一致";
    CHECK_EQ(in.full_h, wbo_.height()) << "WaterSkinRenderer::Draw: 与 Begin 的高度不一致";

    wbo_.BindForDraw();
    // 全屏三角形：关剔除（NDC 绕序在部分驱动上会被判定为背面）。
    glDisable(GL_CULL_FACE);
    glUseProgram(prog_draw_);
    glBindVertexArray(vao_);

    UploadCommonUniforms(prog_draw_, in);

    glActiveTexture(GL_TEXTURE0 + kSceneDepthUnit);
    glBindTexture(GL_TEXTURE_2D, in.scene_depth_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uSceneDepth"), kSceneDepthUnit);
    glActiveTexture(GL_TEXTURE0 + kEnvCubeUnit);
    glBindTexture(GL_TEXTURE_CUBE_MAP, in.env_cube_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uEnvCube"), kEnvCubeUnit);

    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    wbo_.EndDraw();   // 还原 blend func（非常规混合必须显式还原）
    has_content_ = true;
}

void WaterSkinRenderer::Composite(int full_w, int full_h) {
    if (!has_content_ || !wbo_.valid()) {
        return;
    }
    CHECK_GT(full_w, 0);
    CHECK_GT(full_h, 0);

    // 本 pass 改 enable + blend func/equation：两者分属 GL_ENABLE_BIT 与 GL_COLOR_BUFFER_BIT，
    // 必须一起入栈（否则会把自定义混合泄漏给后续 pass）。
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT);
    glViewport(0, 0, full_w, full_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // over（预乘 src）

    glUseProgram(prog_comp_);
    glBindVertexArray(vao_);
    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, wbo_.accum_tex());
    glUniform1i(shader_mgr_->GetUniform(prog_comp_, "uAccum"), 0);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, wbo_.reveal_tex());
    glUniform1i(shader_mgr_->GetUniform(prog_comp_, "uReveal"), 1);
    glUniform1i(shader_mgr_->GetUniform(prog_comp_, "uAdditive"), 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glPopAttrib();   // 还原 enable / blend（含 equation）
    has_content_ = false;
}

}  // namespace jpov
