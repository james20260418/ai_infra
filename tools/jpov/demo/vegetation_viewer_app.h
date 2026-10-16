// JPOV 植被查看器（vegetation viewer）— 渲染核心 App（场景渲染体 + 交互面板宿主）
//
// 定位：**植物生成链路的最后环节工具** —— 装载「已生成的植物 GLB」，
// 在**标准天光（昼夜可调）** 下肉眼观察 + 出图。GLB 路径由主程序以
// `--glb <path>` 传入（见 jpov_vegetation_viewer.cc），本类只负责
// 「装载 + 渲染 + 面板」，不关心资产怎么生成。
//
// 与 skylight viewer 同构：同一套 JPOV::OneIteration 渲染体 + 即时模式 UI 面板；
// 光照直接复用 skylight_scene 的 SkyDegrees / MakeSkyLighting（五个天光自由度 +
// 三色 ambient 开关），因此昼夜 / 浊度 / 月色 / 夜蓝的观察口径与天光查看器**完全一致**。
//
// 面板：天光自由度（浊度 / 季节色温 / 天体仰角+方位 / 月色 / 夜蓝 / 三色 ambient）
//       + SMAA 抗锯齿（**默认开**）。视角沿用 model viewer 的 ViewConfig
//       （右键拖拽转视角 / 滚轮缩放）。
//
// 边界（本版）：**不含风**（风牵涉 pass 与资产改动，另开 PR）；不加载骨架/蒙皮 ——
// 植物是静态几何，直接用 DrawGltfObject 画（含 cutout 叶片）。

#ifndef JPOV_DEMO_VEGETATION_VIEWER_APP_H_
#define JPOV_DEMO_VEGETATION_VIEWER_APP_H_

#include <cstdint>
#include <cstdio>
#include <string>

#include "tools/common/utils.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/demo/viewer_app.h"   // 复用 kViewerWidth/Height/Fps/FontAlias
#include "tools/jpov/interface/ui.h"

namespace jpov_vegetation {

using jpov_viewer::kViewerWidth;
using jpov_viewer::kViewerHeight;
using jpov_viewer::kViewerFps;
using jpov_viewer::kViewerFontAlias;

// 植物在世界里的目标高度（米）。装载时把任意资产的 Y 向包围盒高**归一到此值**，
// 使取景距离 / 地面尺度 / 曝光随资产变化保持稳定（资产原始尺寸差异很大）。
inline constexpr float kPlantTargetHeight = 3.5f;

// 默认植物资产名（分发态放在 exe 旁 plants/；开发态回退源码资产目录）。
inline constexpr const char* kDefaultPlantFile = "willow_small_tex.glb";

// 解析植物 GLB 路径：按顺序试多个候选（分发态 exe 旁 → 开发态源码目录），
// 命中即返回；都不命中返回第一个候选，交回 loader 报它自己的错。
//   - exe 旁 plants/<name>        ：build 脚本把范本拷到此（分发态）
//   - 源码 tools/.../samples/<name>：`bazel run` 时 cwd=工程根（开发态）
inline std::string ResolvePlantPath(const std::string& file_name) {
    const std::string candidates[] = {
        std::string("plants/") + file_name,
        std::string("tools/jpov/tools/vegetation/samples/") + file_name,
    };
    for (const std::string& c : candidates) {
        const std::string resolved = jpov::ResolveResourcePath(c);
        FILE* fp = std::fopen(resolved.c_str(), "rb");
        if (fp != nullptr) {
            std::fclose(fp);
            return resolved;
        }
    }
    return candidates[0];
}

// 植被查看器渲染核心 App。
class VegetationViewerApp : public JPOV {
public:
    using JPOV::JPOV;

    // ── 场景资源（Init() 后装配一次，不在 OneIteration 里重复构造/上传）──
    jpov::GltfObject plant_;            // 被查看的植物 GLB（primitives 独占 GPU 资源）
    uint32_t ground_mesh_ = 0;          // 40×40 地面 quad（skylight_scene）
    jpov::PBRMaterial ground_mat_;      // 高粗糙灰色地面材质

    // ── 视角：沿用 ViewConfig（y-up 球面角 + 右键拖拽/滚轮缩放）──
    jpov_viewer::ViewConfig view_;

    // ── 天光自由度（跨帧持有；天光与光照全由它们推导）──
    jpov_skylight::SkyDegrees deg_;

    // ── 光照选项 ──
    // 三色环境光（[天, 天际线, 地] 垂直梯度）替代单色 ambient，供肉眼对比。
    bool tricolor_ambient_ = false;

    // 是否绘制面板（headless 拍摄 = false，截图即纯 3D 场景）。
    void SetShowPanel(bool show) { show_panel_ = show; }

