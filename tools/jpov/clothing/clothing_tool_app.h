// JPOV 穿衣工具 — 渲染核心 App（可视化框架）
//
// 与 jpov_soft_mesh_viewer 是**姊妹工具**（同款渲染核心骨架：场景静态资源 + 交互
// 面板宿主 + 唯一 OneIteration 渲染体），但职责聚焦：**把「一件衣服」摆到「一个人体
// reference」旁边显示出来**，为后续「衣服贴合人体」的穿衣管线提供可视化底座。
//
// 需求（2026-09-27 Danis 定稿；2026-09-29 完善）：
//   - 框架负责：加载人体 reference（--body_reference_path）、加载衣服模型
//     （--cloth_path），并把二者显示在同一场景里。
//   - 2026-09-29 完善第一、二步：
//     ① **后台初始化**：进入界面时在后台线程为两份 glb 建「点到最近三角形」的
//        加速结构（geom::TriangleMatcher3d，见 clothing_init.h）。建图不快，故主界面
//        在此期间**黑底白字**显示后台进度（"正在..."）。
//     ② **衣服粗调面板**：左上角半透明黑底面板，用三个**填值输入框**（x/y/z）粗调
//        衣服模型的位置（回车 / 焦点丧失即生效，把衣服 center 移到给定坐标）。
//
// 仍不做（后续管线的事）：穿衣物理、衣服贴合。本文件只负责"加载 + 显示 + 粗调位置"。
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

#include <glog/logging.h>

#include "tools/jpov/clothing/clothing_axis_input.h"
#include "tools/jpov/clothing/clothing_init.h"
#include "tools/jpov/demo/skylight_scene.h"
#include "tools/jpov/demo/view_config.h"
#include "tools/jpov/include/jpov/jpov.h"
#include "tools/jpov/interface/ui.h"

namespace jpov {
namespace clothing {

// 视角 / 光照 / 地面工具：视角与地面复用姊妹查看器的纯函数（ViewConfig、ApplyInput、
// MakeGroundQuad、GroundMaterial 在 jpov_viewer 命名空间）。
// 光照改用 **skylight viewer 的天光配置**（太阳仰角 45° + 三色环境光），而非旧版的
// MakeNoonLighting——Danis 2026-09-29 反馈原来 ambient 偏暗，要求参照 skylight viewer。
using jpov_viewer::ApplyInput;
using jpov_viewer::DefaultView;
using jpov_viewer::GroundMaterial;
using jpov_viewer::MakeGroundQuad;
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

// x/y/z 输入框文本容量（含终止符）。填值格式如 "-0.35"，64 字节足够。
inline constexpr size_t kAxisInputCapacity = 64;

// 穿衣工具渲染核心 App。
//
// 场景 = 一件人体 reference + 一件衣服 + 灰地面；光固定正午晴天。
// 视角靠鼠标操作（右键 drag 转、滚轮 zoom），与姊妹查看器完全同款。
class ClothingToolApp : public JPOV {
public:
    using JPOV::JPOV;

    // ── 场景状态（main 在 Init() 后装配）──
    // 人体 reference 与衣服各自一个 glTF 资产（静态，直接整份画）。
    jpov::GltfObject body_;        // 人体 reference（--body_reference_path）
    jpov::GltfObject cloth_;       // 衣服模型（--cloth_path）
    std::string body_path_;        // 人体 reference 来源路径（面板显示）
    std::string cloth_path_;       // 衣服模型来源路径（面板显示）

    // ── 后台初始化（第一步：为两份 glb 建最近邻三角形匹配器）──
    // 由 main 在 Init() 后调 init_.Start(...) 发起；OneIteration 期间据其状态决定是
    // 显示"黑底白字进度页"还是"3D 场景 + 面板"。完成后 main 需据结果上传两个 GPU 资产。
    ClothingInitController init_;

    uint32_t ground_mesh_ = 0;           // 300×300 地面 quad 的 GPU handle
    jpov::PBRMaterial ground_mat_;       // 高粗糙灰色地面材质

    // ── 当前视角：交互（右键 drag / 滚轮实时改）与 headless 出图共用的单一事实源。──
    ViewConfig view_;

    // ── 显示开关（面板勾选）──
    bool show_body_  = true;   // 画人体 reference
    bool show_cloth_ = true;   // 画衣服模型

