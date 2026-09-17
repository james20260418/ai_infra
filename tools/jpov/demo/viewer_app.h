// JPOV 模型查看器 — 渲染核心 App（场景渲染体 + 交互 UI 宿主）
//
// 架构治理动机（重构，见 docs/jpov_model_viewer_arch.md）：早期把「交互宿主」和
// 「无窗口拍摄模式」塞进同一个 class 的 public 面（view_/sliders/ground… 全曝光），
// 拍摄模式靠注释“别走交互输入”维持纪律——这正是 SOUL 里最忌讳的 demo 分叉温床。
//
// 本次重构把职责理清：
//   - 本类 = 视图器唯一的【渲染核心 + 交互面板宿主】：持有场景静态资源、光照
//     滑条状态、当前视角 view_，并提供唯一的 OneIteration 渲染体。
//   - 别再把交互/拍摄“身份”塞进本类：交互输入、UI 面板是否绘制都由本类根据
//     show_panel_ 自洽决定；拍摄模式只是【外部改 view_ 后调 RenderSingleFrame 逐帧
//     出图】，同样走这同一条 OneIteration（zero 分叉，架构 doc 核心承诺）。
//
// 文件系统/ffmpeg/输出路径等纯工具在 viewer_output.h，拍摄模式 driver 在
// viewer_capture.h；本头只负责“场景 + 渲染”，不含拍摄编排。

#ifndef JPOV_DEMO_VIEWER_APP_H_
#define JPOV_DEMO_VIEWER_APP_H_

#include <string>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/interface/ui.h"

namespace jpov_viewer {

// 渲染/窗口分辨率（需求定稿：1280×720，不可 resize）。交互窗口、拍摄出图
// （four_views/round_video）、面板像素布局三处共用——单点定义，杜绝各处硬编码
// 分叉（历史形态曾散落在 main / RunFourViews / RunRoundVideo 三处）。
inline constexpr int kViewerWidth  = 1280;
inline constexpr int kViewerHeight = 720;

// 交互帧率（需求定的窗口刷新；拍摄模式帧率是视频帧率 --fps，与此无关）。
inline constexpr float kViewerFps = 60.0f;

// UI 文本默认字体 = CJK（滑条标签显中文）。拉丁字母回退由渲染层自动处理。
// JPOV 不提供隐式默认字体，cfg.fonts 显式声明；UiTheme::font_alias 用 CJK。
inline constexpr const char* kViewerFontAlias = jpov::kFontBuiltinCJK;

// 视图器渲染核心 App。公有面收敛到“渲染/出图”需要的极小区，交互细节私有。
// JPOV 基类位于全局命名空间（见 include/jpov/jpov.h）；其余类型（Vec3f/光照/
// InputSnapshot/… ）在 namespace jpov。故基类用不带前缀的 JPOV，场景类型带 jpov::。
class ViewerApp : public JPOV {
public:
    using JPOV::JPOV;

    // ── 场景状态（main 在 Init() 后一次性装配；重载时不重复构造/上传）──
    jpov::GltfObject gltf_;          // 被查看的模型
    uint32_t ground_mesh_ = 0;       // 300×300 地面 quad 的 GPU handle
    jpov::PBRMaterial ground_mat_;   // 高粗糙灰色地面材质

    // ── 当前视角 view_：交互（右键 drag/滚轮实时改）与 headless 拍摄（外部
    // 赋固定角/逐帧扫角）共用的“单一事实源”。拍摄模式不改 OneIteration，只
    // 在帧间改 view_（见 viewer_capture.h）——保证交互所见即拍摄所得，zero 分叉。
    ViewConfig view_;                // 见 view_config.h（y-up 球面角相机）

    // ── 光照滑条状态（跨帧持有；交互面板 UI_ 在下方）。色温/强度全由 sky 推导。
    float elev_deg_  = 90.0f;        // 太阳仰角（度 [0,90]）
    float turbidity_ = 2.0f;         // 大气浊度 [2,8]
    float season_r_  = 1.0f;         // 季节 R 色温乘子 [0.5,2.0]
    float ground_y_  = -3.0f;        // 地面高度 [-3,+3]
    float model_scale_ = 1.0f;       // 模型整体缩放 [0.1,20]

