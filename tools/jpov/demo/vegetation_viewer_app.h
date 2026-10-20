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
// 边界：不加载骨架/蒙皮 —— 植物是静态几何，走 **InstancedObjectRenderer**
// （同 mesh 摆 N 份 = 一次 instanced draw；含 cutout 叶片、风、阴影、拾取）。
// 植被风：pivot 链摇摆 + 逐实例「吹倒」（吹倒比例 × 父链总长度）。

#ifndef JPOV_DEMO_VEGETATION_VIEWER_APP_H_
#define JPOV_DEMO_VEGETATION_VIEWER_APP_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

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

// 实例化演示：同一种植物摆 kGridN×kGridN 份（每 primitive 一次 instanced draw）。
//   间距是世界米；目的有二：① 走 InstancedObjectRenderer 这条真实路径；
//   ② 让「逐实例吹倒比例」有地方可见（每株可给不同值）。
inline constexpr int   kGridN = 3;
inline constexpr float kGridSpacing = 2.2f;

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

    // ── 植被风（程序化顶点风；仅对带风数据（关节表 + loc6）的资产生效）──
    bool  wind_on_ = true;          // 默认开（本 viewer 的招牌能力之一）
    float wind_azimuth_deg_ = 35.0f;  // 风向方位角（绕 +Y，度）
    float wind_amplitude_ = 0.35f;    // 主弯振幅
    float wind_frequency_ = 0.5f;     // 基频 Hz
    float wind_turb_amp_ = 0.12f;     // 链外平动湍流幅度（0=关）
    float wind_turb_freq_ = 2.5f;     // 平动湍流频率 Hz
    float wind_gust_freq_ = 0.6f;     // 阵风频率 Hz
    float wind_gust_str_ = 0.7f;      // 阵风强度 [0,1]（0=关起伏）
    float wind_time_scale_ = 1.0f;    // 时间缩放（0=暂停）
    bool  wind_use_fixed_time_ = false;   // headless：钉住时间（确定性出图）
    float wind_fixed_time_ = 0.0f;
    // 风吹倒伏比例（逐实例；0 = 不吹，0.5 = 每单位父链长度水平偏移 0.5）。
    float blow_away_ratio_ = 0.0f;
    // 吹倒位移指数 n（per-command）：位移 = ratio × L^n。1 = 线性，2 = 二次。
    float blow_away_exponent_ = 2.0f;
    // 实例数量（1..kGridN²）：调试用，滑条实时增减实例。
    float instance_count_ui_ = 4.0f;

    // 是否绘制面板（headless 拍摄 = false，截图即纯 3D 场景）。
    void SetShowPanel(bool show) { show_panel_ = show; }

    // headless：固定风时间（秒）+ 开关风。
    void SetWindFixedTime(float t) { wind_use_fixed_time_ = true; wind_fixed_time_ = t; }
    void SetWindEnabled(bool on) { wind_on_ = on; }
    // headless：钉住吹倒比例 / 指数 / 实例数（A/B 出图用；交互由左下角滑条控制）。
    void SetBlowAwayRatio(float r) { blow_away_ratio_ = r; }
    void SetBlowAwayExponent(float n) { blow_away_exponent_ = n; }
    void SetInstanceCount(int n) {
        instance_count_ui_ = static_cast<float>(n);
    }

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
        // 实例网格按**最大**规模取景（实例数滑条变化时相机不跳）。
        // 实例网格也要进画面：x/z 各加 (kGridN-1) 个间距。
        const float grid_extent = static_cast<float>(kGridN - 1) * kGridSpacing;
        const float ex = base_scale_ * (plant_.bounds_max[0] - plant_.bounds_min[0])
                         + grid_extent;
        const float ey = base_scale_ * (plant_.bounds_max[1] - plant_.bounds_min[1]);
        const float ez = base_scale_ * (plant_.bounds_max[2] - plant_.bounds_min[2])
                         + grid_extent;
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

        // 植被风：逐物体参数（这里整株植物一份；同场景其它物体可各自给不同的风）。
        //   时间由帧号推（确定性；headless 可 SetWindFixedTime 钉住）。
        std::optional<jpov::WindCommand> wind;
        if (wind_on_) {
            const float t = wind_use_fixed_time_
                                ? wind_fixed_time_
                                : static_cast<float>(frame_count) /
                                      kViewerFps * wind_time_scale_;
            constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;
            const float az = wind_azimuth_deg_ * kDeg2Rad;
            jpov::WindCommand w;
            w.time = t;
            w.amplitude = wind_amplitude_;
            w.frequency = wind_frequency_;
            w.direction = {std::cos(az), 0.0f, std::sin(az)};
            w.turb_amplitude = wind_turb_amp_;
            w.turb_frequency = wind_turb_freq_;
            w.gust_frequency = wind_gust_freq_;
            w.gust_strength = wind_gust_str_;
            w.blow_away_exponent = blow_away_exponent_;
            wind = w;
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
            // 实例数（滑条 1..kGridN²；排成接近正方形的网格，逐实例只差位置 + yaw）。
            const int want = static_cast<int>(std::lround(instance_count_ui_));
            const int count = std::max(1, std::min(kGridN * kGridN, want));
            const int cols = static_cast<int>(
                std::ceil(std::sqrt(static_cast<float>(count))));
            const int rows = (count + cols - 1) / cols;
            std::vector<jpov::InstanceState> instances;
            instances.reserve(static_cast<size_t>(count));
            for (int idx = 0; idx < count; ++idx) {
                const int gx = idx % cols;
                const int gz = idx / cols;
                const float ox = (static_cast<float>(gx) -
                                  0.5f * static_cast<float>(cols - 1)) * kGridSpacing;
                const float oz = (static_cast<float>(gz) -
                                  0.5f * static_cast<float>(rows - 1)) * kGridSpacing;
                // 逐实例 yaw（黄金角）：避免完全同向的"复制粘贴"感，
                //   同时**顺带验证**「世界风向 → 各实例模型系」的逆变换
                //   （若漏了那一步，绕 Y 旋转过的树会朝各自的局部方向倒）。
                const float yaw = static_cast<float>(idx) * 2.39996323f;
                jpov::InstanceState st;
                st.transform.center = {cx + ox, cy, cz + oz};
                st.transform.up     = {0.0f, 1.0f, 0.0f};
                st.transform.front  = {std::sin(yaw), 0.0f, std::cos(yaw)};
                st.transform.scale  = s;
                st.blow_away_ratio  = blow_away_ratio_;
                instances.push_back(st);
            }
            // 一个 glTF 的每个 primitive 各发一条实例命令（同批必须共享 mesh_id）。
            for (const jpov::GltfPrimitive& prim : plant_.primitives) {
                cmds->DrawInstancedObject(prim.mesh_id, prim.material, instances,
                                          /*highlight*/ false, wind);
            }
        }

        if (show_panel_) {
            DrawPanels(input);
            // 半透明黑底版：在面板控件入队【之前】发出 → 先画，控件叠在其上。
            // （2D 指令按入队顺序绘制：先 DrawRect 再 ui_.Emit。）
            cmds->DrawRect(panel_bg_.pos, panel_bg_.size,
                           /*color*/ {0.0f, 0.0f, 0.0f, 0.5f});
            cmds->DrawRect(wind_bg_.pos, wind_bg_.size,
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

    // 两个面板（**一次** Ui::Begin/Emit，因为 Ui 每帧只允许一次 Begin）：
    //   · 右下角 = 天光（浊度/季节/天体仰角+方位/三色 ambient/SMAA）
    //   · 左下角 = 植被风（开关 + pivot 振幅/频率 + 平动湍流振幅/频率 + 风朝向 + 湍流随机性）
    void DrawPanels(const jpov::InputSnapshot& input) {
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
        const float kMargin = 16.0f;    // 距窗口边距
        const float inner_w = kPanelW - 2.0f * kPad;

        // ── 右下角：天光 ──
        {
            const int kRows = 7;
            const float inner_h = static_cast<float>(kRows) * kRowH
                                  + static_cast<float>(kRows - 1) * kSpace;
            const float panel_h = inner_h + 2.0f * kPad;
            const float panel_x = w - kMargin - kPanelW;
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

        // ── 左下角：植被风 ──
        {
            const int kRows = 9;
            const float inner_h = static_cast<float>(kRows) * kRowH
                                  + static_cast<float>(kRows - 1) * kSpace;
            const float panel_h = inner_h + 2.0f * kPad;
            const float panel_x = kMargin;                 // 贴左下角
            const float panel_y = h - kMargin - panel_h;
            wind_bg_ = jpov::UiRect{{panel_x, panel_y}, {kPanelW, panel_h}};
            const float cx = panel_x + kPad;
            const float cy = panel_y + kPad;
            auto row = [&](int i) {
                return jpov::UiRect{
                    {cx, cy + static_cast<float>(i) * (kRowH + kSpace)},
                    {inner_w, kRowH}};
            };
            ui_.SliderFloat("风朝向 °", &wind_azimuth_deg_, row(0), 0.0f, 360.0f, 0);
            ui_.SliderFloat("实例数 (1..9)", &instance_count_ui_, row(1), 1.0f,
                            static_cast<float>(kGridN * kGridN), 0);
            ui_.SliderFloat("pivot 振幅", &wind_amplitude_, row(2), 0.0f, 1.5f, 2);
            ui_.SliderFloat("pivot 频率 Hz", &wind_frequency_, row(3), 0.05f, 2.0f, 2);
            ui_.SliderFloat("平动湍流 幅度", &wind_turb_amp_, row(4), 0.0f, 0.6f, 3);
            ui_.SliderFloat("平动湍流 频率 Hz", &wind_turb_freq_, row(5), 0.0f, 8.0f, 2);
            ui_.SliderFloat("吹倒比例", &blow_away_ratio_, row(6), 0.0f, 0.5f, 3);
            ui_.SliderFloat("吹倒指数 n (L^n)", &blow_away_exponent_, row(7),
                            0.5f, 3.0f, 2);
            ui_.SliderFloat("湍流随机性 [0,1]", &wind_gust_str_, row(8), 0.0f, 1.0f, 2);
        }
    }

    bool show_panel_ = true;
    bool smaa_on_ = true;            // ★ 默认开（本查看器定位“观察 + 出图”）
    float smaa_threshold_ = 0.1f;    // SMAA luma 边缘阈值（越小越敏感）
    float base_scale_ = 1.0f;        // 资产→世界 的归一缩放（把 Y 高映射到 kPlantTargetHeight）
    float aim_y_ = 0.0f;             // 相机注视点高度 = 植物竖直中点（世界坐标，米）
    jpov::Ui ui_;
    jpov::UiRect panel_bg_{};        // 右下角天光面板底板（本帧 DrawPanels 填）
    jpov::UiRect wind_bg_{};         // 左下角风面板底板（本帧 DrawPanels 填）

    static constexpr float kSliderFontSize = 15.0f;
};

}  // namespace jpov_vegetation

#endif  // JPOV_DEMO_VEGETATION_VIEWER_APP_H_
