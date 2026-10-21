// JPOV 半透明条带查看器（strip viewer）— 渲染核心 App
//
// 场景 = 天光 + 灰色地面 + **一批三条带（一次 draw call）**：
//   ① 波带（宽、柔和边缘、带纵向淡出）
//   ② 螺旋拖尾（窄、绕行进）
//   ③ 弧形扇面（宽度沿长度变化）
// 三条在屏幕空间**部分交叉**（各自形状可辨、交叉处可见覆盖混合）——
// 用来直观验收「WBO 覆盖集 + over 合成」。
//
// 用途：M1 的肉眼验收工具（gold 用同一套生成函数，见 test/strip/strip_gold.cc）。
// 交互：右键拖拽/滚轮绕行；面板可调不透明度、剖面、纹理开关、形状动画。
// 拍摄：--capture <dir>

#ifndef JPOV_DEMO_STRIP_VIEWER_APP_H_
#define JPOV_DEMO_STRIP_VIEWER_APP_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/demo/viewer_app.h"
#include "tools/jpov/interface/strip_types.h"
#include "tools/jpov/interface/ui.h"

namespace jpov_strip {

// 向量小工具（本文件私有）。归一化在零向量时回退，避免 NaN 进几何。
inline jpov::Vec3f NormalizeOr(const jpov::Vec3f& v, const jpov::Vec3f& fallback) {
    const float n = v.Norm();
    if (n < 1e-6f) {
        return fallback;
    }
    return jpov::Vec3f{v.x() / n, v.y() / n, v.z() / n};
}

inline jpov::Vec3f Cross(const jpov::Vec3f& a, const jpov::Vec3f& b) {
    return jpov::Vec3f{a.y() * b.z() - a.z() * b.y(),
                       a.z() * b.x() - a.x() * b.z(),
                       a.x() * b.y() - a.y() * b.x()};
}

// ── 条带生成（纯函数，gold 与 viewer 共用同一份实现，避免"看起来差不多"）──

// 从参数化中心线 + 逐点半宽生成 Strip16。
//
// center_fn(s, phase) → 世界空间中心点（s ∈ [0,1]）
// half_left_fn(s) / half_right_fn(s) → 两侧半宽（米）
// alpha_fn(s) → 纵向 alpha 乘子
//
// 侧向基：side = normalize(cross(tangent, up_hint))；切向取中心差分（端点单侧），
// 与设计文档 §2.1 一致。up_hint 与切向共线时退到 (0,0,1)（避免 NaN）。
template <typename CenterFn, typename HalfLeftFn, typename HalfRightFn, typename AlphaFn>
void BuildRibbon(float phase, CenterFn center_fn, HalfLeftFn half_left_fn,
                 HalfRightFn half_right_fn, AlphaFn alpha_fn, jpov::Strip16* out) {
    CHECK(out != nullptr) << "BuildRibbon: out 不能为空";
    const int n = jpov::kStripSampleCount;
    const float ds = 0.5f / static_cast<float>(n - 1);

    for (int i = 0; i < n; ++i) {
        const float s = static_cast<float>(i) / static_cast<float>(n - 1);
        const jpov::Vec3f c = center_fn(s, phase);

        // 切向：内部中心差分，端点单侧差分。
        const float s0 = (i == 0) ? s : s - ds;
        const float s1 = (i == n - 1) ? s : s + ds;
        const jpov::Vec3f a = center_fn(s0, phase);
        const jpov::Vec3f b = center_fn(s1, phase);
        jpov::Vec3f t = NormalizeOr(b - a, {0.0f, 0.0f, 1.0f});

        jpov::Vec3f up = {0.0f, 1.0f, 0.0f};
        jpov::Vec3f side = Cross(t, up);
        if (side.Norm() < 1e-4f) {
            side = Cross(t, {0.0f, 0.0f, 1.0f});
        }
        side = NormalizeOr(side, {1.0f, 0.0f, 0.0f});

        out->left[static_cast<size_t>(i)] = c + side * half_left_fn(s);
        out->right[static_cast<size_t>(i)] = c - side * half_right_fn(s);
        out->alpha[static_cast<size_t>(i)] = alpha_fn(s);
    }
    out->point_count = static_cast<uint32_t>(n);
}

// ① 波带：沿 +X 前进，Y 方向正弦起伏，带内轻微扭转（用 up_hint 的同向偏移表达）。
inline void BuildWaveRibbon(float phase, float width_scale, jpov::Strip16* out) {
    const float kLen = 7.0f;
    auto center = [kLen](float s, float ph) {
        const float x = (s - 0.5f) * kLen;
        return jpov::Vec3f{x, 0.85f + 0.55f * std::sin(1.7f * s * 3.14159f + ph),
                           -0.45f + 0.30f * std::cos(1.1f * s * 3.14159f + ph)};
    };
    auto half_l = [width_scale](float s) { return (0.35f + 0.10f * s) * width_scale; };
    auto half_r = [width_scale](float /*s*/) { return 0.35f * width_scale; };
    // 尾部淡出：验证「逐点 alpha」这条通路真的进了混合。
    auto alpha = [](float s) {
        const float fade_in = std::min(1.0f, s / 0.08f);
        const float fade_out = std::min(1.0f, (1.0f - s) / 0.22f);
        return fade_in * fade_out;
    };
    BuildRibbon(phase, center, half_l, half_r, alpha, out);
}

// ② 螺旋拖尾：绕 +Y 上升，宽度恒定（窄）。
inline void BuildHelixRibbon(float phase, float width_scale, jpov::Strip16* out) {
    auto center = [](float s, float ph) {
        const float ang = 2.0f * 3.14159f * 1.5f * s + ph;
        const float r = 1.05f - 0.25f * s;
        return jpov::Vec3f{r * std::sin(ang) + 0.2f, 0.30f + 2.20f * s,
                           r * std::cos(ang) - 0.6f};
    };
    auto half_w = [width_scale](float /*s*/) { return 0.22f * width_scale; };
    auto alpha = [](float /*s*/) { return 1.0f; };
    BuildRibbon(phase, center, half_w, half_w, alpha, out);
}

// ③ 弧形扇面：在 XZ 平面上扫一段弧，宽度沿弧长张开（剑气式）。
inline void BuildArcRibbon(float phase, float width_scale, jpov::Strip16* out) {
    auto center = [](float s, float ph) {
        const float ang = -1.00f + 1.90f * s + ph;
        const float r = 2.40f;
        return jpov::Vec3f{r * std::sin(ang) + 0.5f, 1.30f - 0.90f * s,
                           r * std::cos(ang) - 1.0f};
    };
    auto half_l = [width_scale](float s) { return (0.06f + 0.55f * s * s) * width_scale; };
    auto half_r = [width_scale](float s) { return (0.04f + 0.12f * s) * width_scale; };
    auto alpha = [](float s) { return std::min(1.0f, (1.0f - s) / 0.35f); };
    BuildRibbon(phase, center, half_l, half_r, alpha, out);
}

// 按条带整体缩放逐点 alpha（用于让多条同色带在重叠处可分辨）。
inline void ScaleRibbonAlpha(float scale, jpov::Strip16* strip) {
    CHECK(strip != nullptr) << "ScaleRibbonAlpha: strip 不能为空";
    for (float& a : strip->alpha) {
        a *= scale;
    }
}

// 三条带装进**一个 batch** → 一次 draw call。
inline void BuildDemoBatch(float phase, float width_scale, float opacity,
                           bool soft_edges, jpov::TranslucentStripBatch* out) {
    CHECK(out != nullptr) << "BuildDemoBatch: out 不能为空";
    out->strips.resize(3);
    BuildWaveRibbon(phase, width_scale, &out->strips[0]);
    BuildHelixRibbon(phase, width_scale, &out->strips[1]);
    BuildArcRibbon(phase, width_scale, &out->strips[2]);
    // 三条同色时重叠处会糊成一团：给不同的不透明度，交叉处就能看出「谁压在谁上面」，
    // 同时仍然保留形状轮廓（这也是最省的「多色」手段 —— 颜色是整批共享的）。
    ScaleRibbonAlpha(1.00f, &out->strips[0]);
    ScaleRibbonAlpha(0.62f, &out->strips[1]);
    ScaleRibbonAlpha(0.45f, &out->strips[2]);
    out->tint = jpov::Color{1.0f, 1.0f, 1.0f, opacity};
    // 「柔边」= 宽度两端衰减到 0（几何折角被吃掉，视觉上变软）；硬边 = 全 1。
    out->width_profile = soft_edges
                             ? std::array<float, 9>{0.0f, 0.35f, 0.80f, 1.0f, 1.0f,
                                                    1.0f, 0.80f, 0.35f, 0.0f}
                             : std::array<float, 9>{1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                                    1.0f, 1.0f, 1.0f, 1.0f};
}

class StripViewerApp : public JPOV {
public:
    using JPOV::JPOV;

