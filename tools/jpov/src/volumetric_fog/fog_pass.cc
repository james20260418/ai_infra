// JPOV 局部体积雾 — 全屏 fog pass 实现（GL 侧）

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/volumetric_fog/fog_pass.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
#include "third_party/gl_loader-mingw/gl_loader.h"
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#endif

#include <glog/logging.h>

#include "tools/jpov/src/volumetric_fog/fog_common.h"
#include "tools/jpov/src/volumetric_fog/fog_pass_shader.h"
#include "tools/jpov/src/volumetric_fog/tile_fog_culling.h"

namespace jpov {
namespace volumetric_fog {

namespace {

// 一般 4x4 逆（double，cofactor 法）。用于 clip→world 反投影。
bool Mat4Inverse(const float m[16], double out[16]) {
    double inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
             m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
              m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-20) {
        return false;
    }
    det = 1.0 / det;
    for (int i = 0; i < 16; ++i) {
        out[i] = inv[i] * det;
    }
    return true;
}

// 逐团内散射源 L_in（CPU）：ambient + sun（无阴影，MVP 简化）+ Σ 点光源（按距离衰减）。
// 雾没有「被照表面」，光照在「团」上预算一次（见设计文档 §7.2）。
void ComputeLIn(const Vec3f& p, const RenderCommandList& cmds, float out[3]) {
    double r = 0.0, g = 0.0, b = 0.0;
    if (cmds.ambient.has_value()) {
        const AmbientLight& a = *cmds.ambient;
        double cr = a.color.r, cg = a.color.g, cb = a.color.b;
        if (a.tricolor.has_value()) {
            // 三色环境光：取三色均值作各向同性近似（雾没有明确法线朝向）。
            cr = cg = cb = 0.0;
            for (const Color& c : *a.tricolor) {
                cr += c.r / 3.0;
                cg += c.g / 3.0;
                cb += c.b / 3.0;
            }
        }
        r += cr * a.intensity;
        g += cg * a.intensity;
        b += cb * a.intensity;
    }
    if (cmds.sun.has_value()) {
        const DirectionalLight& s = *cmds.sun;
        r += s.color.r * s.intensity;
        g += s.color.g * s.intensity;
        b += s.color.b * s.intensity;
    }
    for (const PointLight& l : cmds.point_lights) {
        const double dx = p.x() - l.position.x();
        const double dy = p.y() - l.position.y();
        const double dz = p.z() - l.position.z();
        const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (l.linear_radius <= 0.0f || dist >= l.linear_radius) {
            continue;
        }
        double att = 1.0 - dist / l.linear_radius;
        att *= att;
        r += l.color.r * l.intensity * att;
        g += l.color.g * l.intensity * att;
        b += l.color.b * l.intensity * att;
    }
    out[0] = static_cast<float>(r);
    out[1] = static_cast<float>(g);
    out[2] = static_cast<float>(b);
}

void EnsureOutFbo(FogPassState* s, int w, int h) {
    if (s->out_fbo != 0 && s->w == w && s->h == h) {
        return;
    }
    if (s->out_fbo) {
        glDeleteFramebuffers(1, &s->out_fbo);
        glDeleteTextures(1, &s->out_tex);
    }
    glGenTextures(1, &s->out_tex);
    glBindTexture(GL_TEXTURE_2D, s->out_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &s->out_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s->out_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           s->out_tex, 0);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE)
        << "Fog out FBO incomplete";
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s->w = w;
    s->h = h;
}

}  // namespace

