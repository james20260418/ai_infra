// JPOV 植被查看器（vegetation viewer）— 渲染核心 App。
//
// 用途：肉眼验收 **L-System 生成的植被拓扑骨架**。MVP 阶段把每根枝画成一根
// 「空间棒棒」（沿线段朝向的细长盒），直观显示「树 = 拓扑树」。
//
// 与 skylight viewer 同构：同一套 JPOV::OneIteration 渲染体 + 即时模式 UI 面板，
// 视角沿用 model viewer 的 ViewConfig（右键拖拽 / 滚轮缩放）。
//
// 面板 knob（改动即重建骨架，确定性）：
//   ① 展开代数 generations [0,40]（越大越完整；小值可看"生长过程"）
//   ② 长度缩放 length_scale [0.01,0.20]（整体尺度）
//   ③ 基本粗细 base_radius [0.01,0.20]（主干半径）
//   ④ 随机种子 seed（确定性语法下无视觉影响，为随机规则预留）
//
// 树种：MVP 内置「龙爪槐」（见 src/vegetation/lsystem.h）。
//
// 设计见 tools/jpov/docs/jpov_vegetation_design.md。

#ifndef JPOV_DEMO_VEGETATION_VIEWER_APP_H_
#define JPOV_DEMO_VEGETATION_VIEWER_APP_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/demo/viewer_app.h"   // kViewerWidth/Height/Fps/FontAlias
#include "tools/jpov/interface/mesh.h"
#include "tools/jpov/interface/ui.h"
#include "tools/jpov/src/vegetation/branch_mesh.h"
#include "tools/jpov/src/vegetation/lsystem.h"

namespace jpov_vegetation {

using jpov_viewer::kViewerWidth;
using jpov_viewer::kViewerHeight;
using jpov_viewer::kViewerFps;
using jpov_viewer::kViewerFontAlias;

// 把拓扑骨架展平成一个可一次 DrawObject3D 的网格：每根线段 → 一根沿其朝向的细长盒。
// GL-free 纯函数（便于单测 / 复用）。
//
// 盒朝向：局部 +Y（盒长轴）对齐线段方向；front 取一个与线段垂直的向量（仅用于定滚动）。
inline jpov::MeshData BuildSkeletonMesh(const jpov::vegetation::Skeleton& skel) {
    jpov::MeshData out;
    out.flags = static_cast<jpov::MeshVertexFlags>(
        static_cast<uint8_t>(jpov::MeshVertexFlags::kPosition) |
        static_cast<uint8_t>(jpov::MeshVertexFlags::kNormal));

    constexpr float kMinSegmentLen = 1.0e-5f;   // 退化（零长）线段跳过
    constexpr float kMinRadius = 1.0e-3f;       // 细到看不清就夹住
    for (const jpov::vegetation::Segment& s : skel.segments) {
        const float dx = s.end.x() - s.start.x();
        const float dy = s.end.y() - s.start.y();
        const float dz = s.end.z() - s.start.z();
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len < kMinSegmentLen) {
            continue;
        }
        const jpov::Vec3f dir(dx / len, dy / len, dz / len);
        // 取一个与 dir 不平行的参考轴，叉出与 dir 垂直的 front。
        const jpov::Vec3f ref =
            (std::abs(dir.y()) > 0.9f) ? jpov::Vec3f(1.0f, 0.0f, 0.0f)
                                       : jpov::Vec3f(0.0f, 1.0f, 0.0f);
        jpov::Vec3f front(ref.y() * dir.z() - ref.z() * dir.y(),
                          ref.z() * dir.x() - ref.x() * dir.z(),
                          ref.x() * dir.y() - ref.y() * dir.x());
        const float flen = std::sqrt(front.x() * front.x() +
                                     front.y() * front.y() +
                                     front.z() * front.z());
        front = jpov::Vec3f(front.x() / flen, front.y() / flen,
                            front.z() / flen);
        const jpov::Vec3f mid((s.start.x() + s.end.x()) * 0.5f,
                              (s.start.y() + s.end.y()) * 0.5f,
                              (s.start.z() + s.end.z()) * 0.5f);
        const float r = std::max(s.radius, kMinRadius);

        // MakeOrientedBox(front_half, up_half, left_half, up, front, translation)
        // up = dir（盒长轴）；front/left 半宽 = 半径。
        const jpov::MeshData box = jpov::MeshData::MakeOrientedBox(
            /*front_half_width*/ r, /*up_half_width*/ len * 0.5f,
            /*left_half_width*/ r, /*up*/ dir, /*front*/ front,
            /*translation*/ mid);

        const uint32_t base = static_cast<uint32_t>(out.positions.size());
        for (const jpov::Vec3f& p : box.positions) {
            out.positions.push_back(p);
        }
        for (const jpov::Vec3f& n : box.normals) {
            out.normals.push_back(n);
        }
        for (uint32_t idx : box.indices) {
            out.indices.push_back(base + idx);
        }
    }
    return out;
}

