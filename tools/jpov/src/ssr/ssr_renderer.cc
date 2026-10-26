// JPOV SSR 子渲染器实现。设计见 ssr_renderer.h 顶部与 docs/jpov_ssr_design.md。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/ssr/ssr_renderer.h"

#include <cmath>

// GL 头文件必须最先 include（在 MinGW #define 宏替换之前）。
// cube map / FBO 等枚举超出 GL 1.1，MinGW 的 GL/gl.h 里没有 ⇒ 两平台都要 glext.h。
#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

// Windows/MinGW: windef.h 定义 near/far 宏，与 Camera::near / Camera::far 字段同名冲突。
#ifdef _WIN32
#undef near
#undef far
#endif

#include <glog/logging.h>

#include "tools/jpov/src/object3d/object3d_renderer.h"
#include "tools/jpov/src/primitives3d/primitives3d_renderer.h"
#include "tools/jpov/src/ssr/ssr_shader.h"

namespace jpov {

namespace {

// 本模块的纹理单元（pass-local，与 3D 主链路的 texture_units.h 不重叠：
// SSR 跑在独立 pass 里，此刻主链路不持有任何纹理绑定）。
constexpr int kTexUnitSceneDepth = 0;
constexpr int kTexUnitSceneColor = 1;
constexpr int kTexUnitEnvCube = 2;      // 天空 cubemap（探针）
constexpr int kTexUnitAttr0 = 3;
constexpr int kTexUnitAttr1 = 4;
constexpr int kTexUnitHiZBase = 5;      // 5..9 = level 0..4
constexpr int kTexUnitNormalMap = 0;    // 属性 pass 内部用（与上面不同 pass，可复用 0）
constexpr int kTexUnitRefl = 0;         // 合成 pass

// 列主序 4×4 乘法：out = a * b
void Mat4Mul(const float a[16], const float b[16], float out[16]) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + r] * b[c * 4 + k];
            }
            out[c * 4 + r] = sum;
        }
    }
}

// 建一张 RGBA16F 单采样纹理（NEAREST，CLAMP）。
void CreateColorTexture(int w, int h, unsigned int* tex /*output*/) {
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// 建一张 R32F 单采样纹理（NEAREST —— Hi-Z 的 min 语义不能被插值破坏）。
void CreateDepthTexture(int w, int h, unsigned int* tex /*output*/) {
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, w, h, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void BindTex(int unit, unsigned int tex) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
}

}  // namespace

SsrRenderer::~SsrRenderer() {
    // 真正的 glDelete 在 Finalize()：析构里碰 GL 会在 context 已销毁时崩（PR #216 的教训）。
    // 这里只做"有没有忘了收尾"的守卫 —— Init 过就必须 Finalize 过。
    CHECK(shader_mgr_ == nullptr)
        << "SsrRenderer::Init 过但没有调用 Finalize()（GL 资源会泄漏）";
}

void SsrRenderer::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr);
    shader_mgr_ = shader_mgr;
    // 空 VAO：core profile 下即使 gl_VertexID 全屏三角形也得绑一个 VAO。
    glGenVertexArrays(1, &vao_);
    // 预热 program（避免第一帧卡顿；编译失败会在 GetOrCreate 里 FATAL）
    AttrProg();
    AttrFullProg();
    HiZProg();
    MarchProg();
    CompositeProg();
}

void SsrRenderer::Finalize() {
    DestroyBuffers();
    DestroyHiZ();
    if (attr_fbo_) { glDeleteFramebuffers(1, &attr_fbo_); attr_fbo_ = 0; }
    if (refl_fbo_) { glDeleteFramebuffers(1, &refl_fbo_); refl_fbo_ = 0; }
    if (hiz_shared_fbo_) { glDeleteFramebuffers(1, &hiz_shared_fbo_); hiz_shared_fbo_ = 0; }
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    for (int i = 0; i < 5; ++i) {
        progs_[i] = 0;   // program 归 ShaderManager 所有
    }
    shader_mgr_ = nullptr;
}

unsigned int SsrRenderer::AttrProg() {
    if (progs_[0] == 0) {
        progs_[0] = shader_mgr_->GetOrCreate("ssr_attr",
            {Object3DRenderer::kMeshVs3dPBR, kSsrAttrFs});
    }
    return static_cast<unsigned int>(progs_[0]);
}

unsigned int SsrRenderer::AttrFullProg() {
    if (progs_[1] == 0) {
        progs_[1] = shader_mgr_->GetOrCreate("ssr_attr_full",
            {Object3DRenderer::kMeshVs3dPBRFull, kSsrAttrFs});
    }
    return static_cast<unsigned int>(progs_[1]);
}