    // SMAA 后处理抗锯齿。**默认开**（本查看器定位“观察 + 出图”，去锯齿是默认期望）。
    void SetSmaa(bool on, float threshold = 0.1f) {
        smaa_on_ = on;
        smaa_threshold_ = threshold;
    }
    void SetTricolorAmbient(bool on) { tricolor_ambient_ = on; }

    void InstallTextMeasure() {
        ui_.SetTextMeasure(&VegetationViewerApp::AppTextWidth, this);
    }

    // 装配：地面 + 装载植物 GLB（路径由主程序给出，可绝对/相对）。
    // Pre-condition: path 指向一个可读的 .gltf/.glb；植物须有有效包围盒。
    void InstallSceneWithGltf(const std::string& path) {
        ground_mesh_ = RegisterMesh(jpov_skylight::MakeGroundQuad());
        ground_mat_  = jpov_skylight::GroundMaterial();

        plant_ = LoadGltf(path);
        CHECK(!plant_.empty()) << "VegetationViewerApp: 植物 GLB 加载失败: " << path;
        CHECK(plant_.bounds_valid)
            << "VegetationViewerApp: 植物 GLB 无有效包围盒，无法归一取景: " << path;
        const float asset_h = plant_.bounds_max[1] - plant_.bounds_min[1];
        CHECK_GT(asset_h, 1.0e-6f)
            << "VegetationViewerApp: 植物 Y 向高度≈0: " << path;
        base_scale_ = kPlantTargetHeight / asset_h;
        aim_y_ = 0.5f * base_scale_ * asset_h;   // 相机注视点 = 植物竖直中点
        FitViewToPlant();
    }

    // 相机自适应：把植物完整框入视野（注视点 = 植物竖直中点，见 OneIteration）。
    void FitViewToPlant() {
        CHECK(plant_.bounds_valid) << "FitViewToPlant: 尚无植物包围盒";
        // 以「竖直中点」为原点构造居中包围盒（世界尺度），喂给 FitRadius。
        const float ex = base_scale_ * (plant_.bounds_max[0] - plant_.bounds_min[0]);
        const float ey = base_scale_ * (plant_.bounds_max[1] - plant_.bounds_min[1]);
        const float ez = base_scale_ * (plant_.bounds_max[2] - plant_.bounds_min[2]);
        const float rel_min[3] = {-0.5f * ex, -0.5f * ey, -0.5f * ez};
        const float rel_max[3] = { 0.5f * ex,  0.5f * ey,  0.5f * ez};
        view_ = jpov_viewer::DefaultView();
        view_.phi   = 0.10;                                   // 略俯视
        view_.theta = 3.14159265358979323846 / 4.0;           // 45° 方位
        view_.R     = jpov_viewer::ViewConfig::FitRadius(rel_min, rel_max, 60.0);
    }

    // ⭐ 唯一渲染体：交互 Run 循环 与 headless 拍摄共用（zero 分叉）。
    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;

        cmds->camera.fbo_3d_width_  = kViewerWidth;
        cmds->camera.fbo_3d_height_ = kViewerHeight;

        // 交互输入 → 改视角（同 model viewer 的 ApplyInput）；headless 不消费输入。
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

        // 相机：绕「植物竖直中点」环绕（view_ 给出相对原点的偏移，平移到注视点上）。
        const jpov::Vec3f off = view_.Position();
        cmds->camera.target   = {0.0f, aim_y_, 0.0f};
        cmds->camera.position = {off.x(), off.y() + aim_y_, off.z()};
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 60.0f;
        cmds->camera.near     = 0.05f;
        cmds->camera.far      = 1000.0f;

        // 天光 + 光照：全由天光自由度推导（白天主光=太阳、夜间主光=月亮）。
        const jpov_skylight::SkyLighting light =
            jpov_skylight::MakeSkyLighting(deg_, tricolor_ambient_);
        cmds->sky     = light.sky;
        cmds->sun     = light.dir_light;
        cmds->ambient = light.ambient;
        cmds->tone_mapping = true;

        // SMAA 后处理抗锯齿：tone map 之后、2D 之前的最后一站（默认开）。
        if (smaa_on_) {
            jpov::SmaaConfig sc;
            sc.enabled = true;
            sc.threshold = smaa_threshold_;
            cmds->smaa = sc;
        }

        // 场景：灰色地面 + 植物（按包围盒把基座落到地面 y=0、水平居中）。
        cmds->DrawObject3D(ground_mesh_, ground_mat_,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f});
        if (!plant_.empty()) {
            const float s  = base_scale_;
            const float cx = -0.5f * s *
                             (plant_.bounds_min[0] + plant_.bounds_max[0]);
            const float cz = -0.5f * s *
                             (plant_.bounds_min[2] + plant_.bounds_max[2]);
            const float cy = -s * plant_.bounds_min[1];   // 基座贴地
            cmds->DrawGltfObject(plant_, /*center*/ {cx, cy, cz},
                                 /*up*/ {0.0f, 1.0f, 0.0f},
                                 /*front*/ {0.0f, 0.0f, 1.0f}, s,
                                 /*highlight*/ false, /*picking_id*/ 0);
        }

