// JPOV 穿衣工具 — 渲染核心 App（可视化框架）
//
// 与 jpov_soft_mesh_viewer 是**姊妹工具**（同款渲染核心骨架：场景静态资源 + 交互
// 面板宿主 + 唯一 OneIteration 渲染体），但职责聚焦：**把「一件衣服」摆到「一个人体
// reference」旁边显示出来**，为后续「衣服贴合人体」的穿衣管线提供可视化底座。
//
// 需求（2026-09-27 Danis 定稿）：
//   - 框架负责两件事：加载人体 reference（--body_reference_path）、加载衣服模型
//     （--cloth_path），并把二者显示在同一场景里。
//   - 本阶段（M0）**只做「加载 + 显示」**：不做人体/衣服的对齐、不做穿衣物理。
//     「先把衣服大致对准人体」是后续穿衣管线的事（对齐不属于本框架的职责，
//     同 soft_mesh_simulator DESIGN.md §1.1 划下的边界）。
//
// 与 soft_mesh_viewer 的关键差异：本工具的两个模型是**静态资产**（M0 不变形），
// 故直接用 LoadGltf → GltfObject 画（cmds->DrawGltfObject），不需要像软体仿真那样
// 每帧 UpdateMesh 把 CPU 侧形变推上 GPU。
//
// 命名空间 jpov::clothing、文件夹 tools/jpov/clothing/ 均为**独立**的一整套，
// 与 soft_mesh_simulator 互不牵连（Danis 要求「单独文件夹和命名空间」）。

#ifndef JPOV_CLOTHING_CLOTHING_TOOL_APP_H_
#define JPOV_CLOTHING_CLOTHING_TOOL_APP_H_

#include <cstdarg>
#include <cstdio>
#include <string>

#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/ui.h"

namespace jpov {
namespace clothing {

// 视角 / 光照 / 地面工具复用姊妹查看器（jpov_model_viewer / soft_mesh_viewer）的
// 纯函数：ViewConfig、ApplyInput、MakeNoonLighting、MakeGroundQuad、GroundMaterial
// 都在 jpov_viewer 命名空间里，不重复实现，保证各查看器的视角手感与光照观感一致。
using jpov_viewer::ApplyInput;
using jpov_viewer::DefaultView;
using jpov_viewer::GroundMaterial;
using jpov_viewer::MakeGroundQuad;
using jpov_viewer::MakeNoonLighting;
using jpov_viewer::NoonLighting;
using jpov_viewer::ViewConfig;

// 渲染/窗口分辨率（与姊妹查看器一致：1280×720，不可 resize）。单点定义。
inline constexpr int kViewerWidth  = 1280;
inline constexpr int kViewerHeight = 720;

// 交互帧率（查看器刷新率，Hz）。
inline constexpr float kViewerFps = 60.0f;

// UI 文本默认字体 = CJK（面板标签显中文）。
inline constexpr const char* kViewerFontAlias = jpov::kFontBuiltinCJK;

// 未显式指定 --body_reference_path 时的演示用人体 reference。
// 项目内自带的 Mixamo 男性人体资产（rest/T-pose），便于快速跑通。
inline constexpr const char* kDefaultBodyReferencePath =
    "tools/jpov/test/object3d/mixamo_male/mixamo_male.glb";

// 穿衣工具渲染核心 App。
//
// 场景 = 一件人体 reference + 一件衣服 + 灰地面；光固定正午晴天。
// 视角靠鼠标操作（右键 drag 转、滚轮 zoom），与姊妹查看器完全同款。
class ClothingToolApp : public JPOV {
public:
    using JPOV::JPOV;

    // ── 场景状态（main 在 Init() 后一次性装配）──
    // 人体 reference 与衣服各自一个 glTF 资产（M0 静态，直接整份画）。
    jpov::GltfObject body_;        // 人体 reference（--body_reference_path）
    jpov::GltfObject cloth_;       // 衣服模型（--cloth_path）
    std::string body_path_;        // 人体 reference 来源路径（面板显示）
    std::string cloth_path_;       // 衣服模型来源路径（面板显示）

    uint32_t ground_mesh_ = 0;           // 300×300 地面 quad 的 GPU handle
    jpov::PBRMaterial ground_mat_;       // 高粗糙灰色地面材质

    // ── 当前视角：交互（右键 drag / 滚轮实时改）与 headless 出图共用的单一事实源。──
    ViewConfig view_;

    // ── 显示开关（面板勾选）──
    bool show_body_  = true;   // 画人体 reference
    bool show_cloth_ = true;   // 画衣服模型

    // 地面高度（米）滑条值，[-3, +3]。
    float ground_y_ = -3.0f;

    // 装配真实字体文本测量回调（UI 内部用），Init() 后调用一次。
    void InstallTextMeasure() {
        ui_.SetTextMeasure(&ClothingToolApp::ViewerTextWidth, this);
    }

    // 渲染/出图时是否绘制面板（交互窗口 = true；headless 纯 3D 截图 = false）。
    void SetShowPanel(bool show) { show_panel_ = show; }
    bool show_panel() const { return show_panel_; }

    // ⭐ 唯一的渲染体：交互循环与 headless 出图共用（zero 分叉）。
    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        cmds->camera.fbo_3d_width_  = kViewerWidth;
        cmds->camera.fbo_3d_height_ = kViewerHeight;

        // 交互输入 → 视角（仅可见窗口消费输入；headless 的相机由外部赋 view_）。
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
        cmds->camera.far      = 1000.0f;

