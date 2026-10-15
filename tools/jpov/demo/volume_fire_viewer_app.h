// JPOV 体积火查看器（volume fire viewer）— 渲染核心 App
//
// 场景 = 灰色地面 + 一个 1m³ 燃料块（无桌子）+ 块顶的**逐像素 raymarch 体积火**。
// 目的：验证「.jpovvol 体纹理 → raymarch → 薄层写进 WBO 透明纹理 → 合成」这条链。
//
// 设计：tools/jpov/docs/jpov_volumetric_fire_design.md

#ifndef JPOV_DEMO_VOLUME_FIRE_VIEWER_APP_H_
#define JPOV_DEMO_VOLUME_FIRE_VIEWER_APP_H_

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/demo/viewer_app.h"
#include "tools/jpov/interface/volume_fire_types.h"
#include "tools/jpov/interface/ui.h"

namespace jpov_volume_fire {

using jpov_viewer::kViewerWidth;
using jpov_viewer::kViewerHeight;
using jpov_viewer::kViewerFps;
using jpov_viewer::kViewerFontAlias;

class VolumeFireViewerApp : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t ground_mesh_ = 0;
    jpov::PBRMaterial ground_mat_;
    uint32_t fuel_mesh_ = 0;                 // 1m³ 燃料块
    jpov::PBRMaterial fuel_mat_;
    uint32_t volume_id_ = 0;                  // .jpovvol 体积 id

    jpov_viewer::ViewConfig view_;
    jpov_skylight::SkyDegrees deg_;
    std::string volume_path_;

    // ---- 「原型火纹理」查看模式（Danis：观察体会四大原型族）----
    // 打开后：只渲染**选中的原型族**（按其「自然尺」世界 AABB，贴地），
    // 下拉菜单切换族。常规场景（燃料块+其火）在关闭时不受影响（零回归）。
    bool proto_mode_ = false;                 // 原型查看模式开关
    int  proto_index_ = 0;                    // 当前选中的原型族下标
    std::vector<std::string> proto_paths_;    // 各族 .jpovvol 路径（由 main 注入）
    std::vector<std::string> proto_names_;    // 各族名（jet/area/pool/transient）

    // 火焰盒（世界 AABB）：块顶 y=1.0 起，向上含整团火。
    jpov::Vec3f fire_min_ = {-0.9f, 1.0f, -0.9f};
    jpov::Vec3f fire_max_ = {0.9f, 3.2f, 0.9f};

    // 旋钮
    float sigma_scale_ = 4.0f;      // 消光 σ = 密度 × 此值 (1/m)
    float emission_scale_ = 1.0f;   // 发射强度
    float steps_ = 64.0f;           // 步数
    bool  jitter_ = true;           // 抖动
    float fps_ = 24.0f;             // 播放帧率（旋钮；默认值由物理推导，见注释）
    float temp_kelvin_scale_ = 2000.0f;  // G(∈[0,1]) → 开尔文
    bool  fire_enable_ = true;
    bool  fuel_enable_ = true;      // 燃料块开关（排查伪影用）
    bool  fs_debug_ = false;         // 调试：全屏 quad 代替盒光栅化
    float play_time_ = 0.0f;
    int   nframes_ = 64;            // 由 asset 决定（96³×64）

    void InstallTextMeasure() { ui_.SetTextMeasure(&VolumeFireViewerApp::TextWidth, this); }
    void SetShowPanel(bool show) { show_panel_ = show; }
    void SetShowCamInfo(bool show) { show_cam_info_ = show; }

