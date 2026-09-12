// JPOV 模型编辑器 — 渲染核心 App（PlacementApp）
//
// 与 viewer_app.h 的 ViewerApp 是**姊妹**关系：同一套场景（sunny day 光照 +
// 可调地面 + 相机右键环绕），但底部面板换成"模型放置"三组控件
// （缩放 / 平移 XYZ / 旋转 RX·RY），并额外消费【左键横向 drag】做旋转。
//
// 为什么另起一个 App 而不是给 ViewerApp 加开关（SOUL：不要 demo 分叉温床）：
//   - 两个工具的手感与面板语义本就不同（viewer 调"光照标定"，editor 调"模型
//     摆位"），塞进一个类的代价是面板布局 + 输入消费全绕 show_editor_ 分支；
//   - 渲染体 OneIteration 的关键部分（相机推导 / 光照推导 / 地面重建）本就可
//     按段落复制——真正必须共享的是【放置数学】（model_placement.h，两边共用）
//     和【场景构造】（view_config.h，两边共用），而非 App 外壳。
// 判据（Danis 定调）："一片代码不能单测，就不要为共享而共享"——放置数学已抽到
// model_placement.h 并被单测覆盖，本类只留"接线 + 布局"。
//
// 与查看器一致的既有约定（勿改）：
//   - 右键 drag 转视角、滚轮 zoom（ApplyInput，需求要求"依然服从相机右键拖动"）；
//   - 光照固定 sunny day 同款配置（MakeLighting，滑条不暴露光照）；
//   - 地面 0 ~ -3 米可调（MakeGroundQuad，需求"地面依然是0~-3米可调"）；
//   - 交互/出图共用同一条 OneIteration（zero 分叉）。

#ifndef JPOV_DEMO_EDITOR_APP_H_
#define JPOV_DEMO_EDITOR_APP_H_

#include <string>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/model_placement.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/interface/ui.h"

namespace jpov_viewer {

// 编辑器窗口/渲染分辨率（与查看器同规格；单点定义）。
inline constexpr int kEditorWidth  = 1280;
inline constexpr int kEditorHeight = 720;

// 交互帧率（同查看器）。
inline constexpr float kEditorFps = 60.0f;

// UI 文本字体别名 = CJK（标签显中文；拉丁字母由渲染层自动回退）。
// 与 viewer_app.h 的 kViewerFontAlias 同值，但**刻意不引用它**——editor 与
// viewer 是两个独立工具，不应因对方改名而连带编译失败（同值不同名，各自单点定义）。
inline constexpr const char* kEditorFontAlias = jpov::kFontBuiltinCJK;

// 编辑器光照固定为 sunny day 同款：太阳仰角 90°（天顶正午）、浊度 2、中性色温。
// 需求原文"光照固定为sunny day 同款配置"——不暴露滑条，常量集中于此，改动只有一处。
inline constexpr float kEditorSunElevDeg = 90.0f;
inline constexpr float kEditorTurbidity  = 2.0f;
inline constexpr float kEditorSeasonR    = 1.0f;

// 地面高度滑条范围（需求："地面依然是0~-3米可调"）。
inline constexpr float kEditorGroundMin = -3.0f;
inline constexpr float kEditorGroundMax = 0.0f;
inline constexpr float kEditorGroundDefault = -3.0f;

class EditorApp : public JPOV {
public:
    using JPOV::JPOV;

    // ── 场景状态（main 在 Init() 后一次性装配）──
    jpov::GltfObject gltf_;          // 被编辑的模型
    uint32_t ground_mesh_ = 0;       // 300×300 地面 quad 的 GPU handle
    jpov::PBRMaterial ground_mat_;   // 高粗糙灰色地面材质

    // ── 相机（右键环绕 + 滚轮 zoom；与查看器同一套 ViewConfig）──
    ViewConfig view_;

    // ── 模型放置状态（缩放/平移/旋转；唯一真相，见 model_placement.h）──
    ModelPlacement placement_;

    // ── 地面高度（[-3,0]，需求定档）──
    float ground_y_ = kEditorGroundDefault;