    uint32_t ground_mesh_ = 0;
    jpov::PBRMaterial ground_mat_;
    uint32_t ribbon_tex_ = 0;                // 0 = 纹理未加载（纯色条带）
    uint32_t box_mesh_ = 0;                  // 背景参考物（验收天光用）
    jpov::PBRMaterial box_mat_low_gloss_;
    jpov::PBRMaterial box_mat_high_gloss_;
    jpov::PBRMaterial box_mat_metal_;

    jpov_viewer::ViewConfig view_;
    jpov_skylight::SkyDegrees deg_;

    // 旋钮（面板 / 拍摄共用同一处状态）
    float opacity_ = 0.92f;        // 整批不透明度（tint.a）
    float width_scale_ = 1.0f;     // 条带宽度整体缩放
    bool use_texture_ = true;      // false = 纯色（不采样纹理）
    // 纯色模式的颜色（线性 HDR）。默认青蓝，和白色/纹理都区分得开。
    jpov::Color solid_color_ = {0.18f, 0.78f, 0.95f, 1.0f};
    bool soft_edges_ = true;       // 宽度剖面：柔边 / 硬边
    bool animate_ = false;         // 形状动画（gold 用固定 phase=0）
    bool show_strips_ = true;      // false = 完全不发条带命令（A/B 对照用）
    float phase_ = 0.0f;           // 当前相位（animate_ 时逐帧推进）

