// JPOV VolumeFireRenderer 实现。见 volume_fire_renderer.h。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/volume_fire/volume_fire_renderer.h"

#include "tools/jpov/src/wbo/wbo_shader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <GL/gl.h>
#include <GL/glext.h>

#include "stb_image_write.h"
#include "third_party/stb/stb_image.h"

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#ifdef _WIN32
// windows.h 的 legacy 宏 `near`/`far` 会搅碎 `camera.near`/`camera.far`（与 particle_renderer.cc 同处理）。
#undef near
#undef far
#endif

#include <glog/logging.h>

namespace jpov {

namespace {

constexpr int kSceneDepthUnit = 0;
constexpr int kVolumeUnit = 1;
constexpr int kPaletteUnit = 3;   // 色谱 LUT（1D，宽 N×高 1）

Vec3f Sub(const Vec3f& a, const Vec3f& b) {
    return Vec3f(a.x() - b.x(), a.y() - b.y(), a.z() - b.z());
}
Vec3f NormalizeOr(const Vec3f& v, const Vec3f& fallback) {
    const float n = v.Norm();
    if (n < 1e-6f) {
        return fallback;
    }
    return Vec3f(v.x() / n, v.y() / n, v.z() / n);
}

// 4x4 列主序矩阵求逆（MESA gluInvertMatrix）。返回 false = 奇异。
bool Invert4(const float m[16], float inv[16]) {
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] +
             m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] -
             m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] +
             m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] -
              m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] -
             m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] +
             m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] -
             m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] +
              m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] +
             m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] -
             m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] +
              m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] -
              m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] -
             m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] +
             m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] -
              m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] +
              m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det == 0.0f) {
        return false;
    }
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) {
        inv[i] *= det;
    }
    return true;
}

}  // namespace

VolumeFireRenderer::VolumeFireRenderer() = default;
VolumeFireRenderer::~VolumeFireRenderer() = default;

void VolumeFireRenderer::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr) << "VolumeFireRenderer::Init: shader_mgr 不能为空";
    CHECK(!initialized_) << "VolumeFireRenderer::Init 重复调用";
    shader_mgr_ = shader_mgr;
    prog_draw_ = shader_mgr_->GetOrCreate("volume_fire_draw", {kVs, kFs});
    // 解析 shader 与粒子共用一份（wbo_shader.h）：同一套覆盖集代数，改一处即全改。
    prog_comp_ = shader_mgr_->GetOrCreate("volume_fire_composite",
                                          {kWboCompositeVs, kWboCompositeFs});
    glGenVertexArrays(1, &vao_);
    CHECK_NE(vao_, 0u);
    initialized_ = true;
}

void VolumeFireRenderer::Finalize() {
    wbo_.Destroy();
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    for (auto& kv : palette_tex_) {
        if (kv.second != 0) {
            glDeleteTextures(1, &kv.second);
        }
    }
    palette_tex_.clear();
    initialized_ = false;
}

unsigned int VolumeFireRenderer::PaletteTex(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return 0;
    }
    auto it = palette_tex_.find(path);
    if (it != palette_tex_.end()) {
        return it->second;
    }
    int w = 0, h = 0, ch = 0;
    unsigned char* data = stbi_load(path, &w, &h, &ch, 3);   // 强制 RGB
    unsigned int tex = 0;
    if (data == nullptr) {
        LOG(WARNING) << "VolumeFireRenderer: 色谱加载失败 " << path << " — " << stbi_failure_reason();
    } else {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        GLint prev_align = 4;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
        glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        stbi_image_free(data);
        LOG(INFO) << "VolumeFireRenderer: 色谱 " << path << " (" << w << "x" << h << ") -> tex " << tex;
    }
    palette_tex_[path] = tex;
    return tex;
}

void VolumeFireRenderer::EnsureTarget(int w, int h) {
    wbo_.Ensure({w, h, /*with_additive=*/false, /*with_depth=*/false,
                 /*linear_filter=*/true});
}

void VolumeFireRenderer::Begin(int w, int h) {
    CHECK(initialized_) << "VolumeFireRenderer::Begin: 未 Init";
    EnsureTarget(w, h);
    wbo_.Clear();
    has_content_ = false;
}

