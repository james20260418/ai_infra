// JPOV SSR 接入层实现。分工见 ssr_integration.h 顶部；算法/降采样/融合各自在自己的文件里。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/ssr/ssr_integration.h"

#include <algorithm>
#include <cmath>

#include <GL/gl.h>
#include <GL/glext.h>

#ifdef _WIN32
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include "third_party/gl_loader-mingw/gl_loader.h"
#endif

#include <glog/logging.h>

#include "tools/jpov/src/skeleton/skinning_shader.h"
#include "tools/jpov/src/ssr/ssr_shader.h"

namespace jpov {
namespace {

constexpr int kTexUnitCount = 8;

}  // namespace

SsrIntegration::SsrIntegration() = default;
SsrIntegration::~SsrIntegration() { Finalize(); }

void SsrIntegration::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr) << "SsrIntegration::Init 需要非空 ShaderManager";
    if (initialized_) {
        Finalize();
    }
    shader_mgr_ = shader_mgr;

    prog_attr_ = shader_mgr_->GetOrCreate("ssr_attr", {kSsrAttrVs, kSsrAttrFs, {}});
    prog_attr_cutout_ =
        shader_mgr_->GetOrCreate("ssr_attr_cutout", {kSsrAttrCutoutVs, kSsrAttrCutoutFs, {}});
    // 蒙皮属性 pass：VS 直接复用蒙皮主 pass 的 kSkinnedVs（它已经输出 vWorldNormal），
    // 只换 FS ⇒ 不再抄一份蒙皮代码。曲面（人体）的法线就来自这里。
    prog_attr_skinned_ =
        shader_mgr_->GetOrCreate("ssr_attr_skinned", {kSkinnedVs, kSsrAttrSkinnedFs, {}});

    // ── 同面并集柱体 pass（2026-11-12）──
    // SSR 分辨率上一次全屏 pass：输入 w（降采样后的 1/z，0 = 天空）+ 视空间法线，
    // 输出每格的 back（w 空间，R32F）。
    //
    // 同面判定照抄融合层：用本格 (P, 法线) 造切平面，预测邻居格中心处的 w，看残差。
    // 必须**双向判**（我判它 + 它判我）：单侧判会让"较近那一格"漏并，缝隙照样留着。
    // 合并**单侧**：back = min(back, 邻居 front)，永远不动 front ⇒ 不会造假近面。
    const std::string slab_fs = std::string(kSsrGlslVersion) + R"glsl(
uniform sampler2D uW;
uniform sampler2D uNormal;
uniform ivec2 uRes;
uniform float uMinThicknessRel;
uniform float uGateRel;
uniform float uGateCos;
uniform float uAspect;
uniform float uTanHalf;
out vec4 FragColor;

vec3 CellRay(ivec2 p) {
    vec2 ndc = (vec2(p) + 0.5) / vec2(uRes) * 2.0 - 1.0;
    return vec3(ndc.x * uAspect * uTanHalf, ndc.y * uTanHalf, -1.0);
}

