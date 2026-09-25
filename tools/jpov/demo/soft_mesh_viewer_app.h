// JPOV 软体仿真查看器 — 渲染核心 App
//
// 与 jpov_model_viewer 的 ViewerApp 是**姊妹关系**（同款渲染核心骨架：场景静态
// 资源 + 交互面板宿主 + 唯一 OneIteration 渲染体），但职责聚焦：**显示一个被软体
// 仿真器驱动的网格**。
//
// 需求（2026-09-24 Danis 定稿）：
//   - 仿 model_viewer，但滑条**只保留**「地面高度」；
//     model_viewer 里的 太阳仰角 / 浊度 / 季节 R / 模型缩放 全部删掉，光照固定正午。
//   - 视角调节 = 直接鼠标操作（右键 drag 转、滚轮 zoom），与 model_viewer 完全同款
//     （走 view_config.h::ApplyInput）；它不需要滑条，故面板里只有一个滑条。
//   - M2 起动力学已接上（重力 + 速度衰减）；顶点间力场、地面/人体排斥尚待接入。
//
// 与 model_viewer 的关键差异（务必注意，别照抄错）：
//   1. 进 scene 的是 **注册进 MeshManager 的 mesh handle**，不是 GltfObject。
//      因为仿真器产出的是 MeshData（顶点在 CPU 侧被物理改），必须每帧 UpdateMesh
//      把新顶点推上 GPU；GltfObject 的几何进 GPU 后就固化了，塞不进动态形变。
//   2. 材质沿用 glTF 资产的第一份材质（保留贴图/颜色），几何用仿真网格。
//   3. 面板：只剩「地面高度」滑条 + 两行只读文本（仿真状态、视角操作提示）。

#ifndef JPOV_DEMO_SOFT_MESH_VIEWER_APP_H_
#define JPOV_DEMO_SOFT_MESH_VIEWER_APP_H_

#include <cstdarg>
#include <cstdio>
#include <string>

#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/soft_mesh_simulator/screen_projection.h"
#include "tools/jpov/soft_mesh_simulator/soft_mesh_simulator.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/interface/ui.h"

namespace jpov {
namespace soft_mesh_viewer {

// 视角 / 光照 / 地面工具来自姊妹查看器（jpov_model_viewer）：ViewConfig、ApplyInput、
// MakeNoonLighting、MakeGroundQuad、GroundMaterial 都在 jpov_viewer 命名空间里，
// 且都是纯函数（不绑定任何 App 状态）。直接引名使用——不重复实现，保证两查看器
// 的视角手感与光照观感完全一致（zero 分叉）。
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

// 交互帧率（查看器刷新率；仿真步长是 Simulator::kDefaultDt，二者独立）。
inline constexpr float kViewerFps = 60.0f;

// UI 文本默认字体 = CJK（面板标签显中文）。
inline constexpr const char* kViewerFontAlias = jpov::kFontBuiltinCJK;

// 软体仿真查看器 App。
class SoftMeshViewerApp : public JPOV {
public:
    using JPOV::JPOV;

    // ── 场景状态（main 在 Init() 后一次性装配）──
    uint32_t mesh_id_ = 0;              // 仿真网格在 MeshManager 里的句柄（每帧可 UpdateMesh）
    jpov::PBRMaterial mesh_mat_;        // 沿用 glTF 资产的材质（贴图/颜色），几何来自仿真器
    uint32_t ground_mesh_ = 0;          // 300×300 地面 quad 的 GPU handle
    jpov::PBRMaterial ground_mat_;      // 高粗糙灰色地面材质

    // ── 被查看的仿真器。查看器只驱动它、读它，不碰它的内部状态。──
    soft_mesh_simulator::Simulator sim_;

    // 原始（未加密）网格的 CPU 副本。仿真器只把它当输入，自己保存自己的副本；
    // 这里另存一份是为了「改 d 后重新 Init」——d 决定加密密度，改 d 必须重建
    // 仿真点集合，而重建的输入永远是这份原始网格（不是仿真器的输出，避免累计漂移）。
    jpov::MeshData original_mesh_;

