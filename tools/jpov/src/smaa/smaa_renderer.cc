// JPOV SmaaRenderer 实现（SMAA 1x）。设计见 smaa_renderer.h。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/smaa/smaa_renderer.h"

#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include <glog/logging.h>

#include "tools/jpov/src/smaa/smaa_shader.h"
// SMAA 预计算查找纹理（参考实现 MIT 数据，见各自文件头）。面积纹理 160x560 RG8、
// 搜索纹理 64x16 R8；用它们各自的 *_WIDTH/*_HEIGHT/*_SIZE 宏保证与数据一致。
#include "tools/jpov/src/smaa/smaa_area_tex.h"
#include "tools/jpov/src/smaa/smaa_search_tex.h"

namespace jpov {

namespace {

// SMAA 内部目标的尺寸上限（与 Renderer::kMaxFboDim 一致；SMAA 目标尺寸 = viewport，
// 而 viewport ≤ 主 FBO ≤ kMaxFboDim，此处再守一道防御式上界）。
constexpr int kSmaaMaxDim = 4096;

// SMAA 是后处理 pass（在 bloom / tone map 之后、2D 之前），本 pass 独占 unit 0..2，
// 与 3D 主链路不重叠（见 texture_units.h 顶部「后处理 pass 自 unit 0 起」约定）。
constexpr int kTexUnitSmaaColor  = 0;
constexpr int kTexUnitSmaaEdges  = 1;
constexpr int kTexUnitSmaaArea   = 2;
constexpr int kTexUnitSmaaSearch = 3;

}  // namespace

SmaaRenderer::SmaaRenderer() = default;

SmaaRenderer::~SmaaRenderer() {
    Finalize();
}

void SmaaRenderer::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr);
    shader_mgr_ = shader_mgr;

    prog_edge_   = shader_mgr_->GetOrCreate("smaa_edge",   {kSmaaEdgeDetectVs, kSmaaEdgeDetectFs, {}});
    prog_weight_ = shader_mgr_->GetOrCreate("smaa_weight", {kSmaaBlendWeightVs, kSmaaBlendWeightFs, {}});
    prog_blend_  = shader_mgr_->GetOrCreate("smaa_blend",  {kSmaaNeighborBlendVs, kSmaaNeighborBlendFs, {}});

    UploadLookupTextures();
    initialized_ = true;
}

void SmaaRenderer::UploadLookupTextures() {
    // 面积纹理：160x560，每像素 2 字节（RG）。按参考 SMAA_AREATEX_SELECT = .rg 上传为 RG8。
    glGenTextures(1, &area_tex_);
    glBindTexture(GL_TEXTURE_2D, area_tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    CHECK_EQ(AREATEX_WIDTH * AREATEX_HEIGHT * 2, static_cast<int>(AREATEX_SIZE));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, AREATEX_WIDTH, AREATEX_HEIGHT, 0,
                 GL_RG, GL_UNSIGNED_BYTE, areaTexBytes);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // 搜索纹理：64x16，每像素 1 字节。按参考 SMAA_SEARCHTEX_SELECT = .r 上传为 R8。
    glGenTextures(1, &search_tex_);
    glBindTexture(GL_TEXTURE_2D, search_tex_);
    CHECK_EQ(SEARCHTEX_WIDTH * SEARCHTEX_HEIGHT, static_cast<int>(SEARCHTEX_SIZE));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 0,
                 GL_RED, GL_UNSIGNED_BYTE, searchTexBytes);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

void SmaaRenderer::Finalize() {
    DestroyTargets();
    if (area_tex_)   { glDeleteTextures(1, &area_tex_);   area_tex_ = 0; }
    if (search_tex_) { glDeleteTextures(1, &search_tex_); search_tex_ = 0; }
    // shader program 由 ShaderManager 统一释放。
    initialized_ = false;
}