    // 装配场景静态资源（Init 之后、Run/RunOnce 之前调用一次）。
    void Setup(const std::string& texture_path);

    void SetShowPanel(bool show) { show_panel_ = show; }

    // UI 文本宽度回调（保持与其它 viewer 同款接线，中文字符宽度用真实字体量）。
    void InstallTextMeasure() { ui_.SetTextMeasure(&StripViewerApp::TextWidth, this); }

    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        cmds->camera.fbo_3d_width_ = jpov_viewer::kViewerWidth;
        cmds->camera.fbo_3d_height_ = jpov_viewer::kViewerHeight;

        if (show_panel_) {
            float dx = 0.0f;
            float dy = 0.0f;
            float scroll = 0.0f;
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
        }

        if (animate_) {
            phase_ += 0.02f;
        }

        cmds->camera.position = view_.Position();
        // 三条带都在 y≈0.3..2.7（离地悬空），所以看向原点会让画面整体偏上；
        // 目标点抬高到 1.15m 才把条带放在画面中央。
        cmds->camera.target = {0.0f, 1.15f, 0.0f};
        cmds->camera.up = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov = 55.0f;
        cmds->camera.near = 0.05f;
        cmds->camera.far = 200.0f;

        // 天光：天色与物体受光同源（见 skylight_scene.h 的约定）。
        const jpov_skylight::SkyLighting light =
            jpov_skylight::MakeSkyLighting(deg_, /*tricolor_ambient=*/true);
        cmds->sky = light.sky;
        cmds->sun = light.dir_light;
        cmds->ambient = light.ambient;
        cmds->tone_mapping = true;

        cmds->DrawObject3D(ground_mesh_, ground_mat_,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f},
                           /*scale*/  1.0f);