        if (show_panel_) {
            DrawLightPanel(input);
            // 半透明黑底版：在面板控件入队【之前】发出 → 先画，控件叠在其上。
            // （2D 指令按入队顺序绘制：先 DrawRect 再 ui_.Emit。）
            cmds->DrawRect(panel_bg_.pos, panel_bg_.size,
                           /*color*/ {0.0f, 0.0f, 0.0f, 0.5f});
            ui_.End();
            ui_.Emit(cmds);
        }
    }

private:
    static float AppTextWidth(const char* text, float font_size,
                              const char* /*font_alias*/, void* userdata) {
        VegetationViewerApp* app = static_cast<VegetationViewerApp*>(userdata);
        return app->MeasureTextWidth(/*alias=*/std::string(),
                                     /*text=*/text ? text : "", font_size);
    }

    // 面板：锚定窗口**右下角**，半透明黑底版 + 单列浓缩天光控件（见 OneIteration 的 DrawRect）。
    // 已按需求删掉「血月 / 夜空偏蓝」这类花哨项，只留最基本的昼夜观察旋钮。
    void DrawLightPanel(const jpov::InputSnapshot& input) {
        const float w = static_cast<float>(kViewerWidth);
        const float h = static_cast<float>(kViewerHeight);
        jpov::UiTheme theme = jpov::UiTheme::Default(kSliderFontSize);
        theme.font_alias = kViewerFontAlias;
        const float frame_dt_ms = 1000.0f / kViewerFps;
        ui_.Begin(input, theme, w, h, frame_dt_ms);

        const float kPanelW = 360.0f;   // 面板外框宽（像素，固定）
        const float kPad    = 12.0f;    // 底板内边距
        const float kRowH   = 26.0f;
        const float kSpace  = 8.0f;
        const int   kRows   = 7;
        const float kMargin = 16.0f;    // 距窗口右/下边距

        const float inner_w = kPanelW - 2.0f * kPad;
        const float inner_h = static_cast<float>(kRows) * kRowH
                              + static_cast<float>(kRows - 1) * kSpace;
        const float panel_h = inner_h + 2.0f * kPad;
        const float panel_x = w - kMargin - kPanelW;    // 贴右下角
        const float panel_y = h - kMargin - panel_h;
        panel_bg_ = jpov::UiRect{{panel_x, panel_y}, {kPanelW, panel_h}};

        const float cx = panel_x + kPad;
        const float cy = panel_y + kPad;
        auto row = [&](int i) {
            return jpov::UiRect{
                {cx, cy + static_cast<float>(i) * (kRowH + kSpace)},
                {inner_w, kRowH}};
        };

        ui_.SliderFloat("浊度 turb", &deg_.turbidity, row(0),
                        jpov_skylight::kTurbidityMin, jpov_skylight::kTurbidityMax, 1);
        ui_.SliderFloat("季节色温 日光 (左蓝..右红)", &deg_.daylight_season,
                        row(1), -1.0f, 1.0f, 2);
        ui_.SliderFloat("天体仰角 ° (负=月光/正=日光)", &deg_.sun_elev_deg,
                        row(2), -90.0f, 90.0f, 0);
        ui_.SliderFloat("天体方位角 °", &deg_.sun_azim_deg, row(3), 0.0f, 360.0f, 0);
        ui_.Checkbox("三色环境光 (天/天际线/地)", &tricolor_ambient_, row(4));
        ui_.Checkbox("SMAA 抗锯齿", &smaa_on_, row(5));
        ui_.SliderFloat("SMAA 阈值 (小=更敏感)", &smaa_threshold_, row(6),
                        0.02f, 0.5f, 2);
    }

    bool show_panel_ = true;
    bool smaa_on_ = true;            // ★ 默认开（本查看器定位“观察 + 出图”）
    float smaa_threshold_ = 0.1f;    // SMAA luma 边缘阈值（越小越敏感）
    float base_scale_ = 1.0f;        // 资产→世界 的归一缩放（把 Y 高映射到 kPlantTargetHeight）
    float aim_y_ = 0.0f;             // 相机注视点高度 = 植物竖直中点（世界坐标，米）
    jpov::Ui ui_;
    jpov::UiRect panel_bg_{};        // 面板底板矩形（本帧 DrawLightPanel 填，供 DrawRect）

    static constexpr float kSliderFontSize = 15.0f;
};

}  // namespace jpov_vegetation

#endif  // JPOV_DEMO_VEGETATION_VIEWER_APP_H_
