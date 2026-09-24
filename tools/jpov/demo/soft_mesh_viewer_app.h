// JPOV 软体仿真查看器 — 渲染核心 App
//
// 与 jpov_model_viewer 的 ViewerApp 是**姊妹关系**（同款渲染核心骨架：场景静态
// 资源 + 交互面板宿主 + 唯一 OneIteration 渲染体），但职责聚焦：**显示一个被软体
// 仿真器驱动的网格**。
//
// 需求（2026-09-24 Danis 定稿）：
//   - 仿 model_viewer，但滑条**只保留**「地面高度」与「视角」两个；
//     model_viewer 里的 太阳仰角 / 浊度 / 季节 R / 模型缩放 全部删掉，光照固定正午。
//   - 第一阶段只验证**静态展示模型**的能力（动力学尚未接上，Step 为恒等桩）。
//
// 「视角调节」= 直接鼠标操作（右键 drag 转、滚轮 zoom），与 model_viewer 完全同款
// （走 view_config.h::ApplyInput）；它不需要滑条，故面板里只有地面高度一个滑条。
//
// 与 model_viewer 的关键差异（务必注意，别照抄错）：
//   1. 进 scene 的是 **注册进 MeshManager 的 mesh handle**，不是 GltfObject。
//      因为仿真器产出的是 MeshData（顶点在 CPU 侧被物理改），必须每帧 UpdateMesh
//      把新顶点推上 GPU；GltfObject 的几何进 GPU 后就固化了，塞不进动态形变。
//   2. 材质沿用 glTF 资产的第一份材质（保留贴图/颜色），几何用 simulation mesh。
//   3. 面板：只剩「地面高度」滑条 + 一行只读的仿真状态（时间/步数/顶点数）。

#ifndef JPOV_DEMO_SOFT_MESH_VIEWER_APP_H_
#define JPOV_DEMO_SOFT_MESH_VIEWER_APP_H_

#include <cstdarg>
#include <cstdio>
#include <string>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/demo/soft_mesh_sim.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/interface/ui.h"

namespace jpov_soft_viewer {

// 复用姊妹查看器（jpov_model_viewer）的视角/光照/地面工具：ViewConfig、ApplyInput、
// MakeNoonLighting、MakeGroundQuad、GroundMaterial 全在 jpov_viewer 命名空间里，
// 都是纯函数（不绑定任何 App 状态），故直接引名使用——不重复实现，保证与模型
// 查看器所见一致（zero 分叉）。
using jpov_viewer::ApplyInput;
using jpov_viewer::GroundMaterial;
using jpov_viewer::MakeGroundQuad;
using jpov_viewer::MakeNoonLighting;
using jpov_viewer::NoonLighting;
using jpov_viewer::ViewConfig;

// 渲染/窗口分辨率（与姊妹查看器一致：1280×720，不可 resize）。单点定义。
inline constexpr int kViewerWidth  = 1280;
inline constexpr int kViewerHeight = 720;

// 交互帧率（查看器刷新率；仿真步长是 soft_mesh_sim.h::kDefaultDt，二者独立）。
inline constexpr float kViewerFps = 60.0f;

// UI 文本默认字体 = CJK（面板标签显中文）。
inline constexpr const char* kViewerFontAlias = jpov::kFontBuiltinCJK;

class SoftMeshViewerApp : public JPOV {
public:
    using JPOV::JPOV;

    // ── 场景状态（main 在 Init() 后一次性装配）──
    uint32_t mesh_id_ = 0;             // 仿真网格在 MeshManager 里的句柄（每帧可 UpdateMesh）
    jpov::PBRMaterial mesh_mat_;       // 沿用 glTF 资产的材质（贴图/颜色），几何来自仿真器
    uint32_t ground_mesh_ = 0;         // 300×300 地面 quad 的 GPU handle
    jpov::PBRMaterial ground_mat_;     // 高粗糙灰色地面材质

    // ── 仿真器（被查看的对象）。查看器只驱动它、读它，不碰它的内部。──
    jpov_soft::SoftMeshSimulator sim_;

    // ── 当前视角 view_：交互（右键 drag/滚轮实时改）与 headless 出图共用的单一事实源。
    ViewConfig view_;

    // ── UI 状态 ──
    float ground_y_ = -3.0f;           // 地面高度 [-3,+3]（需求保留的滑条）
    bool  show_panel_ = true;          // 交互绘制面板；headless 出图不画
    bool  running_ = false;            // 是否在推进仿真（默认暂停：先看静态模型）

    // 装配真实字体文本测量回调（UI 内部用），Init() 后调用一次。
    void InstallTextMeasure() {
        ui_.SetTextMeasure(&SoftMeshViewerApp::ViewerTextWidth, this);
    }

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