// 植被查看器渲染核心 App。
class VegetationApp : public JPOV {
public:
    using JPOV::JPOV;

    // ── 树种（L-System）：构造时建一次，之后只读 ──
    jpov::vegetation::LSystem lsystem_ = jpov::vegetation::BuildDragonClawPagodaTree();

    // ── 视角 ──
    jpov_viewer::ViewConfig view_;

    // ── 生成参数（跨帧持有；改动 → 重建骨架）──
    int   generations_  = 25;       // 展开代数
    float length_scale_ = 0.05f;    // F 的单位长度（米/单位）
    float base_radius_  = 0.06f;    // 主干半径（米）
    int   seed_         = 0;        // 随机种子（确定性语法下无视觉影响）

    // ── 枝干网格表示 ──
    bool  use_tube_    = true;      // true=管状（BuildBranchMesh）；false=棒棒（拓扑预览）
    int   ring_edges_  = 8;         // 管截面边数
    float uv_tile_len_ = 1.0f;      // bark 纹理沿枝循环长度（米）

    // ── 场景资源 ──
    uint32_t ground_mesh_ = 0;
    jpov::PBRMaterial tree_mat_;    // 枝干材质（棕、粗糙）
    jpov::PBRMaterial ground_mat_;  // 灰色地面

    // 最近一次生成的统计（面板显示 + 测试）。
    size_t segment_count_ = 0;

    // 装配：注册地面 + 首次生成树。Init() 后调用一次。
    void InstallScene() {
        ground_mesh_ = RegisterMesh(jpov_viewer::MakeGroundQuad(/*y=*/0.0f));
        ground_mat_ = jpov_viewer::GroundMaterial();
        tree_mat_ = jpov::PBRMaterial::SolidColorMR(
            /*color*/ {0.36f, 0.22f, 0.12f, 1.0f},
            /*metallic*/ 0.0f, /*roughness*/ 0.85f);
        RebuildTree();
        prev_generations_  = generations_;
        prev_length_scale_ = length_scale_;
        prev_base_radius_  = base_radius_;
        prev_seed_         = seed_;
    }

    void InstallTextMeasure() {
        ui_.SetTextMeasure(&VegetationApp::AppTextWidth, this);
    }
    void SetShowPanel(bool show) { show_panel_ = show; }

    // 立即按当前参数重建骨架（headless 拍摄用；交互模式由 OneIteration 检测参数变化自动触发）。
    void RegenerateNow() {
        RebuildTree();
        prev_generations_  = generations_;
        prev_length_scale_ = length_scale_;
        prev_base_radius_  = base_radius_;
        prev_seed_         = seed_;
        prev_use_tube_     = use_tube_;
        prev_ring_edges_   = ring_edges_;
    }

    // ⭐ 唯一渲染体：交互 Run 循环 与 headless 拍摄共用。
    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        cmds->camera.fbo_3d_width_  = kViewerWidth;
        cmds->camera.fbo_3d_height_ = kViewerHeight;

