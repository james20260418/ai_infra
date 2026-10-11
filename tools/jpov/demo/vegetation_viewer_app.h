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
#include "tools/jpov/src/vegetation/leaf_cards.h"
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
    float base_radius_  = 0.09f;    // 主干半径（米）
    int   seed_         = 0;        // 随机种子（确定性语法下无视觉影响）

    // ── 枝干网格表示 ──
    bool  use_tube_    = true;      // true=管状（BuildBranchMesh）；false=棒棒（拓扑预览）
    int   ring_edges_  = 8;         // 管截面边数
    float bark_tile_   = 0.5f;      // 树皮瓦长 v（米）；u 瓦长 = v × 贴图长宽比(2)

    // ── 叶片卡片 ──
    bool  show_leaves_ = true;
    float leaf_length_ = 0.12f;     // 基准叶长（米）
    float leaf_growth_ = 1.0f;      // 生长因子 [0,1] → 叶大小
    int   leaves_per_site_ = 10;    // 每叶位叶片数（叶丛密度）

    // ── 场景资源 ──
    uint32_t ground_mesh_ = 0;
    uint32_t bark_tex_ = 0;         // 树皮纹理（repeat+mipmap）
    uint32_t leaf_tex_ = 0;         // 叶片贴图（白底抠孔，RGBA）
    uint32_t tree_mesh_ = 0;        // 枝干管/棒 mesh
    uint32_t leaf_mesh_ = 0;        // 叶片卡片 mesh
    jpov::PBRMaterial tree_mat_;    // 枝干材质（bark 贴图）
    jpov::PBRMaterial leaf_mat_;    // 叶片 cutout 材质
    jpov::PBRMaterial ground_mat_;  // 灰色地面

    // 最近一次生成的统计（面板显示 + 测试）。
    size_t segment_count_ = 0;
    size_t leaf_count_ = 0;

    // 装配：注册地面 + 纹理 + 首次生成树。Init() 后调用一次。
    void InstallScene() {
        ground_mesh_ = RegisterMesh(jpov_viewer::MakeGroundQuad(/*y=*/0.0f));
        ground_mat_ = jpov_viewer::GroundMaterial();

        bark_tex_ = RegisterTexture("textures/bark_dragonclaw.png",
                                    jpov::TextureOptions{/*mipmap=*/true,
                                                         /*repeat=*/true});
        leaf_tex_ = RegisterTexture("textures/leaf_dragonclaw_1.png");

        tree_mat_ = jpov::PBRMaterial::SolidColorMR(
            /*color*/ {1.0f, 1.0f, 1.0f, 1.0f},
            /*metallic*/ 0.0f, /*roughness*/ 0.85f);
        tree_mat_.base_color_tex = bark_tex_;

        leaf_mat_ = jpov::PBRMaterial::SolidColorMR(
            /*color*/ {1.0f, 1.0f, 1.0f, 1.0f},
            /*metallic*/ 0.0f, /*roughness*/ 0.7f);
        leaf_mat_.base_color_tex = leaf_tex_;
        leaf_mat_.alpha_mode = jpov::AlphaMode::kMask;
        leaf_mat_.alpha_cutoff = 0.5f;
        leaf_mat_.double_sided = true;

        RebuildTree();
        SyncPrev();
    }

    // 同步“上一帧参数”（用于变化检测）。
    void SyncPrev() {
        prev_generations_  = generations_;
        prev_length_scale_ = length_scale_;
        prev_base_radius_  = base_radius_;
        prev_seed_         = seed_;
        prev_use_tube_     = use_tube_;
        prev_ring_edges_   = ring_edges_;
        prev_bark_tile_    = bark_tile_;
        prev_show_leaves_  = show_leaves_;
        prev_leaf_length_  = leaf_length_;
        prev_leaf_growth_  = leaf_growth_;
        prev_leaves_per_site_ = leaves_per_site_;
    }

    void InstallTextMeasure() {
        ui_.SetTextMeasure(&VegetationApp::AppTextWidth, this);
    }
    void SetShowPanel(bool show) { show_panel_ = show; }

    // 立即按当前参数重建骨架（headless 拍摄用；交互模式由 OneIteration 检测参数变化自动触发）。
    void RegenerateNow() {
        RebuildTree();
        SyncPrev();
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
                ring_edges_ != prev_ring_edges_ ||
                bark_tile_ != prev_bark_tile_ ||
                show_leaves_ != prev_show_leaves_ ||
                leaf_length_ != prev_leaf_length_ ||
                leaf_growth_ != prev_leaf_growth_ ||
                leaves_per_site_ != prev_leaves_per_site_) {
                RebuildTree();
                SyncPrev();
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
        if (show_leaves_ && leaf_mesh_ != 0 && leaf_count_ > 0) {
            cmds->DrawObject3D(leaf_mesh_, leaf_mat_,
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
                  << " leaves=" << skel.leaf_sites.size()
                  << " bbox=(" << (mx[0] - mn[0]) << ", " << (mx[1] - mn[1])
                  << ", " << (mx[2] - mn[2]) << ")"
                  << " length_scale=" << length_scale_
                  << " base_radius=" << base_radius_;

        // ── 枝干 mesh（管/棒）──
        if (segment_count_ > 0) {
            jpov::MeshData mesh;
            if (use_tube_) {
                jpov::vegetation::BranchMeshConfig bcfg;
                bcfg.ring_edges = ring_edges_;
                bcfg.bark_tile_v_meters = bark_tile_;
                bcfg.bark_tile_u_meters = bark_tile_ * 2.0f;   // 贴图 2:1
                mesh = jpov::vegetation::BuildBranchMesh(skel, bcfg);
            } else {
                mesh = BuildSkeletonMesh(skel);   // 拓扑预览用（盒棒）
            }
            if (!mesh.positions.empty()) {
                if (tree_mesh_ == 0) {
                    tree_mesh_ = RegisterMesh(mesh);
                } else {
                    UpdateMesh(tree_mesh_, mesh);
                }
            }
        }

        // ── 叶片卡片 mesh ──
        leaf_count_ = skel.leaf_sites.size();
        if (show_leaves_ && leaf_count_ > 0) {
            jpov::vegetation::LeafCardConfig lcfg;
            lcfg.leaf_length = leaf_length_;
            lcfg.aspect = 0.6f;
            lcfg.growth = leaf_growth_;
            lcfg.leaves_per_site = leaves_per_site_;
            lcfg.seed = static_cast<uint64_t>(seed_);
            const jpov::MeshData lmesh =
                jpov::vegetation::BuildLeafCardsMesh(skel, lcfg);
            if (!lmesh.positions.empty()) {
                if (leaf_mesh_ == 0) {
                    leaf_mesh_ = RegisterMesh(lmesh);
                } else {
                    UpdateMesh(leaf_mesh_, lmesh);
                }
            }
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
        const int   kRows   = 12;
        const float top     = h - kBottom -
                              (static_cast<float>(kRows) * kRowH +
                               static_cast<float>(kRows - 1) * kSpace);
        auto row = [&](int i) {
            return jpov::UiRect{{left, top + static_cast<float>(i) * (kRowH + kSpace)},
                                {kWidth, kRowH}};
        };

        // 展开代数用 float 滑条（整数步），再取整回 int。
        float gen_f = static_cast<float>(generations_);
        if (ui_.SliderFloat("展开代数（生长）", &gen_f, row(0), 0.0f, 40.0f, /*dec*/0)) {
            generations_ = static_cast<int>(gen_f + 0.5f);
        }
        ui_.SliderFloat("长度缩放", &length_scale_, row(1), 0.01f, 0.20f, 3);
        ui_.SliderFloat("基本粗细", &base_radius_, row(2), 0.01f, 0.20f, 3);
        float seed_f = static_cast<float>(seed_);
        if (ui_.SliderFloat("随机种子", &seed_f, row(3), 0.0f, 255.0f, /*dec*/0)) {
            seed_ = static_cast<int>(seed_f + 0.5f);
        }
        ui_.Checkbox("枝干：管状 (取消=棒棒拓扑)", &use_tube_, row(4));
        float edges_f = static_cast<float>(ring_edges_);
        if (ui_.SliderFloat("管截面边数", &edges_f, row(5), 3.0f, 16.0f, /*dec*/0)) {
            ring_edges_ = static_cast<int>(edges_f + 0.5f);
        }
        ui_.SliderFloat("树皮瓦长(高) m", &bark_tile_, row(6), 0.1f, 2.0f, 3);
        ui_.Checkbox("显示叶片", &show_leaves_, row(7));
        ui_.SliderFloat("叶长 m", &leaf_length_, row(8), 0.02f, 0.40f, 3);
        ui_.SliderFloat("叶片生长 [0,1]", &leaf_growth_, row(9), 0.0f, 1.0f, 2);
        float lps_f = static_cast<float>(leaves_per_site_);
        if (ui_.SliderFloat("每叶丛叶片数", &lps_f, row(10), 1.0f, 24.0f, /*dec*/0)) {
            leaves_per_site_ = static_cast<int>(lps_f + 0.5f);
        }
        char info[128];
        std::snprintf(info, sizeof(info), "龙爪槐  |  枝段 %zu  叶 %zu 片",
                      segment_count_, leaf_count_);
        ui_.Text(info, row(11));
    }

    bool show_panel_ = true;
    jpov::Ui ui_;

    // 上一帧参数（检测变化 → 重建）。
    int   prev_generations_  = -1;
    float prev_length_scale_ = 0.0f;
    float prev_base_radius_  = 0.0f;
    int   prev_seed_         = -1;
    bool  prev_use_tube_     = true;
    int   prev_ring_edges_   = 8;
    float prev_bark_tile_    = 0.5f;
    bool  prev_show_leaves_  = true;
    float prev_leaf_length_  = 0.15f;
    float prev_leaf_growth_  = 1.0f;
    int   prev_leaves_per_site_ = 3;

    static constexpr float kSliderFontSize = 16.0f;
};

}  // namespace jpov_vegetation

#endif  // JPOV_DEMO_VEGETATION_VIEWER_APP_H_