        // ── 仿真推进：把「当前 mesh 经 1/60 s 动力学」这件事交给仿真器。──
        // 默认暂停（首帧先给人看静态模型）；按面板按钮开始后逐帧推进。
        if (running_) {
            sim_.Step(jpov_soft::SoftMeshSimulator::kDefaultDt);
            // 顶点在 CPU 侧被物理改过 → 必须把新几何推上 GPU。
            // （M0 恒等桩下顶点其实没变，这条 UpdateMesh 仍照走：
            //   接口先接通，动力学一上线就自动生效，无需改查看器。）
            UpdateMesh(mesh_id_, sim_.mesh());
        }

        // ── 相机：由 view_ 推导 ──
        cmds->camera.position = view_.Position();
        cmds->camera.target   = ViewConfig::Target();  // (0,0,0)
        cmds->camera.up       = {0.0f, 1.0f, 0.0f};
        cmds->camera.fov      = 60.0f;
        cmds->camera.near     = 0.05f;
        cmds->camera.far      = 1000.0f;

        // ── 光照：固定正午（需求：光照滑条已删，不给用户调）──
        const NoonLighting light = MakeNoonLighting();
        cmds->sky = light.sky;
        cmds->sun = light.sun;
        cmds->ambient = light.ambient;
        cmds->tone_mapping = true;

        // ── 场景：地面 + 仿真网格。地面高度变化时原地重建 quad。──
        if (ground_y_ != ground_y_prev_) {
            UpdateMesh(ground_mesh_, MakeGroundQuad(ground_y_));
            ground_y_prev_ = ground_y_;
        }
        cmds->DrawObject3D(ground_mesh_, ground_mat_,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f});
        // 仿真网格以恒等摆放画在原点（顶点已是世界坐标：物理在世界系里积分）。
        cmds->DrawObject3D(mesh_id_, mesh_mat_,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f});

        // ── 面板（仅交互窗口；headless 是纯 3D 截图）──
        if (show_panel_) {
            DrawPanel(input);
            ui_.End();
            ui_.Emit(cmds);
        }
    }

private:
    static float ViewerTextWidth(const char* text, float font_size,
                                 const char* /*font_alias*/, void* userdata) {
        SoftMeshViewerApp* app = static_cast<SoftMeshViewerApp*>(userdata);
        return app->MeasureTextWidth(/*alias=*/std::string(),
                                     /*text=*/text ? text : "", font_size);
    }

    // 面板：地面高度滑条 + 仿真状态只读行（需求：只保留地面高度与视角）。
    void DrawPanel(const jpov::InputSnapshot& input) {
        const float w = static_cast<float>(kViewerWidth);
        const float h = static_cast<float>(kViewerHeight);
        jpov::UiTheme theme = jpov::UiTheme::Default(kSliderFontSize);
        theme.font_alias = kViewerFontAlias;
        ui_.Begin(input, theme, w, h, 1000.0f / kViewerFps);

        const float kRowH    = 30.0f;
        const float kSpacing = 12.0f;
        const float kBottom  = 20.0f;
        const float kSliderW = 0.5f * w;
        const float left     = (w - kSliderW) * 0.5f;

        // 地面高度（米）：[-3,+3]，实时看物体落地面/阴影。
        const float top = h - kBottom - kRowH;
        ui_.SliderFloat("地面高度 y", &ground_y_,
                        jpov::UiRect{{left, top}, {kSliderW, kRowH}},
                        -3.0f, 3.0f, /*decimal_places*/2);

        // 仿真状态（只读）：时间 / 步数 / 顶点-三角形。用 Label 直显，
        // 便于一眼判断"仿真在走没有"（M0 恒等桩下位置不变、时间照涨）。
        ui_.Text(fmt_("仿真 t=%.2fs  步=%zu  顶点=%zu  三角形=%zu",
                      sim_.time(), sim_.step_count(),
                      sim_.vertex_count(), sim_.triangle_count()).c_str(),
                 jpov::UiRect{{left, top - (kRowH + kSpacing)}, {kSliderW, kRowH}});
        // 操作提示（只读）。
        ui_.Text("右键 drag 转视角 · 滚轮 zoom",
                 jpov::UiRect{{left, top - 2.0f * (kRowH + kSpacing)},
                              {kSliderW, kRowH}});
    }

    // 极简 snprintf 包装（面板只读文本用；避免在头里引入 printf 变参手写）。
    static std::string fmt_(const char* f, ...) {
        char buf[256];
        va_list args;
        va_start(args, f);
        vsnprintf(buf, sizeof(buf), f, args);
        va_end(args);
        return std::string(buf);
    }

    float ground_y_prev_ = -3.0f;   // 上一帧地面高度（检测变化才 UpdateMesh）
    jpov::Ui ui_;                   // 跨帧持有（滑条拖动态内部记忆）

    static constexpr float kSliderFontSize = 16.0f;
};

}  // namespace jpov_soft_viewer

#endif  // JPOV_DEMO_SOFT_MESH_VIEWER_APP_H_