    // 装配交互程序（可选）：注入真实字体文本测量回调（供 UI 内部用）。
    // 仅在 main 建 app 后、Init() 后调用一次；拍摄 headless 模式同样需要，
    // 因为 UI 需真实字体宽——但拍摄模式不画面板，装了也无副作用，故统一装上。
    void InstallTextMeasure() {
        ui_.SetTextMeasure(&ViewerApp::ViewerTextWidth, this);
    }

    // 渲染/交互是否绘制底部光照面板。
    //   true  = 交互窗口模式（OneIteration 末尾画 DrawLightPanel + Emit）
    //   false = headless 拍摄（four_views/round_video）：截图即纯 3D 场景，
    //           不画 UI 面板（与历史行为一致，架构 doc §4）。
    void SetShowPanel(bool show) { show_panel_ = show; }
    bool show_panel() const { return show_panel_; }

    // ⭐ 唯一的渲染体：交互（Run 循环逐帧）与 headless 拍摄（RunOnce 逐帧）
    // 共用。拍摄模式不改本函数：只在【不同帧/不同视角】之间改 view_（见
    // viewer_capture.h 的 RunCapture），渲染逻辑零分叉。
    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;

        // 渲染分辨率 = 窗口分辨率 1280×720（单点定义，见文件顶部常量）。
        // 注意与传给 RunOnce 的 winfo 解耦：RenderOnce 的截图尺寸由 winfo 决定，
        // 这里的一帧是渲染到 fbo 的分辨率；两者在交互/拍摄下恒为 kViewerWidth×Height。
        cmds->camera.fbo_3d_width_  = kViewerWidth;
        cmds->camera.fbo_3d_height_ = kViewerHeight;

        // 交互输入 → 更新视角（仅交互时响应 input；拍摄模式的相机由外部赋 view_，
        // 传空 input → ApplyInput 无副作用，此处仍走以保证语义单一）。
        // view_ 同时是交互结果与拍摄“当前帧相机”的单一事实源。
        if (show_panel_) {  // 仅可见窗口交互才消费输入；headless 无用户不可乱改相机
            float dx = 0.0f, dy = 0.0f, scroll = 0.0f;
            if (input.right.IsDrag()) {
                dx = input.mouse_dx;
                dy = input.mouse_dy;
            }
            if (input.scroll_delta != 0.0f) scroll = input.scroll_delta;
            ApplyInput(&view_, dx, dy, scroll,
                       static_cast<int>(winfo.width),
                       static_cast<int>(winfo.height));
        }

        // ── 相机：由 view_ 推导 ──
        cmds->camera.position = view_.Position();
        cmds->camera.target   = ViewConfig::Target();  // (0,0,0)
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 60.0f;
        cmds->camera.near     = 0.05f;
        cmds->camera.far      = 1000.0f;  // 场景 R 最大 300，far 足够

        // ── 光照：由滑条状态经 sky 推导（含 Turb*Loss/季节色温）──
        const NoonLighting light = MakeLighting(elev_deg_, turbidity_, season_r_);
        cmds->sky = light.sky;
        cmds->sun = light.sun;
        cmds->ambient = light.ambient;
        cmds->tone_mapping = true;

        // ── 场景：地面 + 被加载的 glTF。地面高度变化时原地重建 quad。──
        if (ground_y_ != ground_y_prev_) {
            UpdateMesh(ground_mesh_, MakeGroundQuad(ground_y_));
            ground_y_prev_ = ground_y_;
        }
        cmds->DrawObject3D(ground_mesh_, ground_mat_,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f});
        cmds->DrawGltfObject(gltf_,
                             /*center*/ {0.0f, 0.0f, 0.0f},
                             /*up*/     {0.0f, 1.0f, 0.0f},
                             /*front*/  {0.0f, 0.0f, 1.0f},
                             /*scale*/ model_scale_, /*highlight*/ false,
                             /*picking_id*/ 0);