        // ── 光照：固定正午（本工具不暴露光照滑条，光照是标定值不是被调对象）──
        const NoonLighting light = MakeNoonLighting();
        cmds->sky     = light.sky;
        cmds->sun     = light.sun;
        cmds->ambient = light.ambient;
        cmds->tone_mapping = true;

        // ── 场景：地面 + 人体 reference + 衣服。──
        // 地面高度变化时重建 quad（滑条实时调，看模型落地面）。
        if (ground_y_ != ground_y_last_built_) {
            UpdateMesh(ground_mesh_, MakeGroundQuad(ground_y_));
            ground_y_last_built_ = ground_y_;
        }
        cmds->DrawObject3D(ground_mesh_, ground_mat_,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f});

        // 人体 reference 与衣服都按恒等摆放画在原点（M0 不做对齐；对齐是后续
        // 「把衣服大致对准人体」那一步的事，见文件头说明）。
        if (show_body_) {
            cmds->DrawGltfObject(body_,
                                 /*center*/ {0.0f, 0.0f, 0.0f},
                                 /*up*/     {0.0f, 1.0f, 0.0f},
                                 /*front*/  {0.0f, 0.0f, 1.0f});
        }
        if (show_cloth_) {
            cmds->DrawGltfObject(cloth_,
                                 /*center*/ {0.0f, 0.0f, 0.0f},
                                 /*up*/     {0.0f, 1.0f, 0.0f},
                                 /*front*/  {0.0f, 0.0f, 1.0f});
        }

        // ── 面板（仅交互窗口；headless 是纯 3D 截图）──
        if (show_panel_) {
            DrawPanel(input);
            ui_.End();
            ui_.Emit(cmds);
        }
    }

private:
    // 文本测量回调：转发到 JPOV::MeasureTextWidth（真实字体进宽）。
    static float ViewerTextWidth(const char* text, float font_size,
                                 const char* /*font_alias*/, void* userdata) {
        ClothingToolApp* app = static_cast<ClothingToolApp*>(userdata);
        return app->MeasureTextWidth(/*alias=*/std::string(),
                                     /*text=*/text ? text : "", font_size);
    }

    // 面板（自上而下）：只读信息行 + 勾选 + 地面高度滑条。
    // 布局用一个“行游标”自下而上堆叠：从底部倒数第一行开始，每画一行向上推一个 step。
    void DrawPanel(const jpov::InputSnapshot& input) {
        const float w = static_cast<float>(kViewerWidth);
        const float h = static_cast<float>(kViewerHeight);
        jpov::UiTheme theme = jpov::UiTheme::Default(kSliderFontSize);
        theme.font_alias = kViewerFontAlias;
        ui_.Begin(input, theme, w, h, 1000.0f / kViewerFps);

        const float kRowH    = 28.0f;
        const float kSpacing = 6.0f;
        const float kBottom  = 16.0f;
        const float kSliderW = 0.5f * w;
        const float left     = (w - kSliderW) * 0.5f;
        const float step     = kRowH + kSpacing;

        // 行游标：row_y 指向当前要画的那一行的 y（从底部向上推进）。
        float row_y = h - kBottom - kRowH;

        // 行 0（最底）：地面高度（米）：[-3,+3]，实时看模型落地面/阴影。
        ui_.SliderFloat("地面高度 y", &ground_y_,
                        jpov::UiRect{{left, row_y}, {kSliderW, kRowH}},
                        -3.0f, 3.0f, /*decimal_places*/2);
        row_y -= step;

        // 行 1：显示开关（人体参考 / 衣服 / —— 各自独立，便于单看其一）。
        const float kCheckW = kSliderW / 2.0f;
        ui_.Checkbox("显示人体 reference", &show_body_,
                     jpov::UiRect{{left, row_y}, {kCheckW, kRowH}});
        ui_.Checkbox("显示衣服", &show_cloth_,
                     jpov::UiRect{{left + kCheckW, row_y}, {kCheckW, kRowH}});
        row_y -= step;

        // 行 2：人体 reference 来源 + primitive 数（只读）。
        const std::string body_line = Format(
            "人体 reference：%s（%zu primitives，%s）", body_path_.c_str(),
            body_.size(), show_body_ ? "显示" : "隐藏");
        ui_.Text(body_line.c_str(), jpov::UiRect{{left, row_y}, {kSliderW, kRowH}});
        row_y -= step;

        // 行 3：衣服来源 + primitive 数（只读）。
        const std::string cloth_line = Format(
            "衣服：%s（%zu primitives，%s）", cloth_path_.c_str(),
            cloth_.size(), show_cloth_ ? "显示" : "隐藏");
        ui_.Text(cloth_line.c_str(), jpov::UiRect{{left, row_y}, {kSliderW, kRowH}});
        row_y -= step;

        // 行 4（最上）：视角操作提示（只读）。M0 只加载显示、不做对齐，故无对齐控件。
        ui_.Text("右键 drag 转视角 · 滚轮 zoom · M0 仅加载显示（对齐/穿衣为后续）",
                 jpov::UiRect{{left, row_y}, {kSliderW, kRowH}});
    }

    // 极简 snprintf 包装（面板只读文本用）。
    static std::string Format(const char* fmt, ...) {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        return std::string(buf);
    }

    bool show_panel_ = true;             // 是否绘制面板（headless 出图 = false）
    float ground_y_last_built_ = -3.0f;  // 上次建 quad 用的地面高度（变了才重建）
    jpov::Ui ui_;                        // 跨帧持有（滑条拖动态内部记忆）

    static constexpr float kSliderFontSize = 16.0f;
};

}  // namespace clothing
}  // namespace jpov

#endif  // JPOV_CLOTHING_CLOTHING_TOOL_APP_H_