        // 背景三块参考方块（低反/高光/金属）：让「天光真的照在物体上」可验，
        // 同时为条带提供可对照的前后遮挡——条带在半透明的同时仍吃场景深度。
        cmds->DrawObject3D(box_mesh_, box_mat_low_gloss_, {-2.0f, 0.5f, -2.6f},
                           {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, /*scale*/ 1.0f);
        cmds->DrawObject3D(box_mesh_, box_mat_high_gloss_, {0.0f, 0.5f, -2.6f},
                           {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, /*scale*/ 1.0f);
        cmds->DrawObject3D(box_mesh_, box_mat_metal_, {2.0f, 0.5f, -2.6f},
                           {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, /*scale*/ 1.0f);

        if (!show_strips_) {
            // A/B 对照：不发条带命令，用于把「条带覆盖了哪些像素」精确分离出来。
            if (show_panel_) {
                DrawPanel(input);
                ui_.End();
                ui_.Emit(cmds);
            }
            return;
        }

        jpov::TranslucentStripBatch batch;
        if (use_texture_) {
            batch.texture_id = ribbon_tex_;
            BuildDemoBatch(phase_, width_scale_, opacity_, soft_edges_, &batch);
        } else {
            // 纯色接口：不采样纹理，颜色 = solid_color_（alpha 由 opacity_ 控）。
            BuildDemoBatch(phase_, width_scale_, 1.0f, soft_edges_, &batch);
            jpov::Color color = solid_color_;
            color.a = opacity_;
            std::vector<jpov::Strip16> strips = std::move(batch.strips);
            batch = jpov::MakeSolidStripBatch(color, std::move(strips));
        }
        cmds->DrawTranslucentStrips(std::move(batch));

        if (show_panel_) {
            DrawPanel(input);
            ui_.End();
            ui_.Emit(cmds);
        }
    }

private:
    static float TextWidth(const char* text, float font_size, const char* /*alias*/,
                           void* userdata) {
        StripViewerApp* app = static_cast<StripViewerApp*>(userdata);
        return app->MeasureTextWidth(std::string(), text ? text : "", font_size);
    }

    void DrawPanel(const jpov::InputSnapshot& input) {
        const float w = static_cast<float>(jpov_viewer::kViewerWidth);
        const float h = static_cast<float>(jpov_viewer::kViewerHeight);
        jpov::UiTheme theme = jpov::UiTheme::Default(kFontSize);
        theme.font_alias = jpov_viewer::kViewerFontAlias;
        ui_.Begin(input, theme, w, h, 1000.0f / jpov_viewer::kViewerFps);

        const float kSliderWidth = 0.42f * w;
        const float kRowH = 28.0f;
        const float kSpacing = 10.0f;
        const float left = 24.0f;
        const float top = 24.0f;
        int row = 0;
        auto row_rect = [&](int index) {
            return jpov::UiRect{{left, top + static_cast<float>(index) * (kRowH + kSpacing)},
                                {kSliderWidth, kRowH}};
        };

        ui_.SliderFloat("太阳仰角 °", &(deg_.sun_elev_deg), row_rect(row++), 0.0f, 90.0f, 0);
        ui_.SliderFloat("不透明度", &opacity_, row_rect(row++), 0.0f, 1.0f, 2);
        ui_.SliderFloat("条带宽度", &width_scale_, row_rect(row++), 0.2f, 2.5f, 2);
        ui_.Checkbox("使用纹理（关 = 纯色）", &use_texture_, row_rect(row++));
        ui_.Checkbox("柔边剖面", &soft_edges_, row_rect(row++));
        ui_.Checkbox("形状动画", &animate_, row_rect(row++));
        ui_.Checkbox("显示条带", &show_strips_, row_rect(row++));
    }

    static constexpr float kFontSize = 16.0f;
    bool show_panel_ = true;
    jpov::Ui ui_;
};

// 查看器的默认取景与天光（viewer 主程序与 gold test 共用同一份，避免"看起来差不多"）。
inline void ApplyDefaultView(StripViewerApp* app) {
    CHECK(app != nullptr) << "ApplyDefaultView: app 不能为空";
    app->view_ = jpov_viewer::DefaultView();
    app->view_.phi = 0.24f;
    app->view_.theta = 0.60f;
    app->view_.R = 5.6f;
    app->deg_.sun_elev_deg = 34.0f;
    app->deg_.sun_azim_deg = 40.0f;
    app->deg_.turbidity = 2.4f;
}

inline void StripViewerApp::Setup(const std::string& texture_path) {
    ground_mesh_ = RegisterMesh(jpov_skylight::MakeGroundQuad());
    ground_mat_ = jpov_skylight::GroundMaterial();
    box_mesh_ = RegisterMesh(jpov::MeshData::MakeBox(0.5f, 0.5f, 0.5f));
    box_mat_low_gloss_ = jpov_skylight::MaterialLowGloss();
    box_mat_high_gloss_ = jpov_skylight::MaterialHighGloss();
    box_mat_metal_ = jpov_skylight::MaterialMetal();
    if (!texture_path.empty()) {
        jpov::TextureOptions opts;
        // 不开 mipmap：本纹理是「横向 4 条色带」的手绘测试卡（1318×158），
        // 条带宽度只占屏幕几十像素 → u 方向压缩上百倍，mip 会把相邻色带（纵向）
        // 混成灰。色带在 u 方向本来就是常量，不需要 mip；关掉后颜色才饱和。
        opts.mipmap = false;
        opts.repeat = false;
        ribbon_tex_ = RegisterTexture(texture_path, opts);
    }
}

}  // namespace jpov_strip

#endif  // JPOV_DEMO_STRIP_VIEWER_APP_H_