        // ── 光照调节面板（仅交互窗口绘制；headless 拍摄是纯 3D 截图）──
        if (show_panel_) {
            DrawLightPanel(input);
            ui_.End();
            ui_.Emit(cmds);
        }
    }

private:
    // 把 Ui 的文本测量回调接到本 App 的 JPOV::MeasureTextWidth（真实字体进宽），
    // 使滑条数值文本/布局用真实字体宽度（UI 内部用行高/居中，不依赖此宽度，但
    // 保持与 jpov_ui_demo 同款接线，避免后续 InputText 类控件宽度分叉）。
    // alias 空串 = 首个注册字体，与 demo 第一个注册字体一致。
    static float ViewerTextWidth(const char* text, float font_size,
                                 const char* /*font_alias*/, void* userdata) {
        ViewerApp* app = static_cast<ViewerApp*>(userdata);
        return app->MeasureTextWidth(/*alias=*/std::string(),
                                     /*text=*/text ? text : "", font_size);
    }

    // 光照调节面板布局与绘制（即时模式；见 view_config / ui 用法）。
    void DrawLightPanel(const jpov::InputSnapshot& input) {
        const float w = static_cast<float>(kViewerWidth);
        const float h = static_cast<float>(kViewerHeight);
        jpov::UiTheme theme = jpov::UiTheme::Default(kSliderFontSize);
        theme.font_alias = kViewerFontAlias;
        const float frame_dt_ms = 1000.0f / kViewerFps;
        ui_.Begin(input, theme, w, h, frame_dt_ms);

        const float kSliderWidth = 0.5f * w;   // 半屏宽
        const float kRowH    = 30.0f;
        const float kSpacing = 12.0f;
        const float kBottom  = 20.0f;
        const float left     = (w - kSliderWidth) * 0.5f;
        const float top      = h - kBottom - (5.0f * kRowH + 4.0f * kSpacing);

        // 太阳仰角直接用度（0~90 覆盖日出→正午全部标定工况），0 位小数。
        ui_.SliderFloat("太阳仰角 °", &elev_deg_,
                        jpov::UiRect{{left, top}, {kSliderWidth, kRowH}},
                        0.0f, 90.0f, /*decimal_places*/0);
        // 大气浊度：[2,8]，同时看霾化 + Turb*Loss 强度衰减。
        ui_.SliderFloat("浊度 turb", &turbidity_,
                        jpov::UiRect{{left, top + (kRowH + kSpacing)},
                                     {kSliderWidth, kRowH}},
                        2.0f, 8.0f, /*decimal_places*/1);
        // 季节 R 色温乘子：[0.5,2.0]，归一化只偏红/青不改亮度。
        ui_.SliderFloat("季节 R", &season_r_,
                        jpov::UiRect{{left, top + 2.0f * (kRowH + kSpacing)},
                                     {kSliderWidth, kRowH}},
                        0.5f, 2.0f, /*decimal_places*/2);
        // 地面高度（米）：[-3,+3]，实时看物体落地面/阴影。
        ui_.SliderFloat("地面高度 y", &ground_y_,
                        jpov::UiRect{{left, top + 3.0f * (kRowH + kSpacing)},
                                     {kSliderWidth, kRowH}},
                        -3.0f, 3.0f, /*decimal_places*/2);
        // 模型整体缩放（先缩放再旋转平移；验证小物体阴影/轮廓是否尺寸所致）。
        ui_.SliderFloat("模型缩放", &model_scale_,
                        jpov::UiRect{{left, top + 4.0f * (kRowH + kSpacing)},
                                     {kSliderWidth, kRowH}},
                        0.1f, 20.0f, /*decimal_places*/1);
    }

    bool show_panel_ = true;              // 是否画交互光照面板（headless=false）
    float ground_y_prev_ = -3.0f;         // 上一帧地面高度（检测变化才 UpdateMesh）
    jpov::Ui ui_;                         // 跨帧持有（滑条拖动态内部记忆）

    // 滑条字号（px），仅供光照面板自身布局使用。
    static constexpr float kSliderFontSize = 16.0f;
};

}  // namespace jpov_viewer

#endif  // JPOV_DEMO_VIEWER_APP_H_
