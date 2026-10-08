// JPOV FireFogRenderer 成员函数实现
//
// 见 fire_fog_renderer.h / fire_fog_shader.h / tools/jpov/docs/jpov_fire_fog_design.md。
// 本实现是点状雾 MVP：命令层 PointFog → FogBody（fire_fog_lower.h）→ CPU 屏幕 tile 剪枝 →
// 全屏 pass shader（逐像素 ZDist 累加 + 降维 + 末端积分）→ 就地混合到当前 3D HDR FBO。

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

}  // anonymous namespace

namespace jpov {

FireFogRenderer::~FireFogRenderer() {
    Finalize();
}

void FireFogRenderer::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr);
    shader_mgr_ = shader_mgr;
    // 编译 + 链接雾火 shader（供 Draw 使用；program 归 ShaderManager 释放）。
    shader_mgr_->GetOrCreate("fire_fog", {kFireFogVs, kFireFogFs});
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
    tile_tex_w_ = 0;
    tile_tex_h_ = 0;
    grid_w_ = 0;
    grid_h_ = 0;
    bodies_.clear();
    shader_mgr_ = nullptr;
}

void FireFogRenderer::EnsureTileTexture(int viewport_w, int viewport_h) {
    CHECK_GT(viewport_w, 0);
    CHECK_GT(viewport_h, 0);
    const int cols = (viewport_w + kTileSize - 1) / kTileSize;
    const int rows = (viewport_h + kTileSize - 1) / kTileSize;
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

void FireFogRenderer::Draw(const std::vector<PointFog>& fogs,
                           const Camera& cam,
                           const float view_proj[16],
                           int viewport_w,
                           int viewport_h,
                           unsigned int scene_depth_tex) {
    CHECK(shader_mgr_ != nullptr) << "FireFogRenderer::Init 未调用";
    if (fogs.empty()) {
        return;   // 零开销
    }
    CHECK_GT(viewport_w, 0);
    CHECK_GT(viewport_h, 0);

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

    // ── CPU 屏幕 tile 剪枝（L1）──
    EnsureTileTexture(viewport_w, viewport_h);
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

    // ── 全屏 pass ──
    const unsigned int prog = shader_mgr_->GetOrCreate("fire_fog", {kFireFogVs, kFireFogFs});
    glUseProgram(prog);

    float inv_vp[16];
    CHECK(Mat4Invert(view_proj, inv_vp))
        << "FireFogRenderer: 相机矩阵不可逆（near/far 非法或退化）";

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_depth_tex);
    glUniform1i(shader_mgr_->GetUniform(prog, "uSceneDepthTex"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tile_index_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog, "uTileFogIndices"), 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, fog_body_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog, "uFogBodyTex"), 2);
    glActiveTexture(GL_TEXTURE0);

    glUniformMatrix4fv(shader_mgr_->GetUniform(prog, "uInvVP"), 1, GL_FALSE, inv_vp);
    glUniform3f(shader_mgr_->GetUniform(prog, "uCamPos"), cam.position.x(),
                cam.position.y(), cam.position.z());
    glUniform1f(shader_mgr_->GetUniform(prog, "uZNear"), cam.near);
    glUniform1f(shader_mgr_->GetUniform(prog, "uZFar"), cam.far);
    glUniform1i(shader_mgr_->GetUniform(prog, "uTotalFogs"), count);

    // 就地混合：out = S + dst·T（S=源累积亮度、T=透射率）。无雾像素 → 恒等。
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_SRC_ALPHA);

    glDrawArrays(GL_TRIANGLES, 0, 3);   // 全屏三角形（无 VAO/VBO，用 gl_VertexID）

    // 复原状态（调用方随后做 resolve / highlight / bloom，均自设状态）。
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace jpov