void SmaaRenderer::DestroyTargets() {
    if (scene_fbo_) { glDeleteFramebuffers(1, &scene_fbo_); scene_fbo_ = 0; }
    if (scene_tex_) { glDeleteTextures(1, &scene_tex_); scene_tex_ = 0; }
    if (edges_fbo_) { glDeleteFramebuffers(1, &edges_fbo_); edges_fbo_ = 0; }
    if (edges_tex_) { glDeleteTextures(1, &edges_tex_); edges_tex_ = 0; }
    if (weights_fbo_) { glDeleteFramebuffers(1, &weights_fbo_); weights_fbo_ = 0; }
    if (weights_tex_) { glDeleteTextures(1, &weights_tex_); weights_tex_ = 0; }
    tex_w_ = 0;
    tex_h_ = 0;
}

namespace {

// 建一张 color-only 单采样纹理 + 配套 FBO，线性过滤 + clamp。
// internal_format/format 按需要传（如 GL_RGBA8/GL_RGBA、GL_RGBA16F/GL_RGBA）。
// 像素类型传 GL_UNSIGNED_BYTE：data=nullptr 时不发生像素传输，类型无意义。
void MakeColorTarget(unsigned int* fbo, unsigned int* tex, int w, int h,
                     GLint internal_format, GLenum format) {
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, w, h, 0, format, GL_UNSIGNED_BYTE,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    // 显式置单个绘制缓冲：本 FBO 只写 attachment0（避免继承上一 FBO 的 MRT 多缓冲状态）。
    const GLenum draw_buf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &draw_buf);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    CHECK_EQ(status, GL_FRAMEBUFFER_COMPLETE)
        << "SMAA target FBO incomplete, status=" << status
        << " (" << w << "x" << h << ")";
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

}  // namespace

void SmaaRenderer::EnsureSceneFBO(int w, int h) {
    CHECK(initialized_) << "SmaaRenderer::EnsureSceneFBO 需先 Init";
    CHECK_GT(w, 0);
    CHECK_GT(h, 0);
    CHECK_LE(w, kSmaaMaxDim);
    CHECK_LE(h, kSmaaMaxDim);
    if (scene_fbo_ && tex_w_ == w && tex_h_ == h) {
        return;
    }
    DestroyTargets();
    // 场景（SMAA 输入）：LDR sRGB，RGBA8 足够。
    MakeColorTarget(&scene_fbo_, &scene_tex_, w, h, GL_RGBA8, GL_RGBA);
    tex_w_ = w;
    tex_h_ = h;
}

void SmaaRenderer::EnsureEdgesFBO(int w, int h) {
    if (edges_fbo_ && tex_w_ == w && tex_h_ == h) {
        return;
    }
    if (edges_fbo_) { glDeleteFramebuffers(1, &edges_fbo_); edges_fbo_ = 0; }
    if (edges_tex_) { glDeleteTextures(1, &edges_tex_); edges_tex_ = 0; }
    // 边缘/权重是中间量，用半精度浮点避免 8-bit 量化在权重上丢精度（线性过滤下也稳）。
    MakeColorTarget(&edges_fbo_, &edges_tex_, w, h, GL_RGBA16F, GL_RGBA);
}

void SmaaRenderer::EnsureWeightsFBO(int w, int h) {
    if (weights_fbo_ && tex_w_ == w && tex_h_ == h) {
        return;
    }
    if (weights_fbo_) { glDeleteFramebuffers(1, &weights_fbo_); weights_fbo_ = 0; }
    if (weights_tex_) { glDeleteTextures(1, &weights_tex_); weights_tex_ = 0; }
    MakeColorTarget(&weights_fbo_, &weights_tex_, w, h, GL_RGBA16F, GL_RGBA);
}