    // ── 当前视角 view_：交互（右键 drag/滚轮实时改）与 headless 出图共用的单一事实源。
    ViewConfig view_;

    // ── M1 可视化开关（仿真点 + 地面栅格）──
    bool show_sim_points_ = true;   // 画仿真点（红=原始 / 蓝=虚拟）
    bool show_ground_grid_ = true;  // 画地面 1m 栅格（±5m，XZ 平面）

    // 仿真点像素半径（Danis 需求：2px，"就点个点"）。
    static constexpr float kSimPointRadiusPx = 2.0f;

    // d 滑条范围（米）。下限 5mm：高密度模型也能拆出成片虚拟点；
    // 上限 1m：覆盖「大 d ⇒ 几乎不加密」的对照情形。
    static constexpr float kBindDistanceMinM = 0.005f;
    static constexpr float kBindDistanceMaxM = 1.0f;

    // 地面栅格参数（Danis 需求：1m 格子、±5m、只画 XZ 平面）。
    static constexpr float kGridHalfExtentM = 5.0f;  // 每边 5m → 总 10m×10m
    static constexpr float kGridStepM = 1.0f;        // 1m 一格
    static constexpr float kGridLineHalfWidthM = 0.01f;  // 线半宽 1cm → 线宽 2cm

    // ── UI 状态 ──
    float ground_y_ = -3.0f;            // 地面高度 [-3,+3]（需求保留的滑条）
    bool  sim_running_ = false;         // 是否推进仿真（默认暂停：先看静态模型）

    // 关联距离 d（米）的滑条镜像值。拖它 → 与 sim_.bind_distance() 不一致时
    // 重新 Init（重建仿真点集合）。范围见 kBindDistanceMinM/MaxM。
    float bind_distance_ui_ = soft_mesh_simulator::Simulator::kDefaultBindDistance;

    // 重力加速度 g（m/s²）的滑条镜像值。拖它 → 直接 SetGravity 写到仿真器。
    // 范围 [kMinGravity, kMaxGravity]，默认 kDefaultGravity（9.8）。
    float gravity_ui_ = soft_mesh_simulator::Simulator::kDefaultGravity;

    // 装配真实字体文本测量回调（UI 内部用），Init() 后调用一次。
    void InstallTextMeasure() {
        ui_.SetTextMeasure(&SoftMeshViewerApp::ViewerTextWidth, this);
    }

    // 渲染/出图时是否绘制面板（交互窗口 = true；headless 纯 3D 截图 = false）。
    void SetShowPanel(bool show) { show_panel_ = show; }
    bool show_panel() const { return show_panel_; }

    // 设置地面高度（米）：同步视觉地面 quad 与仿真器的物理地面。
    // 两者必须同值，否则会看到模型“悬浮/陷入”。装配期（Init 后）与面板滑条
    // 变化时都走这里，单一入口。
    void SetGroundHeight(float y) {
        ground_y_ = y;
        sim_.SetGroundY(y);
        UpdateMesh(ground_mesh_, MakeGroundQuad(y));
        ground_y_last_built_ = y;
    }

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

        // ── 拖动 g 滑条 → 写到仿真器（不需重建，g 不改变仿真点集合）。──
        if (gravity_ui_ != sim_.gravity()) {
            sim_.SetGravity(gravity_ui_);
        }

        // ── 拖动 d 滑条 → 重建仿真点集合。──
        // d 只在 Init 时生效（决定长边加密密度 ⇒ 虚拟点数量），所以拖完必须
        // 重建。重建输入用 original_mesh_（原始网格，非仿真器当前输出）：
        // 保证 d 来回拖动后回到同一状态，不累计任何历史。
        if (bind_distance_ui_ != sim_.bind_distance()) {
            ReinitSimulation();
        }

