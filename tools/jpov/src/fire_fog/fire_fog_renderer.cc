// JPOV FireFogRenderer 成员函数实现（froxel 版）
//
// 见 fire_fog_renderer.h / fire_fog_shader.h / tools/jpov/docs/jpov_froxel_design.md。
//
// 三趟（全部在主 FBO 尺寸）：
//   1. inject   → inject_tex_（局部 (τ, S)：tile 中心视线 × [z_k,z_{k+1}) 段 × 候选雾团）
//   2. scatter  → scatter_tex_（累积 (τ, S)：沿 z 有序前缀）
//   3. composite→ 就地把 vec4(S.rgb, T) 混合进调用方 3D HDR FBO（GL_ONE/GL_SRC_ALPHA）
//
// ⚠️ 本文件自行保存/复原 FBO 绑定 / viewport / blend func / 纹理单元绑定，不踩调用方状态
//（尤其 blend func 不属 GL_ENABLE_BIT，必须显式复位，否则污染后续 2D/字体绘制）。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/fire_fog/fire_fog_renderer.h"

#include <cmath>
#include <cstring>

// GL 头文件必须最先 include（在 MinGW #define 宏替换之前）
#ifdef _WIN32
#include <GL/gl.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#ifdef _WIN32
// MinGW: windows.h 定义 ERROR 宏与 glog 冲突，必须在 glog 之前 suppress
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

// Windows/MinGW: windef.h 定义 near/far 宏，与 Camera::near/far 字段冲突。
#ifdef _WIN32
#undef near
#undef far
#endif

#include <glog/logging.h>

#include "tools/jpov/src/fire_fog/fire_fog_lower.h"
#include "tools/jpov/src/fire_fog/fire_fog_shader.h"

namespace {

constexpr int kBodyTexelsPerFog = 3;   // 每团 3 个 RGBA32F texel
constexpr int kShadowTexUnitBase = 3;  // inject 的 CSM 阴影纹理起始单元（0/1 已用）

// 4x4 列主序求逆（Gauss-Jordan + 部分主元）。无法求逆返回 false。
//（与 horizon_fog_renderer.cc / 点雾版 fire_fog_renderer.cc 同款实现。）
bool Mat4Invert(const float m[16], float out[16]) {
    float a[16];
    for (int i = 0; i < 16; ++i) {
        a[i] = m[i];
    }
    float inv[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        float best = std::fabs(a[col * 4 + col]);
        for (int r = col + 1; r < 4; ++r) {
            const float v = std::fabs(a[col * 4 + r]);
            if (v > best) {
                best = v;
                piv = r;
            }
        }
        if (best < 1e-12f) {
            return false;
        }
        if (piv != col) {
            for (int c = 0; c < 4; ++c) {
                float t = a[c * 4 + col];
                a[c * 4 + col] = a[c * 4 + piv];
                a[c * 4 + piv] = t;
                t = inv[c * 4 + col];
                inv[c * 4 + col] = inv[c * 4 + piv];
                inv[c * 4 + piv] = t;
            }
        }
        const float d = a[col * 4 + col];
        for (int c = 0; c < 4; ++c) {
            a[c * 4 + col] /= d;
            inv[c * 4 + col] /= d;
        }
        for (int r = 0; r < 4; ++r) {
            if (r == col) {
                continue;
            }
            const float f = a[col * 4 + r];
            for (int c = 0; c < 4; ++c) {
                a[c * 4 + r] -= f * a[c * 4 + col];
                inv[c * 4 + r] -= f * inv[c * 4 + col];
            }
        }
    }
    for (int i = 0; i < 16; ++i) {
        out[i] = inv[i];
    }
    return true;
}

// 组装一趟的 FS 源码（#version 330 core + 公共前置 + 趟体）。
std::string AssembleFragmentShader(const char* body) {
    std::string s = "#version 330 core\n";
    s += jpov::kFireFogCommonGlsl;
    s += body;
    return s;
}

}  // anonymous namespace

