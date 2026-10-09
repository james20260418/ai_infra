// JPOV FireFogRenderer 成员函数实现
//
// 见 fire_fog_renderer.h / fire_fog_shader.h / tools/jpov/docs/jpov_fire_fog_design.md。
//
// 本实现是「低分辨率 ZDist + 屏幕空间深度敏感高斯」管线（Danis 2026-10-09 定「简单加权」版）：
//   命令层 PointFog → FogBody（fire_fog_lower.h）→ CPU 屏幕 tile 剪枝（低分辨率）
//   → 趟 A：低分辨率逐像素 ZDist 累加 + 降维 ≤8 + 打包成 6 RGBA32F
//   → 趟 B：低分辨率高斯合并（g·ρ 简单加权）→ 打包
//   → 趟 C：主分辨率双线性重建 ZDist + 末端积分 → 就地混合到当前 3D HDR FBO。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/fire_fog/fire_fog_renderer.h"

#include <algorithm>
#include <cmath>

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

// 4x4 列主序求逆（Gauss-Jordan + 部分主元）。把 view_proj 转成逆 VP（由屏幕坐标反推
// 世界方向/可见面位置）。不可逆返回 false。（与 horizon_fog_renderer.cc 同款实现。）
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

constexpr int kBodyTexelsPerFog = 3;   // 每团 3 个 RGBA32F texel

// 组装一趟的 FS 源码（#version + 公共前置 + 趟体）。
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
    // 组装 + 编译三趟 program（program 归 ShaderManager 释放；源码需存活到编译完成，
    // 故存成成员字符串）。
    fs_zdst_ = AssembleFragmentShader(kFireFogZdstBody);
    fs_gauss_ = AssembleFragmentShader(kFireFogGaussBody);
    fs_compose_ = AssembleFragmentShader(kFireFogComposeBody);
    prog_zdst_ = shader_mgr_->GetOrCreate(
        "fire_fog_zdst", {kFireFogVs, fs_zdst_.c_str()},
        {"out0", "out1", "out2", "out3", "out4", "out5"});
    prog_gauss_ = shader_mgr_->GetOrCreate(
        "fire_fog_gauss", {kFireFogVs, fs_gauss_.c_str()},
        {"out0", "out1", "out2", "out3", "out4", "out5"});
    prog_compose_ = shader_mgr_->GetOrCreate(
        "fire_fog_compose", {kFireFogVs, fs_compose_.c_str()});
}

void FireFogRenderer::Finalize() {
    if (tile_index_tex_ != 0) {
        glDeleteTextures(1, &tile_index_tex_);
        tile_index_tex_ = 0;
    }
    if (fog_body_tex_ != 0) {
        glDeleteTextures(1, &fog_body_tex_);
        fog_body_tex_ = 0;
    }
    for (int s = 0; s < 2; ++s) {
        if (zdist_fbo_[s] != 0) {
            glDeleteFramebuffers(1, &zdist_fbo_[s]);
            zdist_fbo_[s] = 0;
        }
        for (int t = 0; t < kZDistTexelsPerPixel; ++t) {
            if (zdist_tex_[s][t] != 0) {
                glDeleteTextures(1, &zdist_tex_[s][t]);
                zdist_tex_[s][t] = 0;
            }
        }
    }
    zdist_w_ = 0;
    zdist_h_ = 0;
    tile_tex_w_ = 0;
    tile_tex_h_ = 0;
    grid_w_ = 0;
    grid_h_ = 0;
    bodies_.clear();
    prog_zdst_ = 0;
    prog_gauss_ = 0;
    prog_compose_ = 0;
    shader_mgr_ = nullptr;
}

void FireFogRenderer::EnsureTileTexture(int low_w, int low_h) {
    CHECK_GT(low_w, 0);
    CHECK_GT(low_h, 0);
    const int cols = (low_w + kTileSize - 1) / kTileSize;
    const int rows = (low_h + kTileSize - 1) / kTileSize;
    const int tex_w = cols * kTexelsPerTile;
    const int tex_h = rows;
    const bool need_rebuild = (tile_index_tex_ == 0) || (tex_w != tile_tex_w_) ||
                              (tex_h != tile_tex_h_);
    if (need_rebuild) {
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
        grid_w_ = cols;
        grid_h_ = rows;
    }
}