    // 地面高度（米）滑条值，[-3, +3]。
    float ground_y_ = -3.0f;

    // ── 衣服粗调：衣服模型 center（世界米）。第二部面板的 x/y/z 填值输入框写这里。──
    // 作为 DrawGltfObject(cloth_, center, up, front) 的 center（= 平移）。
    jpov::Vec3f cloth_center_{0.0f, 0.0f, 0.0f};

    // x/y/z 三个填值输入框的文本缓冲（跨帧持有；InputText 就地改写）。
    // 初始化为 "0"（与 cloth_center_ 初值一致）。commit 时解析回 cloth_center_。
    char axis_text_[3][kAxisInputCapacity] = {"0", "0", "0"};

    // 上一帧三个输入框的聚焦态（用于检测"焦点丧失/回车"这一提交边界）。
    // InputText 返回"帧末是否聚焦"，故 was_focused && !now_focused ⇒ 本帧发生提交。
    bool axis_focused_prev_[3] = {false, false, false};

    // 装配真实字体文本测量回调（UI 内部用），Init() 后调用一次。
    void InstallTextMeasure() {
        ui_.SetTextMeasure(&ClothingToolApp::ViewerTextWidth, this);
    }

    // 渲染/出图时是否绘制面板（交互窗口 = true；headless 纯 3D 截图 = false）。
    void SetShowPanel(bool show) { show_panel_ = show; }
    bool show_panel() const { return show_panel_; }

    // 后台初始化泵（每帧，在主/GL 线程调用；供 OneIteration 与 headless main 循环使用）。
    //
    // 三阶段：
    //   1) kBuilding：后台线程建图，屏幕上显示进度文案（不阻帧）。
    //   2) kDone 第一帧：先把进度页切成"正在上传 GPU 资源..."（本帧只画不做事，
    //      保证用户能看到文案，而不是被 LoadGltf 阻帧时停留在上一帧的旧文案）。
    //   3) 下一帧：真正做 GPU 上传（LoadGltf 必须走 GL，仅主线程；会阻帧几秒，
    //      但屏幕上已显示正确文案）+ 相机自适应。
    //
    // 返回：本帧是否已就绪（可画 3D 场景，即 GPU 资产已上传）。
    bool TickInit() {
        const bool changed = init_.Tick();
        if (init_.state() != InitState::kDone) {
            return false;  // kBuilding（继续显进度）/ kFailed（由调用方显失败页）
        }
        if (gpu_uploaded_) {
            return true;
        }
        // kDone 后分两帧：先刷屏显"上传中"，下一帧才真做（见上）。
        if (!upload_screen_drawn_) {
            upload_screen_drawn_ = true;
            return false;
        }
        UploadGpuAssets();
        (void)changed;
        return true;
    }

    // 上传两份 GPU 资产（仅主线程/GL 上下文）+ 相机自适应。
    // Pre-condition: init_.state() == kDone。
    void UploadGpuAssets() {
        CHECK_EQ(static_cast<int>(init_.state()), static_cast<int>(InitState::kDone));
        // GPU 上传（主线程）。失败会 LOG(FATAL)（与旧版一致，资产缺失是硬错误）。
        body_ = LoadGltf(body_path_);
        CHECK(!body_.empty()) << "人体 reference 加载失败或为空: " << body_path_;
        cloth_ = LoadGltf(cloth_path_);
        CHECK(!cloth_.empty()) << "衣服模型加载失败或为空: " << cloth_path_;
        gpu_uploaded_ = true;

        LOG(INFO) << "人体 reference: " << body_path_ << "（" << body_.size()
                  << " primitives）";
        LOG(INFO) << "衣服模型: " << cloth_path_ << "（" << cloth_.size()
                  << " primitives）";
        FitViewToScene();
    }