namespace jpov {

FireFogRenderer::~FireFogRenderer() {
    Finalize();
}

void FireFogRenderer::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr);
    shader_mgr_ = shader_mgr;
    fs_inject_ = AssembleFragmentShader(kFireFogInjectBody);
    fs_scatter_ = AssembleFragmentShader(kFireFogScatterBody);
    fs_composite_ = AssembleFragmentShader(kFireFogCompositeBody);
    prog_inject_ = shader_mgr_->GetOrCreate(
        "fire_fog_inject", {kFireFogVs, fs_inject_.c_str()});
    prog_scatter_ = shader_mgr_->GetOrCreate(
        "fire_fog_scatter", {kFireFogVs, fs_scatter_.c_str()});
    prog_composite_ = shader_mgr_->GetOrCreate(
        "fire_fog_composite", {kFireFogVs, fs_composite_.c_str()});

    // 1×1 深度哑元（值=1.0）：供调用方传 scene_depth_tex==0 时使用。
    const float one = 1.0f;
    glGenTextures(1, &dummy_depth_tex_);
    glBindTexture(GL_TEXTURE_2D, dummy_depth_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, 1, 1, 0, GL_RED, GL_FLOAT, &one);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void FireFogRenderer::Finalize() {
    if (inject_tex_ != 0) {
        glDeleteTextures(1, &inject_tex_);
        inject_tex_ = 0;
    }
    if (scatter_tex_ != 0) {
        glDeleteTextures(1, &scatter_tex_);
        scatter_tex_ = 0;
    }
    if (inject_fbo_ != 0) {
        glDeleteFramebuffers(1, &inject_fbo_);
        inject_fbo_ = 0;
    }
    if (scatter_fbo_ != 0) {
        glDeleteFramebuffers(1, &scatter_fbo_);
        scatter_fbo_ = 0;
    }
    if (tile_index_tex_ != 0) {
        glDeleteTextures(1, &tile_index_tex_);
        tile_index_tex_ = 0;
    }
    if (fog_body_tex_ != 0) {
        glDeleteTextures(1, &fog_body_tex_);
        fog_body_tex_ = 0;
    }
    if (dummy_depth_tex_ != 0) {
        glDeleteTextures(1, &dummy_depth_tex_);
        dummy_depth_tex_ = 0;
    }
    froxel_w_ = 0;
    froxel_h_ = 0;
    tile_tex_w_ = 0;
    tile_tex_h_ = 0;
    grid_w_ = 0;
    grid_h_ = 0;
    bodies_.clear();
    prog_inject_ = 0;
    prog_scatter_ = 0;
    prog_composite_ = 0;
    shader_mgr_ = nullptr;
}

void FireFogRenderer::EnsureTileTexture(int grid_cols, int grid_rows) {
    CHECK_GE(grid_cols, 1);
    CHECK_GE(grid_rows, 1);
    const int tex_w = grid_cols * kTexelsPerTile;
    const int tex_h = grid_rows;
    const bool need_rebuild = (tile_index_tex_ == 0) || (tex_w != tile_tex_w_) ||
                              (tex_h != tile_tex_h_);
    if (!need_rebuild) {
        return;
    }
    if (tile_index_tex_ != 0) {
        glDeleteTextures(1, &tile_index_tex_);
    }
    glGenTextures(1, &tile_index_tex_);
    glBindTexture(GL_TEXTURE_2D, tile_index_tex_);
    // 无 mip；严格最近采样（整数 index 不可插值）；边缘 clamp。
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_w, tex_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    tile_tex_w_ = tex_w;
    tile_tex_h_ = tex_h;
    grid_w_ = grid_cols;
    grid_h_ = grid_rows;
}