void FireFogRenderer::EnsureZDistTargets(int low_w, int low_h) {
    CHECK_GT(low_w, 0);
    CHECK_GT(low_h, 0);
    const bool rebuild = (zdist_fbo_[0] == 0) || (low_w != zdist_w_) || (low_h != zdist_h_);
    if (!rebuild) {
        return;
    }
    GLint prev_binding = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_binding);
    for (int s = 0; s < 2; ++s) {
        if (zdist_fbo_[s] != 0) {
            glDeleteFramebuffers(1, &zdist_fbo_[s]);
            zdist_fbo_[s] = 0;
        }
        for (int t = 0; t < kZDistTexelsPerPixel; ++t) {
            if (zdist_tex_[s][t] != 0) {
                glDeleteTextures(1, &zdist_tex_[s][t]);
                zdist_tex_[s][t] = 0;
            }
        }
        glGenFramebuffers(1, &zdist_fbo_[s]);
        glBindFramebuffer(GL_FRAMEBUFFER, zdist_fbo_[s]);
        for (int t = 0; t < kZDistTexelsPerPixel; ++t) {
            glGenTextures(1, &zdist_tex_[s][t]);
            glBindTexture(GL_TEXTURE_2D, zdist_tex_[s][t]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            // 打包后的 lane 以位模式存（floatBitsToUint），必须用 fp32 附件不做任何换算。
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, low_w, low_h, 0,
                         GL_RGBA, GL_FLOAT, nullptr);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + t,
                                   GL_TEXTURE_2D, zdist_tex_[s][t], 0);
        }
        const GLenum bufs[6] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
                                GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3,
                                GL_COLOR_ATTACHMENT4, GL_COLOR_ATTACHMENT5};
        glDrawBuffers(6, bufs);
        const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        CHECK(st == GL_FRAMEBUFFER_COMPLETE)
            << "FireFogRenderer: ZDist FBO incomplete, status=0x" << std::hex << st;
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    // 复原调用方 FBO 绑定（本函数会临时绑自己的 FBO）。
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_binding));
    zdist_w_ = low_w;
    zdist_h_ = low_h;
}