    // 相机 + 火盒 + 关键旋钮的一行文本（屏上叠字 / 控制台打印共用）。
    std::string CamString() const {
        const jpov::Vec3f p = view_.Position();
        const double kDeg = 180.0 / 3.14159265358979323846;
        char b[640];
        std::snprintf(
            b, sizeof(b),
            "cam pos=(%.3f,%.3f,%.3f) R=%.3f phi=%.1fdeg theta=%.1fdeg | "
            "box=(%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f) | sigma=%.2f emis=%.2f steps=%d jit=%d Tmax=%.0f frame=%.2f",
            p.x(), p.y(), p.z(), view_.R, view_.phi * kDeg, view_.theta * kDeg,
            fire_min_.x(), fire_min_.y(), fire_min_.z(),
            fire_max_.x(), fire_max_.y(), fire_max_.z(),
            sigma_scale_, emission_scale_, static_cast<int>(steps_ + 0.5f),
            jitter_ ? 1 : 0, temp_kelvin_scale_,
            std::fmod(play_time_ * fps_, static_cast<float>(nframes_)));
        return std::string(b);
    }

    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        // 按 P（或 C）→ 控制台打印当前相机信息（供截图对位/复现）。
        if (input.GetKey(jpov::KeyCode::P).IsClick() ||
            input.GetKey(jpov::KeyCode::C).IsClick()) {
            LOG(INFO) << "[camera] " << CamString();
        }
        cmds->camera.fbo_3d_width_ = kViewerWidth;
        cmds->camera.fbo_3d_height_ = kViewerHeight;
        if (show_panel_) {
            float dx = 0.0f, dy = 0.0f, scroll = 0.0f;
            if (input.right.IsDrag()) { dx = input.mouse_dx; dy = input.mouse_dy; }
            if (input.scroll_delta != 0.0f) { scroll = input.scroll_delta; }
            jpov_viewer::ApplyInput(&view_, dx, dy, scroll,
                                    static_cast<int>(winfo.width),
                                    static_cast<int>(winfo.height));
        }
        cmds->camera.position = view_.Position();
        cmds->camera.target = jpov_viewer::ViewConfig::Target();
        cmds->camera.up = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov = 60.0f;
        cmds->camera.near = 0.05f;
        cmds->camera.far = 1000.0f;

        const jpov_skylight::SkyLighting nl =
            jpov_skylight::MakeSkyLighting(deg_, /*tricolor_ambient=*/false);
        cmds->sky = nl.sky;
        cmds->sun = nl.dir_light;
        cmds->ambient = nl.ambient;
        cmds->tone_mapping = true;

        // 懒加载原型族 volumes（原型模式下使用）。
        if (!proto_paths_.empty() && !proto_registered_) {
            proto_registered_ = true;
            for (const std::string& p : proto_paths_) {
                const uint32_t id = RegisterVolumeTexture(p);
                proto_ids_.push_back(id);
                int nx = 0, ny = 0, nz = 0, nf = 0;
                if (!VolumeDims(id, &nx, &ny, &nz, &nf)) { nf = VolumeFrameCount(id); }
                proto_nframes_.push_back(nf > 0 ? nf : 1);
                proto_nx_.push_back(nx > 0 ? nx : 48);
                proto_ny_.push_back(ny > 0 ? ny : 48);
                proto_nz_.push_back(nz > 0 ? nz : 48);
            }
            LOG(INFO) << "[volume_fire_viewer] prototypes registered = "
                      << proto_ids_.size();
        }

        // 地面（两种模式都画，作尺度参照）。
        cmds->DrawObject3D(ground_mesh_, ground_mat_, {0.0f, 0.0f, 0.0f},
                           {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});