void VolumeFireRenderer::Draw(const VolumeFireCommand& cmd, VolumeManager& volume_mgr,
                              const Camera& camera,
                              const float view_proj[16],
                              unsigned int scene_depth_tex, int w, int h) {
    CHECK(initialized_) << "VolumeFireRenderer::Draw: 未 Init";
    VolumeInfo vol;
    if (!volume_mgr.GetInfo(cmd.volume_id, &vol)) {
        return;
    }
    CHECK_NE(scene_depth_tex, 0u);
    EnsureTarget(w, h);

    const Vec3f cam_pos = camera.position;
    const Vec3f fwd = NormalizeOr(Sub(camera.target, camera.position), Vec3f(0, 0, -1));

    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_POLYGON_BIT);
    wbo_.BindForDraw();
    if (cmd.debug_fullscreen) {
        glDisable(GL_CULL_FACE);   // 全屏 quad 不能被剔
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);   // 只留背面 → 每像素恰好一个盒面 → FS 只跑一次
    }

    glUseProgram(prog_draw_);
    glUniformMatrix4fv(shader_mgr_->GetUniform(prog_draw_, "uViewProj"), 1, GL_FALSE, view_proj);
    {
        float inv_vp[16];
        if (Invert4(view_proj, inv_vp)) {
            glUniformMatrix4fv(shader_mgr_->GetUniform(prog_draw_, "uInvViewProj"), 1,
                               GL_FALSE, inv_vp);
        }
    }
    glUniform3f(shader_mgr_->GetUniform(prog_draw_, "uBoxMin"),
                cmd.aabb_min.x(), cmd.aabb_min.y(), cmd.aabb_min.z());
    glUniform3f(shader_mgr_->GetUniform(prog_draw_, "uBoxMax"),
                cmd.aabb_max.x(), cmd.aabb_max.y(), cmd.aabb_max.z());
    glUniform3f(shader_mgr_->GetUniform(prog_draw_, "uCamPos"),
                cam_pos.x(), cam_pos.y(), cam_pos.z());
    glUniform3f(shader_mgr_->GetUniform(prog_draw_, "uCamForward"), fwd.x(), fwd.y(), fwd.z());
    glUniform3f(shader_mgr_->GetUniform(prog_draw_, "uVolDim"),
                static_cast<float>(vol.nx), static_cast<float>(vol.ny),
                static_cast<float>(vol.nz));
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uNFrames"), vol.nframes);
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uSigma"), cmd.sigma_scale);
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uEmission"), cmd.emission_scale);
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uTempScale"), cmd.temp_kelvin_scale);
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uTempMin"), cmd.temp_kelvin_min);
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uTempPow"), cmd.temp_emission_pow);
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uTempRef"), cmd.temp_kelvin_ref);
    glUniform3f(shader_mgr_->GetUniform(prog_draw_, "uSmokeAlbedo"),
                cmd.smoke_albedo.x(), cmd.smoke_albedo.y(), cmd.smoke_albedo.z());
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uSmokeScatter"), cmd.smoke_scatter);
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uSmokeSigma"), cmd.smoke_sigma_scale);
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uSmokePow"), cmd.smoke_pow);
    glUniform3f(shader_mgr_->GetUniform(prog_draw_, "uLightColor"),
                cmd.light_color.x(), cmd.light_color.y(), cmd.light_color.z());
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uChannels"), vol.channels);
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uFrame"), cmd.frame);
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uSteps"), cmd.steps);
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uJitter"), cmd.jitter ? 1 : 0);
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uFullscreen"),
                cmd.debug_fullscreen ? 1 : 0);
    glUniform2f(shader_mgr_->GetUniform(prog_draw_, "uViewport"),
                static_cast<float>(w), static_cast<float>(h));
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uSeed"), cmd.frame * 17.13f + 3.7f);
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uNear"), camera.near);
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uFar"), camera.far);

    glActiveTexture(GL_TEXTURE0 + kSceneDepthUnit);
    glBindTexture(GL_TEXTURE_2D, scene_depth_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uSceneDepth"), kSceneDepthUnit);
    glActiveTexture(GL_TEXTURE0 + kVolumeUnit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, vol.gl_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uVol"), kVolumeUnit);
    // 【色谱】Fire Color Ramp LUT（可选）：色相来自查表，失败/未配置时回退黑体。
    unsigned int pal_tex = PaletteTex(cmd.palette_png);
    glActiveTexture(GL_TEXTURE0 + kPaletteUnit);
    glBindTexture(GL_TEXTURE_2D, pal_tex);
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uPalette"), kPaletteUnit);
    glUniform1i(shader_mgr_->GetUniform(prog_draw_, "uUsePalette"), pal_tex != 0 ? 1 : 0);
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uRampLo"), cmd.palette_ramp_lo);
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uRampHi"), cmd.palette_ramp_hi);
    glUniform1f(shader_mgr_->GetUniform(prog_draw_, "uColorSat"), cmd.color_sat);

    glBindVertexArray(vao_);
    if (cmd.debug_fullscreen) {
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }
    glBindVertexArray(0);

    wbo_.EndDraw();                 // 还原 blend + 解绑
    glActiveTexture(GL_TEXTURE0);   // 归还纹理单元（P8：不留激活单元给后续 pass）
    glPopAttrib();
    has_content_ = true;
}

void VolumeFireRenderer::DumpWbo(int w, int h) {
    const std::string dir = debug_dump_dir_;
    auto dump = [&](GLenum buf, const char* suffix, bool is_alpha) {
        std::vector<float> px(static_cast<size_t>(w) * h * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, wbo_.fbo());
        glReadBuffer(buf);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, px.data());
        std::vector<unsigned char> out(static_cast<size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t s = (static_cast<size_t>(y) * w + x) * 4;
                const size_t d = (static_cast<size_t>(h - 1 - y) * w + x) * 4;  // 翻转
                float r, g, b;
                if (is_alpha) {
                    float a = 1.0f - px[s + 0];   // reveal → coverage
                    r = g = b = a;
                } else {
                    r = px[s + 0]; g = px[s + 1]; b = px[s + 2];
                }
                out[d + 0] = static_cast<unsigned char>(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
                out[d + 1] = static_cast<unsigned char>(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
                out[d + 2] = static_cast<unsigned char>(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
                out[d + 3] = 255;
            }
        }
        char name[512];
        std::snprintf(name, sizeof(name), "%s/dump_%03d_%s.png", dir.c_str(), dump_seq_, suffix);
        stbi_write_png(name, w, h, 4, out.data(), w * 4);
        LOG(INFO) << "[volume_fire] dump " << name;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    };
    dump(GL_COLOR_ATTACHMENT0, "accum", false);
    dump(GL_COLOR_ATTACHMENT1, "alpha", true);
    ++dump_seq_;
}

void VolumeFireRenderer::Composite(int w, int h) {
    if (!has_content_ || !wbo_.valid()) {
        return;
    }
    if (!debug_dump_dir_.empty()) {
        DumpWbo(w, h);
    }
    glPushAttrib(GL_ENABLE_BIT);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // over（预乘 src）
    glUseProgram(prog_comp_);
    glUniform1i(shader_mgr_->GetUniform(prog_comp_, "uAdditive"), 0);
    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, wbo_.accum_tex());
    glUniform1i(shader_mgr_->GetUniform(prog_comp_, "uAccum"), 0);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, wbo_.reveal_tex());
    glUniform1i(shader_mgr_->GetUniform(prog_comp_, "uReveal"), 1);
    // 共享 FS 里还有 uAdd 采样器（加法集分支）。本渲染器没有加法集，
    // 但**必须绑一张有效纹理**：编译器可能把采样提到分支之前，绑 0 会读到未定义内容。
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, wbo_.accum_tex());
    glUniform1i(shader_mgr_->GetUniform(prog_comp_, "uAdd"), 2);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);   // 归还激活单元（不留 unit 2 给后续 pass）
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glPopAttrib();
    has_content_ = false;
}

}  // namespace jpov