bool SameSurface(float w_self, vec3 n_self, ivec2 p_self, float w_nb, vec3 n_nb, ivec2 p_nb) {
    if (!(w_self > 0.0) || !(w_nb > 0.0)) {
        return false;
    }
    if (dot(n_self, n_nb) < uGateCos) {
        return false;
    }
    vec3 c_self = CellRay(p_self);
    vec3 P = c_self / w_self;               // 视空间点（z = -1/w）
    vec3 c_nb = CellRay(p_nb);
    float den = dot(n_self, c_nb);
    if (abs(den) < 1.0e-7) {
        return false;
    }
    float t = dot(n_self, P) / den;         // 沿视轴的视距
    if (!(t > 1.0e-4)) {
        return false;
    }
    float w_pred = 1.0 / t;
    return abs(w_nb - w_pred) / max(w_pred, 1.0e-9) < uGateRel;
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float w = texelFetch(uW, p, 0).r;
    float base = w * (1.0 - uMinThicknessRel);
    if (!(w > 0.0)) {
        FragColor = vec4(0.0);
        return;
    }
    vec3 n = normalize(texelFetch(uNormal, p, 0).xyz);
    if (!(dot(n, n) > 0.5)) {          // 没有法线数据 ⇒ 只能给基线厚度
        FragColor = vec4(base, 0.0, 0.0, 1.0);
        return;
    }
    float back = base;
    ivec2 off[4] = ivec2[4](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1));
    for (int i = 0; i < 4; ++i) {
        ivec2 q = clamp(p + off[i], ivec2(0), uRes - 1);
        float w_nb = texelFetch(uW, q, 0).r;
        vec3 n_nb = normalize(texelFetch(uNormal, q, 0).xyz);
        bool fwd = SameSurface(w, n, p, w_nb, n_nb, q);
        bool rev = SameSurface(w_nb, n_nb, q, w, n, p);
        if ((fwd || rev) && w_nb > 0.0 && w_nb < back) {
            back = w_nb;
        }
    }
    FragColor = vec4(back, 0.0, 0.0, 1.0);
}
)glsl";
    prog_slab_ = shader_mgr_->GetOrCreate("ssr_slab_back", {kSsrFullscreenVs, slab_fs.c_str(), {}});

    downsample_.Init(shader_mgr_);
    filter_.Init(shader_mgr_);
    fusion_.Init(shader_mgr_);
    ssr_.Init(shader_mgr_);
    initialized_ = true;
}

void SsrIntegration::Finalize() {
    if (!initialized_) {
        return;
    }
    ssr_.Finalize();
    fusion_.Finalize();
    filter_.Finalize();
    downsample_.Finalize();

    if (slab_fbo_ != 0) {
        glDeleteFramebuffers(1, &slab_fbo_);
        slab_fbo_ = 0;
    }
    if (slab_tex_ != 0) {
        glDeleteTextures(1, &slab_tex_);
        slab_tex_ = 0;
    }
    slab_w_ = 0;
    slab_h_ = 0;
    if (copy_fbo_ != 0) {
        glDeleteFramebuffers(1, &copy_fbo_);
        copy_fbo_ = 0;
    }
    if (copy_tex_ != 0) {
        glDeleteTextures(1, &copy_tex_);
        copy_tex_ = 0;
    }
    width_ = 0;
    height_ = 0;
    prog_attr_ = 0;
    prog_attr_cutout_ = 0;
    prog_attr_skinned_ = 0;
    shader_mgr_ = nullptr;
    initialized_ = false;
}

void SsrIntegration::EnsureCopyTarget(int w, int h) {
    if (w == width_ && h == height_ && copy_fbo_ != 0) {
        return;
    }
    if (copy_fbo_ != 0) {
        glDeleteFramebuffers(1, &copy_fbo_);
        copy_fbo_ = 0;
    }
    if (copy_tex_ != 0) {
        glDeleteTextures(1, &copy_tex_);
        copy_tex_ = 0;
    }
    width_ = w;
    height_ = h;

    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);

    glGenTextures(1, &copy_tex_);
    glBindTexture(GL_TEXTURE_2D, copy_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &copy_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, copy_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, copy_tex_, 0);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
        << "SSR copy FBO 不完整";
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
}

void SsrIntegration::EnsureSlabTarget(int w, int h) {
    if (w == slab_w_ && h == slab_h_ && slab_fbo_ != 0) {
        return;
    }
    if (slab_fbo_ != 0) {
        glDeleteFramebuffers(1, &slab_fbo_);
        slab_fbo_ = 0;
    }
    if (slab_tex_ != 0) {
        glDeleteTextures(1, &slab_tex_);
        slab_tex_ = 0;
    }
    slab_w_ = w;
    slab_h_ = h;

    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGenTextures(1, &slab_tex_);
    glBindTexture(GL_TEXTURE_2D, slab_tex_);
    // R32F：只存 back（front 就是 w_tex 本身）。线性深度比 G16F 稳得多。
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, w, h, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &slab_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, slab_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slab_tex_, 0);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
        << "SSR slab FBO 不完整";
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
}

