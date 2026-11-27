// JPOV SSR 装置实现。设计与契约见 ssr_chain.h 顶部；这里只有**编排**，没有算法。

#define GL_GLEXT_PROTOTYPES

#include "tools/jpov/src/ssr/ssr_chain.h"

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

#include "tools/jpov/src/shader_manager.h"

namespace jpov {
namespace {

// 查纹理尺寸。
// ⚠️ 必须**自己保存/恢复绑定**：本类对调用方的 GL 状态零假设（它可能被任意位置调用），
//    「顺手改了一个单元的纹理绑定」在这里就是真 bug（调用方下一个 pass 会采错图）。
// 一次绑定查宽高两个参数（每帧 6 张纹理 ⇒ 6 次绑定而不是 12 次）。
struct TexSize {
    int width = 0;
    int height = 0;
};

TexSize QueryTexSize(unsigned int tex) {
    GLint prev_bind = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_bind);
    glBindTexture(GL_TEXTURE_2D, tex);
    GLint w = 0;
    GLint h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_bind));
    return TexSize{static_cast<int>(w), static_cast<int>(h)};
}

bool IsValidTile(int tile) {
    return tile == 1 || tile == 2 || tile == 4 || tile == 8;
}

}  // namespace

SsrChain::SsrChain() = default;
SsrChain::~SsrChain() { Finalize(); }

void SsrChain::Init(ShaderManager* shader_mgr) {
    CHECK(shader_mgr != nullptr) << "SsrChain::Init 需要非空 ShaderManager";
    if (initialized_) {
        Finalize();
    }
    shader_mgr_ = shader_mgr;

    downsample_source_.Init(shader_mgr_, SsrDownsampleKind::kSource);
    downsample_query_.Init(shader_mgr_, SsrDownsampleKind::kQuery);
    slab_back_.Init(shader_mgr_);
    renderer_.Init(shader_mgr_);
    filter_.Init(shader_mgr_);
    upsample_.Init(shader_mgr_);
    initialized_ = true;
}

void SsrChain::Finalize() {
    if (!initialized_) {
        return;
    }
    upsample_.Finalize();
    filter_.Finalize();
    renderer_.Finalize();
    slab_back_.Finalize();
    downsample_query_.Finalize();
    downsample_source_.Finalize();
    result_ = SsrChainResult{};
    shader_mgr_ = nullptr;
    initialized_ = false;
}