    // ⭐ 唯一的渲染体：交互循环与 headless 出图共用（zero 分叉）。
    void OneIteration(int64_t frame_count, const jpov::InputSnapshot& input,
                      const jpov::WindowInfo& winfo,
                      jpov::RenderCommandList* cmds) override {
        (void)frame_count;
        cmds->camera.fbo_3d_width_  = kViewerWidth;
        cmds->camera.fbo_3d_height_ = kViewerHeight;

        // 后台初始化泵：每帧 Tick 一次；完成分两帧（先显"上传中"再真传，见 TickInit）。
        // 交互循环（Run）与 headless 出图（RunOnce）共用同一条流程（zero 分叉）。
        const bool scene_ready = TickInit();

        // 后台初始化/上传未完成 → 黑底白字进度页（不画 3D 场景、不画面板）。
        // 完成后（kDone/kFailed）回到正常渲染；失败页也由 main 决定是否退出。
        if (init_.state() == InitState::kFailed) {
            DrawInitFailedScreen(cmds);
            return;
        }
        if (!scene_ready) {
            DrawInitProgressScreen(cmds);
            return;
        }

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

        // ── 光照：skylight viewer 同款天光（太阳仰角 45° + 三色环境光）──
        // 天光与主平行光、ambient 全由同一 SkyCommand 推导（同源，不漂移）；
        // 三色 ambient 按片元法线仰角在 [天, 天际线, 地] 间插值，去单色平感、
        // 避免旧 MakeNoonLighting 偏暗。太阳角度固定 45°（Danis 指定，不暴露滑条）。
        const jpov_skylight::SkyDegrees sky_deg;  // 默认：仰角 45°、方位 45°、浊度默认
        const jpov_skylight::SkyLighting light =
            jpov_skylight::MakeSkyLighting(sky_deg, /*tricolor_ambient*/ true);
        cmds->sky     = light.sky;
        cmds->sun     = light.dir_light;
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

        // 人体 reference 固定画在原点（不进粗调）；衣服按其 center（cloth_center_）
        // 平移——粗调面板的 x/y/z 输入框改的就是它。
        if (show_body_) {
            cmds->DrawGltfObject(body_,
                                 /*center*/ {0.0f, 0.0f, 0.0f},
                                 /*up*/     {0.0f, 1.0f, 0.0f},
                                 /*front*/  {0.0f, 0.0f, 1.0f});
        }
        if (show_cloth_) {
            cmds->DrawGltfObject(cloth_,
                                 /*center*/ cloth_center_,
                                 /*up*/     {0.0f, 1.0f, 0.0f},
                                 /*front*/  {0.0f, 0.0f, 1.0f});
        }

        // ── 面板（仅交互窗口；headless 是纯 3D 截图）──
        if (show_panel_) {
            DrawPanel(input, cmds);
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

    // 资产包围盒的并集（相机自适应用）。
    // 任一资产为空时只取另一份；两份都空时 valid=false。
    struct BoundsUnion {
        float min[3] = {0.0f, 0.0f, 0.0f};
        float max[3] = {0.0f, 0.0f, 0.0f};
        bool valid = false;
    };

    static void AccumulateBounds(const jpov::GltfObject& obj,
                                 BoundsUnion* out /*inout*/) {
        CHECK(out != nullptr);
        if (!obj.bounds_valid) {
            return;
        }
        for (int i = 0; i < 3; ++i) {
            if (!out->valid) {
                out->min[i] = obj.bounds_min[i];
                out->max[i] = obj.bounds_max[i];
            } else {
                out->min[i] = std::min(out->min[i], obj.bounds_min[i]);
                out->max[i] = std::max(out->max[i], obj.bounds_max[i]);
            }
        }
        out->valid = true;
    }

    // 按「人体 ∪ 衣服」两个资产的包围盒，把相机距离 R 自适当前夹。
    // 两份均无有效包围盒（退化）时保持当前 R 不变（DefaultView 的兜底值）。
    void FitViewToScene() {
        BoundsUnion bounds;
        AccumulateBounds(body_, &bounds);
        AccumulateBounds(cloth_, &bounds);
        if (!bounds.valid) {
            return;
        }
        view_.R = ViewConfig::FitRadius(bounds.min, bounds.max, /*fov_deg*/ 60.0);
        LOG(INFO) << "场景包围盒 [" << bounds.min[0] << "," << bounds.min[1] << ","
                  << bounds.min[2] << "] ~ [" << bounds.max[0] << "," << bounds.max[1]
                  << "," << bounds.max[2] << "]，初始 R=" << view_.R;
    }

    // 后台初始化进度页：整屏黑底（不透明）+ 居中白字。
    // 用 fbo 分辨率坐标，整屏铺满，确保"黑底白字"覆盖整个画面。
    void DrawInitProgressScreen(jpov::RenderCommandList* cmds) {
        const jpov::Color kBlack{0.0f, 0.0f, 0.0f, 1.0f};
        const jpov::Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};
        cmds->DrawRect(/*pos*/ {0.0f, 0.0f},
                       /*size*/ {static_cast<float>(kViewerWidth),
                                 static_cast<float>(kViewerHeight)},
                       kBlack);

        // 建图完成但 GPU 资产尚未上传时，显示"上传中"（见 TickInit 的两帧安排）。
        std::string msg = init_.progress_message();
        if (init_.state() == InitState::kDone && !gpu_uploaded_) {
            msg = "正在上传 GPU 资源...";
        }
        cmds->DrawText(msg.empty() ? "正在初始化..." : msg,
                       /*pos*/ {kViewerWidth * 0.5f, kViewerHeight * 0.5f},
                       /*font_size*/ 24.0f, kWhite,
                       jpov::TextAlignment::kCenter, kViewerFontAlias);
    }

    // 后台初始化失败页：整屏黑底 + 居中白字（出错原因）。
    void DrawInitFailedScreen(jpov::RenderCommandList* cmds) {
        const jpov::Color kBlack{0.0f, 0.0f, 0.0f, 1.0f};
        const jpov::Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};
        cmds->DrawRect(/*pos*/ {0.0f, 0.0f},
                       /*size*/ {static_cast<float>(kViewerWidth),
                                 static_cast<float>(kViewerHeight)},
                       kBlack);
        const std::string msg = init_.error_message();
        cmds->DrawText(msg.empty() ? "初始化失败" : ("初始化失败：" + msg),
                       /*pos*/ {kViewerWidth * 0.5f, kViewerHeight * 0.5f},
                       /*font_size*/ 20.0f, kWhite,
                       jpov::TextAlignment::kCenter, kViewerFontAlias);
    }