// 同面并集柱体：SSR 分辨率一次全屏 pass。必须在降采样之后、march 之前。
void SsrIntegration::BuildSlabBack(const SsrPassInputs& in, int w, int h) {
    EnsureSlabTarget(w, h);
    GLint prev_fbo = 0;
    GLint prev_viewport[4] = {0, 0, 0, 0};
    GLint prev_program = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);

    glBindFramebuffer(GL_FRAMEBUFFER, slab_fbo_);
    glViewport(0, 0, w, h);
    glUseProgram(prog_slab_);
    auto set1f = [&](const char* name, float v) {
        const int loc = shader_mgr_->GetUniform(prog_slab_, name);
        if (loc >= 0) {
            glUniform1f(loc, v);
        }
    };
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, downsample_.w_tex());
    glUniform1i(shader_mgr_->GetUniform(prog_slab_, "uW"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, downsample_.normal_tex());
    glUniform1i(shader_mgr_->GetUniform(prog_slab_, "uNormal"), 1);
    glUniform2i(shader_mgr_->GetUniform(prog_slab_, "uRes"), w, h);
    set1f("uMinThicknessRel", in.cfg.slab_min_thickness);
    set1f("uGateRel", in.cfg.slab_gate_rel);
    set1f("uGateCos", in.cfg.slab_gate_cos);
    set1f("uAspect", in.aspect);
    set1f("uTanHalf", in.tan_half_fov_y);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glUseProgram(static_cast<GLuint>(prev_program));
}