const SsrChainResult& SsrChain::Render(const SsrSource& source, const SsrQuery& query,
                                  const SsrParams& params) {
    // ── Pre-condition（不静默退化）──
    CHECK(initialized_) << "SsrChain::Render 前必须先 Init";
    CHECK_NE(source.color_tex, 0u) << "SsrSource::color_tex 未设置";
    CHECK_NE(source.depth_tex, 0u) << "SsrSource::depth_tex 未设置（窗口深度）";
    CHECK_NE(source.normal_tex, 0u) << "SsrSource::normal_tex 未设置";
    CHECK_NE(query.depth_tex, 0u) << "SsrQuery::depth_tex 未设置（窗口深度）";
    CHECK_NE(query.normal_tex, 0u) << "SsrQuery::normal_tex 未设置";
    CHECK_NE(query.rough_tex, 0u) << "SsrQuery::rough_tex 未设置（只读 .a）";
    CHECK_GT(source.width, 0) << "SsrSource::width 未设置";
    CHECK_GT(source.height, 0) << "SsrSource::height 未设置";

    CHECK_GT(params.near_plane, 0.0f) << "SsrParams::near_plane 未设置";
    CHECK_GT(params.far_plane, params.near_plane) << "SsrParams::far_plane 必须 > near_plane";
    CHECK_GT(params.tan_half_fov_y, 0.0f) << "SsrParams::tan_half_fov_y 未设置";
    CHECK_GT(params.aspect, 0.0f) << "SsrParams::aspect 未设置";
    CHECK(IsValidScale(params.resolution_scale))
        << "SsrParams::resolution_scale 只支持 1/N（N = 1..4），得到 " << params.resolution_scale;
    CHECK_GT(params.thickness_pixels, 0.0f) << "SsrParams::thickness_pixels 未设置";
    CHECK_GT(params.min_thickness_m, 0.0f) << "SsrParams::min_thickness_m 未设置";
    CHECK_GT(params.max_distance_m, 0.0f) << "SsrParams::max_distance_m 未设置";
    CHECK_GT(params.max_ray_pixels, 0) << "SsrParams::max_ray_pixels 未设置";
    CHECK(IsValidTile(params.tile_size))
        << "SsrParams::tile_size 必须是 1/2/4/8，得到 " << params.tile_size;
    CHECK_GE(params.roughness_cutoff, 0.0f) << "SsrParams::roughness_cutoff 必须 ∈ [0,1]";
    CHECK_LE(params.roughness_cutoff, 1.0f) << "SsrParams::roughness_cutoff 必须 ∈ [0,1]";
    CHECK_GT(params.slab_min_thickness, 0.0f) << "SsrParams::slab_min_thickness 未设置";
    CHECK_GT(params.slab_gate_rel, 0.0f) << "SsrParams::slab_gate_rel 未设置";
    CHECK_GE(params.slab_gate_cos, 0.0f) << "SsrParams::slab_gate_cos 必须 ∈ [0,1]";
    CHECK_LE(params.slab_gate_cos, 1.0f) << "SsrParams::slab_gate_cos 必须 ∈ [0,1]";
    // 三个淡出带宽允许 0（= 关闭），负值 / NaN 一律炸。
    CHECK(params.fade_budget_pixels >= 0.0f) << "SsrParams::fade_budget_pixels 必须 ≥ 0";
    CHECK(params.fade_edge_pixels >= 0.0f) << "SsrParams::fade_edge_pixels 必须 ≥ 0";
    CHECK(params.fade_hit_edge_pixels >= 0.0f) << "SsrParams::fade_hit_edge_pixels 必须 ≥ 0";
    CHECK_GT(params.upsample_depth_tolerance, 0.0f)
        << "SsrParams::upsample_depth_tolerance 未设置";
    CHECK_GT(params.upsample_hard_cut, 0.0f) << "SsrParams::upsample_hard_cut 未设置";
    CHECK_GE(params.upsample_weight_floor, 0.0f) << "SsrParams::upsample_weight_floor 必须 ≥ 0";
    CHECK_GE(params.roughness_to_lod, 0.0f) << "SsrParams::roughness_to_lod 必须 ≥ 0";
    CHECK_GT(params.filter_clamp_sigma, 0.0f) << "SsrParams::filter_clamp_sigma 必须 > 0";
    CHECK_GE(params.filter_atrous_iterations, 1);
    CHECK_LE(params.filter_atrous_iterations, 3);

    // 源的两张 + 查询的三张必须都是**声明的那个尺寸**：屏幕参数化（σ、w 对屏幕仿射）
    // 只在同一张屏幕网格上成立；尺寸不一致的症状是"倒影整体平移 + 命中错格"。
    {
        const TexSize c = QueryTexSize(source.color_tex);
        const TexSize d = QueryTexSize(source.depth_tex);
        const TexSize n = QueryTexSize(source.normal_tex);
        const TexSize qd = QueryTexSize(query.depth_tex);
        const TexSize qn = QueryTexSize(query.normal_tex);
        const TexSize qr = QueryTexSize(query.rough_tex);
        CHECK(c.width == source.width && c.height == source.height)
            << "SsrSource::color_tex 实际 " << c.width << "×" << c.height << "，与声明的 "
            << source.width << "×" << source.height << " 不一致";
        CHECK(d.width == source.width && d.height == source.height)
            << "SsrSource::depth_tex 尺寸与声明不一致";
        CHECK(n.width == source.width && n.height == source.height)
            << "SsrSource::normal_tex 尺寸与声明不一致";
        CHECK(qd.width == source.width && qd.height == source.height)
            << "SsrQuery::depth_tex 必须与 Source 同尺寸";
        CHECK(qn.width == source.width && qn.height == source.height)
            << "SsrQuery::normal_tex 必须与 Source 同尺寸";
        CHECK(qr.width == source.width && qr.height == source.height)
            << "SsrQuery::rough_tex 必须与 Source 同尺寸";
    }

    // 反馈环（契约：不改输入、不读自身输出）。
    // 三个输出都要查：raw_tex / backface_tex 也是本类持有的真实纹理（不是别名）。
    auto own_output = [&](unsigned int tex) {
        return tex != 0 && (tex == result_.reflection_tex || tex == result_.backface_tex ||
                            tex == result_.raw_tex);
    };
    CHECK(!own_output(source.color_tex) && !own_output(source.depth_tex) &&
              !own_output(source.normal_tex) && !own_output(query.depth_tex) &&
              !own_output(query.normal_tex) && !own_output(query.rough_tex))
        << "输入纹理是本装置上一帧的输出（反馈环）";

    const int scale = static_cast<int>(std::lround(1.0f / params.resolution_scale));

    // ── ① 降采样：源与查询**各跑一趟**（选点规则同源，规则只有一份实现）──
    // 实心→实心时两者输入相同，两趟结果逐字节相同 —— 多一趟是刻意的：
    // "源与查询可以不同" 是这套接口存在的理由，代码里不为同源情形留特例分支。
    {
        SsrDownsample::Inputs si;
        si.color_tex = source.color_tex;
        si.depth_tex = source.depth_tex;
        si.normal_tex = source.normal_tex;
        si.full_w = source.width;
        si.full_h = source.height;
        si.scale = scale;
        si.near_plane = params.near_plane;
        si.far_plane = params.far_plane;
        downsample_source_.Run(si);
    }
    {
        SsrDownsample::Inputs qi;
        qi.depth_tex = query.depth_tex;
        qi.normal_tex = query.normal_tex;
        qi.rough_tex = query.rough_tex;
        qi.full_w = source.width;
        qi.full_h = source.height;
        qi.scale = scale;
        qi.near_plane = params.near_plane;
        qi.far_plane = params.far_plane;
        downsample_query_.Run(qi);
    }
    const int ssr_w = downsample_source_.width();
    const int ssr_h = downsample_source_.height();
    CHECK_EQ(downsample_query_.width(), ssr_w);
    CHECK_EQ(downsample_query_.height(), ssr_h);

    // ── ② 同面并集柱体（**源**几何的准备；命中判据的厚度源之一）──
    unsigned int slab_back_tex = 0;
    {
        SsrSlabBack::Inputs bi;
        bi.w_tex = downsample_source_.w_tex();
        bi.normal_tex = downsample_source_.normal_tex();
        bi.width = ssr_w;
        bi.height = ssr_h;
        bi.min_thickness_rel = params.slab_min_thickness;
        bi.gate_rel = params.slab_gate_rel;
        bi.gate_cos = params.slab_gate_cos;
        bi.aspect = params.aspect;
        bi.tan_half_fov_y = params.tan_half_fov_y;
        slab_back_tex = slab_back_.Run(bi);
    }

    // ── ③ 算法：SSR 分辨率 march + resolve ──
    // 三个淡出带宽：调用方给的是**全分辨率屏幕像素**（人对"留 24 像素缓冲带"有直觉），
    // 模块吃 SSR 像素 ⇒ 在这里 /N。改 N 时屏幕上的带宽保持不变。
    const float fade_scale = 1.0f / static_cast<float>(scale);
    SsrInputs si;
    si.color_tex = downsample_source_.color_tex();
    si.source_w_tex = downsample_source_.w_tex();
    si.source_normal_tex = downsample_source_.normal_tex();
    si.query_w_tex = downsample_query_.w_tex();
    si.query_normal_tex = downsample_query_.normal_tex();
    si.query_rough_tex = downsample_query_.rough_tex();
    si.width = ssr_w;
    si.height = ssr_h;
    si.near_plane = params.near_plane;
    si.far_plane = params.far_plane;
    si.tan_half_fov_y = params.tan_half_fov_y;
    si.aspect = params.aspect;
    si.thickness_pixels = params.thickness_pixels;
    si.min_thickness_m = params.min_thickness_m;
    si.max_distance_m = params.max_distance_m;
    si.max_ray_pixels = params.max_ray_pixels;
    si.tile_size = params.tile_size;
    si.roughness_cutoff = params.roughness_cutoff;
    si.grazing_fade_start = params.grazing_fade_start;
    si.grazing_fade_end = params.grazing_fade_end;
    si.fade_budget_pixels = params.fade_budget_pixels * fade_scale;
    si.fade_edge_pixels = params.fade_edge_pixels * fade_scale;
    si.fade_hit_edge_pixels = params.fade_hit_edge_pixels * fade_scale;
    si.slab_back_tex = slab_back_tex;
    si.debug_reason = false;
    const SsrOutputs& outs = renderer_.Render(si);

    // ── ④ 粗糙度预滤波：A 预乘 mip 链（决定"磨砂"的模糊量）──
    // 引导纹理用**查询**那一侧的 w/法线：结果图长在查询像素的网格上，
    // 跨剪影串色的判据必须是"查询表面"的几何（实心→实心时与源同一份）。
    SsrFilterGuides fg;
    fg.down_w_tex = downsample_query_.w_tex();
    fg.down_normal_tex = downsample_query_.normal_tex();
    SsrFilterConfig fc;
    fc.hole_fill = params.filter_hole_fill;
    fc.outlier_clamp = params.filter_outlier_clamp;
    fc.atrous = params.filter_atrous;
    fc.clamp_sigma = params.filter_clamp_sigma;
    fc.atrous_iterations = params.filter_atrous_iterations;
    filter_.Run(outs.radiance_tex, outs.type_tex, ssr_w, ssr_h, fg, fc);

    // ── ⑤ 升采样：SSR 分辨率 → 全分辨率（反射图 + 背面 flag）──
    SsrUpsample::Inputs ui;
    ui.mip_tex = filter_.mip_tex();
    ui.mip_levels = filter_.level_count();
    ui.down_w_tex = downsample_query_.w_tex();
    ui.down_normal_tex = downsample_query_.normal_tex();
    ui.query_depth_tex = query.depth_tex;
    ui.query_normal_tex = query.normal_tex;
    ui.query_rough_tex = query.rough_tex;
    ui.backface_tex = outs.backface_tex;
    ui.full_w = source.width;
    ui.full_h = source.height;
    ui.ssr_w = ssr_w;
    ui.ssr_h = ssr_h;
    ui.scale = scale;
    ui.near_plane = params.near_plane;
    ui.far_plane = params.far_plane;
    ui.roughness_to_lod = params.roughness_to_lod;
    ui.depth_tolerance = params.upsample_depth_tolerance;
    ui.hard_cut = params.upsample_hard_cut;
    ui.weight_floor = params.upsample_weight_floor;
    const SsrUpsample::Outputs& uo = upsample_.Run(ui);

    result_.reflection_tex = uo.reflection_tex;
    result_.backface_tex = uo.backface_tex;
    result_.raw_tex = outs.radiance_tex;
    result_.width = uo.width;
    result_.height = uo.height;
    result_.ssr_width = ssr_w;
    result_.ssr_height = ssr_h;
    return result_;
}

}  // namespace jpov