        // ── 仿真推进：把「当前 mesh 经 1/60 s 动力学」这件事交给仿真器。──
        // 默认暂停（首帧先给人看静态模型）；开始后逐帧推进。
        if (sim_running_) {
            sim_.Step(soft_mesh_simulator::Simulator::kDefaultDt);
            // 顶点在 CPU 侧被物理改过 → 必须把新几何推上 GPU。
            // （M2 下顶点真的在动，这条 UpdateMesh 把新位置推上 GPU：
            //   不推就会看到模型停在原地不动。）
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
        cmds->sky     = light.sky;
        cmds->sun     = light.sun;
        cmds->ambient = light.ambient;
        cmds->tone_mapping = true;

        // ── 场景：地面 + 地面栅格 + 仿真网格。地面高度变化时同步视觉与物理。──
        // 滑条拖到新值 → SetGroundHeight（重建 quad + 写仿真器地面高度）。
        if (ground_y_ != ground_y_last_built_) {
            SetGroundHeight(ground_y_);
        }
        cmds->DrawObject3D(ground_mesh_, ground_mat_,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f});

        // 地面 1m 栅格（±5m，XZ 平面；用 3D 条带画线，便于目测距离）。
        if (show_ground_grid_) {
            DrawGroundGrid(cmds);
        }

        // 仿真网格以恒等摆放画在原点（顶点已是世界坐标：物理在世界系里积分）。
        cmds->DrawObject3D(mesh_id_, mesh_mat_,
                           /*center*/ {0.0f, 0.0f, 0.0f},
                           /*up*/     {0.0f, 1.0f, 0.0f},
                           /*front*/  {0.0f, 0.0f, 1.0f});

        // ── 仿真点可视化（M1 需求：把仿真点画成 2D 圆点，看它长什么样）──
        // 画在 3D 网格之前 → 2D 圆点始终盖在 3D 内容之上（"不看遮挡"）。
        if (show_sim_points_) {
            DrawSimulationPoints(cmds);
        }

        // ── 面板（仅交互窗口；headless 是纯 3D 截图）──
        if (show_panel_) {
            DrawPanel(input);
            ui_.End();
            ui_.Emit(cmds);
        }
    }

private:
    // ⭐ 用当前 d 滑条值重建仿真点集合（原始网格不变，只重建加密/邻接）。
    //
    // 调用者：OneIteration 里检测到 bind_distance_ui_ 与 sim_.bind_distance() 不一致时。
    // 用 original_mesh_ 作为重建输入 —— 它永远是 Init 时那份原始网格，
    // 不是 sim_.mesh()（后者在 M3+ 会被物理改过，拿它重建会把形变当绑定姿态）。
    void ReinitSimulation() {
        CHECK(!original_mesh_.positions.empty())
            << "ReinitSimulation 前必须先设置 original_mesh_（Init 时保存）";
        sim_.Init(original_mesh_, bind_distance_ui_);
        // 重建后网格回到绑定姿态 → 推上 GPU，并把推进时钟归零。
        UpdateMesh(mesh_id_, sim_.mesh());
        LOG(INFO) << "重建仿真点（d=" << bind_distance_ui_ << " m）：原始 "
                  << sim_.original_point_count() << " + 虚拟 "
                  << sim_.virtual_point_count() << " = "
                  << sim_.sim_point_count();
    }