void SsrIntegration::Render(const SsrPassInputs& in, unsigned int dst_fbo) {
    CHECK(initialized_) << "SsrIntegration::Render 前必须先 Init";
    CHECK(in.cfg.enabled) << "SsrIntegration::Render 只在 enabled 时调用";
    CHECK_NE(in.hdr_color_tex, 0u);
    CHECK_NE(in.scene_depth_tex, 0u);
    CHECK_NE(in.attr_tex, 0u);
    CHECK_NE(in.env_cube_tex, 0u) << "融合需要环境 cubemap（与 3D pass 同一份）";
    CHECK_GT(in.full_w, 0);
    CHECK_GT(in.full_h, 0);
    CHECK_GT(in.near_plane, 0.0f);
    CHECK_GT(in.far_plane, in.near_plane);
    CHECK_GT(in.tan_half_fov_y, 0.0f);
    CHECK_GT(in.aspect, 0.0f);
    CHECK(IsValidScale(in.cfg.resolution_scale))
        << "SsrConfig::resolution_scale 只支持 1/N（N = 1..4，即 1.0 / 0.5 / 0.3333 / 0.25），得到 "
        << in.cfg.resolution_scale;

    const int scale = static_cast<int>(std::lround(1.0f / in.cfg.resolution_scale));

    // ── 保存调用方状态（返回前原样恢复）──
    GLint prev_fbo = 0;
    GLint prev_read = 0;
    GLint prev_viewport[4] = {0, 0, 0, 0};
    GLint prev_active = 0;
    GLint prev_blend = 0;
    GLint prev_depth = 0;
    GLint prev_cull = 0;
    GLint prev_scissor = 0;
    GLint prev_program = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    prev_blend = glIsEnabled(GL_BLEND);
    prev_depth = glIsEnabled(GL_DEPTH_TEST);
    prev_cull = glIsEnabled(GL_CULL_FACE);
    prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint prev_bind[kTexUnitCount] = {0};
    for (int u = 0; u < kTexUnitCount; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_bind[u]);
    }

    EnsureCopyTarget(in.full_w, in.full_h);

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    // ── ① HDR 副本：blit（不采样、不重建，纯拷贝）──
    glBindFramebuffer(GL_READ_FRAMEBUFFER, dst_fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, copy_fbo_);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glBlitFramebuffer(0, 0, in.full_w, in.full_h, 0, 0, in.full_w, in.full_h,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);

    // ── ② 输入准备（降采样）：全分辨率 → SSR 分辨率 ──
    downsample_.Run(in.hdr_color_tex, in.scene_depth_tex, in.attr_tex, in.attr_mat_tex,
                    in.full_w, in.full_h, scale, in.near_plane, in.far_plane);

    // ── ②b 同面并集柱体（slab_mode = 1）：SSR 分辨率一次全屏 pass ──
    const bool use_slab = (in.cfg.slab_mode == 1);
    if (use_slab) {
        BuildSlabBack(in, downsample_.width(), downsample_.height());
    }

    // ── ③ 算法（模块）：SSR 分辨率 → radiance + ray_ratio + 类型 ──
    SsrInputs si;
    si.color_tex = downsample_.color_tex();
    si.w_tex = downsample_.w_tex();
    si.normal_tex = downsample_.normal_tex();
    si.width = downsample_.width();
    si.height = downsample_.height();
    si.near_plane = in.near_plane;
    si.far_plane = in.far_plane;
    si.tan_half_fov_y = in.tan_half_fov_y;
    si.aspect = in.aspect;
    si.thickness_pixels = in.cfg.thickness_pixels;
    si.min_thickness_m = in.cfg.min_thickness_m;
    si.max_distance_m = in.cfg.max_distance_m;
    si.max_ray_pixels = in.cfg.max_ray_pixels;
    si.tile_size = in.cfg.tile_size;
    si.rough_tex = downsample_.rough_tex();       // 逐像素 roughness（发射线截止用）
    si.roughness_cutoff = in.cfg.roughness_cutoff;
    si.debug_reason = false;
    si.grazing_fade = in.cfg.grazing_fade;
    si.grazing_fade_start = in.cfg.grazing_fade_start;
    si.grazing_fade_end = in.cfg.grazing_fade_end;
    if (use_slab) {
        si.slab_back_tex = slab_tex_;
        si.use_slab = true;
    }
    const SsrOutputs& outs = ssr_.Render(si);

    // ── ④ 粗糙度预滤波：A 预乘 mip 链（决定了"磨砂"的模糊量）──
    filter_.Run(outs.radiance_tex, outs.type_tex, downsample_.width(), downsample_.height());

    // ── ⑤ 融合：升采样 + 替换环境镜面项，写回 3D FBO 颜色 ──
    SsrFusionInputs fi;
    fi.hdr_copy_tex = copy_tex_;
    fi.attr_tex = in.attr_tex;
    fi.attr_mat_tex = in.attr_mat_tex;
    fi.ssr_mip_tex = filter_.mip_tex();
    fi.ssr_mip_levels = filter_.level_count();
    fi.scene_depth_tex = in.scene_depth_tex;
    fi.full_w = in.full_w;
    fi.full_h = in.full_h;
    fi.ssr_radiance_tex = outs.radiance_tex;
    fi.down_w_tex = downsample_.w_tex();
    fi.down_normal_tex = downsample_.normal_tex();
    fi.ssr_w = downsample_.width();
    fi.ssr_h = downsample_.height();
    fi.scale = scale;
    fi.near_plane = in.near_plane;
    fi.far_plane = in.far_plane;
    fi.tan_half_fov_y = in.tan_half_fov_y;
    fi.aspect = in.aspect;
    fi.env_cube_tex = in.env_cube_tex;
    fi.env_max_lod = in.env_max_lod;
    fi.env_sky_scale = in.env_sky_scale;
    fi.env_enabled = in.env_enabled;
    fi.cfg = in.cfg;
    fi.dst_fbo = dst_fbo;
    fusion_.Run(fi);

    // ── 恢复 ──
    glUseProgram(static_cast<GLuint>(prev_program));
    for (int u = 0; u < kTexUnitCount; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_bind[u]));
    }
    glActiveTexture(static_cast<GLenum>(prev_active));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_read));
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    if (prev_blend != 0) glEnable(GL_BLEND);
    if (prev_depth != 0) glEnable(GL_DEPTH_TEST);
    if (prev_cull != 0) glEnable(GL_CULL_FACE);
    if (prev_scissor != 0) glEnable(GL_SCISSOR_TEST);
}

}  // namespace jpov
