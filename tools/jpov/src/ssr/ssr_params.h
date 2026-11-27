// JPOV SSR —— `SsrConfig`（应用层）→ `SsrParams`（装置）的**唯一一份**映射。
//
// 为什么单独一个文件（2026-11-27）：
//   SSR 装置（ssr_chain.h）刻意不认识 `SsrConfig`，那是应用层的结构。但应用层现在有
//   **两个独立实例**（实心场景与水面各一套，值差得很远），两份各自手写 26 行映射必然漂移
//   ——「加一个旋钮改一处、另一处静默用默认值」是最难查的那类 bug。
//
// 调用方责任：`SsrConfig` 里用不到的字段（水皮不看 ao / backface_* / debug_coverage）
// 照样被复制进 `SsrParams`，无害 —— 装置层面这些量只由消费端解释。

#ifndef JPOV_SRC_SSR_SSR_PARAMS_H_
#define JPOV_SRC_SSR_SSR_PARAMS_H_

#include "tools/jpov/interface/render_command.h"
#include "tools/jpov/src/ssr/ssr_chain.h"

namespace jpov {

// 装置的相机参数（针孔）。与 `interface::Camera` 解耦：调用方自己算 tan(fov/2) 与 aspect。
struct SsrCamera {
    float near_plane = 0.0f;      // > 0
    float far_plane = 0.0f;       // > near
    float tan_half_fov_y = 0.0f;  // > 0
    float aspect = 0.0f;          // > 0
};

// Pre-condition: cam 的四个字段已填（Render 会再校验一遍）。
inline SsrParams BuildSsrParams(const SsrConfig& cfg, const SsrCamera& cam) {
    SsrParams p;
    p.near_plane = cam.near_plane;
    p.far_plane = cam.far_plane;
    p.tan_half_fov_y = cam.tan_half_fov_y;
    p.aspect = cam.aspect;
    p.resolution_scale = cfg.resolution_scale;
    p.thickness_pixels = cfg.thickness_pixels;
    p.min_thickness_m = cfg.min_thickness_m;
    p.max_distance_m = cfg.max_distance_m;
    p.max_ray_pixels = cfg.max_ray_pixels;
    p.tile_size = cfg.tile_size;
    p.roughness_cutoff = cfg.roughness_cutoff;
    p.slab_min_thickness = cfg.slab_min_thickness;
    p.slab_gate_rel = cfg.slab_gate_rel;
    p.slab_gate_cos = cfg.slab_gate_cos;
    p.grazing_fade_start = cfg.grazing_fade_start;
    p.grazing_fade_end = cfg.grazing_fade_end;
    p.fade_budget_pixels = cfg.fade_budget_pixels;
    p.fade_edge_pixels = cfg.fade_edge_pixels;
    p.fade_hit_edge_pixels = cfg.fade_hit_edge_pixels;
    p.filter_hole_fill = cfg.filter_hole_fill;
    p.filter_outlier_clamp = cfg.filter_outlier_clamp;
    p.filter_atrous = cfg.filter_atrous;
    p.filter_clamp_sigma = cfg.filter_clamp_sigma;
    p.filter_atrous_iterations = cfg.filter_atrous_iterations;
    p.upsample_depth_tolerance = cfg.upsample_depth_tolerance;
    p.upsample_hard_cut = cfg.upsample_hard_cut;
    p.upsample_weight_floor = cfg.upsample_weight_floor;
    p.roughness_to_lod = cfg.roughness_to_lod;
    return p;
}

}  // namespace jpov

#endif  // JPOV_SRC_SSR_SSR_PARAMS_H_