    // ── 左键旋转的按键边沿检测（跨帧；Ctrl 按下瞬间记一次，避免"先拖后按 Ctrl"
    // 中途切换旋转轴导致手心不一致）。true = 本帧左 drag 的"轴选择"已被冻结。──
    bool rotate_mode_latched_ = false;  // 冻结的轴：true=绕 Y，false=绕 X
    bool rotate_drag_prev_ = false;    // 上一帧左键是否处于 drag 态

    void InstallTextMeasure() {
        ui_.SetTextMeasure(&EditorApp::EditorTextWidth, this);
    }
    void SetShowPanel(bool show) { show_panel_ = show; }
    bool show_panel() const { return show_panel_; }

    // ⭐ 唯一渲染体（交互与 headless 出图共用）。拍摄模式不改本函数：
    // 只要在帧间改 view_ / placement_，然后 RunOnce 出图即可。
    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;

        cmds->camera.fbo_3d_width_  = kEditorWidth;
        cmds->camera.fbo_3d_height_ = kEditorHeight;

        if (show_panel_) {  // 仅可见窗口消费输入；headless 相机/放置由外部指定
            // 1) 右键 drag + 滚轮 → 相机（与查看器同款）。
            float dx = 0.0f, dy = 0.0f, scroll = 0.0f;
            if (input.right.IsDrag()) {
                dx = input.mouse_dx;
                dy = input.mouse_dy;
            }
            if (input.scroll_delta != 0.0f) scroll = input.scroll_delta;
            ApplyInput(&view_, dx, dy, scroll,
                       static_cast<int>(winfo.width),
                       static_cast<int>(winfo.height));

            // 2) 左键横向 drag → 模型旋转（纵向分量丢弃；Ctrl 选轴，按下时冻结）。
            UpdateRotateFromDrag(input);
        }

        // ── 相机：由 view_ 推导（目标点恒为原点，与查看器一致）──
        cmds->camera.position = view_.Position();
        cmds->camera.target   = ViewConfig::Target();
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 60.0f;
        cmds->camera.near     = 0.05f;
        cmds->camera.far      = 1000.0f;

        // ── 光照：固定 sunny day（不暴露滑条）──
        const NoonLighting light =
            MakeLighting(kEditorSunElevDeg, kEditorTurbidity, kEditorSeasonR);
        cmds->sky = light.sky;
        cmds->sun = light.sun;
        cmds->ambient = light.ambient;
        cmds->tone_mapping = true;

        // ── 场景：地面 + 被编辑的 glTF ──
        if (ground_y_ != ground_y_prev_) {
            UpdateMesh(ground_mesh_, MakeGroundQuad(ground_y_));
            ground_y_prev_ = ground_y_;
        }
        cmds->DrawObject3D(ground_mesh_, ground_mat_,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f},
                           /*scale*/  1.0f);

        // 模型：放置状态 → (center,up,front,scale) → 原样喂给既有绘制指令。
        // 本 PR 的**全部**编辑能力都落在这四行——后端渲染器零改动。
        const DrawPlacement dp = ToDrawParams(placement_);
        cmds->DrawGltfObject(gltf_, dp.center, dp.up, dp.front, dp.scale,
                             /*highlight*/ false, /*picking_id*/ 0);

        if (show_panel_) {
            DrawPlacementPanel(input);
            ui_.End();
            ui_.Emit(cmds);
        }
    }

