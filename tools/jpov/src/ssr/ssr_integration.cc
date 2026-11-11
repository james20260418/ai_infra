// JPOV SSR 接入层实现。分工见 ssr_integration.h 顶部；算法/降采样/融合各自在自己的文件里。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/ssr/ssr_integration.h"

#include <vector>

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
                    in.backface_depth_tex, in.full_w, in.full_h, scale, in.near_plane,
                    in.far_plane);

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
    si.thick_tex = downsample_.thick_tex();
    si.use_real_thickness = in.cfg.backface_depth;
    si.min_thickness_m = in.cfg.min_thickness_m;
    si.max_distance_m = in.cfg.max_distance_m;
    si.max_ray_pixels = in.cfg.max_ray_pixels;
    si.tile_size = in.cfg.tile_size;
    si.rough_tex = downsample_.rough_tex();       // 逐像素 roughness（发射线截止用）
    si.roughness_cutoff = in.cfg.roughness_cutoff;
    si.debug_reason = false;
    const SsrOutputs& outs = ssr_.Render(si);

    // TEMP(2026-11-11)：把这一帧 SSR 的中间结果 dump 到磁盘，供离线复算某个像素的碰撞过程。
    if (std::getenv("SSR_DUMP") != nullptr) {
        auto dump_tex = [](unsigned int tex, int w, int h, unsigned int fmt,
                           unsigned int type, const char* path) {
            std::vector<unsigned char> buf(static_cast<size_t>(w) * h * 16);
            glBindTexture(GL_TEXTURE_2D, tex);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glGetTexImage(GL_TEXTURE_2D, 0, fmt, type, buf.data());
            FILE* f = std::fopen(path, "wb");
            if (f != nullptr) {
                std::fwrite(buf.data(), 1, buf.size(), f);
                std::fclose(f);
            }
        };
        const int sw = downsample_.width();
        const int sh = downsample_.height();
        dump_tex(outs.type_tex, sw, sh, GL_RED_INTEGER, GL_UNSIGNED_BYTE, "/tmp/ssr_dump/type.bin");
        dump_tex(outs.radiance_tex, sw, sh, GL_RGBA, GL_FLOAT, "/tmp/ssr_dump/radiance.bin");
        dump_tex(downsample_.w_tex(), sw, sh, GL_RED, GL_FLOAT, "/tmp/ssr_dump/w.bin");
        dump_tex(downsample_.normal_tex(), sw, sh, GL_RGBA, GL_FLOAT, "/tmp/ssr_dump/normal.bin");
        dump_tex(downsample_.color_tex(), sw, sh, GL_RGBA, GL_FLOAT, "/tmp/ssr_dump/color.bin");
        dump_tex(downsample_.thick_tex(), sw, sh, GL_RED, GL_FLOAT, "/tmp/ssr_dump/thick.bin");
        dump_tex(in.scene_depth_tex, in.full_w, in.full_h, GL_RED, GL_FLOAT,
                 "/tmp/ssr_dump/scenedepth.bin");
        if (in.backface_depth_tex != 0) {
            dump_tex(in.backface_depth_tex, in.full_w, in.full_h, GL_RGBA, GL_FLOAT,
                     "/tmp/ssr_dump/backface.bin");
        }
        FILE* m = std::fopen("/tmp/ssr_dump/meta.txt", "w");
        if (m != nullptr) {
            std::fprintf(m, "w %d\nh %d\nnear %g\nfar %g\ntan_half %g\naspect %g\ntile %d\nscale %d\nmaxdist %g\n",
                         sw, sh, in.near_plane, in.far_plane, in.tan_half_fov_y, in.aspect,
                         in.cfg.tile_size, scale, in.cfg.max_distance_m);
            std::fclose(m);
        }
        LOG(INFO) << "[TEMP dump] " << sw << "x" << sh << " -> /tmp/ssr_dump";
    }

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