void FireFogRenderer::EnsureFroxelTargets(int w, int h) {
    CHECK_GT(w, 0);
    CHECK_GT(h, 0);
    const bool rebuild = (inject_fbo_ == 0) || (w != froxel_w_) || (h != froxel_h_);
    if (!rebuild) {
        return;
    }
    GLint prev_binding = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_binding);

    unsigned int* texs[2] = {&inject_tex_, &scatter_tex_};
    unsigned int* fbos[2] = {&inject_fbo_, &scatter_fbo_};
    for (int s = 0; s < 2; ++s) {
        if (*fbos[s] != 0) {
            glDeleteFramebuffers(1, fbos[s]);
            *fbos[s] = 0;
        }
        if (*texs[s] != 0) {
            glDeleteTextures(1, texs[s]);
            *texs[s] = 0;
        }
        glGenTextures(1, texs[s]);
        glBindTexture(GL_TEXTURE_2D, *texs[s]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
        glGenFramebuffers(1, fbos[s]);
        glBindFramebuffer(GL_FRAMEBUFFER, *fbos[s]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               *texs[s], 0);
        const GLenum draw_buf[1] = {GL_COLOR_ATTACHMENT0};
        glDrawBuffers(1, draw_buf);
        const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        CHECK(st == GL_FRAMEBUFFER_COMPLETE)
            << "FireFogRenderer: froxel FBO incomplete, status=0x" << std::hex << st;
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_binding));
    froxel_w_ = w;
    froxel_h_ = h;
}