unsigned int SsrRenderer::HiZProg() {
    if (progs_[2] == 0) {
        progs_[2] = shader_mgr_->GetOrCreate("ssr_hiz_min",
            {kSsrFullscreenVs, kSsrHiZMinFs});
    }
    return static_cast<unsigned int>(progs_[2]);
}

unsigned int SsrRenderer::MarchProg() {
    if (progs_[3] == 0) {
        progs_[3] = shader_mgr_->GetOrCreate("ssr_march",
            {kSsrFullscreenVs, kSsrMarchFs});
    }
    return static_cast<unsigned int>(progs_[3]);
}

unsigned int SsrRenderer::CompositeProg() {
    if (progs_[4] == 0) {
        progs_[4] = shader_mgr_->GetOrCreate("ssr_composite",
            {kSsrFullscreenVs, kSsrCompositeFs});
    }
    return static_cast<unsigned int>(progs_[4]);
}

void SsrRenderer::EnsureBuffers(int fbo_w, int fbo_h, float scale) {
    CHECK_GT(fbo_w, 0);
    CHECK_GT(fbo_h, 0);
    // 属性缓冲 = 主 3D 分辨率（要逐像素对齐深度）
    if (attr_fbo_ == 0 || attr_w_ != fbo_w || attr_h_ != fbo_h) {
        DestroyBuffers();
        CreateColorTexture(fbo_w, fbo_h, &attr_normal_tex_);
        CreateColorTexture(fbo_w, fbo_h, &attr_f0_tex_);
        glGenFramebuffers(1, &attr_fbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, attr_fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, attr_normal_tex_, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                               GL_TEXTURE_2D, attr_f0_tex_, 0);
        const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(2, bufs);
        // 深度附件在每次 DrawAttributes 里挂主深度（随主 FBO 重建而变化），这里不挂。
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        attr_w_ = fbo_w;
        attr_h_ = fbo_h;
    }
    const int rw = std::max(1, static_cast<int>(static_cast<float>(fbo_w) * scale));
    const int rh = std::max(1, static_cast<int>(static_cast<float>(fbo_h) * scale));
    if (refl_tex_ == 0 || refl_w_ != rw || refl_h_ != rh) {
        if (refl_tex_) { glDeleteTextures(1, &refl_tex_); refl_tex_ = 0; }
        CreateColorTexture(rw, rh, &refl_tex_);
        if (refl_fbo_ == 0) { glGenFramebuffers(1, &refl_fbo_); }
        glBindFramebuffer(GL_FRAMEBUFFER, refl_fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, refl_tex_, 0);
        CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE)
            << "SSR reflection FBO incomplete";
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        refl_w_ = rw;
        refl_h_ = rh;
    }
}

void SsrRenderer::DestroyBuffers() {
    if (attr_normal_tex_) { glDeleteTextures(1, &attr_normal_tex_); attr_normal_tex_ = 0; }
    if (attr_f0_tex_) { glDeleteTextures(1, &attr_f0_tex_); attr_f0_tex_ = 0; }
    if (refl_tex_) { glDeleteTextures(1, &refl_tex_); refl_tex_ = 0; }
    attr_w_ = attr_h_ = 0;
    refl_w_ = refl_h_ = 0;
}

void SsrRenderer::EnsureHiZ(int level, int w, int h) {
    CHECK_GE(level, 1);
    CHECK_LE(level, kHiZLevels);
    const int idx = level - 1;
    if (hiz_tex_[idx] != 0 && hiz_w_[idx] == w && hiz_h_[idx] == h) {
        return;
    }
    if (hiz_tex_[idx]) { glDeleteTextures(1, &hiz_tex_[idx]); hiz_tex_[idx] = 0; }
    CreateDepthTexture(w, h, &hiz_tex_[idx]);
    hiz_w_[idx] = w;
    hiz_h_[idx] = h;
}

void SsrRenderer::DestroyHiZ() {
    for (int i = 0; i < kHiZLevels; ++i) {
        if (hiz_tex_[i]) { glDeleteTextures(1, &hiz_tex_[i]); hiz_tex_[i] = 0; }
        hiz_w_[i] = hiz_h_[i] = 0;
    }
}

