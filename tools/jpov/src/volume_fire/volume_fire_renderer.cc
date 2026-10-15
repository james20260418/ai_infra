// JPOV VolumeFireRenderer 实现。见 volume_fire_renderer.h。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/volume_fire/volume_fire_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include <GL/gl.h>
#include <GL/glext.h>

#include "stb_image_write.h"

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include <glog/logging.h>

namespace jpov {

namespace {

constexpr int kSceneDepthUnit = 0;
constexpr int kVolumeUnit = 1;

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

// JPVOL2 header（little-endian，40 字节）。
struct JpovVol2Header {
    char magic[8];
    uint32_t version;
    uint32_t nx, ny, nz;
    uint32_t nframes;
    uint32_t channels;
    uint32_t dtype;   // 0=float32, 1=uint8(归一化)
    uint32_t reserved;
};

}  // namespace

VolumeFireRenderer::VolumeFireRenderer() = default;
VolumeFireRenderer::~VolumeFireRenderer() = default;

void VolumeFireRenderer::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr) << "VolumeFireRenderer::Init: shader_mgr 不能为空";
    CHECK(!initialized_) << "VolumeFireRenderer::Init 重复调用";
    shader_mgr_ = shader_mgr;
    prog_draw_ = shader_mgr_->GetOrCreate("volume_fire_draw", {kVs, kFs});
    prog_comp_ = shader_mgr_->GetOrCreate("volume_fire_composite", {kVsComp, kFsComp});
    glGenVertexArrays(1, &vao_);
    CHECK_NE(vao_, 0u);
    initialized_ = true;
}

void VolumeFireRenderer::Finalize() {
    DestroyWbo();
    for (auto& kv : volumes_) {
        if (kv.second.gl_tex != 0) {
            glDeleteTextures(1, &kv.second.gl_tex);
        }
    }
    volumes_.clear();
    path_to_volume_.clear();
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    initialized_ = false;
}

uint32_t VolumeFireRenderer::RegisterVolume(const std::string& path) {
    const auto it = path_to_volume_.find(path);
    if (it != path_to_volume_.end()) {
        return it->second;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) {
        LOG(WARNING) << "VolumeFireRenderer: 打不开 " << path;
        return 0;
    }
    JpovVol2Header h{};
    f.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (std::memcmp(h.magic, "JPVOL2", 6) != 0) {
        LOG(WARNING) << "VolumeFireRenderer: 非 JPVOL2 文件 " << path;
        return 0;
    }
    CHECK_GT(h.nx, 0u);
    CHECK_GT(h.ny, 0u);
    CHECK_GT(h.nz, 0u);
    CHECK_GE(h.nframes, 1u);
    CHECK_EQ(h.channels, 2u) << "JPVOL2 期望 2 通道 (R=density, G=temperature)";
    CHECK(h.dtype == 0u || h.dtype == 1u) << "JPVOL2 dtype 仅支持 0(f32)/1(u8)";

    const size_t per_frame = static_cast<size_t>(h.nx) * h.ny * h.nz * h.channels;
    const size_t bytes_per_frame = per_frame * (h.dtype == 1u ? 1u : 4u);
    std::vector<unsigned char> buf(bytes_per_frame);
    // 数据紧跟 40 字节 header。
    f.seekg(sizeof(JpovVol2Header), std::ios::beg);

    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    const GLenum internal = (h.dtype == 1u) ? GL_RG8 : GL_RG16F;
    const GLenum type = (h.dtype == 1u) ? GL_UNSIGNED_BYTE : GL_FLOAT;
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, internal,
                 static_cast<GLsizei>(h.nx), static_cast<GLsizei>(h.ny * h.nz),
                 static_cast<GLsizei>(h.nframes), 0, GL_RG, type, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    for (uint32_t fr = 0; fr < h.nframes; ++fr) {
        f.read(reinterpret_cast<char*>(buf.data()),
               static_cast<std::streamsize>(bytes_per_frame));
        CHECK(f.good()) << "JPVOL2 数据读取不足: " << path << " frame " << fr;
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(fr),
                        static_cast<GLsizei>(h.nx), static_cast<GLsizei>(h.ny * h.nz),
                        1, GL_RG, type, buf.data());
    }
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    const uint32_t id = next_volume_id_++;
    volumes_[id] = VolumeEntry{tex, static_cast<int>(h.nx), static_cast<int>(h.ny),
                               static_cast<int>(h.nz), static_cast<int>(h.nframes)};
    path_to_volume_[path] = id;
    LOG(INFO) << "[volume_fire] loaded " << path << " dims=(" << h.nx << "," << h.ny
              << "," << h.nz << ") frames=" << h.nframes << " dtype=" << h.dtype
              << " -> id " << id;
    return id;
}