    // 把仿真点集画成屏幕空间的 2D 圆点（2px 半径）。
    //
    // 颜色：原始顶点 = 红，虚拟顶点（加密插入）= 蓝。
    // 位置：世界坐标经 screen_projection 投到屏幕像素；相机背后的点不画。
    //
    // ⚠️ 用 2D 圆（非 3D 圆球）是刻意的（Danis 需求）：
    //   - 屏幕空间恒定 2px，不随距离缩放 → 远处的密集点也能分辨
    //   - 2D 图元在 3D 之后绘制，**不看遮挡** → 背面的点也能看见
    void DrawSimulationPoints(jpov::RenderCommandList* cmds) {
        const auto& pts = sim_.sim_positions();
        const size_t original = sim_.original_point_count();

        // 相机参数 → 投影用（与 cmds->camera 同源，保证圆点与 3D 网格对齐）。
        jpov::soft_mesh_simulator::ProjectionCamera cam;
        cam.position = cmds->camera.position;
        cam.target   = cmds->camera.target;
        cam.up       = cmds->camera.up;
        cam.fov_deg  = cmds->camera.fov;
        cam.near     = cmds->camera.near;
        cam.far      = cmds->camera.far;
        cam.fbo_w    = static_cast<int>(cmds->camera.fbo_3d_width_);
        cam.fbo_h    = static_cast<int>(cmds->camera.fbo_3d_height_);

        // 颜色（sRGB 屏显语义；2D 图元不经 tone map，直接用）。
        const jpov::Color kOriginalColor = {1.0f, 0.15f, 0.15f, 1.0f};  // 红
        const jpov::Color kVirtualColor  = {0.2f, 0.45f, 1.0f, 1.0f};   // 蓝

        for (size_t i = 0; i < pts.size(); ++i) {
            const auto sp = jpov::soft_mesh_simulator::ProjectToScreen(pts[i], cam);
            if (!sp.visible) {
                continue;
            }
            const jpov::Color c = (i < original) ? kOriginalColor : kVirtualColor;
            cmds->DrawCircle({sp.x, sp.y}, kSimPointRadiusPx, c);
        }
    }
    // 地面 1m 栅格：在 y = ground_y_ 的 XZ 平面上画直线网（±5m），
    // 用 3D 条带（DrawStrip3D）画 1m 间隔的平行线。用于目测仿真点的间距/尺度。
    //
    // 实现：每条线是一个「宽度 2cm 的窄条带」（两个三角形）。
    // 条纹贴在稍高于地面的 y（+1cm）避免与地面 z-fighting。
    void DrawGroundGrid(jpov::RenderCommandList* cmds) {
        const float y = ground_y_ + 0.01f;  // 抬高 1cm 避 z-fighting
        const float half = kGridHalfExtentM;
        const float hw = kGridLineHalfWidthM;  // 线半宽
        // 颜色：比地面（0.5 中灰）暗 → 形成可辨对比（同为中灰会像“隐形”）
        const jpov::Color color = {0.18f, 0.20f, 0.24f, 1.0f};

        // 平行于 Z 轴的线（固定 x）：x = -5, -4, ..., +5
        //
        // ⚠️ 缠绕必须是从 +Y 俯视的逆时针（CCW）。渲染器用 glFrontFace(GL_CCW)
        //   + glCullFace(GL_BACK)，地面朝 +Y，故从上方看的三角形顶点序须为 CCW，
        //   否则整片被背面裁剪掉（表现为“栅格完全不出现”）。
        for (float x = -half; x <= half + 1e-4f; x += kGridStepM) {
            // 条带规则：三角形 (p0,p1,p2) 与 (p1,p2,p3)。
            // 顶点序 = 从上方俯视 CCW。
            const std::vector<jpov::Vec3f> strip = {
                {x - hw, y, -half}, {x - hw, y, half},
                {x + hw, y, -half}, {x + hw, y, half}};
            cmds->DrawStrip3D(strip, color);
        }
        // 平行于 X 轴的线（固定 z）：z = -5, -4, ..., +5
        for (float z = -half; z <= half + 1e-4f; z += kGridStepM) {
            const std::vector<jpov::Vec3f> strip = {
                {-half, y, z - hw}, {-half, y, z + hw},
                { half, y, z - hw}, { half, y, z + hw}};
            cmds->DrawStrip3D(strip, color);
        }
    }

    // 文本测量回调：转发到 JPOV::MeasureTextWidth（真实字体进宽）。
    static float ViewerTextWidth(const char* text, float font_size,
                                 const char* /*font_alias*/, void* userdata) {
        SoftMeshViewerApp* app = static_cast<SoftMeshViewerApp*>(userdata);
        return app->MeasureTextWidth(/*alias=*/std::string(),
                                     /*text=*/text ? text : "", font_size);
    }