void SsrRenderer::DrawAttributes(const RenderCommandList& cmds, MeshManager& mesh_mgr,
                                 TextureManager& texture_mgr, const float view[16],
                                 const float vp[16], int fbo_w, int fbo_h) {
    glBindFramebuffer(GL_FRAMEBUFFER, attr_fbo_);
    glViewport(0, 0, fbo_w, fbo_h);
    // 只清颜色：深度附件是**主 3D 的深度**，清掉就把不透明场景的遮挡关系毁了。
    const GLfloat zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glClearBufferfv(GL_COLOR, 0, zero);
    glClearBufferfv(GL_COLOR, 1, zero);

    // 深度测试对主深度做 LEQUAL（顶点变换与主 pass 逐字一致 ⇒ 等值必然成立），
    // 深度写关闭：属性缓冲不参与任何深度写入。
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);

    const unsigned int prog = AttrProg();
    const unsigned int prog_full = AttrFullProg();

    for (const Object3DCommand& cmd : cmds.object3d) {
        if (!cmd.ssr_reflect) { continue; }
        if (cmd.material.alpha_mode != AlphaMode::kOpaque) { continue; }
        const GPUMesh* mesh = mesh_mgr.GetMesh(cmd.mesh_id);
        CHECK(mesh != nullptr) << "SSR: mesh_id=" << cmd.mesh_id << " 未注册";
        if (mesh->vbo_normals == 0) { continue; }   // 无法线 ⇒ 算不了反射方向

        const bool want_full = (cmd.material.normal_tex != 0);
        if (want_full && (mesh->vbo_uvs == 0 || mesh->vbo_tangents == 0)) {
            // 材质带法线贴图但 mesh 没有 UV/切线 —— 主 pass 会 CHECK 掉，这里只能跳过，
            // 否则属性缓冲缺一块（那个物体的倒影会"没有"）。
            LOG(WARNING) << "SSR: mesh_id=" << cmd.mesh_id
                         << " 有法线贴图但缺 UV/切线，跳过属性写入";
            continue;
        }
        const unsigned int p = want_full ? prog_full : prog;
        glUseProgram(p);

        float model[16];
        Primitives3DRenderer::BuildModelMatrix(cmd.center, cmd.up, cmd.front, model,
                                               cmd.scale);
        float mvp[16];
        Mat4Mul(vp, model, mvp);
        glUniformMatrix4fv(shader_mgr_->GetUniform(p, "uMVP"), 1, GL_FALSE, mvp);
        glUniformMatrix4fv(shader_mgr_->GetUniform(p, "uModel"), 1, GL_FALSE, model);
        glUniformMatrix4fv(shader_mgr_->GetUniform(p, "uView"), 1, GL_FALSE, view);
        glUniform3f(shader_mgr_->GetUniform(p, "uBaseColor"),
                    cmd.material.base_color.r, cmd.material.base_color.g,
                    cmd.material.base_color.b);
        glUniform1f(shader_mgr_->GetUniform(p, "uMetallic"), cmd.material.metallic);
        glUniform1f(shader_mgr_->GetUniform(p, "uRoughness"), cmd.material.roughness);
        glUniform1f(shader_mgr_->GetUniform(p, "uNormalScale"),
                    cmd.material.normal_scale);

        if (want_full) {
            const unsigned int tex = texture_mgr.GetGLTexture(cmd.material.normal_tex);
            CHECK_NE(tex, 0u) << "SSR: normal_tex 未注册";
            BindTex(kTexUnitNormalMap, tex);
            glUniform1i(shader_mgr_->GetUniform(p, "uNormalTex"), kTexUnitNormalMap);
            glUniform1i(shader_mgr_->GetUniform(p, "uHasNormalTex"), 1);
        } else {
            glUniform1i(shader_mgr_->GetUniform(p, "uHasNormalTex"), 0);
        }

        glBindVertexArray(mesh->vao);
        if (mesh->index_count > 0) {
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh->index_count),
                           GL_UNSIGNED_INT, nullptr);
        } else {
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(mesh->vertex_count));
        }
        glBindVertexArray(0);
    }

    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SsrRenderer::BuildHiZ(unsigned int scene_depth_tex, int fbo_w, int fbo_h) {
    const unsigned int prog = HiZProg();
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    if (hiz_shared_fbo_ == 0) {
        glGenFramebuffers(1, &hiz_shared_fbo_);
    }
    unsigned int src = scene_depth_tex;
    int src_w = fbo_w;
    int src_h = fbo_h;
    for (int level = 1; level <= kHiZLevels; ++level) {
        const int dw = std::max(1, src_w / 2);
        const int dh = std::max(1, src_h / 2);
        EnsureHiZ(level, dw, dh);
        glBindFramebuffer(GL_FRAMEBUFFER, hiz_shared_fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               hiz_tex_[level - 1], 0);
        glViewport(0, 0, dw, dh);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src);
        glUniform1i(shader_mgr_->GetUniform(prog, "uSrc"), 0);
        glUniform2f(shader_mgr_->GetUniform(prog, "uDstResolution"),
                    static_cast<float>(dw), static_cast<float>(dh));
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        src = hiz_tex_[level - 1];
        src_w = dw;
        src_h = dh;
    }
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void SsrRenderer::March(const SsrConfig& cfg, const Camera& cam, const float proj[16],
                        const float view[16], unsigned int scene_color_tex,
                        unsigned int scene_depth_tex,
                        unsigned int env_cube_tex, float env_max_lod,
                        int fbo_w, int fbo_h) {
    const unsigned int prog = MarchProg();
    glBindFramebuffer(GL_FRAMEBUFFER, refl_fbo_);
    glViewport(0, 0, refl_w_, refl_h_);
    const GLfloat zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glClearBufferfv(GL_COLOR, 0, zero);

    glUseProgram(prog);
    BindTex(kTexUnitSceneDepth, scene_depth_tex);
    BindTex(kTexUnitSceneColor, scene_color_tex);
    glActiveTexture(GL_TEXTURE0 + kTexUnitEnvCube);
    glBindTexture(GL_TEXTURE_CUBE_MAP, env_cube_tex);
    BindTex(kTexUnitAttr0, attr_normal_tex_);
    BindTex(kTexUnitAttr1, attr_f0_tex_);
    BindTex(kTexUnitHiZBase + 0, scene_depth_tex);   // level 0 = 全分辨率场景深度
    BindTex(kTexUnitHiZBase + 1, hiz_tex_[0]);
    BindTex(kTexUnitHiZBase + 2, hiz_tex_[1]);
    BindTex(kTexUnitHiZBase + 3, hiz_tex_[2]);
    BindTex(kTexUnitHiZBase + 4, hiz_tex_[3]);

    // ⚠️ 每个 sampler 的单元号**逐条显式写**，不要用「基址 + 下标」推算：
    //    中间只要有"非 2D 纹理"或"插入了新纹理"，推算就会整体错位，
    //    症状是 sampler 绑到不匹配的纹理目标上（本例：sampler2D 绑到 cubemap），
    //    采样返回 0 ⇒ 属性全空 ⇒ 整块反射体静默没有任何倒影。
    glUniform1i(shader_mgr_->GetUniform(prog, "uSceneDepth"), kTexUnitSceneDepth);
    glUniform1i(shader_mgr_->GetUniform(prog, "uSceneColor"), kTexUnitSceneColor);
    glUniform1i(shader_mgr_->GetUniform(prog, "uEnvCube"), kTexUnitEnvCube);
    glUniform1i(shader_mgr_->GetUniform(prog, "uHasProbe"), env_cube_tex != 0 ? 1 : 0);
    glUniform1f(shader_mgr_->GetUniform(prog, "uEnvMaxLod"), env_max_lod);
    glUniform3f(shader_mgr_->GetUniform(prog, "uProbeFallback"), 0.30f, 0.36f, 0.45f);
    glUniform1i(shader_mgr_->GetUniform(prog, "uAttr0"), kTexUnitAttr0);
    glUniform1i(shader_mgr_->GetUniform(prog, "uAttr1"), kTexUnitAttr1);
    const char* kHiZNames[] = {"uHiZ0", "uHiZ1", "uHiZ2", "uHiZ3", "uHiZ4"};
    for (int i = 0; i < 5; ++i) {
        glUniform1i(shader_mgr_->GetUniform(prog, kHiZNames[i]),
                    kTexUnitHiZBase + i);
    }

    glUniformMatrix4fv(shader_mgr_->GetUniform(prog, "uProj"), 1, GL_FALSE, proj);
    glUniformMatrix4fv(shader_mgr_->GetUniform(prog, "uView"), 1, GL_FALSE, view);
    glUniform2f(shader_mgr_->GetUniform(prog, "uResolution"),
                static_cast<float>(refl_w_), static_cast<float>(refl_h_));
    glUniform1f(shader_mgr_->GetUniform(prog, "uNear"), cam.near);
    glUniform1f(shader_mgr_->GetUniform(prog, "uFar"), cam.far);
    const float fov_rad = cam.fov * 3.14159265358979323846f / 180.0f;
    glUniform1f(shader_mgr_->GetUniform(prog, "uTanHalfFovY"), std::tan(fov_rad * 0.5f));
    glUniform1f(shader_mgr_->GetUniform(prog, "uAspect"),
                static_cast<float>(fbo_w) / static_cast<float>(fbo_h));
    glUniform1i(shader_mgr_->GetUniform(prog, "uStartLevel"), cfg.hiz_start_level);
    glUniform1i(shader_mgr_->GetUniform(prog, "uMaxSteps"), cfg.max_steps);
    glUniform1i(shader_mgr_->GetUniform(prog, "uRefineSteps"), cfg.refine_steps);
    glUniform1f(shader_mgr_->GetUniform(prog, "uThicknessM"), cfg.thickness_m);
    glUniform1f(shader_mgr_->GetUniform(prog, "uMaxDistanceM"), cfg.max_distance_m);
    glUniform1f(shader_mgr_->GetUniform(prog, "uRoughnessCutoff"), cfg.roughness_cutoff);
    glUniform1i(shader_mgr_->GetUniform(prog, "uFadeEdges"),
                cfg.fade_screen_edges ? 1 : 0);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
}