void VolumeFireRenderer::EnsureWbo(int w, int h) {
    if (wbo_fbo_ != 0 && wbo_w_ == w && wbo_h_ == h) {
        return;
    }
    CHECK_GT(w, 0);
    CHECK_GT(h, 0);
    DestroyWbo();
    glGenTextures(1, &accum_tex_);
    glBindTexture(GL_TEXTURE_2D, accum_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &reveal_tex_);
    glBindTexture(GL_TEXTURE_2D, reveal_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, w, h, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &depth_tex_);
    glBindTexture(GL_TEXTURE_2D, depth_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, w, h, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &wbo_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, wbo_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, accum_tex_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, reveal_tex_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, depth_tex_, 0);
    const GLenum bufs[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    glDrawBuffers(3, bufs);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE)
        << "VolumeFireRenderer WBO FBO incomplete";
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    wbo_w_ = w;
    wbo_h_ = h;
}

void VolumeFireRenderer::DestroyWbo() {
    if (wbo_fbo_) { glDeleteFramebuffers(1, &wbo_fbo_); wbo_fbo_ = 0; }
    if (accum_tex_) { glDeleteTextures(1, &accum_tex_); accum_tex_ = 0; }
    if (reveal_tex_) { glDeleteTextures(1, &reveal_tex_); reveal_tex_ = 0; }
    if (depth_tex_) { glDeleteTextures(1, &depth_tex_); depth_tex_ = 0; }
    wbo_w_ = 0;
    wbo_h_ = 0;
}

void VolumeFireRenderer::Begin(int w, int h) {
    CHECK(initialized_) << "VolumeFireRenderer::Begin: 未 Init";
    EnsureWbo(w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, wbo_fbo_);
    glViewport(0, 0, w, h);
    const GLfloat clr_accum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLfloat clr_reveal[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    const GLfloat clr_depth[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glClearBufferfv(GL_COLOR, 0, clr_accum);
    glClearBufferfv(GL_COLOR, 1, clr_reveal);
    glClearBufferfv(GL_COLOR, 2, clr_depth);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    has_content_ = false;
}

void VolumeFireRenderer::Draw(const VolumeFireCommand& cmd, const Camera& camera,
                              const float view_proj[16],
                              unsigned int scene_depth_tex, int w, int h) {
    CHECK(initialized_) << "VolumeFireRenderer::Draw: 未 Init";
    const auto vit = volumes_.find(cmd.volume_id);
    if (vit == volumes_.end()) {
        return;
    }
    CHECK_NE(scene_depth_tex, 0u);
    const VolumeEntry& vol = vit->second;
    EnsureWbo(w, h);

    const Vec3f cam_pos = camera.position;
    const Vec3f fwd = NormalizeOr(Sub(camera.target, camera.position), Vec3f(0, 0, -1));

    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, wbo_fbo_);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    if (cmd.debug_fullscreen) {
        glDisable(GL_CULL_FACE);   // 全屏 quad 不能被剔
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);   // 只留背面 → 每像素恰好一个盒面 → FS 只跑一次
    }
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunci(0, GL_ONE, GL_ONE);                    // accum += 色·α
    glBlendFunci(1, GL_ZERO, GL_ONE_MINUS_SRC_COLOR);   // reveal *= (1−α)
    glBlendFunci(2, GL_ONE, GL_ONE);                    // depth += z·α

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

    glBindVertexArray(vao_);
    if (cmd.debug_fullscreen) {
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }
    glBindVertexArray(0);

    glBlendFunci(0, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendFunci(1, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendFunci(2, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPopAttrib();
    has_content_ = true;
}

void VolumeFireRenderer::DumpWbo(int w, int h) {
    const std::string dir = debug_dump_dir_;
    auto dump = [&](GLenum buf, const char* suffix, bool is_alpha) {
        std::vector<float> px(static_cast<size_t>(w) * h * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, wbo_fbo_);
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
                out[d + 0] = static_cast<unsigned char>(std::min(std::max(r, 0.0f), 1.0f) * 255.0f + 0.5f);
                out[d + 1] = static_cast<unsigned char>(std::min(std::max(g, 0.0f), 1.0f) * 255.0f + 0.5f);
                out[d + 2] = static_cast<unsigned char>(std::min(std::max(b, 0.0f), 1.0f) * 255.0f + 0.5f);
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
    if (!has_content_ || wbo_fbo_ == 0) {
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
    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, accum_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_comp_, "uAccum"), 0);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, reveal_tex_);
    glUniform1i(shader_mgr_->GetUniform(prog_comp_, "uReveal"), 1);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glPopAttrib();
    has_content_ = false;
}

}  // namespace jpov