unsigned int RunFogPass(const RenderCommandList& cmds, ShaderManager& shader_mgr,
                        const float mvp[16], const Vec3f& cam_pos,
                        unsigned int hdr_input_tex, unsigned int scene_depth_tex,
                        int fbo_w, int fbo_h, FogPassState* state) {
    CHECK(state != nullptr);
    const size_t num_blobs = cmds.fog_spheres.size() + cmds.fog_cylinders.size();
    if (num_blobs == 0) {
        return 0;
    }

    // 1) tile 索引表（CPU）
    const FogTileLayout layout =
        ComputeFogTileLayout(fbo_w, fbo_h, kFogTileSize, kMaxFogPerTile);
    std::vector<uint16_t> indices(static_cast<size_t>(layout.tex_w) * layout.tex_h);
    BuildFogTileIndices(cmds.fog_spheres, cmds.fog_cylinders, fbo_w, fbo_h, mvp,
                        kFogTileSize, kMaxFogPerTile, layout, &indices);

    if (state->tile_tex == 0 || state->tile_tex_w != layout.tex_w ||
        state->tile_tex_h != layout.tex_h) {
        if (state->tile_tex) {
            glDeleteTextures(1, &state->tile_tex);
        }
        glGenTextures(1, &state->tile_tex);
        glBindTexture(GL_TEXTURE_2D, state->tile_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, layout.tex_w, layout.tex_h, 0,
                     GL_RED_INTEGER, GL_UNSIGNED_SHORT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        state->tile_tex_w = layout.tex_w;
        state->tile_tex_h = layout.tex_h;
    }
    glBindTexture(GL_TEXTURE_2D, state->tile_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, layout.tex_w, layout.tex_h,
                    GL_RED_INTEGER, GL_UNSIGNED_SHORT, indices.data());

    // 2) 属性纹理（RGBA32F，宽 = kFogAttrTexels，高 = 雾体数）
    const int attr_h = static_cast<int>(num_blobs);
    std::vector<float> attr(static_cast<size_t>(kFogAttrTexels) * attr_h * 4, 0.0f);
    auto write_texel = [&](int texel, int row, float x, float y, float z, float w) {
        const size_t base = (static_cast<size_t>(row) * kFogAttrTexels + texel) * 4;
        attr[base + 0] = x;
        attr[base + 1] = y;
        attr[base + 2] = z;
        attr[base + 3] = w;
    };
    int row = 0;
    for (const FogSphere& f : cmds.fog_spheres) {
        float lin[3];
        ComputeLIn(f.center, cmds, lin);
        write_texel(0, row, f.center.x(), f.center.y(), f.center.z(), f.radius);
        write_texel(1, row, 0.0f, 0.0f, 0.0f, 0.0f);
        write_texel(2, row, f.color.r, f.color.g, f.color.b, f.intensity);
        write_texel(3, row, lin[0], lin[1], lin[2], kFogShapeSphere);
        write_texel(4, row, static_cast<float>(f.profile), 0.0f, 0.0f, 0.0f);
        ++row;
    }
    for (const FogCylinder& f : cmds.fog_cylinders) {
        const Vec3f mid = {f.base.x() + f.axis.x() * 0.5f * f.height,
                           f.base.y() + f.axis.y() * 0.5f * f.height,
                           f.base.z() + f.axis.z() * 0.5f * f.height};
        float lin[3];
        ComputeLIn(mid, cmds, lin);
        write_texel(0, row, f.base.x(), f.base.y(), f.base.z(), f.radius);
        write_texel(1, row, f.axis.x(), f.axis.y(), f.axis.z(), f.height);
        write_texel(2, row, f.color.r, f.color.g, f.color.b, f.intensity);
        write_texel(3, row, lin[0], lin[1], lin[2], kFogShapeCylinder);
        write_texel(4, row, static_cast<float>(f.radial_profile),
                    static_cast<float>(f.axial_profile), 0.0f, 0.0f);
        ++row;
    }
    if (state->attr_tex == 0 || state->attr_h != attr_h) {
        if (state->attr_tex) {
            glDeleteTextures(1, &state->attr_tex);
        }
        glGenTextures(1, &state->attr_tex);
        glBindTexture(GL_TEXTURE_2D, state->attr_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, kFogAttrTexels, attr_h, 0,
                     GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        state->attr_h = attr_h;
    }
    glBindTexture(GL_TEXTURE_2D, state->attr_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kFogAttrTexels, attr_h, GL_RGBA,
                    GL_FLOAT, attr.data());

    // 3) 执行全屏 pass
    EnsureOutFbo(state, fbo_w, fbo_h);
    const unsigned int prog =
        shader_mgr.GetOrCreate("fog_pass", {kFogPassVs, kFogPassFs});
    glUseProgram(prog);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hdr_input_tex);
    glUniform1i(shader_mgr.GetUniform(prog, "uHdrTex"), 0);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, scene_depth_tex);
    glUniform1i(shader_mgr.GetUniform(prog, "uDepthTex"), 1);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, state->tile_tex);
    glUniform1i(shader_mgr.GetUniform(prog, "uTileTex"), 2);
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D, state->attr_tex);
    glUniform1i(shader_mgr.GetUniform(prog, "uAttrTex"), 3);

    double inv[16];
    CHECK(Mat4Inverse(mvp, inv)) << "fog: MVP 不可逆";
    const float invf[16] = {static_cast<float>(inv[0]),  static_cast<float>(inv[1]),
                            static_cast<float>(inv[2]),  static_cast<float>(inv[3]),
                            static_cast<float>(inv[4]),  static_cast<float>(inv[5]),
                            static_cast<float>(inv[6]),  static_cast<float>(inv[7]),
                            static_cast<float>(inv[8]),  static_cast<float>(inv[9]),
                            static_cast<float>(inv[10]), static_cast<float>(inv[11]),
                            static_cast<float>(inv[12]), static_cast<float>(inv[13]),
                            static_cast<float>(inv[14]), static_cast<float>(inv[15])};
    glUniformMatrix4fv(shader_mgr.GetUniform(prog, "uInvVP"), 1, GL_FALSE, invf);
    glUniform3f(shader_mgr.GetUniform(prog, "uCamPos"), cam_pos.x(), cam_pos.y(),
                cam_pos.z());
    glUniform1i(shader_mgr.GetUniform(prog, "uTileSize"), kFogTileSize);
    glUniform1i(shader_mgr.GetUniform(prog, "uK"), kMaxFogPerTile);

    glBindFramebuffer(GL_FRAMEBUFFER, state->out_fbo);
    glViewport(0, 0, fbo_w, fbo_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    return state->out_tex;
}

void DestroyFogPass(FogPassState* state) {
    if (state == nullptr) {
        return;
    }
    if (state->out_fbo) glDeleteFramebuffers(1, &state->out_fbo);
    if (state->out_tex) glDeleteTextures(1, &state->out_tex);
    if (state->tile_tex) glDeleteTextures(1, &state->tile_tex);
    if (state->attr_tex) glDeleteTextures(1, &state->attr_tex);
    *state = FogPassState{};
}

}  // namespace volumetric_fog
}  // namespace jpov