    // 面板（自上而下）：只读信息行 + 滑条 + 勾选。
    //
    // 布局用一个“行游标”自下而上堆叠：从底部倒数第一行开始，每画一行
    // 向上推一个 step。不写死行号，加/减行时只动局部。
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

        // 行游标：row_y 指向当前要画的那一行的 y（从底部向上递增行号）。
        float row_y = h - kBottom - kRowH;

        // 行 0（最底）：地面高度（米）：[-3,+3]，实时看物体落地面/阴影。
        ui_.SliderFloat("地面高度 y", &ground_y_,
                        jpov::UiRect{{left, row_y}, {kSliderW, kRowH}},
                        -3.0f, 3.0f, /*decimal_places*/2);
        row_y -= step;

        // 行 1：重力加速度 g（m/s²）。本阶段唯一的力；拖它看下坠快慢。
        ui_.SliderFloat("重力 g (m/s²)", &gravity_ui_,
                        jpov::UiRect{{left, row_y}, {kSliderW, kRowH}},
                        jpov::soft_mesh_simulator::Simulator::kMinGravity,
                        jpov::soft_mesh_simulator::Simulator::kMaxGravity,
                        /*decimal_places*/1);
        row_y -= step;

        // 行 2：关联距离 d（米）。拖它重建仿真点集合 → 红/蓝点实时变化。
        // 默认 0.1m；往小拖 → 更多蓝点（切得更碎），往大拖 → 蓝点消失。
        ui_.SliderFloat("关联距离 d (m)", &bind_distance_ui_,
                        jpov::UiRect{{left, row_y}, {kSliderW, kRowH}},
                        kBindDistanceMinM, kBindDistanceMaxM,
                        /*decimal_places*/3);
        row_y -= step;

        // 行 3：三个勾选（同一行排三个）：推进仿真 / 仿真点 / 地面栅格。
        // 说明：「推进仿真」默认关（首帧先看静态模型）；开启后每帧调 Step。
        const float kCheckW = kSliderW / 3.0f;
        ui_.Checkbox("推进仿真", &sim_running_,
                     jpov::UiRect{{left, row_y}, {kCheckW, kRowH}});
        ui_.Checkbox("仿真点", &show_sim_points_,
                     jpov::UiRect{{left + kCheckW, row_y}, {kCheckW, kRowH}});
        ui_.Checkbox("地面栅格", &show_ground_grid_,
                     jpov::UiRect{{left + 2.0f * kCheckW, row_y},
                                  {kCheckW, kRowH}});
        row_y -= step;

        // 行 4：仿真点计数（本步验收的核心数字）——原始/虚拟/合计。
        const std::string point_status =
            Format("仿真点：原始 %zu + 虚拟 %zu = %zu",
                   sim_.original_point_count(), sim_.virtual_point_count(),
                   sim_.sim_point_count());
        ui_.Text(point_status.c_str(), jpov::UiRect{{left, row_y}, {kSliderW, kRowH}});
        row_y -= step;

        // 行 5：仿真状态（只读）：时间 / 步数 / 顶点-三角形。
        const std::string sim_status =
            Format("仿真 t=%.2fs  步=%zu  顶点=%zu  三角形=%zu",
                   sim_.time(), sim_.step_count(),
                   sim_.vertex_count(), sim_.triangle_count());
        ui_.Text(sim_status.c_str(), jpov::UiRect{{left, row_y}, {kSliderW, kRowH}});
        row_y -= step;

        // 行 6（最上）：视角与颜色提示（只读）。
        ui_.Text("右键 drag 转视角 · 滚轮 zoom · 红=原始顶点 蓝=虚拟顶点",
                 jpov::UiRect{{left, row_y}, {kSliderW, kRowH}});
    }

    // 极简 snprintf 包装（面板只读文本用；避免在头里引入 printf 变参手写）。
    static std::string Format(const char* fmt, ...) {
        char buf[256];
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

}  // namespace soft_mesh_viewer
}  // namespace jpov

#endif  // JPOV_DEMO_SOFT_MESH_VIEWER_APP_H_