        if (show_panel_) {
            float dx = 0.0f, dy = 0.0f, scroll = 0.0f;
            if (input.right.IsDrag()) {
                dx = input.mouse_dx;
                dy = input.mouse_dy;
            }
            if (input.scroll_delta != 0.0f) {
                scroll = input.scroll_delta;
            }
            jpov_viewer::ApplyInput(&view_, dx, dy, scroll,
                                    static_cast<int>(winfo.width),
                                    static_cast<int>(winfo.height));
            // 参数改动 → 重建骨架（确定性；每帧只重建一次）。
            if (generations_ != prev_generations_ ||
                length_scale_ != prev_length_scale_ ||
                base_radius_ != prev_base_radius_ ||
                seed_ != prev_seed_ ||
                use_tube_ != prev_use_tube_ ||
                ring_edges_ != prev_ring_edges_) {
                RebuildTree();
                prev_generations_  = generations_;
                prev_length_scale_ = length_scale_;
                prev_base_radius_  = base_radius_;
                prev_seed_         = seed_;
                prev_use_tube_     = use_tube_;
                prev_ring_edges_   = ring_edges_;
            }
        }

        cmds->camera.position = view_.Position();
        cmds->camera.target   = jpov_viewer::ViewConfig::Target();
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 60.0f;
        cmds->camera.near     = 0.05f;
        cmds->camera.far      = 1000.0f;

        const jpov_viewer::NoonLighting nl = jpov_viewer::MakeNoonLighting();
        cmds->sky     = nl.sky;
        cmds->sun     = nl.sun;
        cmds->ambient = nl.ambient;
        cmds->tone_mapping = true;

        cmds->DrawObject3D(ground_mesh_, ground_mat_,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f});
        if (tree_mesh_ != 0 && segment_count_ > 0) {
            cmds->DrawObject3D(tree_mesh_, tree_mat_,
                               /*center*/ {0.0f, 0.0f, 0.0f},
                               /*up*/     {0.0f, 1.0f, 0.0f},
                               /*front*/  {0.0f, 0.0f, 1.0f});
        }

        if (show_panel_) {
            DrawPanel(input);
            ui_.End();
            ui_.Emit(cmds);
        }
    }

