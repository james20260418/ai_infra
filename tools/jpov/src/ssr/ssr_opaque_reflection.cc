// JPOV SSR 不透明接入层实现。分层见 ssr_opaque_reflection.h 顶部。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/ssr/ssr_opaque_reflection.h"

#include "tools/jpov/src/ssr/ssr_params.h"

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

SsrOpaqueReflection::SsrOpaqueReflection() = default;
SsrOpaqueReflection::~SsrOpaqueReflection() { Finalize(); }

void SsrOpaqueReflection::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr) << "SsrOpaqueReflection::Init 需要非空 ShaderManager";
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

    chain_.Init(shader_mgr_);
    composite_.Init(shader_mgr_);
    initialized_ = true;
}

void SsrOpaqueReflection::Finalize() {
    if (!initialized_) {
        return;
    }
    composite_.Finalize();
    chain_.Finalize();
    prog_attr_ = 0;
    prog_attr_cutout_ = 0;
    prog_attr_skinned_ = 0;
    shader_mgr_ = nullptr;
    initialized_ = false;
}

void SsrOpaqueReflection::Render(const SsrPassInputs& in, unsigned int dst_fbo) {
    CHECK(initialized_) << "SsrOpaqueReflection::Render 前必须先 Init";
    CHECK(in.cfg.enabled) << "SsrOpaqueReflection::Render 只在 enabled 时调用";
    CHECK_NE(in.hdr_color_tex, 0u);
    CHECK_NE(in.scene_depth_tex, 0u);
    CHECK_NE(in.attr_tex, 0u);
    CHECK_NE(in.attr_mat_tex, 0u);
    CHECK_NE(in.env_cube_tex, 0u) << "合成需要环境 cubemap（与 3D pass 同一份）";
    CHECK_GT(in.full_w, 0);
    CHECK_GT(in.full_h, 0);
    CHECK_GT(in.near_plane, 0.0f);
    CHECK_GT(in.far_plane, in.near_plane);
    CHECK_GT(in.tan_half_fov_y, 0.0f);
    CHECK_GT(in.aspect, 0.0f);
    // 面板/调用方能给的都拦住（NaN 用 "≥/>" 天然拦下：NaN 与任何数比较都为假）。
    CHECK(in.cfg.fade_budget_pixels >= 0.0f)
        << "SsrConfig::fade_budget_pixels 必须 ≥ 0（0 = 关闭），得到 " << in.cfg.fade_budget_pixels;
    CHECK(in.cfg.fade_edge_pixels >= 0.0f)
        << "SsrConfig::fade_edge_pixels 必须 ≥ 0（0 = 关闭），得到 " << in.cfg.fade_edge_pixels;
    CHECK(in.cfg.fade_hit_edge_pixels >= 0.0f)
        << "SsrConfig::fade_hit_edge_pixels 必须 ≥ 0（0 = 关闭），得到 "
        << in.cfg.fade_hit_edge_pixels;

    // ── SsrConfig → SsrParams（映射的唯一一份：ssr_params.h）──
    SsrCamera cam;
    cam.near_plane = in.near_plane;
    cam.far_plane = in.far_plane;
    cam.tan_half_fov_y = in.tan_half_fov_y;
    cam.aspect = in.aspect;
    const SsrParams p = BuildSsrParams(in.cfg, cam);

    // ── Source / Query ──
    // 不透明场景：被反射的世界与"谁在问"是**同一份屏幕数据**（地板/人体自己也在画面里），
    // 所以四张纹理原样传两遍。装置照样各跑一趟降采样（结果逐字节相同）—— 这是刻意的：
    // 接口允许两者不同（水皮就是这样用的），代码里不为同源情形留特例。
    SsrSource src;
    src.color_tex = in.hdr_color_tex;
    src.depth_tex = in.scene_depth_tex;
    src.normal_tex = in.attr_tex;
    src.width = in.full_w;
    src.height = in.full_h;

    SsrQuery qry;
    qry.depth_tex = in.scene_depth_tex;
    qry.normal_tex = in.attr_tex;
    qry.rough_tex = in.attr_mat_tex;   // 只读 .a = roughness

    const SsrChainResult& result = chain_.Render(src, qry, p);

    // ── 合成：用 SSR 结果替换 3D pass 已加的环境镜面项（写回 3D FBO 的颜色）──
    SsrCompositeInputs ci;
    ci.hdr_tex = in.hdr_color_tex;
    ci.attr_tex = in.attr_tex;
    ci.attr_mat_tex = in.attr_mat_tex;
    ci.scene_depth_tex = in.scene_depth_tex;
    ci.reflection_tex = result.reflection_tex;
    ci.backface_tex = result.backface_tex;
    ci.raw_ssr_tex = result.raw_tex;
    ci.full_w = in.full_w;
    ci.full_h = in.full_h;
    ci.ssr_w = result.ssr_width;
    ci.ssr_h = result.ssr_height;
    ci.scale = static_cast<int>(std::lround(1.0f / in.cfg.resolution_scale));
    ci.near_plane = in.near_plane;
    ci.far_plane = in.far_plane;
    ci.tan_half_fov_y = in.tan_half_fov_y;
    ci.aspect = in.aspect;
    ci.world_to_view = in.world_to_view;
    ci.env_cube_tex = in.env_cube_tex;
    ci.env_max_lod = in.env_max_lod;
    ci.env_sky_scale = in.env_sky_scale;
    ci.env_enabled = in.env_enabled;
    ci.ao = in.cfg.ao;
    ci.backface_use_ssr = in.cfg.backface_use_ssr;
    ci.backface_dim = in.cfg.backface_dim;
    ci.debug_view = in.cfg.debug_coverage;
    ci.dst_fbo = dst_fbo;
    composite_.Run(ci);
}

}  // namespace jpov