void FireFogRenderer::Draw(const std::vector<PointFog>& fogs,
                           const Camera& cam,
                           const float view_proj[16],
                           int viewport_w,
                           int viewport_h,
                           unsigned int scene_depth_tex,
                           const FireFogParams& params) {
    CHECK(shader_mgr_ != nullptr) << "FireFogRenderer::Init 未调用";
    if (fogs.empty()) {
        return;   // 零开销
    }
    CHECK_GT(viewport_w, 0);
    CHECK_GT(viewport_h, 0);
    CHECK_GT(params.gaussian_sigma, 0.0f) << "FireFogParams.gaussian_sigma 必须 > 0";
    const int down = std::min(std::max(params.downsample, 1), kMaxDownsample);

    // 保存调用方状态（趟 C 要回到调用方的 FBO / viewport 才能就地混合）。
    // ⚠️ 必须在本函数内任何可能重绑 FBO 的操作（EnsureZDistTargets）**之前**取，
    //    否则重建帧会拿到被 clobber 后的 binding。
    GLint prev_fbo = 0;
    GLint prev_vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_vp);

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

    // ── 低分辨率 ZDist 尺寸 ──
    const int low_w = std::max(1, (viewport_w + down - 1) / down);
    const int low_h = std::max(1, (viewport_h + down - 1) / down);

    // ── CPU 屏幕 tile 剪枝（L1，低分辨率域）──
    EnsureTileTexture(low_w, low_h);
    const std::vector<uint8_t> tile_data = BuildTileFogIndexData(
        bodies_, view_proj, grid_w_, grid_h_, kTileSize, kMaxFogsPerTile,
        kTexelsPerTile, kFogIndexSentinel);
    glBindTexture(GL_TEXTURE_2D, tile_index_tex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tile_tex_w_, tile_tex_h_,
                    GL_RGBA, GL_UNSIGNED_BYTE, tile_data.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // ── 团属性纹理（RGBA32F；每团 3 texel）──
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

    EnsureZDistTargets(low_w, low_h);

    float inv_vp[16];
    CHECK(Mat4Invert(view_proj, inv_vp))
        << "FireFogRenderer: 相机矩阵不可逆（near/far 非法或退化）";

    const GLenum mrt6[6] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
                            GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3,
                            GL_COLOR_ATTACHMENT4, GL_COLOR_ATTACHMENT5};

    // ═══════════ 趟 A：低分辨率 ZDist 累加 → zdist_tex_[0] ═══════════
    glBindFramebuffer(GL_FRAMEBUFFER, zdist_fbo_[0]);
    glViewport(0, 0, low_w, low_h);
    glDrawBuffers(6, mrt6);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glUseProgram(prog_zdst_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_depth_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_zdst_, "uSceneDepthTex"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tile_index_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_zdst_, "uTileFogIndices"), 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, fog_body_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_zdst_, "uFogBodyTex"), 2);
    glActiveTexture(GL_TEXTURE0);
    glUniformMatrix4fv(shader_mgr_->GetUniform(prog_zdst_, "uInvVP"), 1, GL_FALSE, inv_vp);
    glUniform3f(shader_mgr_->GetUniform(prog_zdst_, "uCamPos"), cam.position.x(),
                cam.position.y(), cam.position.z());
    glUniform1f(shader_mgr_->GetUniform(prog_zdst_, "uZNear"), cam.near);
    glUniform1f(shader_mgr_->GetUniform(prog_zdst_, "uZFar"), cam.far);
    glUniform1i(shader_mgr_->GetUniform(prog_zdst_, "uTotalFogs"), count);
    glUniform1i(shader_mgr_->GetUniform(prog_zdst_, "uJitterEnable"),
                params.jitter_enable ? 1 : 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // ═══════════ 趟 B：低分辨率高斯合并 → zdist_tex_[1] ═══════════
    const int src = params.gaussian_enable ? 1 : 0;
    if (params.gaussian_enable) {
        glBindFramebuffer(GL_FRAMEBUFFER, zdist_fbo_[1]);
        glViewport(0, 0, low_w, low_h);
        glDrawBuffers(6, mrt6);
        glDisable(GL_BLEND);
        glUseProgram(prog_gauss_);
        for (int t = 0; t < kZDistTexelsPerPixel; ++t) {
            glActiveTexture(GL_TEXTURE0 + t);
            glBindTexture(GL_TEXTURE_2D, zdist_tex_[0][t]);
            const std::string name = "uZDist" + std::to_string(t);
            glUniform1i(shader_mgr_->GetUniform(prog_gauss_, name.c_str()), t);
        }
        // 核半径 = clamp(ceil(2σ), 1, kMaxKernelRadius)。
        const float sigma = params.gaussian_sigma;
        int radius = static_cast<int>(std::ceil(2.0f * sigma));
        radius = std::min(std::max(radius, 1), kMaxKernelRadius);
        glUniform1i(shader_mgr_->GetUniform(prog_gauss_, "uKernelRadius"), radius);
        glUniform1f(shader_mgr_->GetUniform(prog_gauss_, "uGaussSigma"), sigma);
        glActiveTexture(GL_TEXTURE0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // ═══════════ 趟 C：主分辨率合成 → 就绪混合进调用方 HDR FBO ═══════════
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    const GLenum one_buf[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, one_buf);
    glUseProgram(prog_compose_);
    for (int t = 0; t < kZDistTexelsPerPixel; ++t) {
        glActiveTexture(GL_TEXTURE0 + t);
        glBindTexture(GL_TEXTURE_2D, zdist_tex_[src][t]);
        const std::string name = "uZDist" + std::to_string(t);
        glUniform1i(shader_mgr_->GetUniform(prog_compose_, name.c_str()), t);
    }
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, scene_depth_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_compose_, "uSceneDepthTex"), 6);
    glActiveTexture(GL_TEXTURE0);
    glUniformMatrix4fv(shader_mgr_->GetUniform(prog_compose_, "uInvVP"), 1, GL_FALSE, inv_vp);
    glUniform3f(shader_mgr_->GetUniform(prog_compose_, "uCamPos"), cam.position.x(),
                cam.position.y(), cam.position.z());
    glUniform1f(shader_mgr_->GetUniform(prog_compose_, "uZNear"), cam.near);
    glUniform1f(shader_mgr_->GetUniform(prog_compose_, "uZFar"), cam.far);

    // 就地混合：out = S + dst·T（S=源累积亮度、T=透射率）。无雾像素 → 恒等。
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // 复原状态（调用方随后做 resolve / highlight / bloom / tone map / 2D，均自设状态）。
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    // ⭐ 复位混合函数：调用方的 3D 段用 glPushAttrib(GL_ENABLE_BIT|GL_VIEWPORT_BIT)，
    //   **不包含** blend func（属 GL_COLOR_BUFFER_BIT）。若不复位，趟 C 设的
    //   GL_ONE/GL_SRC_ALPHA 会沿用到后续 2D/字体绘制 ⇒ 文字糊成一片。
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(0);
    for (int t = 0; t < kZDistTexelsPerPixel; ++t) {
        glActiveTexture(GL_TEXTURE0 + t);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
}

}  // namespace jpov