void SsrRenderer::Draw(const RenderCommandList& cmds, const SsrConfig& cfg,
                       MeshManager& mesh_mgr, TextureManager& texture_mgr,
                       const Camera& cam, const float proj[16], const float view[16],
                       unsigned int scene_color_tex, unsigned int scene_depth_tex,
                       unsigned int hw_depth_tex, unsigned int env_cube_tex,
                       float env_max_lod, int fbo_w, int fbo_h) {
    CHECK(shader_mgr_ != nullptr) << "SsrRenderer::Init 未调用";
    CHECK_NE(scene_color_tex, 0u);
    CHECK_NE(scene_depth_tex, 0u);
    CHECK_GT(cfg.resolution_scale, 0.0f);
    CHECK_LE(cfg.resolution_scale, 1.0f);
    CHECK_GE(cfg.hiz_start_level, 0);
    CHECK_LE(cfg.hiz_start_level, 4) << "Hi-Z 起始层最多 4（金字塔只有 4 级）";
    CHECK_GE(cfg.max_steps, 1);

    EnsureBuffers(fbo_w, fbo_h, cfg.resolution_scale);

    // ① 属性：把反射体的视空间法线 / 粗糙度 / F0 画进属性缓冲。
    //    深度用**硬件深度纹理**（DEPTH_COMPONENT24）：MRT#1 的 R32F 是颜色格式，
    //    挂到 GL_DEPTH_ATTACHMENT 会直接让 FBO 变成 INCOMPLETE_ATTACHMENT。
    glBindFramebuffer(GL_FRAMEBUFFER, attr_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                           hw_depth_tex, 0);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE)
        << "SSR attr FBO incomplete（挂上主深度后）";
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    float vp[16];
    Mat4Mul(proj, view, vp);
    DrawAttributes(cmds, mesh_mgr, texture_mgr, view, vp, fbo_w, fbo_h);

    // ② Hi-Z 金字塔（只有真要粗走时才建；start_level==0 = 关 Hi-Z，省掉建塔成本）
    if (cfg.hiz_start_level > 0) {
        BuildHiZ(scene_depth_tex, fbo_w, fbo_h);
    }

    // ③ 射线行进 → 反射缓冲
    March(cfg, cam, proj, view, scene_color_tex, scene_depth_tex,
          env_cube_tex, env_max_lod, fbo_w, fbo_h);
}

void SsrRenderer::Composite(int fbo_w, int fbo_h) {
    if (refl_tex_ == 0) { return; }
    const unsigned int prog = CompositeProg();
    glUseProgram(prog);
    BindTex(kTexUnitRefl, refl_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog, "uRefl"), kTexUnitRefl);
    glUniform2f(shader_mgr_->GetUniform(prog, "uResolution"),
                static_cast<float>(fbo_w), static_cast<float>(fbo_h));
    glUniform2f(shader_mgr_->GetUniform(prog, "uTexel"),
                refl_w_ > 0 ? 1.0f / static_cast<float>(refl_w_) : 0.0f,
                refl_h_ > 0 ? 1.0f / static_cast<float>(refl_h_) : 0.0f);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    // 加性：反射是"打到已有画面上的光"，不是替换。
    // 分离 alpha 混合函数：只加 rgb，不污染目标的 alpha（HDR 颜色附件 alpha 无意义，
    // 但保持为 1 可以避免下游任何按 alpha 判断的路径出意外）。
    glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_BLEND);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
}

}  // namespace jpov