    // 面板（左上角半透明黑底 + 自上而下的控件）：
    //   行：衣服位置 x / y / z 三个填值输入框（回车或焦点丧失即生效）。
    //   行：地面高度滑条。
    //   行：显示开关（人体参考 / 衣服）。
    //   行：只读信息（来源路径 + primitive 数）。
    //
    // 布局用一个"行游标"自上而下堆叠：从面板顶部开始，每画一行向下推一个 step。
    void DrawPanel(const jpov::InputSnapshot& input,
                   jpov::RenderCommandList* cmds) {
        const float w = static_cast<float>(kViewerWidth);
        const float h = static_cast<float>(kViewerHeight);

        // 半透明黑底版（Danis 指定 0.5 半透明黑）。
        // 面板几何：左上角，留 margin，宽度取屏宽 1/3。
        const float kMargin  = 12.0f;
        const float kPad     = 10.0f;
        const float kRowH    = 28.0f;
        const float kSpacing = 6.0f;
        const float panel_w  = 0.34f * w;
        const float panel_x  = kMargin;
        const float panel_y  = kMargin;
        // 行数：3 个填值框 + 1 滑条 + 1 开关行 + 2 只读行 = 7 行。
        constexpr int kRows = 7;
        const float panel_h = kPad * 2.0f + kRows * kRowH +
                              (kRows - 1) * kSpacing;
        const jpov::Color kPanelBg{0.0f, 0.0f, 0.0f, 0.5f};
        cmds->DrawRect(/*pos*/ {panel_x, panel_y},
                       /*size*/ {panel_w, panel_h}, kPanelBg);

        // Ui 控件发出的坐标是**屏幕像素**（非面板局部坐标），故控件位置 = 面板屏幕位置 + 内边距。
        const float left = panel_x + kPad;
        const float top  = panel_y + kPad;
        const float row_w = panel_w - kPad * 2.0f;
        const float step = kRowH + kSpacing;
        float row_y = top;

        jpov::UiTheme theme = jpov::UiTheme::Default(kFontSize);
        theme.font_alias = kViewerFontAlias;
        ui_.Begin(input, theme, w, h, 1000.0f / kViewerFps);

        // ── 行 0~2：衣服位置 x / y / z（填值输入框，回车/焦点丧失即生效）──
        // 左边一列标签（输入框内部只在空值时画占位符，取值时标签会消失，
        // 故标签另起一列独立画，保证始终可见）。
        static const char* const kAxisLabels[3] = {"衣服 X", "衣服 Y", "衣服 Z"};
        const float label_w = 64.0f;
        const float axis_box_w = row_w - label_w - kSpacing;
        for (int axis = 0; axis < 3; ++axis) {
            ui_.Text(kAxisLabels[axis],
                     jpov::UiRect{{left, row_y}, {label_w, kRowH}}, /*stretch_w*/ false);
            const bool focused_now = ui_.InputText(
                "", axis_text_[axis], kAxisInputCapacity,
                jpov::UiRect{{left + label_w + kSpacing, row_y}, {axis_box_w, kRowH}});
            // 提交边界：上一帧聚焦、本帧不聚焦 ⇒ 框内发生了回车或焦点丧失。
            if (axis_focused_prev_[axis] && !focused_now) {
                CommitAxisInput(axis);
            }
            axis_focused_prev_[axis] = focused_now;
            row_y += step;
        }

        // ── 行 3：地面高度（米）：[-3,+3] ──
        ui_.SliderFloat("地面高度 y", &ground_y_,
                        jpov::UiRect{{left, row_y}, {row_w, kRowH}},
                        -3.0f, 3.0f, /*decimal_places*/2);
        row_y += step;

        // ── 行 4：显示开关（人体参考 / 衣服，各自独立）──
        const float kCheckW = row_w / 2.0f;
        ui_.Checkbox("显示人体 reference", &show_body_,
                     jpov::UiRect{{left, row_y}, {kCheckW, kRowH}});
        ui_.Checkbox("显示衣服", &show_cloth_,
                     jpov::UiRect{{left + kCheckW, row_y}, {kCheckW, kRowH}});
        row_y += step;

        // ── 行 5：人体 reference 来源 + primitive 数（只读，取文件名避免溢出）──
        const std::string body_line = Format(
            "人体：%s（%zu primitives）", BaseNameOf(body_path_).c_str(),
            body_.size());
        ui_.Text(body_line.c_str(), jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        row_y += step;

        // ── 行 6：衣服来源 + primitive 数（只读）──
        const std::string cloth_line = Format(
            "衣服：%s（%zu primitives）", BaseNameOf(cloth_path_).c_str(),
            cloth_.size());
        ui_.Text(cloth_line.c_str(), jpov::UiRect{{left, row_y}, {row_w, kRowH}});
        row_y += step;
    }

    // 取路径的文件名部分（去目录），用于面板只读行避免长路径溢出。
    static std::string BaseNameOf(const std::string& path) {
        const size_t slash = path.find_last_of("/\\");
        return (slash == std::string::npos) ? path : path.substr(slash + 1);
    }

    // 提交某个轴的填值输入：解析文本框内容为 float 写入 cloth_center_ 对应分量。
    // 解析失败（非法文本）→ 回退该框文本为当前值（不改变 cloth_center_）。
    // axis：0=X，1=Y，2=Z。Pre-condition: 0 <= axis < 3。
    void CommitAxisInput(int axis) {
        CHECK_GE(axis, 0);
        CHECK_LT(axis, 3);
        float value = 0.0f;
        const bool ok = ParseAxisValue(axis_text_[axis], &value);
        float* target = nullptr;
        switch (axis) {
            case 0:
                target = &cloth_center_.x();
                break;
            case 1:
                target = &cloth_center_.y();
                break;
            case 2:
                target = &cloth_center_.z();
                break;
            default:
                LOG(FATAL) << "CommitAxisInput: 非法 axis=" << axis;
        }
        if (ok) {
            *target = value;
        } else {
            // 非法输入：把文本框还原成当前值（给用户可见的"未生效"反馈）。
            snprintf(axis_text_[axis], kAxisInputCapacity, "%g", *target);
        }
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
    bool gpu_uploaded_ = false;          // 后台建图完成后是否已上传 GPU 资产（一次性）
    bool upload_screen_drawn_ = false;   // 是否已画过"上传中"页（见 TickInit 两帧安排）
    float ground_y_last_built_ = -3.0f;  // 上次建 quad 用的地面高度（变了才重建）
    jpov::Ui ui_;                        // 跨帧持有（滑条拖动态内部记忆）

    static constexpr float kFontSize = 16.0f;
};

}  // namespace clothing
}  // namespace jpov

#endif  // JPOV_CLOTHING_CLOTHING_TOOL_APP_H_