void SmaaRenderer::Apply(float threshold, int w, int h,
                         unsigned int target_fbo, int vp_x, int vp_y) {
    CHECK(initialized_) << "SmaaRenderer::Apply 需先 Init";
    CHECK(scene_tex_ != 0) << "SmaaRenderer::Apply 需先 EnsureSceneFBO";
    CHECK_GT(w, 0);
    CHECK_GT(h, 0);
    CHECK(target_fbo != 0);
    EnsureEdgesFBO(w, h);
    EnsureWeightsFBO(w, h);

    // SMAA 的 RT 度量 vec4(1/w, 1/h, w, h)。用 glUniform4f（逐分量）而非 glUniform4fv：
    // MinGW 的 GL loader 未提供 glUniform4fv。
    const float rm_x = 1.0f / static_cast<float>(w);
    const float rm_y = 1.0f / static_cast<float>(h);
    const float rm_z = static_cast<float>(w);
    const float rm_w = static_cast<float>(h);

    // 本 pass 全为全屏三角形，不需要深度/剔除/混合（edges/weights 要写精确值）。
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDepthMask(GL_FALSE);

    // ── 第 1 趟：luma 边检测 → edges_tex_ ──
    glBindFramebuffer(GL_FRAMEBUFFER, edges_fbo_);
    glViewport(0, 0, w, h);
    glUseProgram(prog_edge_);
    glUniform4f(shader_mgr_->GetUniform(prog_edge_, "uRtMetrics"), rm_x, rm_y, rm_z, rm_w);
    glUniform1f(shader_mgr_->GetUniform(prog_edge_, "uThreshold"), threshold);
    glActiveTexture(GL_TEXTURE0 + kTexUnitSmaaColor);
    glBindTexture(GL_TEXTURE_2D, scene_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_edge_, "uColorTex"), kTexUnitSmaaColor);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // ── 第 2 趟：混合权重（查 area/search LUT）→ weights_tex_ ──
    glBindFramebuffer(GL_FRAMEBUFFER, weights_fbo_);
    glViewport(0, 0, w, h);
    glUseProgram(prog_weight_);
    glUniform4f(shader_mgr_->GetUniform(prog_weight_, "uRtMetrics"), rm_x, rm_y, rm_z, rm_w);
    glActiveTexture(GL_TEXTURE0 + kTexUnitSmaaEdges);
    glBindTexture(GL_TEXTURE_2D, edges_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_weight_, "uEdgesTex"), kTexUnitSmaaEdges);
    glActiveTexture(GL_TEXTURE0 + kTexUnitSmaaArea);
    glBindTexture(GL_TEXTURE_2D, area_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_weight_, "uAreaTex"), kTexUnitSmaaArea);
    glActiveTexture(GL_TEXTURE0 + kTexUnitSmaaSearch);
    glBindTexture(GL_TEXTURE_2D, search_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_weight_, "uSearchTex"), kTexUnitSmaaSearch);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // ── 第 3 趟：邻域混合 → target（屏幕主 FBO 的 viewport 矩形）──
    glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
    glViewport(vp_x, vp_y, w, h);
    glUseProgram(prog_blend_);
    glUniform4f(shader_mgr_->GetUniform(prog_blend_, "uRtMetrics"), rm_x, rm_y, rm_z, rm_w);
    glActiveTexture(GL_TEXTURE0 + kTexUnitSmaaColor);
    glBindTexture(GL_TEXTURE_2D, scene_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_blend_, "uColorTex"), kTexUnitSmaaColor);
    glActiveTexture(GL_TEXTURE0 + kTexUnitSmaaEdges);
    glBindTexture(GL_TEXTURE_2D, weights_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_blend_, "uBlendTex"), kTexUnitSmaaEdges);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // 收尾：解绑纹理/程序、复位到此 pass 之前的常规状态（3D 段用 glPushAttrib 包裹，
    // 这里只清本 pass 自己的局部绑定，不动 enable 位）。
    glActiveTexture(GL_TEXTURE0 + kTexUnitSmaaEdges);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0 + kTexUnitSmaaArea);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0 + kTexUnitSmaaSearch);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0 + kTexUnitSmaaColor);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(0);
    glDepthMask(GL_TRUE);
}

}  // namespace jpov