private:
    static float AppTextWidth(const char* text, float font_size,
                              const char* /*font_alias*/, void* userdata) {
        VegetationApp* app = static_cast<VegetationApp*>(userdata);
        return app->MeasureTextWidth(/*alias=*/std::string(),
                                     /*text=*/text ? text : "", font_size);
    }

    // 生成骨架 → 展平成盒棒网格 → 上传/更新 GPU mesh。
    void RebuildTree() {
        jpov::vegetation::TurtleConfig cfg;
        cfg.length_scale = length_scale_;
        cfg.base_radius  = base_radius_;
        const jpov::vegetation::Skeleton skel =
            jpov::vegetation::GenerateSkeleton(lsystem_, cfg, generations_,
                                         static_cast<uint64_t>(seed_));
        segment_count_ = skel.segments.size();
        // 量一下骨架包围盒（判断是否为平面结构：某一轴跨度远小于另两轴）。
        float mn[3] = {1e30f, 1e30f, 1e30f};
        float mx[3] = {-1e30f, -1e30f, -1e30f};
        for (const jpov::vegetation::Segment& s : skel.segments) {
            for (const jpov::Vec3f& p : {s.start, s.end}) {
                mn[0] = std::min(mn[0], p.x()); mx[0] = std::max(mx[0], p.x());
                mn[1] = std::min(mn[1], p.y()); mx[1] = std::max(mx[1], p.y());
                mn[2] = std::min(mn[2], p.z()); mx[2] = std::max(mx[2], p.z());
            }
        }
        LOG(INFO) << "vegetation: generations=" << generations_
                  << " segments=" << segment_count_
                  << " bbox=(" << (mx[0] - mn[0]) << ", " << (mx[1] - mn[1])
                  << ", " << (mx[2] - mn[2]) << ")"
                  << " length_scale=" << length_scale_
                  << " base_radius=" << base_radius_;
        if (segment_count_ == 0) {
            return;   // 无线段（代数太小）→ 保持不画，不构造空网格
        }
        jpov::MeshData mesh;
        if (use_tube_) {
            jpov::vegetation::BranchMeshConfig bcfg;
            bcfg.ring_edges = ring_edges_;
            bcfg.uv_tile_length = uv_tile_len_;
            mesh = jpov::vegetation::BuildBranchMesh(skel, bcfg);
        } else {
            mesh = BuildSkeletonMesh(skel);   // 拓扑预览用（盒棒）
        }
        if (mesh.positions.empty()) {
            return;
        }
        if (tree_mesh_ == 0) {
            tree_mesh_ = RegisterMesh(mesh);
        } else {
            UpdateMesh(tree_mesh_, mesh);
        }
    }

    void DrawPanel(const jpov::InputSnapshot& input) {
        const float w = static_cast<float>(kViewerWidth);
        const float h = static_cast<float>(kViewerHeight);
        jpov::UiTheme theme = jpov::UiTheme::Default(kSliderFontSize);
        theme.font_alias = kViewerFontAlias;
        ui_.Begin(input, theme, w, h, 1000.0f / kViewerFps);

        const float kWidth  = 0.5f * w;
        const float kRowH   = 28.0f;
        const float kSpace  = 12.0f;
        const float kBottom = 20.0f;
        const float left    = (w - kWidth) * 0.5f;
        const int   kRows   = 7;
        const float top     = h - kBottom -
                              (static_cast<float>(kRows) * kRowH +
                               static_cast<float>(kRows - 1) * kSpace);
        auto row = [&](int i) {
            return jpov::UiRect{{left, top + static_cast<float>(i) * (kRowH + kSpace)},
                                {kWidth, kRowH}};
        };

        // 展开代数用 float 滑条（整数步），再取整回 int。
        float gen_f = static_cast<float>(generations_);
        if (ui_.SliderFloat("展开代数", &gen_f, row(0), 0.0f, 40.0f, /*dec*/0)) {
            generations_ = static_cast<int>(gen_f + 0.5f);
        }
        ui_.SliderFloat("长度缩放", &length_scale_, row(1), 0.01f, 0.20f, 3);
        ui_.SliderFloat("基本粗细", &base_radius_, row(2), 0.01f, 0.20f, 3);
        float seed_f = static_cast<float>(seed_);
        if (ui_.SliderFloat("随机种子", &seed_f, row(3), 0.0f, 255.0f, /*dec*/0)) {
            seed_ = static_cast<int>(seed_f + 0.5f);
        }
        // 状态行：线段数（= 拓扑树的边数）+ 树种名。
        ui_.Checkbox("枝干：管状 (取消=棒棒拓扑)", &use_tube_, row(4));
        float edges_f = static_cast<float>(ring_edges_);
        if (ui_.SliderFloat("管截面边数", &edges_f, row(5), 3.0f, 16.0f, /*dec*/0)) {
            ring_edges_ = static_cast<int>(edges_f + 0.5f);
        }
        char info[96];
        std::snprintf(info, sizeof(info), "龙爪槐  |  线段 %zu",
                      segment_count_);
        ui_.Text(info, row(6));
    }

    bool show_panel_ = true;
    uint32_t tree_mesh_ = 0;
    jpov::Ui ui_;

    // 上一帧参数（检测变化 → 重建）。
    int   prev_generations_  = -1;
    float prev_length_scale_ = 0.0f;
    float prev_base_radius_  = 0.0f;
    int   prev_seed_         = -1;
    bool  prev_use_tube_     = true;
    int   prev_ring_edges_   = 8;

    static constexpr float kSliderFontSize = 16.0f;
};

}  // namespace jpov_vegetation

#endif  // JPOV_DEMO_VEGETATION_VIEWER_APP_H_