private:
    // 文本测量接线（与查看器同款；UI 内部布局/居中不依赖，但保持一致性）。
    static float EditorTextWidth(const char* text, float font_size,
                                 const char* /*font_alias*/, void* userdata) {
        EditorApp* app = static_cast<EditorApp*>(userdata);
        return app->MeasureTextWidth(/*alias=*/std::string(),
                                     /*text=*/text ? text : "", font_size);
    }

    // 左键横向 drag → 旋转。细节：
    //   - 只在左键处于 Drag 态时消费；不用 click/hold（"横向拖动"语义）；
    //   - Ctrl 按下时绕世界 Y，否则绕世界 X；轴在【本次 drag 开始时】冻结
    //     （按下 Ctrl 的瞬间即定轴，drag 中途改 Ctrl 不换轴，避免手心打滑）；
    //   - 纵向分量丢弃（不参与任何计算）。
    void UpdateRotateFromDrag(const jpov::InputSnapshot& input) {
        const bool dragging = input.left.IsDrag();
        if (dragging && !rotate_drag_prev_) {
            // drag 起点：按此刻 Ctrl 状态冻结旋转轴。
            rotate_mode_latched_ =
                input.GetKey(jpov::KeyCode::LeftCtrl).IsHold() ||
                input.GetKey(jpov::KeyCode::RightCtrl).IsHold();
        }
        if (!dragging) {
            rotate_drag_prev_ = false;
            return;
        }
        rotate_drag_prev_ = true;
        ApplyRotateDrag(&placement_, input.mouse_dx, rotate_mode_latched_,
                        static_cast<int>(kEditorWidth));
    }

    // 模型放置面板：三组控件（缩放 / 平移 / 旋转）+ 地面高度。
    // 全部滑条数值由 placement_ / ground_y_ 外置持有。
    void DrawPlacementPanel(const jpov::InputSnapshot& input) {
        const float w = static_cast<float>(kEditorWidth);
        const float h = static_cast<float>(kEditorHeight);
        jpov::UiTheme theme = jpov::UiTheme::Default(kEditorFontSize);
        theme.font_alias = kEditorFontAlias;
        ui_.Begin(input, theme, w, h, 1000.0f / kEditorFps);

        const float kSliderWidth = 0.5f * w;   // 半屏宽（与查看器面板同款）
        const float kRowH    = 30.0f;
        const float kSpacing = 10.0f;
        const float kBottom  = 18.0f;
        const float rows     = 8.0f;           // 缩放1 + 平移3 + 旋转2 + 地面1 + 提示1
        const float left     = (w - kSliderWidth) * 0.5f;
        const float top      = h - kBottom - (rows * kRowH + (rows - 1.0f) * kSpacing);

        // 行盒辅助（避免手写 8 次 "top + i*(row+spacing)"）。
        auto row = [&](int i) {
            return jpov::UiRect{{left, top + static_cast<float>(i) * (kRowH + kSpacing)},
                                {kSliderWidth, kRowH}};
        };

        // ── 缩放 [0.1,10]，默认 1.0 ──
        ui_.SliderFloat("缩放", &placement_.scale, row(0),
                        ModelPlacement::kScaleMin, ModelPlacement::kScaleMax,
                        /*decimal_places*/2);

        // ── 平移 XYZ 各一条，范围 ±3 ──
        ui_.SliderFloat("平移 X", &placement_.tx, row(1),
                        ModelPlacement::kTransMin, ModelPlacement::kTransMax, 2);
        ui_.SliderFloat("平移 Y", &placement_.ty, row(2),
                        ModelPlacement::kTransMin, ModelPlacement::kTransMax, 2);
        ui_.SliderFloat("平移 Z", &placement_.tz, row(3),
                        ModelPlacement::kTransMin, ModelPlacement::kTransMax, 2);

        // ── 旋转（角度；左键横向 drag 的等价滑条，便于精确回读/复现）──
        ui_.SliderFloat("旋转 RX °", &placement_.rx_deg, row(4),
                        ModelPlacement::kRotMin, ModelPlacement::kRotMax, 1);
        ui_.SliderFloat("旋转 RY °", &placement_.ry_deg, row(5),
                        ModelPlacement::kRotMin, ModelPlacement::kRotMax, 1);

        // ── 地面高度 [-3, 0] ──
        ui_.SliderFloat("地面高度 y", &ground_y_, row(6),
                        kEditorGroundMin, kEditorGroundMax, 2);

        // ── 操作提示（无交互，纯文本）──
        ui_.Text("左键横拖=绕X旋转 / Ctrl+左键横拖=绕Y旋转 · 右键拖=视角 · 滚轮=缩放视角",
                 row(7));
    }

    bool show_panel_ = true;
    float ground_y_prev_ = kEditorGroundDefault;
    jpov::Ui ui_;

    static constexpr float kEditorFontSize = 16.0f;
};

}  // namespace jpov_viewer

#endif  // JPOV_DEMO_EDITOR_APP_H_