        if (proto_mode_ && !proto_ids_.empty()) {
            // ---- 原型火纹理查看：只渲染选中的原型族（按其自然尺）。----
            const int pi = ClampedProtoIndex();
            const std::string& fam = proto_names_[static_cast<size_t>(pi)];
            jpov::Vec3f mn, mx;
            ProtoBoxForIndex(pi, &mn, &mx);
            if (pi != last_proto_index_) {
                // 切换族：重置播放相位 + 自动取景 + 套用该族的默认消光。
                // （密度按自身上限归一化 ⇒ 小火焰需要更大的 σ 才有可见光深；
                //   各族默认值不同，滑条仍可手调。）
                play_time_ = 0.0f;
                const float maxdim = std::max(mx.x() - mn.x(),
                                              std::max(mx.y() - mn.y(), mx.z() - mn.z()));
                view_.R = std::max(0.6, 1.9 * static_cast<double>(maxdim));
                sigma_scale_ = ProtoSigmaFor(fam);
                last_proto_index_ = pi;
            }
            const uint32_t vid = proto_ids_[static_cast<size_t>(pi)];
            const int pf = proto_nframes_[static_cast<size_t>(pi)];
            if (vid != 0 && fire_enable_) {
                const float dt = 1.0f / static_cast<float>(kViewerFps);
                play_time_ += dt;
                jpov::VolumeFireCommand vc;
                vc.volume_id = vid;
                vc.aabb_min = mn;
                vc.aabb_max = mx;
                vc.sigma_scale = sigma_scale_;
                vc.emission_scale = emission_scale_;
                vc.temp_kelvin_scale = temp_kelvin_scale_;
                vc.steps = static_cast<int>(steps_ + 0.5f);
                vc.jitter = jitter_;
                vc.debug_fullscreen = fs_debug_;
                vc.frame = std::fmod(play_time_ * fps_, static_cast<float>(pf));
                cmds->DrawVolumeFire(std::move(vc));
            }
        } else {
            // ---- 常规场景：地面 + 1m³ 燃料块 + 块顶体积火 ----
            if (fuel_enable_) {
                cmds->DrawObject3D(fuel_mesh_, fuel_mat_, {0.0f, 0.5f, 0.0f},
                                   {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
            }

            // 懒加载体积。
            if (volume_id_ == 0 && !volume_path_.empty()) {
                volume_id_ = RegisterVolumeTexture(volume_path_);
                const int nf = VolumeFrameCount(volume_id_);
                if (nf > 0) { nframes_ = nf; }
                LOG(INFO) << "[volume_fire_viewer] volume id = " << volume_id_
                          << " frames = " << nframes_ << " path = " << volume_path_;
            }

            // 体积火（逐像素 raymarch → 薄层写 WBO）。
            if (fire_enable_ && volume_id_ != 0) {
                const float dt = 1.0f / static_cast<float>(kViewerFps);
                play_time_ += dt;
                jpov::VolumeFireCommand vc;
                vc.volume_id = volume_id_;
                vc.aabb_min = fire_min_;
                vc.aabb_max = fire_max_;
                vc.sigma_scale = sigma_scale_;
                vc.emission_scale = emission_scale_;
                vc.temp_kelvin_scale = temp_kelvin_scale_;
                vc.steps = static_cast<int>(steps_ + 0.5f);
                vc.jitter = jitter_;
                vc.debug_fullscreen = fs_debug_;
                vc.frame = std::fmod(play_time_ * fps_, static_cast<float>(nframes_));
                cmds->DrawVolumeFire(std::move(vc));
            }
        }

        if (show_panel_) {
            DrawPanel(input);
            ui_.End();
            ui_.Emit(cmds);
        }
        // 相机信息叠字（右上角）—— 截图自带参数。
        if (show_cam_info_) {
            cmds->DrawText(CamString(), {static_cast<float>(kViewerWidth) - 8.0f, 8.0f},
                           13.0f, {1.0f, 1.0f, 1.0f, 1.0f},
                           jpov::TextAlignment::kTopRight, jpov::kFontBuiltinLatin);
        }
        // 原型族标识（左上角）：族名 + 体维数 + 显示盒尺寸。
        if (proto_mode_) {
            const int pi = ClampedProtoIndex();
            if (pi >= 0) {
                jpov::Vec3f mn, mx;
                ProtoBoxForIndex(pi, &mn, &mx);
                char b[256];
                std::snprintf(b, sizeof(b),
                              "prototype: %s   vol=%dx%dx%d   box=%.2fx%.2fx%.2f m",
                              proto_names_[static_cast<size_t>(pi)].c_str(),
                              proto_nx_[static_cast<size_t>(pi)], proto_ny_[static_cast<size_t>(pi)],
                              proto_nz_[static_cast<size_t>(pi)],
                              mx.x() - mn.x(), mx.y() - mn.y(), mx.z() - mn.z());
                cmds->DrawText(b, {8.0f, 8.0f}, 15.0f, {1.0f, 0.95f, 0.8f, 1.0f},
                               jpov::TextAlignment::kTopLeft, jpov::kFontBuiltinLatin);
            }
        }
    }

private:
    static float TextWidth(const char* text, float font_size, const char* /*alias*/,
                           void* userdata) {
        VolumeFireViewerApp* app = static_cast<VolumeFireViewerApp*>(userdata);
        return app->MeasureTextWidth(std::string(), text ? text : "", font_size);
    }

    void DrawPanel(const jpov::InputSnapshot& input) {
        const float w = static_cast<float>(kViewerWidth);
        const float h = static_cast<float>(kViewerHeight);
        jpov::UiTheme theme = jpov::UiTheme::Default(15.0f);
        theme.font_alias = kViewerFontAlias;
        ui_.Begin(input, theme, w, h, 1000.0f / kViewerFps);
        const float colw = 0.26f * w;
        const float cx = 0.02f * w;
        const float top = h - 16.0f - (10 * 24.0f + 9 * 5.0f);
        auto row = [&](int i) {
            return jpov::UiRect{{cx, top + i * 29.0f}, {colw, 24.0f}};
        };
        ui_.Checkbox("体积火", &fire_enable_, row(0));
        ui_.SliderFloat("消光 σ (1/m)", &sigma_scale_, row(1), 0.0f, 150.0f, 2);
        ui_.SliderFloat("发射强度", &emission_scale_, row(2), 0.0f, 5.0f, 2);
        ui_.SliderFloat("步数", &steps_, row(3), 8.0f, 256.0f, 0);
        ui_.Checkbox("抖动", &jitter_, row(4));
        ui_.SliderFloat("帧率 fps", &fps_, row(5), 4.0f, 60.0f, 0);
        ui_.SliderFloat("温度上限 K", &temp_kelvin_scale_, row(6), 800.0f, 4000.0f, 0);
        ui_.Checkbox("相机信息", &show_cam_info_, row(7));
        ui_.Checkbox("燃料块", &fuel_enable_, row(8));
        ui_.Checkbox("全屏quad(调试)", &fs_debug_, row(9));

        // 「原型火纹理」查看模式（面板左上角）：开关 + 下拉选族。
        ui_.Checkbox("原型火纹理", &proto_mode_, jpov::UiRect{{cx, 8.0f}, {colw, 24.0f}});
        if (proto_mode_ && !proto_names_.empty()) {
            std::vector<const char*> items;
            items.reserve(proto_names_.size());
            for (const std::string& s : proto_names_) { items.push_back(s.c_str()); }
            ui_.Combo("原型族", &proto_index_, items,
                      jpov::UiRect{{cx, 40.0f}, {colw, 26.0f}});
        }
    }

    // 原型族下标 clamp 到 [0, size)；无族时返回 -1。
    int ClampedProtoIndex() const {
        const int n = static_cast<int>(proto_ids_.size());
        if (n == 0) { return -1; }
        if (proto_index_ < 0) { return 0; }
        if (proto_index_ >= n) { return n - 1; }
        return proto_index_;
    }

    // 每族「显示尺度」最大边（米）——仅决定观感大小；**盒比例 = 体维数比例**。
    static float ProtoTargetMax(const std::string& name) {
        if (name == "jet") { return 0.50f; }     // 高 0.5m 的细柱
        if (name == "area") { return 0.80f; }
        if (name == "pool") { return 1.60f; }
        return 1.20f;                              // transient
    }

    // 由「体维数 + 目标最大边」推世界 AABB：贴地 y=0，**各轴等比 ⇒ 不拉伸**。
    void ProtoBoxForIndex(int i, jpov::Vec3f* mn, jpov::Vec3f* mx) const {
        const float nx = static_cast<float>(proto_nx_[static_cast<size_t>(i)]);
        const float ny = static_cast<float>(proto_ny_[static_cast<size_t>(i)]);
        const float nz = static_cast<float>(proto_nz_[static_cast<size_t>(i)]);
        const float maxd = std::max(nx, std::max(ny, nz));
        const float sc = ProtoTargetMax(proto_names_[static_cast<size_t>(i)]) / std::max(1.0f, maxd);
        *mn = {-0.5f * nx * sc, 0.0f, -0.5f * nz * sc};
        *mx = {0.5f * nx * sc, ny * sc, 0.5f * nz * sc};
    }

    // 每族「默认消光 σ」(1/m)：密度归一化 ⇒ 小火焰需更大 σ 才有可见光深；
    // 大族（池火）不能太大，否则整团不透明变“实心块”。
    static float ProtoSigmaFor(const std::string& name) {
        if (name == "jet") { return 30.0f; }    // 蜡烛/细柱：细而密
        if (name == "area") { return 5.0f; }    // 面火
        if (name == "pool") { return 2.5f; }    // 池火（大团→低 σ 见结构）
        return 3.0f;                              // transient
    }

    bool show_panel_ = true;
    bool show_cam_info_ = true;
    jpov::Ui ui_;

    // 原型族体积的懒加载状态（GL id / 帧数）。
    std::vector<uint32_t> proto_ids_;
    std::vector<int> proto_nframes_;
    std::vector<int> proto_nx_, proto_ny_, proto_nz_;
    int  last_proto_index_ = -1;   // 触发「切族→重置+取景」的一次性哨兵
    bool proto_registered_ = false;
};

}  // namespace jpov_volume_fire

#endif  // JPOV_DEMO_VOLUME_FIRE_VIEWER_APP_H_