void FireFogRenderer::Draw(const std::vector<PointFog>& fogs,
                           const Camera& cam,
                           const float view_proj[16],
                           int viewport_w,
                           int viewport_h,
                           unsigned int scene_depth_tex,
                           const FireFogParams& params,
                           const FireFogLighting& light) {
    CHECK(shader_mgr_ != nullptr) << "FireFogRenderer::Init 未调用";
    if (fogs.empty()) {
        return;   // 零开销
    }
    CHECK_GT(viewport_w, 0);
    CHECK_GT(viewport_h, 0);
    CHECK_GT(cam.near, 0.0f);
    CHECK_GT(cam.far, cam.near);
    CHECK_GT(params.z_near, 0.0f) << "FireFogParams.z_near 必须 > 0";
    CHECK_GT(params.z_far, params.z_near) << "FireFogParams.z_far 必须 > z_near";
    CHECK_GE(params.sun_phase_g, 0.0f);
    CHECK_LT(params.sun_phase_g, 0.95f) << "FireFogParams.sun_phase_g 必须 ∈ [0, 0.95)";
    // ── froxel 网格分辨率：nz（完全平方）+ tile_px（屏幕像素）→ 派生量 ──
    //   sblock = √nz（每柱在 froxel 纹理上的 texel 边长）；
    //   Nxy    = ceil(W/tile_px) × ceil(H/tile_px)；
    //   froxel 纹理 = Nxy.x·sblock × Nxy.y·sblock。
    // 不合法直接 crash（不静默回退）。
    CHECK_GE(params.nz, kMinNz) << "FireFogParams.nz 必须 >= " << kMinNz;
    CHECK_LE(params.nz, kMaxNz) << "FireFogParams.nz 必须 <= " << kMaxNz;
    const int sblock =
        static_cast<int>(std::lround(std::sqrt(static_cast<double>(params.nz))));
    CHECK_EQ(sblock * sblock, params.nz)
        << "FireFogParams.nz 必须是完全平方（sblock=√nz 需为整数），得到 " << params.nz;
    CHECK_GE(params.tile_px, kMinTilePx) << "FireFogParams.tile_px 必须 >= " << kMinTilePx;
    CHECK_LE(params.tile_px, kMaxTilePx) << "FireFogParams.tile_px 必须 <= " << kMaxTilePx;
    const int grid_cols = (viewport_w + params.tile_px - 1) / params.tile_px;
    const int grid_rows = (viewport_h + params.tile_px - 1) / params.tile_px;
    const int froxel_w = grid_cols * sblock;
    const int froxel_h = grid_rows * sblock;
    CHECK_LE(froxel_w, kMaxFroxelDim) << "froxel 纹理宽 " << froxel_w << " 超上限 "
                                      << kMaxFroxelDim << "（调小 nz 或调大 tile_px）";
    CHECK_LE(froxel_h, kMaxFroxelDim) << "froxel 纹理高 " << froxel_h << " 超上限 "
                                      << kMaxFroxelDim << "（调小 nz 或调大 tile_px）";

    // 保存调用方状态：composite 要回到调用方的 FBO / viewport 才能就地混合。
    // ⚠️ 必须在本函数内任何可能重绑 FBO 的操作（EnsureFroxelTargets）**之前**取。
    GLint prev_fbo = 0;
    GLint prev_vp[4] = {0, 0, 0, 0};
    GLint prev_active_tex = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_vp);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active_tex);
    // 全程在纹理单元 0 为活动单元的前提下操作（避免 Ensure* 的 glBindTexture 踩到调用方当前单元）。
    glActiveTexture(GL_TEXTURE0);

    // ── 命令层点雾 → 后端统一团（FogBody）──
    int count = static_cast<int>(fogs.size());
    if (count > kMaxTotalFogs) {
        LOG(WARNING) << "point_fogs 数量 " << count << " 超过上限 " << kMaxTotalFogs
                     << "，仅前 " << kMaxTotalFogs << " 个生效";
        count = kMaxTotalFogs;
    }
    bodies_.clear();
    bodies_.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        bodies_.push_back(LowerPointFog(fogs[i]));
    }

    // ── CPU 屏幕 tile 剪枝（每 tile ≤K 个团索引；GL-free）—— tile 网格 = Nxy ──
    EnsureTileTexture(grid_cols, grid_rows);
    const std::vector<uint8_t> tile_data = BuildTileFogIndexData(
        bodies_, view_proj, grid_w_, grid_h_, params.tile_px, kMaxFogsPerTile,
        kTexelsPerTile, kFogIndexSentinel);
    glBindTexture(GL_TEXTURE_2D, tile_index_tex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tile_tex_w_, tile_tex_h_,
                    GL_RGBA, GL_UNSIGNED_BYTE, tile_data.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // ── 团属性纹理（RGBA32F；每团 3 texel：t0=(center,radius) t1=(color,intensity) t2=(atten,..)）──
    const int body_tex_w = kMaxTotalFogs * kBodyTexelsPerFog;
    if (fog_body_tex_ == 0) {
        glGenTextures(1, &fog_body_tex_);
        glBindTexture(GL_TEXTURE_2D, fog_body_tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, body_tex_w, 1, 0,
                     GL_RGBA, GL_FLOAT, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    std::vector<float> body_attr(static_cast<size_t>(count) * kBodyTexelsPerFog * 4);
    for (int i = 0; i < count; ++i) {
        const FogBody& b = bodies_[i];
        float* t0 = &body_attr[(static_cast<size_t>(i) * 3 + 0) * 4];
        float* t1 = &body_attr[(static_cast<size_t>(i) * 3 + 1) * 4];
        float* t2 = &body_attr[(static_cast<size_t>(i) * 3 + 2) * 4];
        t0[0] = b.bound.center.x();
        t0[1] = b.bound.center.y();
        t0[2] = b.bound.center.z();
        t0[3] = b.bound.half_extent.x();   // 立方 OBB ⇒ 各半轴相同 = 球半径
        t1[0] = b.color.r;
        t1[1] = b.color.g;
        t1[2] = b.color.b;
        t1[3] = b.intensity;
        t2[0] = b.params[0];               // attenuation 枚举
        t2[1] = 0.0f;
        t2[2] = 0.0f;
        t2[3] = 0.0f;
    }
    glBindTexture(GL_TEXTURE_2D, fog_body_tex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, count * kBodyTexelsPerFog, 1,
                    GL_RGBA, GL_FLOAT, body_attr.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    EnsureFroxelTargets(froxel_w, froxel_h);

    float inv_vp[16];
    CHECK(Mat4Invert(view_proj, inv_vp))
        << "FireFogRenderer: 相机矩阵不可逆（near/far 非法或退化）";

    // ═══════════ 趟 1：inject（逐 froxel 局部 (τ, S) → inject_tex_）═══════════
    glBindFramebuffer(GL_FRAMEBUFFER, inject_fbo_);
    glViewport(0, 0, froxel_w, froxel_h);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glUseProgram(prog_inject_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tile_index_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uTileFogIndices"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, fog_body_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uFogBodyTex"), 1);
    glActiveTexture(GL_TEXTURE0);
    glUniformMatrix4fv(shader_mgr_->GetUniform(prog_inject_, "uInvVP"), 1, GL_FALSE, inv_vp);
    glUniform3f(shader_mgr_->GetUniform(prog_inject_, "uCamPos"), cam.position.x(),
                cam.position.y(), cam.position.z());
    glUniform1f(shader_mgr_->GetUniform(prog_inject_, "uZNear"), params.z_near);
    glUniform1f(shader_mgr_->GetUniform(prog_inject_, "uZFar"), params.z_far);
    glUniform2f(shader_mgr_->GetUniform(prog_inject_, "uFboSize"),
                static_cast<float>(froxel_w), static_cast<float>(froxel_h));
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uNz"), params.nz);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uBlockSide"), sblock);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uTilePx"), params.tile_px);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uTotalFogs"), count);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uSunEnable"),
                params.sun_enable ? 1 : 0);

    // ── 物理光照 + CSM 阴影（仅 sun_enable 时 inject shader 使用；否则 uSunEnable=0）──
    const int cascade_count =
        (params.sun_enable && light.sun.has_value())
            ? std::min(light.shadow_cfg.cascade_count, ShadowConfig::kMaxCascades)
            : 0;
    glUniform3f(shader_mgr_->GetUniform(prog_inject_, "uAmbientColor"),
                light.ambient_color.r, light.ambient_color.g, light.ambient_color.b);
    glUniform1f(shader_mgr_->GetUniform(prog_inject_, "uAmbientIntensity"),
                light.ambient_intensity);
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uHasSun"),
                light.sun.has_value() ? 1 : 0);
    glUniform1f(shader_mgr_->GetUniform(prog_inject_, "uSunPhaseG"), params.sun_phase_g);
    glUniform1f(shader_mgr_->GetUniform(prog_inject_, "uSunGain"), params.sun_gain);
    if (light.sun.has_value()) {
        glUniform3f(shader_mgr_->GetUniform(prog_inject_, "uSunColor"),
                    light.sun->color.r, light.sun->color.g, light.sun->color.b);
        glUniform1f(shader_mgr_->GetUniform(prog_inject_, "uSunIntensity"),
                    light.sun->intensity);
        glUniform3f(shader_mgr_->GetUniform(prog_inject_, "uSunDir"),
                    light.sun->direction.x(), light.sun->direction.y(),
                    light.sun->direction.z());
    } else {
        // 无太阳：清零（防止上一帧残留）。
        glUniform3f(shader_mgr_->GetUniform(prog_inject_, "uSunColor"), 0.0f, 0.0f, 0.0f);
        glUniform1f(shader_mgr_->GetUniform(prog_inject_, "uSunIntensity"), 0.0f);
        glUniform3f(shader_mgr_->GetUniform(prog_inject_, "uSunDir"), 0.0f, -1.0f, 0.0f);
    }
    glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uCascadeCount"), cascade_count);
    if (cascade_count > 0) {
        CHECK_EQ(light.shadow_fbos.size(), static_cast<size_t>(cascade_count))
            << "FireFog: shadow_fbos 与 cascade_count 不一致";
        const ShadowConfig& sc = light.shadow_cfg;
        glUniform1fv(shader_mgr_->GetUniform(prog_inject_, "uCascadeRanges"),
                     cascade_count, sc.cascade_ranges);
        glUniform1fv(shader_mgr_->GetUniform(prog_inject_, "uShadowTexelWorld"),
                     ShadowConfig::kMaxCascades, light.shadow_texel_world);
        glUniform1fv(shader_mgr_->GetUniform(prog_inject_, "uShadowBiasCascade"),
                     ShadowConfig::kMaxCascades, sc.cascade_bias);
        glUniform1i(shader_mgr_->GetUniform(prog_inject_, "uShadowBiasOverride"),
                    sc.override_cascade_bias ? 1 : 0);
        glUniform1f(shader_mgr_->GetUniform(prog_inject_, "uShadowFadeStart"), sc.fade_start);
        glUniform1f(shader_mgr_->GetUniform(prog_inject_, "uShadowFadeEnd"), sc.fade_end);
        for (int c = 0; c < cascade_count; ++c) {
            const unsigned int unit =
                static_cast<unsigned int>(kShadowTexUnitBase) + static_cast<unsigned int>(c);
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, light.shadow_fbos[c].tex);
            const std::string n_unit = "uShadowMap[" + std::to_string(c) + "]";
            glUniform1i(shader_mgr_->GetUniform(prog_inject_, n_unit.c_str()),
                        static_cast<int>(unit));
            const std::string n_vp = "uShadowVP[" + std::to_string(c) + "]";
            glUniformMatrix4fv(shader_mgr_->GetUniform(prog_inject_, n_vp.c_str()),
                               1, GL_FALSE, light.shadow_vp[c]);
            const std::string n_dvp = "uShadowDepthVP[" + std::to_string(c) + "]";
            glUniformMatrix4fv(shader_mgr_->GetUniform(prog_inject_, n_dvp.c_str()),
                               1, GL_FALSE, light.shadow_depth_vp[c]);
        }
        glActiveTexture(GL_TEXTURE0);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // 解绑 CSM 单元。
    for (int c = 0; c < ShadowConfig::kMaxCascades; ++c) {
        glActiveTexture(GL_TEXTURE0 + kShadowTexUnitBase + c);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(GL_TEXTURE0);

    // ═══════════ 趟 2：scatter（沿 z 有序前缀 → scatter_tex_）═══════════
    glBindFramebuffer(GL_FRAMEBUFFER, scatter_fbo_);
    glViewport(0, 0, froxel_w, froxel_h);
    glDisable(GL_BLEND);
    glUseProgram(prog_scatter_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, inject_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_scatter_, "uInject"), 0);
    glUniform1i(shader_mgr_->GetUniform(prog_scatter_, "uBlockSide"), sblock);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // ═══════════ 趟 3：composite（全屏查表 → 就地混合进调用方 HDR FBO）═══════════
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    const GLenum one_buf[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, one_buf);
    glUseProgram(prog_composite_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scatter_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uScatter"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, scene_depth_tex != 0 ? scene_depth_tex : dummy_depth_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uSceneDepthTex"), 1);
    glActiveTexture(GL_TEXTURE0);
    glUniformMatrix4fv(shader_mgr_->GetUniform(prog_composite_, "uInvVP"), 1, GL_FALSE, inv_vp);
    glUniform3f(shader_mgr_->GetUniform(prog_composite_, "uCamPos"), cam.position.x(),
                cam.position.y(), cam.position.z());
    glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uZNear"), params.z_near);
    glUniform1f(shader_mgr_->GetUniform(prog_composite_, "uZFar"), params.z_far);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uNz"), params.nz);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uBlockSide"), sblock);
    glUniform1i(shader_mgr_->GetUniform(prog_composite_, "uTilePx"), params.tile_px);

    // 就地混合：out = S + dst·T（S=源累积亮度、T=透射率）。无雾像素 → 恒等。
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // ── 复原状态（调用方随后做 resolve / highlight / bloom / tone map / 2D，均自设状态）──
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    // ⭐ 复位混合函数：调用方的 3D 段用 glPushAttrib(GL_ENABLE_BIT|GL_VIEWPORT_BIT)，
    //   **不包含** blend func（属 GL_COLOR_BUFFER_BIT）。若不复位，composite 设的
    //   GL_ONE/GL_SRC_ALPHA 会沿用到后续 2D/字体绘制 ⇒ 文字糊成一片。
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);                        // 单元 0（当前活动）
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);                        // 单元 1
    glActiveTexture(static_cast<GLenum>(prev_active_tex));  // 复原调用方的活动纹理单元
}

}  // namespace jpov
